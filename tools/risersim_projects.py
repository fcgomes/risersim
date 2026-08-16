"""
risersim_projects.py
=====================
Filesystem persistence layer for the "run manager" (see docs/roadmap.md, Axis 3b): projects
(model + compiled config) and their runs (executions of the C++ binary). No database -- the
directory layout itself (shared between `web` and `worker` via a Docker volume) is the source of
truth, exactly as designed in the spec:

    projects/
      <project-id>/
        project.json
        input_simulation.json
        runs/
          run-YYYYMMDD-HHMMSS/
            run.json
            input_simulation.json
            stdout.log
            <Project>_<Case>_results.h5   (name known from creation, file appears once finished)

Important: this is a NEW root (`risersim/projects/`), separate from the single-slot
`risersim_results/` used by the manual `run_from_aml.py` workflow -- we don't touch that one.

Used both by `run_server.py` (REST API: creates projects/runs, reads status) and by
`run_worker.py` (serial loop: finds pending runs, updates status/results). Neither process keeps
in-memory state beyond what's on disk -- that's how the two processes (separate containers)
communicate, with no broker at all.
"""

import hashlib
import json
import os
import re
import random
import shutil
import string
from datetime import datetime, timezone
from pathlib import Path

from risersim_version import WEB_VERSION
from risersim_runner import list_aml_load_cases, build_config_from_aml

_SCRIPT_DIR = Path(__file__).parent.resolve()

# Default root: risersim/projects (a sibling of risersim_results/, never the same folder).
# Overridden via RISERSIM_PROJECTS_ROOT in docker-compose.yml's Docker environment -- but the
# default already resolves correctly inside the container too, because the directory structure
# copied by the Dockerfile (WORKDIR /app/risersim, tools/ inside it) is the same relative
# relationship that exists locally.
DEFAULT_PROJECTS_ROOT = Path(os.environ.get("RISERSIM_PROJECTS_ROOT", str(_SCRIPT_DIR.parent / "projects")))


def _now_iso():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def _slugify(name):
    slug = re.sub(r"[^a-z0-9]+", "-", (name or "").strip().lower()).strip("-")
    return slug or "projeto"


def _sanitize_filename_component(name):
    """Turns a display name (project name, load case name) into a safe results-filename
    component -- whitespace becomes `_`, anything else outside `[A-Za-z0-9_.-]` is dropped.
    Deliberately different from `_slugify()` above (which lowercases and uses dashes, for URL
    project IDs): this keeps the name recognizable as-is -- "Far" stays "Far", not "far" -- since
    it ends up in a filename the user actually sees (see `results_filename` in
    ProjectStore.create_run())."""
    component = re.sub(r"\s+", "_", (name or "").strip())
    component = re.sub(r"[^A-Za-z0-9_.-]", "", component)
    return component


def generate_project_id(name, root):
    """Slug of the project name, with a short numeric suffix only if it collides with an
    already-existing project directory (see spec: human-readable IDs)."""
    slug = _slugify(name)
    if not (root / slug).exists():
        return slug
    for _ in range(20):
        suffix = ''.join(random.choices(string.digits, k=4))
        candidate = f"{slug}-{suffix}"
        if not (root / candidate).exists():
            return candidate
    # Extremely unlikely (20 collisions in a row), but doesn't leave it stuck.
    return f"{slug}-{int(datetime.now().timestamp())}"


def generate_run_id(project_dir):
    """Timestamp-based ID (`run-YYYYMMDD-HHMMSS`), with a counter suffix only in the rare case of
    a same-second collision (see spec). Sorts chronologically by construction -- the worker uses
    this to find "the oldest pending run" without needing another field."""
    runs_dir = project_dir / "runs"
    base = "run-" + datetime.now().strftime("%Y%m%d-%H%M%S")
    if not (runs_dir / base).exists():
        return base
    counter = 2
    while (runs_dir / f"{base}-{counter}").exists():
        counter += 1
    return f"{base}-{counter}"


def _count_statuses(runs):
    counts = {}
    for r in runs:
        status = r.get("status", "unknown")
        counts[status] = counts.get(status, 0) + 1
    return counts


