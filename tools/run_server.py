"""
run_server.py
==============
Flask app do gerenciador de rodadas (ver docs/roadmap.md Eixo 3b): API REST (projetos/rodadas) +
serve os mesmos arquivos estáticos que `dev_server.py` servia (posprocessor.html/preprocessor.html,
js/, css/) -- ou seja, é um substituto drop-in de `dev_server.py` com rotas de API a mais, não um
serviço à parte que exige outro processo pra servir o frontend.

Não executa nenhuma rodada diretamente: `POST /api/projects/<id>/runs` só grava `status: pending`
no disco (via `risersim_projects.ProjectStore`) e retorna na hora -- quem efetivamente roda o
binário C++ é o processo separado `run_worker.py` (serviço `worker` do docker-compose.yml),
consumindo a mesma fila no mesmo volume compartilhado. `web` e `worker` só se falam através do
sistema de arquivos, sem nenhum broker.

Uso: `python3 run_server.py [porta]` (default 8000, mesma porta que `dev_server.py` usava).
"""

import hashlib
import os
import sys
import tempfile
from pathlib import Path

from flask import Flask, Response, abort, jsonify, request, send_from_directory

_SCRIPT_DIR = Path(__file__).parent.resolve()
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))

from risersim_projects import ProjectStore
from risersim_runner import build_config_from_xml_h5, discover_xml_h5_examples
from risersim_version import WEB_VERSION

# Raiz de `trunk/exemplos/` -- de tools/ (_SCRIPT_DIR) sobe dois níveis (risersim/, depois
# trunk/); a mesma relação vale dentro do container (WORKDIR /app/risersim, tools/ dentro dele,
# então parent.parent = /app -- ver docker-compose.yml, que monta trunk/exemplos em /app/exemplos).
DEFAULT_EXEMPLOS_ROOT = _SCRIPT_DIR.parent.parent / "exemplos"
EXEMPLOS_ROOT = Path(os.environ.get("RISERSIM_EXEMPLOS_ROOT", str(DEFAULT_EXEMPLOS_ROOT)))

RESULT_FILENAMES = {
    "catenary_results.json": "application/json",
    "catenary_results.h5": "application/octet-stream",
    # Snapshot do input que essa rodada específica de fato usou (congelado por
    # ProjectStore.create_run() -- ver risersim_projects.py) -- reaproveita a mesma rota genérica
    # de resultados por rodada em vez de um endpoint dedicado, já que é só mais um arquivo dentro
    # do diretório da rodada. Consumido por preprocessor_app.js (?project=&run=), ver Eixo 3b.
    "input_simulation.json": "application/json",
}

app = Flask(__name__, static_folder=None)
store = ProjectStore()


@app.after_request
def add_no_cache_headers(resp):
    # Mesma motivação de dev_server.py: sem isso o navegador cacheia agressivamente os módulos ES
    # (js/*.js) e HTML, fazendo edições parecerem "não aplicadas" sem um hard refresh.
    resp.headers['Cache-Control'] = 'no-store, no-cache, must-revalidate'
    resp.headers['Pragma'] = 'no-cache'
    resp.headers['Expires'] = '0'
    return resp


# ---------------------------------------------------------------------------
# API: exemplos disponíveis para criar projeto (Phase 1: só XML+H5, ver spec)
# ---------------------------------------------------------------------------

@app.route('/api/examples', methods=['GET'])
def api_list_examples():
    return jsonify(discover_xml_h5_examples(EXEMPLOS_ROOT))


# ---------------------------------------------------------------------------
# API: versão da web app (interface+pré+pós, mesmo deploy -- ver risersim_version.py e
# docs/roadmap.md Eixo 3b, proveniência de versões)
# ---------------------------------------------------------------------------

@app.route('/api/version', methods=['GET'])
def api_version():
    return jsonify({"web_version": WEB_VERSION})


# ---------------------------------------------------------------------------
# API: projetos
# ---------------------------------------------------------------------------

@app.route('/api/projects', methods=['GET'])
def api_list_projects():
    return jsonify(store.list_projects())


