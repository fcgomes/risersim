"""
run_worker.py
==============
Persistent process (serial loop) that executes the run manager's runs (see docs/roadmap.md, Axis
3b). A separate entrypoint from `run_server.py` (different containers/processes -- the `worker`
service in docker-compose.yml), but shares the same data layer (`risersim_projects.py`) and the
same binary-invocation logic (`risersim_runner.py`).

Execution model (decided in the spec, not up for renegotiation in this Phase 1):
- A single persistent process, no Docker-in-Docker, no per-run container spawns.
- Serial queue: only one run at a time, no concurrency.
- No broker at all -- the shared filesystem itself (Docker volume) is the communication channel:
  `run.json` is the status, `stdout.log` is the live progress (the C++ binary already uses
  `std::endl` on every line, so the file already gets flushed frequently with no change needed in
  the C++ -- see src/main.cpp/simulation.cpp).

Usage: `python3 run_worker.py` (no arguments; reads the optional `RISERSIM_PROJECTS_ROOT`/
`RISERSIM_EXE_PATH` from the environment, like the rest of the run manager).
"""

import hashlib
import os
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

_SCRIPT_DIR = Path(__file__).parent.resolve()
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))

from risersim_runner import find_executable
from risersim_projects import ProjectStore

POLL_INTERVAL_S = 1.0
# Interval for checking the `abort_requested` sentinel file WHILE the solver subprocess is
# running -- shorter than POLL_INTERVAL_S (which only regulates the wait for a NEXT pending run)
# because this is the reaction time to an "Abort" click in the middle of a real run.
ABORT_POLL_INTERVAL_S = 0.5
# Time given for the process to exit on its own after SIGTERM before forcing it with SIGKILL.
ABORT_TERMINATE_TIMEOUT_S = 5.0