class DuplicateRunError(Exception):
    """Raised by `ProjectStore.create_run()` when a finished run with the same `model_hash`
    already exists and `force` wasn't set -- carries the existing run (`.run`) so the caller
    (`run_server.py`) can report it back (409, same shape `find_run_by_model_hash` already
    produced before this moved inside `create_run()`)."""
    def __init__(self, run):
        super().__init__(f"já existe uma simulação terminada com o mesmo model_hash (run '{run.get('id')}')")
        self.run = run


class ProjectStore:
    def __init__(self, root=None):
        self.root = Path(root) if root else DEFAULT_PROJECTS_ROOT
        self.root.mkdir(parents=True, exist_ok=True)

    # ---- projects ----

    def project_dir(self, project_id):
        return self.root / project_id

    def list_projects(self):
        projects = []
        for d in sorted(self.root.iterdir()) if self.root.is_dir() else []:
            if not d.is_dir():
                continue
            project = self._read_project_file(d)
            if project is None:
                continue
            runs = self.list_runs(project["id"])
            project["run_count"] = len(runs)
            project["status_counts"] = _count_statuses(runs)
            projects.append(project)
        projects.sort(key=lambda p: p.get("created_at", ""), reverse=True)
        return projects

    def get_project(self, project_id):
        return self._read_project_file(self.project_dir(project_id))

    def _read_project_file(self, project_dir):
        pfile = project_dir / "project.json"
        if not pfile.is_file():
            return None
        try:
            return json.loads(pfile.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            return None

    def _store_source_files(self, pdir, xml_path=None, h5_path=None, aml_path=None):
        """Copies the source files into `projects/<id>/source/`, making the project
        self-contained -- closes the gap where `source.xml_path` used to point outside the
        project directory (dependent on the trunk/exemplos mount staying in the same place).
        Single code path used by both the pre-discovered-example flow and the upload flow (see
        run_server.py `api_create_project`/`api_upload_project`) -- neither duplicates this logic.

        `xml_path`/`h5_path` are optional as a PAIR (either both given -- a real XML+H5 export --
        or neither): an `.aml`-only project (no XML+H5 export at all, see
        `risersim_runner.py::build_config_from_aml()`) has only `aml_path`. `aml_path` alone is
        also optional on its own for either case (an XML+H5 project's `.aml` isn't always
        available next to it).

        Preserves each file's ORIGINAL NAME (not a generic name like "model.xml") --
        `xml_path`/`h5_path`/`aml_path` already arrive here with the right basename in both flows
        (the real one for the pre-discovered example; the uploaded one, already sanitized by
        `api_upload_project::secure_filename` before being written to the temp directory) -- so
        the user recognizes which real case it came from just by looking at the filename on
        screen.

        Returns the paths RELATIVE to the project directory (to write into
        `project.json["source"]` without leaking an absolute host path); `None` for whichever of
        `xml_path`/`h5_path` wasn't given."""
        source_dir = pdir / "source"
        source_dir.mkdir(parents=True, exist_ok=True)
        stored = {"xml_path": None, "h5_path": None, "aml_path": None}
        if xml_path and h5_path:
            xml_name = Path(xml_path).name
            h5_name = Path(h5_path).name
            shutil.copy2(xml_path, source_dir / xml_name)
            shutil.copy2(h5_path, source_dir / h5_name)
            stored["xml_path"] = f"source/{xml_name}"
            stored["h5_path"] = f"source/{h5_name}"
        if aml_path and Path(aml_path).is_file():
            aml_name = Path(aml_path).name
            shutil.copy2(aml_path, source_dir / aml_name)
            stored["aml_path"] = f"source/{aml_name}"
        return stored

    def create_project(self, name, config, xml_path=None, h5_path=None, aml_path=None, origin=None, description=""):
        """Creates a new project: `project.json` + `input_simulation.json` (the compiled config,
        ready for `ModelBuilder` to consume), copying the real source files into the project's own
        directory (`_store_source_files`). Doesn't trigger any run -- that's a separate step
        (`create_run`).

        `xml_path`+`h5_path` (a real XML+H5 export) and `aml_path` alone (an `.aml`-only project,
        see `risersim_runner.py::build_config_from_aml()`) are the two supported combinations --
        the caller (`run_server.py`) is responsible for compiling `config` with whichever pipeline
        matches what was actually given here; this method itself doesn't care which one produced
        `config`, it only stores whatever source files it was handed.

        `origin` is reference metadata about where the model came from (e.g.
        `{"example_id": "..."}` for the pre-discovered-example flow, `{"kind": "upload"}` for the
        upload flow) -- merged with the internal paths already copied into
        `project.json["source"]`, with no purpose beyond display/debugging."""
        project_id = generate_project_id(name, self.root)
        pdir = self.project_dir(project_id)
        (pdir / "runs").mkdir(parents=True, exist_ok=True)

        stored_source = self._store_source_files(pdir, xml_path, h5_path, aml_path)
        source = {**(origin or {}), **stored_source}

        project = {
            "id": project_id,
            "name": name,
            "description": description or "",
            "created_at": _now_iso(),
            "web_version": WEB_VERSION,
            "source": source,
        }
        self._write_project(project_id, project)
        (pdir / "input_simulation.json").write_text(json.dumps(config, indent=2, ensure_ascii=False), encoding="utf-8")
        return project

    def _write_project(self, project_id, project):
        # Writes to a temp file + atomic rename -- same pattern as `_write_run` (only `web` writes
        # `project.json`, so there's less concurrency than with `run.json`, but rename() is
        # basically free and avoids any partial read).
        pdir = self.project_dir(project_id)
        tmp = pdir / "project.json.tmp"
        tmp.write_text(json.dumps(project, indent=2, ensure_ascii=False), encoding="utf-8")
        os.replace(str(tmp), str(pdir / "project.json"))

    def rename_project(self, project_id, new_name):
        """Renames a project -- only the `name` field in `project.json` changes; the `id`/
        directory on disk stay the same (avoids having to move files or invalidate any already
        shared/saved `?project=<id>`). Raises `FileNotFoundError` if the project doesn't exist,
        `ValueError` if the new name is empty."""
        new_name = (new_name or "").strip()
        if not new_name:
            raise ValueError("nome não pode ser vazio")
        project = self.get_project(project_id)
        if project is None:
            raise FileNotFoundError(f"projeto '{project_id}' não encontrado")
        project["name"] = new_name
        self._write_project(project_id, project)
        return project

    def duplicate_project(self, project_id, new_name=None):
        """Creates a NEW project (its own id/directory) that starts as a copy of `project_id`'s
        current source files + compiled `input_simulation.json` -- lets the user branch off an
        existing model (e.g. to try a parameter variation) without touching the original. Only
        the project-level input is copied; run history is deliberately NOT copied, a duplicate
        always starts with an empty `runs/` (see run_server.py::api_duplicate_project /
        project.js::duplicateProject). Raises `FileNotFoundError` if the source project (or its
        `input_simulation.json`) doesn't exist."""
        src_project = self.get_project(project_id)
        if src_project is None:
            raise FileNotFoundError(f"projeto '{project_id}' não encontrado")
        src_dir = self.project_dir(project_id)
        src_config_file = src_dir / "input_simulation.json"
        if not src_config_file.is_file():
            raise FileNotFoundError(f"input_simulation.json não encontrado para '{project_id}'")

        name = (new_name or "").strip() or f"{src_project['name']} (cópia)"
        new_id = generate_project_id(name, self.root)
        new_dir = self.project_dir(new_id)
        (new_dir / "runs").mkdir(parents=True, exist_ok=True)

        src_source_dir = src_dir / "source"
        if src_source_dir.is_dir():
            shutil.copytree(src_source_dir, new_dir / "source")
        shutil.copy2(src_config_file, new_dir / "input_simulation.json")

        project = {
            "id": new_id,
            "name": name,
            "description": src_project.get("description", ""),
            "created_at": _now_iso(),
            "web_version": WEB_VERSION,
            "source": src_project.get("source", {}),
        }
        self._write_project(new_id, project)
        return project

    # ---- runs ----

    def run_dir(self, project_id, run_id):
        return self.project_dir(project_id) / "runs" / run_id

    def list_runs(self, project_id):
        runs_dir = self.project_dir(project_id) / "runs"
        if not runs_dir.is_dir():
            return []
        runs = []
        for d in sorted(runs_dir.iterdir(), reverse=True):
            run = self._read_run_file(d)
            if run is not None:
                runs.append(run)
        return runs

    def get_run(self, project_id, run_id):
        return self._read_run_file(self.run_dir(project_id, run_id))

    def _read_run_file(self, run_dir):
        rfile = run_dir / "run.json"
        if not rfile.is_file():
            return None
        try:
            return json.loads(rfile.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            return None

    def create_run(self, project_id, load_case_id=None, force=False):
        """Creates a new run and writes `run.json` with `status: "pending"`. Doesn't block waiting
        for the run to finish -- `run_worker.py` (a separate process) is what picks it up from the
        queue and executes it.

        `load_case_id=None` (default): identical to this method's original behavior -- freezes
        (snapshots) the project's current `input_simulation.json` unchanged. `load_case_id` given:
        the project must have an `.aml` in `source` (raises `ValueError` if not); rebuilds the
        config for that specific %LOAD_CASE via `build_config_from_aml()` (risersim_runner.py,
        the same pipeline `run_from_aml.py --load-case-id` uses, including the MoorPy mesh-length
        correction -- docs/roadmap.md, Eixo 2a) instead of reusing the project-level snapshot --
        different load cases of the same physical model need different current/wave data, so a
        single project-level `input_simulation.json` can't represent all of them at once.

        Also records here the provenance that's already knowable at creation time (see
        docs/roadmap.md, Axis 3b): `model_hash` (sha256 of the config bytes this run will actually
        execute -- used by `find_run_by_model_hash` to avoid duplicate runs of the same model+case
        combination), `schema_version` (read back from the config itself), `web_version` (which
        interface version this run was created with), and the new `load_case_id`/`load_case_name`
        (both `None` for the default/no-selection path, same as any pre-existing run created before
        this field existed -- read everywhere via `.get()`, no migration needed).
        `solver_fingerprint` starts as `None` -- only `run_worker.py` has access to the compiled
        binary (separate containers), so this field is only filled in once the run is executed.

        `results_filename` (`<Project>[_<Case>]_results.h5`, sanitized via
        `_sanitize_filename_component()`) is computed and recorded HERE too, even though the
        actual file doesn't exist until the solver finishes -- project name + case name are both
        already known at creation time, so there's no reason to wait: `run_worker.py` reads this
        field back and passes it to the solver binary as the exact filename to write, instead of
        the solver writing a fixed placeholder name that gets renamed afterward.

        Raises `DuplicateRunError` (carrying the existing run) instead of creating a new one when
        a finished run with the same `model_hash` already exists and `force` isn't set -- moved
        here (from the caller) so the dedup check always hashes the bytes THIS run would actually
        use, not unconditionally the project-level file regardless of `load_case_id`."""
        project = self.get_project(project_id)
        if project is None:
            raise FileNotFoundError(f"projeto '{project_id}' não encontrado")

        pdir = self.project_dir(project_id)
        load_case_name = None

        if load_case_id is None:
            input_json = pdir / "input_simulation.json"
            if not input_json.is_file():
                raise FileNotFoundError(f"projeto '{project_id}' não tem input_simulation.json")
            config_bytes = input_json.read_bytes()
        else:
            aml_rel = (project.get("source") or {}).get("aml_path")
            if not aml_rel:
                raise ValueError(f"projeto '{project_id}' não tem .aml associado -- não é possível selecionar caso de carregamento")
            aml_path = pdir / aml_rel
            load_cases = list_aml_load_cases(aml_path)
            match = next((lc for lc in load_cases if lc.get("id") == load_case_id), None)
            if match is None:
                raise ValueError(f"load_case_id {load_case_id} não encontrado no .aml do projeto '{project_id}'")
            load_case_name = match.get("name")
            config = build_config_from_aml(aml_path, load_case_id=load_case_id)
            config_bytes = json.dumps(config, indent=2, ensure_ascii=False).encode("utf-8")

        model_hash = hashlib.sha256(config_bytes).hexdigest()
        if not force:
            dup = self.find_run_by_model_hash(project_id, model_hash)
            if dup is not None:
                raise DuplicateRunError(dup)

        run_id = generate_run_id(pdir)
        rdir = self.run_dir(project_id, run_id)
        rdir.mkdir(parents=True)
        (rdir / "input_simulation.json").write_bytes(config_bytes)
        (rdir / "stdout.log").touch()

        try:
            schema_version = json.loads(config_bytes).get("schema_version")
        except json.JSONDecodeError:
            schema_version = None

        parts = [_sanitize_filename_component(project.get("name"))]
        if load_case_name:
            parts.append(_sanitize_filename_component(load_case_name))
        results_filename = "_".join(p for p in parts if p) + "_results.h5"

        run = {
            "id": run_id,
            "project_id": project_id,
            "status": "pending",
            "created_at": _now_iso(),
            "started_at": None,
            "finished_at": None,
            "exit_code": None,
            "model_hash": model_hash,
            "schema_version": schema_version,
            "load_case_id": load_case_id,
            "load_case_name": load_case_name,
            "web_version": WEB_VERSION,
            "solver_fingerprint": None,
            "results_filename": results_filename,
        }
        self._write_run(project_id, run_id, run)
        return run

    def request_abort(self, project_id, run_id):
        """Signals `run_worker.py` to interrupt a `pending`/`running` run -- the usual
        communication channel (a file on the shared volume, no broker): creates an empty sentinel
        file `abort_requested` inside the run's directory. The worker polls for this file (see
        `run_worker.py::process_one_run`) both before launching the subprocess (run still
        `pending` -- just marks it `aborted` without ever running it) and during execution
        (`running` -- sends SIGTERM/SIGKILL to the subprocess). Doesn't delete anything or change
        `run.json` directly here -- only the worker makes that status transition, to avoid two
        processes writing to the same `run.json` at the same time.

        Raises `FileNotFoundError` if the run doesn't exist, `ValueError` if it's already in a
        terminal state (nothing to abort)."""
        run = self.get_run(project_id, run_id)
        if run is None:
            raise FileNotFoundError(f"simulação '{run_id}' do projeto '{project_id}' não encontrada")
        if run.get("status") not in ("pending", "running"):
            raise ValueError(f"simulação '{run_id}' já está '{run.get('status')}' -- nada a abortar")
        (self.run_dir(project_id, run_id) / "abort_requested").touch()

    def delete_run(self, project_id, run_id):
        """Deletes an entire run's directory (run.json, snapshot, log, results). Refuses
        `pending`/`running` runs: `run_worker.py`'s solver subprocess may be writing into this
        directory (`cwd=run_dir`, see `process_one_run`) at this very moment -- deleting it out
        from under it would cause unpredictable write failures mid-run, not a clean cancellation
        (that's what `request_abort` is for, which aborts first AND lets the worker finish
        writing run.json before the directory can be safely deleted). `converged`/`failed`/
        `aborted` are safe at any time (the worker no longer touches them). Raises `ValueError`
        (doesn't delete) in that case, `FileNotFoundError` if the run doesn't exist."""
        run = self.get_run(project_id, run_id)
        if run is None:
            raise FileNotFoundError(f"simulação '{run_id}' do projeto '{project_id}' não encontrada")
        if run.get("status") in ("pending", "running"):
            raise ValueError(f"simulação '{run_id}' está '{run.get('status')}' -- espere terminar antes de apagar")
        shutil.rmtree(self.run_dir(project_id, run_id))

    def delete_project(self, project_id):
        """Deletes the whole project (all its runs along with it). Same reasoning as
        `delete_run`: refuses if ANY run in the project is `pending`/`running` (the worker could
        be in the middle of it), so as not to delete the directory out from under a subprocess
        in progress."""
        pdir = self.project_dir(project_id)
        if not pdir.is_dir():
            raise FileNotFoundError(f"projeto '{project_id}' não encontrado")
        active = [r for r in self.list_runs(project_id) if r.get("status") in ("pending", "running")]
        if active:
            raise ValueError(f"projeto '{project_id}' tem {len(active)} simulaç{'ão' if len(active) == 1 else 'ões'} em andamento -- espere terminar antes de apagar")
        shutil.rmtree(pdir)

    def recover_orphaned_running_runs(self):
        """Called once at `run_worker.py` startup (main()), before the serial loop begins. The
        design is a single worker, no concurrency (see spec) -- so any run that's already
        `running` at the exact instant THIS process is coming up can only be orphaned: leftover
        from a previous worker execution that died mid-run (e.g. `docker compose up -d worker`
        after a redeploy, or the container going down for any other reason) without a chance to
        finish writing `run.json`. Without this check, the run stays stuck in `running` forever --
        the real solver isn't running anymore either (the subprocess died along with the old
        container), and nothing ever marks a terminal status.

        Marks it as `aborted` (not `failed`): there's no evidence the SOLVER failed on this
        input, only that the infrastructure was interrupted mid-way -- the same "no reliable
        result" semantics that already apply to a user-requested abort, including for
        `find_run_by_model_hash`'s dedup (which already ignores `aborted`, so a recovered orphan
        run doesn't stop the user from running that model again). Leaves a note in the run's own
        `stdout.log` explaining what happened (different from the "aborted by user" note that
        `run_worker.py::process_one_run` writes).

        Returns the list of recovered `(project_id, run_id)` pairs, for `run_worker.py` to log."""
        recovered = []
        for project in self.list_projects():
            for run in self.list_runs(project["id"]):
                if run.get("status") != "running":
                    continue
                self.update_run(project["id"], run["id"], status="aborted", finished_at=_now_iso())
                log_path = self.run_dir(project["id"], run["id"]) / "stdout.log"
                try:
                    with open(log_path, "a", encoding="utf-8") as f:
                        f.write("\n[run_worker] simulação interrompida por reinício do worker (ex.: "
                                "redeploy) antes de terminar -- marcada como 'aborted', sem "
                                "resultado confiável.\n")
                except OSError:
                    pass
                recovered.append((project["id"], run["id"]))
        return recovered

    def find_run_by_model_hash(self, project_id, model_hash):
        """Looks, among a project's runs, for the most recent already-FINISHED one
        (`status in (converged, failed)`) with the same `model_hash` -- used by
        `POST /api/projects/<id>/runs` to avoid accidentally triggering a duplicate run of the
        same `input_simulation.json` (deterministic solver given the same binary: two runs of the
        same model give the same result). `pending`/`running` runs don't count (the result isn't
        known yet, and blocking the serial queue by accident isn't worth the risk). Returns the
        `run` (dict) or `None`. `list_runs` already returns most-recent-first."""
        for run in self.list_runs(project_id):
            if run.get("model_hash") == model_hash and run.get("status") in ("converged", "failed"):
                return run
        return None

    def update_run(self, project_id, run_id, **fields):
        run = self.get_run(project_id, run_id)
        if run is None:
            raise FileNotFoundError(f"simulação '{run_id}' do projeto '{project_id}' não encontrada")
        run.update(fields)
        self._write_run(project_id, run_id, run)
        return run

    def _write_run(self, project_id, run_id, run):
        rdir = self.run_dir(project_id, run_id)
        # Writes to a temp file + atomic rename: web and worker read/write run.json concurrently
        # (same volume, different processes) -- this prevents a concurrent read from catching a
        # half-written JSON.
        tmp = rdir / "run.json.tmp"
        tmp.write_text(json.dumps(run, indent=2, ensure_ascii=False), encoding="utf-8")
        os.replace(str(tmp), str(rdir / "run.json"))

    def find_oldest_pending_run(self):
        """Used by the worker: finds the oldest `pending` run across all projects (a single serial
        queue, no priority). Returns `(project_id, run_id)` or `None`. Sorts by the run_id itself
        (format `run-YYYYMMDD-HHMMSS[-N]`, which already sorts chronologically as a string) --
        without needing any separate index/database."""
        candidates = []
        for project in self.list_projects():
            for run in self.list_runs(project["id"]):
                if run.get("status") == "pending":
                    candidates.append((project["id"], run["id"]))
        if not candidates:
            return None
        candidates.sort(key=lambda pr: pr[1])
        return candidates[0]
