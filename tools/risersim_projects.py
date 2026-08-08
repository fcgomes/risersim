"""
risersim_projects.py
=====================
Camada de persistência em sistema de arquivos para o "gerenciador de rodadas" (ver
docs/roadmap.md Eixo 3b): projetos (modelo + config compilada) e suas rodadas (execuções do
binário C++). Sem banco de dados -- o próprio layout de diretórios (compartilhado entre `web` e
`worker` via volume Docker) é a fonte de verdade, exatamente como desenhado na spec:

    projects/
      <project-id>/
        project.json
        input_simulation.json
        runs/
          run-YYYYMMDD-HHMMSS/
            run.json
            input_simulation.json
            stdout.log
            catenary_results.json   (após terminar)
            catenary_results.h5     (após terminar)

Importante: essa é uma raiz NOVA (`risersim/projects/`), separada do `risersim_results/` de
slot único usado pelo workflow manual `run_from_aml.py` -- não tocamos nesse último.

Usado tanto por `run_server.py` (API REST: cria projetos/rodadas, lê status) quanto por
`run_worker.py` (loop serial: acha rodadas pendentes, atualiza status/resultados). Nenhum dos
dois processos mantém estado em memória além do que está no disco -- é assim que os dois
processos (containers separados) se comunicam, sem broker nenhum.
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

_SCRIPT_DIR = Path(__file__).parent.resolve()

# Raiz padrão: risersim/projects (irmã de risersim_results/, nunca a mesma pasta). Sobrescrita
# via RISERSIM_PROJECTS_ROOT no ambiente Docker do docker-compose.yml -- mas o default já resolve
# certo dentro do container também, porque a estrutura de diretórios copiada pelo Dockerfile
# (WORKDIR /app/risersim, tools/ dentro dele) é a mesma relação relativa que existe localmente.
DEFAULT_PROJECTS_ROOT = Path(os.environ.get("RISERSIM_PROJECTS_ROOT", str(_SCRIPT_DIR.parent / "projects")))


def _now_iso():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def _slugify(name):
    slug = re.sub(r"[^a-z0-9]+", "-", (name or "").strip().lower()).strip("-")
    return slug or "projeto"


def generate_project_id(name, root):
    """Slug do nome do projeto, com sufixo numérico curto só se colidir com um diretório de
    projeto já existente (ver spec: IDs legíveis por humanos)."""
    slug = _slugify(name)
    if not (root / slug).exists():
        return slug
    for _ in range(20):
        suffix = ''.join(random.choices(string.digits, k=4))
        candidate = f"{slug}-{suffix}"
        if not (root / candidate).exists():
            return candidate
    # Extremamente improvável (20 colisões seguidas), mas não deixa travado.
    return f"{slug}-{int(datetime.now().timestamp())}"


def generate_run_id(project_dir):
    """ID baseado em timestamp (`run-YYYYMMDD-HHMMSS`), com sufixo contador só no raro caso de
    colisão no mesmo segundo (ver spec). Ordena cronologicamente por construção -- o worker
    usa isso pra achar "a rodada pendente mais antiga" sem precisar de outro campo."""
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

    def _store_source_files(self, pdir, xml_path, h5_path, aml_path=None):
        """Copia os arquivos de origem (XML+H5 reais exportados pelo ANFLEX, AML opcional) pra
        dentro de `projects/<id>/source/{model.xml, model.h5, model.aml}`, tornando o projeto
        autocontido -- resolve o gap onde `source.xml_path` apontava pra fora do diretório do
        projeto (dependente do mount de trunk/exemplos continuar no mesmo lugar). Caminho de
        código único usado tanto pelo fluxo de exemplo pré-descoberto quanto pelo de upload (ver
        run_server.py `api_create_project`/`api_upload_project`) -- nenhum dos dois duplica essa
        lógica.

        Retorna os caminhos RELATIVOS ao diretório do projeto (pra gravar em
        `project.json["source"]` sem vazar caminho absoluto de host)."""
        source_dir = pdir / "source"
        source_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(xml_path, source_dir / "model.xml")
        shutil.copy2(h5_path, source_dir / "model.h5")
        stored = {"xml_path": "source/model.xml", "h5_path": "source/model.h5", "aml_path": None}
        if aml_path and Path(aml_path).is_file():
            shutil.copy2(aml_path, source_dir / "model.aml")
            stored["aml_path"] = "source/model.aml"
        return stored

    def create_project(self, name, config, xml_path, h5_path, aml_path=None, origin=None, description=""):
        """Cria um novo projeto: `project.json` + `input_simulation.json` (o config compilado,
        pronto pro `ModelBuilder` consumir), copiando os arquivos de origem reais (XML+H5+AML
        opcional) pra dentro do próprio diretório do projeto (`_store_source_files`). Não dispara
        nenhuma rodada -- isso é um passo separado (`create_run`).

        `origin` é metadado de referência sobre de onde o modelo veio (ex.
        `{"example_id": "..."}` pro fluxo por exemplo pré-descoberto, `{"kind": "upload"}` pro
        fluxo de upload) -- mesclado com os caminhos internos já copiados em
        `project.json["source"]`, sem função além de exibição/depuração."""
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
        (pdir / "project.json").write_text(json.dumps(project, indent=2, ensure_ascii=False), encoding="utf-8")
        (pdir / "input_simulation.json").write_text(json.dumps(config, indent=2, ensure_ascii=False), encoding="utf-8")
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

    def create_run(self, project_id):
        """Cria uma nova rodada: congela (snapshot) o `input_simulation.json` atual do projeto
        dentro do diretório da rodada, e escreve `run.json` com `status: "pending"`. Não bloqueia
        esperando a rodada terminar -- o `run_worker.py` (processo separado) que vai pegá-la da
        fila e executar.

        Também grava aqui a proveniência que já dá pra saber na criação (ver docs/roadmap.md
        Eixo 3b): `model_hash` (sha256 do snapshot recém-copiado -- usado por
        `find_run_by_model_hash` pra evitar rodadas duplicadas do mesmo modelo), `schema_version`
        (lido de volta do próprio snapshot, gravado por `xml_h5_reader.py::to_risersim_json()`) e
        `web_version` (com qual versão da interface essa rodada foi criada). `solver_fingerprint`
        começa `None` -- só o `run_worker.py` tem acesso ao binário compilado (containers
        separados), então esse campo só é preenchido quando a rodada é de fato executada."""
        project = self.get_project(project_id)
        if project is None:
            raise FileNotFoundError(f"projeto '{project_id}' não encontrado")

        pdir = self.project_dir(project_id)
        input_json = pdir / "input_simulation.json"
        if not input_json.is_file():
            raise FileNotFoundError(f"projeto '{project_id}' não tem input_simulation.json")

        run_id = generate_run_id(pdir)
        rdir = self.run_dir(project_id, run_id)
        rdir.mkdir(parents=True)
        shutil.copy2(input_json, rdir / "input_simulation.json")
        (rdir / "stdout.log").touch()

        snapshot_bytes = (rdir / "input_simulation.json").read_bytes()
        model_hash = hashlib.sha256(snapshot_bytes).hexdigest()
        try:
            schema_version = json.loads(snapshot_bytes).get("schema_version")
        except json.JSONDecodeError:
            schema_version = None

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
            "web_version": WEB_VERSION,
            "solver_fingerprint": None,
        }
        self._write_run(project_id, run_id, run)
        return run

    def find_run_by_model_hash(self, project_id, model_hash):
        """Procura, entre as rodadas de um projeto, a mais recente já TERMINADA
        (`status in (converged, failed)`) com o mesmo `model_hash` -- usado por
        `POST /api/projects/<id>/runs` pra evitar disparar uma rodada duplicada do mesmo
        `input_simulation.json` sem querer (solver determinístico dado o mesmo binário: duas
        rodadas do mesmo modelo dão o mesmo resultado). Rodadas `pending`/`running` não contam
        (ainda não se sabe o resultado, e travar a fila serial por engano não vale o risco).
        Retorna o `run` (dict) ou `None`. `list_runs` já devolve mais recente primeiro."""
        for run in self.list_runs(project_id):
            if run.get("model_hash") == model_hash and run.get("status") in ("converged", "failed"):
                return run
        return None

    def update_run(self, project_id, run_id, **fields):
        run = self.get_run(project_id, run_id)
        if run is None:
            raise FileNotFoundError(f"rodada '{run_id}' do projeto '{project_id}' não encontrada")
        run.update(fields)
        self._write_run(project_id, run_id, run)
        return run

    def _write_run(self, project_id, run_id, run):
        rdir = self.run_dir(project_id, run_id)
        # Escreve em arquivo temporário + rename atômico: web e worker leem/escrevem run.json
        # concorrentemente (mesmo volume, processos diferentes) -- isso evita que uma leitura
        # concorrente pegue um JSON pela metade.
        tmp = rdir / "run.json.tmp"
        tmp.write_text(json.dumps(run, indent=2, ensure_ascii=False), encoding="utf-8")
        os.replace(str(tmp), str(rdir / "run.json"))

    def find_oldest_pending_run(self):
        """Usado pelo worker: acha a rodada `pending` mais antiga entre todos os projetos (fila
        serial única, sem prioridade). Retorna `(project_id, run_id)` ou `None`. Ordena pelo
        próprio run_id (formato `run-YYYYMMDD-HHMMSS[-N]`, que já ordena cronologicamente como
        string) -- sem precisar de nenhum índice/banco separado."""
        candidates = []
        for project in self.list_projects():
            for run in self.list_runs(project["id"]):
                if run.get("status") == "pending":
                    candidates.append((project["id"], run["id"]))
        if not candidates:
            return None
        candidates.sort(key=lambda pr: pr[1])
        return candidates[0]
