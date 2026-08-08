"""
run_worker.py
==============
Processo persistente (loop serial) que executa as rodadas do gerenciador de rodadas (ver
docs/roadmap.md Eixo 3b). Entrypoint separado de `run_server.py` (containers/processos
diferentes -- serviço `worker` do docker-compose.yml), mas compartilha a mesma camada de dados
(`risersim_projects.py`) e a mesma lógica de invocação do binário (`risersim_runner.py`).

Modelo de execução (decidido na spec, não renegociável neste Phase 1):
- Um único processo persistente, sem Docker-in-Docker, sem spawns de container por rodada.
- Fila serial: só uma rodada de cada vez, sem concorrência.
- Sem broker nenhum -- o próprio sistema de arquivos compartilhado (volume Docker) é o canal de
  comunicação: `run.json` é o status, `stdout.log` é o progresso ao vivo (o binário C++ já usa
  `std::endl` a cada linha, então o arquivo já fica com flush frequente sem nenhuma mudança no
  C++ -- ver src/main.cpp/simulation.cpp).

Uso: `python3 run_worker.py` (sem argumentos; lê `RISERSIM_PROJECTS_ROOT`/`RISERSIM_EXE_PATH`
opcionais do ambiente, como o resto do gerenciador de rodadas).
"""

import hashlib
import os
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

_SCRIPT_DIR = Path(__file__).parent.resolve()
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))

from risersim_runner import find_executable, run_simulation_subprocess
from risersim_projects import ProjectStore

POLL_INTERVAL_S = 1.0


def _now_iso():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def compute_solver_fingerprint(exe_path: Path) -> str:
    """sha256 do binário compilado `risersim_test_main` -- fingerprint automático de versão do
    solver (ver docs/roadmap.md Eixo 3b, proveniência de versões). Este diretório não é um
    repositório git, então não há commit hash pra usar; o próprio bug de memória não-inicializada
    corrigido nesta sessão (mesmo config, binário diferente, resultado diferente) é o motivo real
    de valer a pena rastrear isso por rodada. Só o `worker` (este processo) tem acesso ao binário
    compilado -- `web` roda num container separado, sem o binário -- por isso esse campo só é
    preenchido aqui, não em `risersim_projects.py::create_run()`."""
    h = hashlib.sha256()
    with open(exe_path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def process_one_run(store: ProjectStore, exe_path: Path, solver_fingerprint: str, project_id: str, run_id: str) -> None:
    """Executa uma única rodada até o fim: pending -> running -> converged/failed. Bloqueia até
    o processo `risersim_test_main` terminar (é exatamente por isso que o worker processa uma
    rodada de cada vez -- fila serial, sem concorrência, ver spec)."""
    run_dir = store.run_dir(project_id, run_id)
    input_json = run_dir / "input_simulation.json"
    stdout_log = run_dir / "stdout.log"

    print(f"[run_worker] iniciando rodada {project_id}/{run_id}", flush=True)
    store.update_run(project_id, run_id, status="running", started_at=_now_iso(), solver_fingerprint=solver_fingerprint)

    try:
        exit_code = run_simulation_subprocess(exe_path, input_json, run_dir, stdout_file=stdout_log)
    except Exception as exc:
        # Falha ao nem conseguir lançar o processo (ex.: binário sumiu entre o find_executable()
        # de startup e agora) -- registra como failed em vez de derrubar o worker inteiro, que
        # precisa continuar vivo para pegar a próxima rodada da fila.
        with open(stdout_log, "a", encoding="utf-8") as log_f:
            log_f.write(f"\n[run_worker] ERRO ao executar: {exc}\n")
        store.update_run(project_id, run_id, status="failed", finished_at=_now_iso(), exit_code=None)
        print(f"[run_worker] rodada {project_id}/{run_id} falhou ao iniciar: {exc}", flush=True)
        return

    status = "converged" if exit_code == 0 else "failed"
    store.update_run(project_id, run_id, status=status, finished_at=_now_iso(), exit_code=exit_code)
    print(f"[run_worker] rodada {project_id}/{run_id} terminou: {status} (exit_code={exit_code})", flush=True)


def main() -> None:
    store = ProjectStore()
    exe_path = find_executable(os.environ.get("RISERSIM_EXE_PATH"))
    # Calculado uma vez quando o binário é achado/redescoberto (não a cada rodada -- o binário só
    # muda entre rebuilds da imagem, nunca em runtime), reaproveitado por todas as rodadas até o
    # worker reiniciar ou o binário sumir e reaparecer (ver loop abaixo).
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
            # Rodada sumiu do disco entre find_oldest_pending_run() e agora (não deveria
            # acontecer em uso normal, sem nenhuma limpeza automática ainda) -- loga e segue.
            print(f"[run_worker] rodada {project_id}/{run_id} não encontrada mais: {exc}", flush=True)


if __name__ == "__main__":
    main()