@app.route('/api/projects', methods=['POST'])
def api_create_project():
    body = request.get_json(force=True, silent=True) or {}

    example_id = body.get('example_id')
    if example_id:
        examples = {e['id']: e for e in discover_xml_h5_examples(EXEMPLOS_ROOT)}
        example = examples.get(example_id)
        if example is None:
            return jsonify({"error": f"example_id desconhecido: {example_id}"}), 400
        name = body.get('name') or example['name']
        xml_path, h5_path, aml_path = example['xml_path'], example['h5_path'], example['aml_path']
        origin = {"example_id": example_id}
    else:
        name = body.get('name')
        xml_path = body.get('xml_path')
        h5_path = body.get('h5_path')
        aml_path = body.get('aml_path')
        if not name or not xml_path or not h5_path:
            return jsonify({
                "error": "informe 'example_id', ou 'name'+'xml_path'+'h5_path' ('aml_path' é opcional)"
            }), 400
        if not Path(xml_path).is_file():
            return jsonify({"error": f"xml_path não encontrado: {xml_path}"}), 400
        if not Path(h5_path).is_file():
            return jsonify({"error": f"h5_path não encontrado: {h5_path}"}), 400
        origin = None

    try:
        config = build_config_from_xml_h5(xml_path, h5_path, aml_path=aml_path)
    except Exception as exc:
        return jsonify({"error": f"falha ao compilar o modelo a partir do XML+H5: {exc}"}), 400

    # create_project() copia xml_path/h5_path/aml_path pra dentro de projects/<id>/source/ (ver
    # ProjectStore._store_source_files) -- o projeto fica autocontido mesmo que trunk/exemplos/
    # deixe de estar montado depois.
    project = store.create_project(name=name, config=config, xml_path=xml_path, h5_path=h5_path,
                                    aml_path=aml_path, origin=origin, description=body.get('description', ''))
    return jsonify(project), 201


@app.route('/api/projects/upload', methods=['POST'])
def api_upload_project():
    """Cria um projeto a partir de um upload direto (multipart/form-data: `name`, `xml_file`,
    `h5_file`, `aml_file` opcional) -- alternativa ao fluxo por exemplo pré-descoberto
    (`api_create_project` acima) pra quando o usuário tem um par XML+H5 (ou um XML/AML próprio)
    fora de `trunk/exemplos/`. Reaproveita `build_config_from_xml_h5()` tal e qual (mesma função
    do fluxo por exemplo), sem duplicar a lógica de compilação -- só muda de onde os arquivos de
    origem vêm."""
    name = (request.form.get('name') or '').strip()
    xml_file = request.files.get('xml_file')
    h5_file = request.files.get('h5_file')
    aml_file = request.files.get('aml_file')

    if not name:
        return jsonify({"error": "informe 'name'"}), 400
    if xml_file is None or not xml_file.filename:
        return jsonify({"error": "'xml_file' é obrigatório"}), 400
    if h5_file is None or not h5_file.filename:
        return jsonify({"error": "'h5_file' é obrigatório"}), 400

    with tempfile.TemporaryDirectory() as tmp_str:
        tmp = Path(tmp_str)
        xml_path = tmp / "upload.xml"
        h5_path = tmp / "upload.h5"
        xml_file.save(str(xml_path))
        h5_file.save(str(h5_path))
        aml_path = None
        if aml_file is not None and aml_file.filename:
            aml_path = tmp / "upload.aml"
            aml_file.save(str(aml_path))

        try:
            config = build_config_from_xml_h5(xml_path, h5_path, aml_path=aml_path)
        except Exception as exc:
            return jsonify({"error": f"falha ao compilar o modelo a partir do XML+H5: {exc}"}), 400

        # A cópia pra projects/<id>/source/ (dentro de create_project(), via
        # _store_source_files()) acontece ainda dentro do `with` -- os arquivos temporários
        # continuam existindo até aqui, só são apagados na saída do bloco.
        project = store.create_project(name=name, config=config, xml_path=xml_path, h5_path=h5_path,
                                        aml_path=aml_path, origin={"kind": "upload"})
    return jsonify(project), 201


@app.route('/api/projects/<project_id>', methods=['GET'])
def api_get_project(project_id):
    project = store.get_project(project_id)
    if project is None:
        abort(404)
    project = dict(project)
    project['runs'] = store.list_runs(project_id)
    return jsonify(project)


@app.route('/api/projects/<project_id>/input', methods=['GET'])
def api_project_input(project_id):
    """Serve o `input_simulation.json` do NÍVEL DO PROJETO -- o "atual", fora de qualquer rodada
    específica (o que uma `POST .../runs` nova usaria agora). Complementa a rota já existente de
    resultados por rodada (que serve o SNAPSHOT congelado de uma rodada específica, via
    RESULT_FILENAMES) -- útil pra inspecionar o modelo no pré-processador antes de disparar
    qualquer rodada. Ver preprocessor_app.js::resolveInputUrl()."""
    pdir = store.project_dir(project_id)
    file_path = pdir / "input_simulation.json"
    if not file_path.is_file():
        abort(404)
    return send_from_directory(str(pdir), "input_simulation.json", mimetype="application/json")


# ---------------------------------------------------------------------------
# API: rodadas
# ---------------------------------------------------------------------------