def _now_iso():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def compute_solver_fingerprint(exe_path: Path) -> str:
    """sha256 of the compiled `risersim_test_main` binary -- an automatic solver version
    fingerprint (see docs/roadmap.md, Axis 3b, version provenance). This directory isn't a git
    repository, so there's no commit hash to use; the very uninitialized-memory bug fixed in this
    session (same config, different binary, different result) is the real reason it's worth
    tracking this per run. Only the `worker` (this process) has access to the compiled binary --
    `web` runs in a separate container, without the binary -- which is why this field is only
    filled in here, not in `risersim_projects.py::create_run()`."""
    h = hashlib.sha256()
    with open(exe_path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def process_one_run(store: ProjectStore, exe_path: Path, solver_fingerprint: str, project_id: str, run_id: str) -> None:
    """Executes a single run to completion: pending -> running -> converged/failed/aborted.
    Blocks until the `risersim_test_main` process finishes OR is aborted (this is exactly why the
    worker processes one run at a time -- serial queue, no concurrency, see spec).

    Uses `subprocess.Popen` (not `subprocess.run`, which would block with no chance to react to
    anything) so it can periodically check the `abort_requested` sentinel file (see
    `risersim_projects.py::request_abort`) while the subprocess runs -- if it appears, sends
    SIGTERM and gives the solver a short time to exit on its own before forcing it with
    SIGKILL."""
    run_dir = store.run_dir(project_id, run_id)
    input_json = run_dir / "input_simulation.json"
    stdout_log = run_dir / "stdout.log"
    abort_flag = run_dir / "abort_requested"

    if abort_flag.is_file():
        # Aborted while still "pending" (the user cancelled before the worker even picked the run
        # up from the queue) -- doesn't even get to launch the subprocess.
        abort_flag.unlink(missing_ok=True)
        now = _now_iso()
        store.update_run(project_id, run_id, status="aborted", started_at=now, finished_at=now, exit_code=None)
        print(f"[run_worker] rodada {project_id}/{run_id} abortada antes de iniciar", flush=True)
        return

    print(f"[run_worker] iniciando rodada {project_id}/{run_id}", flush=True)
    store.update_run(project_id, run_id, status="running", started_at=_now_iso(), solver_fingerprint=solver_fingerprint)

    cmd = [str(exe_path), str(input_json), str(run_dir)]
    try:
        log_f = open(stdout_log, "w", encoding="utf-8")
        proc = subprocess.Popen(cmd, cwd=str(run_dir), stdout=log_f, stderr=subprocess.STDOUT)
    except Exception as exc:
        # Failed to even launch the process (e.g. the binary disappeared between startup's
        # find_executable() and now) -- records it as failed instead of bringing down the whole
        # worker, which needs to stay alive to pick up the next run in the queue.
        with open(stdout_log, "a", encoding="utf-8") as err_f:
            err_f.write(f"\n[run_worker] ERRO ao executar: {exc}\n")
        store.update_run(project_id, run_id, status="failed", finished_at=_now_iso(), exit_code=None)
        print(f"[run_worker] rodada {project_id}/{run_id} falhou ao iniciar: {exc}", flush=True)
        return

    aborted = False
    try:
        while proc.poll() is None:
            if abort_flag.is_file():
                aborted = True
                proc.terminate()
                try:
                    proc.wait(timeout=ABORT_TERMINATE_TIMEOUT_S)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
                break
            time.sleep(ABORT_POLL_INTERVAL_S)
    finally:
        log_f.close()

    if aborted:
        abort_flag.unlink(missing_ok=True)
        with open(stdout_log, "a", encoding="utf-8") as err_f:
            err_f.write("\n[run_worker] simulação abortada pelo usuário.\n")
        store.update_run(project_id, run_id, status="aborted", finished_at=_now_iso(), exit_code=proc.returncode)
        print(f"[run_worker] rodada {project_id}/{run_id} abortada pelo usuário", flush=True)
        return

    exit_code = proc.returncode
    status = "converged" if exit_code == 0 else "failed"
    store.update_run(project_id, run_id, status=status, finished_at=_now_iso(), exit_code=exit_code)
    print(f"[run_worker] rodada {project_id}/{run_id} terminou: {status} (exit_code={exit_code})", flush=True)


def main() -> None:
    store = ProjectStore()

    # Runs stuck in "running" can only be orphans of a previous execution of this same worker
    # that died mid-way (container recreated/restarted -- e.g. a redeploy) -- see
    # risersim_projects.py::recover_orphaned_running_runs. Without this they'd stay stuck forever
    # (nothing else re-evaluates that status), even blocking deletion of their project.
    for project_id, run_id in store.recover_orphaned_running_runs():
        print(f"[run_worker] rodada {project_id}/{run_id} estava 'running' de uma execução "
              f"anterior do worker -- recuperada como 'aborted'", flush=True)

    exe_path = find_executable(os.environ.get("RISERSIM_EXE_PATH"))
    # Computed once when the binary is found/rediscovered (not on every run -- the binary only
    # changes between image rebuilds, never at runtime), reused by every run until the worker
    # restarts or the binary disappears and reappears (see loop below).
    solver_fingerprint = compute_solver_fingerprint(exe_path) if exe_path is not None else None

    print(f"[run_worker] projects_root={store.root}", flush=True)
    if exe_path is None:
        print("[run_worker] AVISO: risersim_test_main não encontrado ainda -- aguardando "
              "(rodadas pendentes ficarão paradas em 'pending' até o binário aparecer).", flush=True)
    else:
        print(f"[run_worker] usando binário: {exe_path} (fingerprint {solver_fingerprint[:12]}...)", flush=True)

    print("[run_worker] loop serial iniciado, aguardando rodadas pendentes...", flush=True)
    while True:
        if exe_path is None:
            exe_path = find_executable(os.environ.get("RISERSIM_EXE_PATH"))
            if exe_path is not None:
                solver_fingerprint = compute_solver_fingerprint(exe_path)
                print(f"[run_worker] binário encontrado: {exe_path} (fingerprint {solver_fingerprint[:12]}...)", flush=True)

        job = store.find_oldest_pending_run()
        if job is None or exe_path is None:
            time.sleep(POLL_INTERVAL_S)
            continue

        project_id, run_id = job
        try:
            process_one_run(store, exe_path, solver_fingerprint, project_id, run_id)
        except FileNotFoundError as exc:
            # Run disappeared from disk between find_oldest_pending_run() and now (shouldn't
            # happen in normal use, since there's no automatic cleanup yet) -- log it and move on.
            print(f"[run_worker] rodada {project_id}/{run_id} não encontrada mais: {exc}", flush=True)


if __name__ == "__main__":
    main()
