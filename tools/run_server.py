"""
run_server.py
==============
Flask app for the run manager (see docs/roadmap.md, Axis 3b): REST API (projects/runs) + serves
the frontend (dashboard/project/posprocessor/preprocessor.html + js/ + css/, all under `web/`).

Doesn't execute any run directly: `POST /api/projects/<id>/runs` just writes `status: pending` to
disk (via `risersim_projects.ProjectStore`) and returns immediately -- the separate `run_worker.py`
process (the `worker` service in docker-compose.yml) is what actually runs the C++ binary,
consuming the same queue on the same shared volume. `web` and `worker` only talk to each other
through the filesystem, with no broker.

Usage: `python3 run_server.py [port]` (default 8000).
"""

import hashlib
import os
import sys
import tempfile
from pathlib import Path

from flask import Flask, Response, abort, jsonify, request, send_from_directory
from werkzeug.utils import secure_filename

_SCRIPT_DIR = Path(__file__).parent.resolve()
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))

# Frontend lives in tools/web/ (dashboard/project/posprocessor/preprocessor.html + js/ + css/),
# separate from the backend modules importable via _SCRIPT_DIR above.
_WEB_DIR = _SCRIPT_DIR / "web"

from risersim_projects import ProjectStore
from risersim_runner import build_config_from_xml_h5, discover_xml_h5_examples
from risersim_version import WEB_VERSION

# Root of `trunk/exemplos/` -- from tools/ (_SCRIPT_DIR), go up two levels (risersim/, then
# trunk/); the same relationship holds inside the container (WORKDIR /app/risersim, tools/ inside
# it, so parent.parent = /app -- see docker-compose.yml, which mounts trunk/exemplos at /app/exemplos).
DEFAULT_EXEMPLOS_ROOT = _SCRIPT_DIR.parent.parent / "exemplos"
EXEMPLOS_ROOT = Path(os.environ.get("RISERSIM_EXEMPLOS_ROOT", str(DEFAULT_EXEMPLOS_ROOT)))

RESULT_FILENAMES = {
    "catenary_results.json": "application/json",
    "catenary_results.h5": "application/octet-stream",
    # Snapshot of the input this specific run actually used (frozen by ProjectStore.create_run()
    # -- see risersim_projects.py) -- reuses the same generic per-run results route instead of a
    # dedicated endpoint, since it's just one more file inside the run's directory. Consumed by
    # preprocessor_app.js (?project=&run=), see Axis 3b.
    "input_simulation.json": "application/json",
}

app = Flask(__name__, static_folder=None)
store = ProjectStore()


@app.after_request
def add_no_cache_headers(resp):
    # Without this the browser aggressively caches the ES modules (js/*.js) and HTML, making
    # edits look "not applied" without a hard refresh.
    resp.headers['Cache-Control'] = 'no-store, no-cache, must-revalidate'
    resp.headers['Pragma'] = 'no-cache'
    resp.headers['Expires'] = '0'
    return resp


# ---------------------------------------------------------------------------
# API: examples available for creating a project (Phase 1: XML+H5 only, see spec)
# ---------------------------------------------------------------------------

@app.route('/api/examples', methods=['GET'])
def api_list_examples():
    return jsonify(discover_xml_h5_examples(EXEMPLOS_ROOT))


# ---------------------------------------------------------------------------
# API: web app version (interface+pre+post, same deploy -- see risersim_version.py and
# docs/roadmap.md Axis 3b, version provenance)
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

    # create_project() copies xml_path/h5_path/aml_path into projects/<id>/source/ (see
    # ProjectStore._store_source_files) -- the project stays self-contained even if
    # trunk/exemplos/ later stops being mounted.
    project = store.create_project(name=name, config=config, xml_path=xml_path, h5_path=h5_path,
                                    aml_path=aml_path, origin=origin, description=body.get('description', ''))
    return jsonify(project), 201


@app.route('/api/projects/upload', methods=['POST'])
def api_upload_project():
    """Creates a project from a direct upload (multipart/form-data: `name`, `xml_file`,
    `h5_file`, `aml_file` optional) -- an alternative to the pre-discovered-example flow
    (`api_create_project` above) for when the user has their own XML+H5 pair (or XML/AML)
    outside `trunk/exemplos/`. Reuses `build_config_from_xml_h5()` as-is (the same function used
    by the example flow), without duplicating the compilation logic -- only where the source
    files come from changes."""
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
        # Preserves the ORIGINAL uploaded filename (sanitized by secure_filename -- strips path
        # traversal/dangerous characters, but keeps something recognizable like
        # "Exemplo_03_A1.xml") instead of a generic name -- create_project()/_store_source_files()
        # uses this same name to copy into projects/<id>/source/, so the user recognizes the case
        # on the project screen afterwards.
        xml_name = secure_filename(xml_file.filename) or "input.xml"
        h5_name = secure_filename(h5_file.filename) or "input.h5"
        xml_path = tmp / xml_name
        h5_path = tmp / h5_name
        xml_file.save(str(xml_path))
        h5_file.save(str(h5_path))
        aml_path = None
        if aml_file is not None and aml_file.filename:
            aml_name = secure_filename(aml_file.filename) or "input.aml"
            aml_path = tmp / aml_name
            aml_file.save(str(aml_path))

        try:
            config = build_config_from_xml_h5(xml_path, h5_path, aml_path=aml_path)
        except Exception as exc:
            return jsonify({"error": f"falha ao compilar o modelo a partir do XML+H5: {exc}"}), 400

        # The copy into projects/<id>/source/ (inside create_project(), via
        # _store_source_files()) happens while still inside the `with` block -- the temporary
        # files still exist up to this point, and are only deleted when the block exits.
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