@app.route('/api/projects/<project_id>/runs', methods=['POST'])
def api_create_run(project_id):
    if store.get_project(project_id) is None:
        abort(404)

    body = request.get_json(force=True, silent=True) or {}
    force = bool(body.get('force'))

    # Sem 'force', evita disparar uma rodada duplicada por engano: solver determinístico dado o
    # mesmo binário, então uma rodada TERMINADA (converged/failed) do mesmo model_hash já diz o
    # resultado -- gastar a fila serial (só uma rodada de cada vez) rodando de novo não ajuda em
    # nada. Hasheia o input_simulation.json do NÍVEL DO PROJETO com o mesmo algoritmo
    # (sha256 dos bytes) que create_run() usa sobre o snapshot recém-copiado -- shutil.copy2
    # preserva os bytes exatamente, então os dois hashes batem.
    if not force:
        input_json = store.project_dir(project_id) / "input_simulation.json"
        if input_json.is_file():
            model_hash = hashlib.sha256(input_json.read_bytes()).hexdigest()
            dup = store.find_run_by_model_hash(project_id, model_hash)
            if dup is not None:
                return jsonify({
                    "error": "já existe uma rodada terminada com o mesmo model_hash",
                    "run_id": dup["id"],
                    "status": dup["status"],
                }), 409

    try:
        run = store.create_run(project_id)
    except FileNotFoundError as exc:
        return jsonify({"error": str(exc)}), 400
    # Retorna na hora, sem esperar a rodada terminar -- run_worker.py (processo separado) que vai
    # pegá-la da fila (status "pending") e executar.
    return jsonify(run), 201


@app.route('/api/projects/<project_id>/runs/<run_id>', methods=['GET'])
def api_get_run(project_id, run_id):
    run = store.get_run(project_id, run_id)
    if run is None:
        abort(404)
    return jsonify(run)


@app.route('/api/projects/<project_id>/runs/<run_id>/stream', methods=['GET'])
def api_stream_run(project_id, run_id):
    """Progresso ao vivo de uma rodada: 'cauda' de stdout.log a partir de um offset em bytes
    (query param `offset`, default 0). Mecanismo de polling simples (não SSE) -- o cliente chama
    de novo com `offset` = o `next_offset` da resposta anterior a cada ~300ms-1s; não precisa de
    nenhuma infraestrutura nova (sem message broker), só lê o mesmo arquivo que o worker escreve
    (ver risersim_projects.py e o cabeçalho deste módulo)."""
    run = store.get_run(project_id, run_id)
    if run is None:
        abort(404)

    try:
        offset = max(0, int(request.args.get('offset', 0)))
    except ValueError:
        offset = 0

    log_path = store.run_dir(project_id, run_id) / "stdout.log"
    content = ""
    next_offset = offset
    if log_path.is_file():
        with open(log_path, 'rb') as f:
            f.seek(offset)
            chunk = f.read()
        next_offset = offset + len(chunk)
        content = chunk.decode('utf-8', errors='replace')

    return jsonify({
        "status": run.get("status"),
        "offset": offset,
        "next_offset": next_offset,
        "content": content,
        "done": run.get("status") in ("converged", "failed"),
    })


@app.route('/api/projects/<project_id>/runs/<run_id>/results/<path:filename>', methods=['GET'])
def api_run_results(project_id, run_id, filename):
    if filename not in RESULT_FILENAMES:
        abort(404)
    run_dir = store.run_dir(project_id, run_id)
    file_path = run_dir / filename
    if not file_path.is_file():
        abort(404)
    return send_from_directory(str(run_dir), filename, mimetype=RESULT_FILENAMES[filename])


# ---------------------------------------------------------------------------
# Frontend estático: tools/ inteiro (dashboard/project/posprocessor/preprocessor + js/ + css/),
# igual ao que dev_server.py já servia a partir do mesmo diretório -- `web` é um substituto
# drop-in dele, só com rotas de API a mais.
# ---------------------------------------------------------------------------

@app.route('/')
def index():
    return send_from_directory(str(_SCRIPT_DIR), 'dashboard.html')


@app.route('/<path:filename>')
def static_files(filename):
    full_path = (_SCRIPT_DIR / filename).resolve()
    # Confinamento simples ao diretório tools/ -- evita um filename tipo "../../etc/passwd"
    # escapar por trás de send_from_directory (que já protege isso, mas falhar cedo com 404 aqui
    # deixa a intenção explícita).
    if _SCRIPT_DIR not in full_path.parents and full_path != _SCRIPT_DIR:
        abort(404)
    if not full_path.is_file():
        abort(404)
    return send_from_directory(str(_SCRIPT_DIR), filename)


if __name__ == '__main__':
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    print(f"[run_server] servindo tools/ + API em http://0.0.0.0:{port}")
    print(f"[run_server] projects_root={store.root}")
    print(f"[run_server] exemplos_root={EXEMPLOS_ROOT}")
    app.run(host='0.0.0.0', port=port, threaded=True)