@app.route('/api/projects/<project_id>', methods=['PATCH'])
def api_rename_project(project_id):
    body = request.get_json(force=True, silent=True) or {}
    try:
        project = store.rename_project(project_id, body.get('name'))
    except FileNotFoundError:
        abort(404)
    except ValueError as exc:
        return jsonify({"error": str(exc)}), 400
    return jsonify(project)


@app.route('/api/projects/<project_id>/duplicate', methods=['POST'])
def api_duplicate_project(project_id):
    """Creates a new project that starts as a copy of this one's source files + compiled input,
    with an empty run history -- see ProjectStore.duplicate_project. Optional JSON body `{"name":
    "..."}`; without it, defaults to "<original name> (cópia)"."""
    body = request.get_json(force=True, silent=True) or {}
    try:
        project = store.duplicate_project(project_id, body.get('name'))
    except FileNotFoundError as exc:
        return jsonify({"error": str(exc)}), 404
    return jsonify(project), 201


@app.route('/api/projects/<project_id>', methods=['DELETE'])
def api_delete_project(project_id):
    """Deletes the whole project (all its runs along with it). Refuses (409) if any run is
    pending/running -- see ProjectStore.delete_project. No server-side confirmation (the frontend
    already confirms with the user before calling, see project.js/dashboard.js) -- an
    irreversible action, but confined to the run manager's own data scope (doesn't touch
    risersim_results/ or trunk/exemplos/)."""
    try:
        store.delete_project(project_id)
    except FileNotFoundError:
        abort(404)
    except ValueError as exc:
        return jsonify({"error": str(exc)}), 409
    return '', 204


@app.route('/api/projects/<project_id>/input', methods=['GET'])
def api_project_input(project_id):
    """Serves the PROJECT-LEVEL `input_simulation.json` -- the "current" one, outside any
    specific run (what a new `POST .../runs` would use right now). Complements the existing
    per-run results route (which serves the frozen SNAPSHOT of a specific run, via
    RESULT_FILENAMES) -- useful for inspecting the model in the preprocessor before triggering
    any run. See preprocessor_app.js::resolveInputUrl().

    `?download=1` -- same route, but with `Content-Disposition: attachment` (used by the
    project's "Download compiled JSON" button; without the parameter, it stays inline for the
    preprocessor's fetch(), which doesn't care about that header anyway)."""
    pdir = store.project_dir(project_id)
    file_path = pdir / "input_simulation.json"
    if not file_path.is_file():
        abort(404)
    as_attachment = request.args.get('download') is not None
    return send_from_directory(str(pdir), "input_simulation.json", mimetype="application/json", as_attachment=as_attachment)


@app.route('/api/projects/<project_id>/source/<path:filename>', methods=['GET'])
def api_project_source_file(project_id, filename):
    """Serves an original source file (XML/H5/AML, copied by
    ProjectStore._store_source_files) for download -- the project's "Download original file"
    buttons. `filename` must match exactly one of the names already registered in
    `project.json['source']` (always `"source/<name>"`) -- prevents serving an arbitrary file
    from the directory via URL manipulation."""
    project = store.get_project(project_id)
    if project is None:
        abort(404)
    source = project.get('source') or {}
    allowed_names = {Path(v).name for k, v in source.items() if k.endswith('_path') and v}
    if filename not in allowed_names:
        abort(404)
    source_dir = store.project_dir(project_id) / "source"
    file_path = source_dir / filename
    if not file_path.is_file():
        abort(404)
    return send_from_directory(str(source_dir), filename, as_attachment=True)


# ---------------------------------------------------------------------------
# API: simulações
# ---------------------------------------------------------------------------

@app.route('/api/projects/<project_id>/runs', methods=['POST'])
def api_create_run(project_id):
    if store.get_project(project_id) is None:
        abort(404)

    body = request.get_json(force=True, silent=True) or {}
    force = bool(body.get('force'))

    # Without 'force', avoids accidentally triggering a duplicate run: the solver is
    # deterministic given the same binary, so a FINISHED run (converged/failed) with the same
    # model_hash already tells us the result -- spending the serial queue (only one run at a
    # time) re-running it doesn't help. Hashes the PROJECT-LEVEL input_simulation.json with the
    # same algorithm (sha256 of the bytes) that create_run() uses on the freshly-copied snapshot
    # -- shutil.copy2 preserves the bytes exactly, so the two hashes match.
    if not force:
        input_json = store.project_dir(project_id) / "input_simulation.json"
        if input_json.is_file():
            model_hash = hashlib.sha256(input_json.read_bytes()).hexdigest()
            dup = store.find_run_by_model_hash(project_id, model_hash)
            if dup is not None:
                return jsonify({
                    "error": "já existe uma simulação terminada com o mesmo model_hash",
                    "run_id": dup["id"],
                    "status": dup["status"],
                }), 409

    try:
        run = store.create_run(project_id)
    except FileNotFoundError as exc:
        return jsonify({"error": str(exc)}), 400
    # Returns immediately, without waiting for the run to finish -- run_worker.py (a separate
    # process) is what picks it up from the queue (status "pending") and executes it.
    return jsonify(run), 201


@app.route('/api/projects/<project_id>/runs/<run_id>', methods=['GET'])
def api_get_run(project_id, run_id):
    run = store.get_run(project_id, run_id)
    if run is None:
        abort(404)
    return jsonify(run)


@app.route('/api/projects/<project_id>/runs/<run_id>/abort', methods=['POST'])
def api_abort_run(project_id, run_id):
    """Signals run_worker.py to interrupt this run (pending or running) -- see
    ProjectStore.request_abort. Only creates the sentinel file and returns: the worker (see
    run_worker.py::process_one_run) is what actually changes the status to 'aborted', to avoid
    two processes writing to run.json at the same time -- hence 202 (accepted, not yet
    completed), not 200."""
    try:
        store.request_abort(project_id, run_id)
    except FileNotFoundError:
        abort(404)
    except ValueError as exc:
        return jsonify({"error": str(exc)}), 409
    return jsonify({"status": "abort_requested"}), 202


@app.route('/api/projects/<project_id>/runs/<run_id>', methods=['DELETE'])
def api_delete_run(project_id, run_id):
    """Deletes a specific run (run.json, snapshot, log, results). Refuses (409) if it's
    pending/running -- see ProjectStore.delete_run."""
    try:
        store.delete_run(project_id, run_id)
    except FileNotFoundError:
        abort(404)
    except ValueError as exc:
        return jsonify({"error": str(exc)}), 409
    return '', 204


@app.route('/api/projects/<project_id>/runs/<run_id>/stream', methods=['GET'])
def api_stream_run(project_id, run_id):
    """Live progress of a run: 'tail' of stdout.log starting from a byte offset (query param
    `offset`, default 0). Simple polling mechanism (not SSE) -- the client calls again with
    `offset` = the previous response's `next_offset` every ~300ms-1s; needs no new infrastructure
    (no message broker), just reads the same file the worker writes to (see risersim_projects.py
    and this module's header)."""
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
    """`?download=1` -- the usual route, but with `Content-Disposition: attachment` (the
    "Download results" button in the run table); without the parameter, keeps serving inline
    (normal posprocessor/preprocessor usage via fetch())."""
    if filename not in RESULT_FILENAMES:
        abort(404)
    run_dir = store.run_dir(project_id, run_id)
    file_path = run_dir / filename
    if not file_path.is_file():
        abort(404)
    as_attachment = request.args.get('download') is not None
    return send_from_directory(str(run_dir), filename, mimetype=RESULT_FILENAMES[filename], as_attachment=as_attachment)


# ---------------------------------------------------------------------------
# Static frontend: everything under tools/web/ (dashboard/project/posprocessor/preprocessor.html
# + js/ + css/), served at the URL root -- e.g. tools/web/js/dashboard.js is served as
# /js/dashboard.js.
# ---------------------------------------------------------------------------

@app.route('/')
def index():
    return send_from_directory(str(_WEB_DIR), 'dashboard.html')


@app.route('/<path:filename>')
def static_files(filename):
    full_path = (_WEB_DIR / filename).resolve()
    # Simple confinement to the web/ directory -- prevents a filename like "../../etc/passwd"
    # from escaping past send_from_directory (which already guards against this, but failing
    # early with a 404 here makes the intent explicit).
    if _WEB_DIR not in full_path.parents and full_path != _WEB_DIR:
        abort(404)
    if not full_path.is_file():
        abort(404)
    return send_from_directory(str(_WEB_DIR), filename)


if __name__ == '__main__':
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    print(f"[run_server] servindo tools/ + API em http://0.0.0.0:{port}")
    print(f"[run_server] projects_root={store.root}")
    print(f"[run_server] exemplos_root={EXEMPLOS_ROOT}")
    app.run(host='0.0.0.0', port=port, threaded=True)
