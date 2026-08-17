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

import json
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

from risersim_projects import ProjectStore, DuplicateRunError
from risersim_runner import (
    build_config_from_xml_h5, discover_xml_h5_examples, list_aml_load_cases,
    build_config_from_aml, discover_aml_only_examples, list_interface_runs,
    build_config_from_interface,
)
from risersim_version import WEB_VERSION

# Root of `trunk/exemplos/` -- from tools/ (_SCRIPT_DIR), go up two levels (risersim/, then
# trunk/); the same relationship holds inside the container (WORKDIR /app/risersim, tools/ inside
# it, so parent.parent = /app -- see docker-compose.yml, which mounts trunk/exemplos at /app/exemplos).
DEFAULT_EXEMPLOS_ROOT = _SCRIPT_DIR.parent.parent / "exemplos"
EXEMPLOS_ROOT = Path(os.environ.get("RISERSIM_EXEMPLOS_ROOT", str(DEFAULT_EXEMPLOS_ROOT)))

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
# API: examples available for creating a project
# ---------------------------------------------------------------------------

@app.route('/api/examples', methods=['GET'])
def api_list_examples():
    """Every example under `trunk/exemplos/` a project can be created from -- both real XML+H5
    exports (`discover_xml_h5_examples()`, the original Phase 1 scope) and plain `.aml` files with
    no export at all (`discover_aml_only_examples()`, the majority of examples -- previously only
    reachable via the CLI's `run_from_aml.py`, never the web). Both share the same dict shape
    (`xml_path`/`h5_path` are `None` for an `.aml`-only entry) so the frontend can render one list
    without a special case; `load_cases` (only present on `.aml`-only entries, always `[]`/absent
    on an XML+H5 one, which is already resolved to a single case) is what lets the "new project"
    form offer a case picker before the project even exists."""
    return jsonify(discover_xml_h5_examples(EXEMPLOS_ROOT) + discover_aml_only_examples(EXEMPLOS_ROOT))


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
    """Creates a project from either a real XML+H5 export or a plain `.aml` alone (no export --
    see `risersim_runner.py::build_config_from_aml()`/`discover_aml_only_examples()`). Which
    pipeline runs is decided purely by which paths are present after resolving `example_id` (or
    the explicit `xml_path`/`h5_path`/`aml_path` body fields): `xml_path`+`h5_path` -> XML+H5;
    `aml_path` alone -> `.aml`-only. `load_case_id` (optional) only matters for the `.aml`-only
    path -- it picks which %LOAD_CASE becomes this project's DEFAULT/project-level input (the one
    a run with no `load_case_id` of its own would use); `None` falls back to the first case in the
    file, same default `build_config_from_aml()`/the CLI already use. A real XML+H5 export has no
    such choice -- it's already resolved to one case by the time it was exported."""
    body = request.get_json(force=True, silent=True) or {}
    load_case_id = body.get('load_case_id')

    example_id = body.get('example_id')
    if example_id:
        examples = {e['id']: e for e in discover_xml_h5_examples(EXEMPLOS_ROOT) + discover_aml_only_examples(EXEMPLOS_ROOT)}
        example = examples.get(example_id)
        if example is None:
            return jsonify({"error": f"example_id desconhecido: {example_id}"}), 400
        name = body.get('name') or example['name']
        xml_path, h5_path, aml_path = example.get('xml_path'), example.get('h5_path'), example.get('aml_path')
        origin = {"example_id": example_id}
    else:
        name = body.get('name')
        xml_path = body.get('xml_path')
        h5_path = body.get('h5_path')
        aml_path = body.get('aml_path')
        if not name or not ((xml_path and h5_path) or aml_path):
            return jsonify({
                "error": "informe 'example_id', ou 'name'+'xml_path'+'h5_path' ('aml_path' opcional), "
                         "ou 'name'+'aml_path' sozinho (.aml puro, sem XML+H5)"
            }), 400
        if xml_path and not Path(xml_path).is_file():
            return jsonify({"error": f"xml_path não encontrado: {xml_path}"}), 400
        if h5_path and not Path(h5_path).is_file():
            return jsonify({"error": f"h5_path não encontrado: {h5_path}"}), 400
        if aml_path and not Path(aml_path).is_file():
            return jsonify({"error": f"aml_path não encontrado: {aml_path}"}), 400
        origin = None

    try:
        if xml_path and h5_path:
            config = build_config_from_xml_h5(xml_path, h5_path, aml_path=aml_path)
        else:
            config = build_config_from_aml(aml_path, load_case_id=load_case_id)
    except Exception as exc:
        return jsonify({"error": f"falha ao compilar o modelo: {exc}"}), 400

    # create_project() copies xml_path/h5_path/aml_path into projects/<id>/source/ (see
    # ProjectStore._store_source_files) -- the project stays self-contained even if
    # trunk/exemplos/ later stops being mounted.
    project = store.create_project(name=name, config=config, xml_path=xml_path, h5_path=h5_path,
                                    aml_path=aml_path, origin=origin, description=body.get('description', ''))
    return jsonify(project), 201


@app.route('/api/projects/upload', methods=['POST'])
def api_upload_project():
    """Creates a project from a direct upload (multipart/form-data: `name`, plus either
    `xml_file`+`h5_file` (real export) or `aml_file` alone (`.aml`-only, no export -- see
    `risersim_runner.py::build_config_from_aml()`); `aml_file` is also accepted ALONGSIDE
    `xml_file`+`h5_file` as extra metadata, same as the example flow. `load_case_id` (optional
    form field) only matters for the `.aml`-only path -- see `api_create_project`'s docstring.
    An alternative to the pre-discovered-example flow (`api_create_project` above) for when the
    user has their own files outside `trunk/exemplos/`. Reuses `build_config_from_xml_h5()`/
    `build_config_from_aml()` as-is (the same functions the example flow uses), without
    duplicating the compilation logic -- only where the source files come from changes."""
    name = (request.form.get('name') or '').strip()
    xml_file = request.files.get('xml_file')
    h5_file = request.files.get('h5_file')
    aml_file = request.files.get('aml_file')
    load_case_id_raw = request.form.get('load_case_id')
    load_case_id = int(load_case_id_raw) if load_case_id_raw else None

    if not name:
        return jsonify({"error": "informe 'name'"}), 400
    has_xml_h5 = xml_file is not None and xml_file.filename and h5_file is not None and h5_file.filename
    has_aml = aml_file is not None and aml_file.filename
    if not has_xml_h5 and not has_aml:
        return jsonify({"error": "informe 'xml_file'+'h5_file', ou 'aml_file' sozinho (.aml puro, sem XML+H5)"}), 400

    with tempfile.TemporaryDirectory() as tmp_str:
        tmp = Path(tmp_str)
        # Preserves the ORIGINAL uploaded filename (sanitized by secure_filename -- strips path
        # traversal/dangerous characters, but keeps something recognizable like
        # "Exemplo_03_A1.xml") instead of a generic name -- create_project()/_store_source_files()
        # uses this same name to copy into projects/<id>/source/, so the user recognizes the case
        # on the project screen afterwards.
        xml_path = h5_path = aml_path = None
        if has_xml_h5:
            xml_name = secure_filename(xml_file.filename) or "input.xml"
            h5_name = secure_filename(h5_file.filename) or "input.h5"
            xml_path = tmp / xml_name
            h5_path = tmp / h5_name
            xml_file.save(str(xml_path))
            h5_file.save(str(h5_path))
        if has_aml:
            aml_name = secure_filename(aml_file.filename) or "input.aml"
            aml_path = tmp / aml_name
            aml_file.save(str(aml_path))

        try:
            if has_xml_h5:
                config = build_config_from_xml_h5(xml_path, h5_path, aml_path=aml_path)
            else:
                config = build_config_from_aml(aml_path, load_case_id=load_case_id)
        except Exception as exc:
            return jsonify({"error": f"falha ao compilar o modelo: {exc}"}), 400

        # The copy into projects/<id>/source/ (inside create_project(), via
        # _store_source_files()) happens while still inside the `with` block -- the temporary
        # files still exist up to this point, and are only deleted when the block exits.
        project = store.create_project(name=name, config=config, xml_path=xml_path, h5_path=h5_path,
                                        aml_path=aml_path, origin={"kind": "upload"})
    return jsonify(project), 201


@app.route('/api/projects/blank', methods=['POST'])
def api_create_blank_project():
    """Creates a project directly from a hand-built "JSON de interface" (docs/roadmap.md, Eixo
    3a) -- the web editor's "novo projeto em branco" entry point (`dashboard.js`). Body:
    `{"name": "...", "interface": {...}, "description": "..."}`. Unlike `api_create_project()`/
    `api_upload_project()` above, there's no XML/H5/AML to validate paths for -- the interface
    JSON's own structure is validated by `build_config_from_interface()` (via
    `ProjectStore.create_blank_project()`), which returns a clear `ValueError` (400) for anything
    it can't resolve (bad id references, no valid analysis/load-case combination, ...) instead of
    a generic 500."""
    body = request.get_json(force=True, silent=True) or {}
    name = (body.get('name') or '').strip()
    interface_json = body.get('interface')
    if not name or not isinstance(interface_json, dict):
        return jsonify({"error": "informe 'name' e 'interface' (objeto JSON de interface)"}), 400
    try:
        project = store.create_blank_project(name, interface_json, description=body.get('description', ''))
    except ValueError as exc:
        return jsonify({"error": str(exc)}), 400
    except Exception as exc:
        return jsonify({"error": f"falha ao compilar o modelo: {exc}"}), 400
    return jsonify(project), 201


@app.route('/api/interface/preview', methods=['POST'])
def api_interface_preview():
    """Compiles a "JSON de interface" (docs/roadmap.md, Eixo 3a) into node/element positions for
    the editor's live 3D preview (`editor.html`'s "Linhas" tab) -- unlike
    `api_create_blank_project()`/`api_update_project_interface()`, this is entirely STATELESS: no
    project id, nothing written to disk, safe to call on every debounced edit while the user is
    still typing. Body: `{"interface": {...}, "analysis_id"?: int, "load_case_id"?: int}` --
    both ids optional, defaulting to the first valid combination (`list_interface_runs()`), same
    as `create_blank_project()`'s own project-level default (the preview should show *some*
    result even before the user has picked a specific run to preview).

    Returns only `{nodes, elements}` (never the full simulation JSON -- environmental/analysis
    fields aren't needed to draw the mesh) on success, or `{"error": "..."}` (400) for anything
    `build_config_from_interface()` can't resolve (e.g. no lines yet, or a dangling id reference)
    -- expected/frequent while the user is still filling in the form, not a server fault."""
    body = request.get_json(force=True, silent=True) or {}
    interface_json = body.get('interface')
    if not isinstance(interface_json, dict):
        return jsonify({"error": "informe 'interface' (objeto JSON de interface)"}), 400

    analysis_id = body.get('analysis_id')
    load_case_id = body.get('load_case_id')
    if analysis_id is None or load_case_id is None:
        runs = list_interface_runs(interface_json)
        if not runs:
            return jsonify({"error": "nenhuma análise com pelo menos um caso de carregamento definida ainda"}), 400
        analysis_id, load_case_id = runs[0]['analysis_id'], runs[0]['load_case_id']

    try:
        config, warnings = build_config_from_interface(interface_json, analysis_id, load_case_id)
    except ValueError as exc:
        return jsonify({"error": str(exc)}), 400
    except Exception as exc:
        return jsonify({"error": f"falha ao compilar: {exc}"}), 400
    # Reshapes `{"id", "coords": [x,y,z]}` (the simulation JSON's own node shape) into
    # `{"id", "x", "y", "z"}` -- what the frontend's `Riser3DRenderer`/`Node3D` (built to consume
    # already-parsed HDF5 results, never raw `model.nodes`) actually expects.
    nodes = [{"id": n["id"], "x": n["coords"][0], "y": n["coords"][1], "z": n["coords"][2]} for n in config['model']['nodes']]
    return jsonify({
        "nodes": nodes,
        "elements": config['model']['elements'],
        "warnings": warnings,
    })


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
    per-run results route (`api_run_results`, which serves the frozen SNAPSHOT of a specific run,
    named after that run's own `input_filename`) -- useful for inspecting the model in the
    preprocessor before triggering any run. See preprocessor_app.js::resolveInputUrl().

    `?download=1` -- same route, but with `Content-Disposition: attachment` (used by the
    project's "Download compiled JSON" button; without the parameter, it stays inline for the
    preprocessor's fetch(), which doesn't care about that header anyway)."""
    pdir = store.project_dir(project_id)
    file_path = pdir / "input_simulation.json"
    if not file_path.is_file():
        abort(404)
    as_attachment = request.args.get('download') is not None
    return send_from_directory(str(pdir), "input_simulation.json", mimetype="application/json", as_attachment=as_attachment)


@app.route('/api/projects/<project_id>/load-cases', methods=['GET'])
def api_project_load_cases(project_id):
    """Lists the %LOAD_CASE bundles (e.g. "Near"/"Far"/"Transverse"/"Cross") available for this
    project's `.aml`, if it has one -- feeds the "new run" load-case selector (project.js/
    preprocessor_app.js), letting a run pick a DIFFERENT case than whatever the project's own
    XML+H5 export represents (see `ProjectStore.create_run(load_case_id=...)`).

    A project without an `.aml` in `source` (or one whose `.aml` has 0-1 load cases) returns
    `available: false` -- the frontend hides the selector in that case, zero behavior change for
    those projects. Never 500s on a missing/unreadable `.aml` -- degrades to `available: false`
    with an `error` field instead, since this is a best-effort/decorative lookup, not required for
    the rest of the page to function."""
    project = store.get_project(project_id)
    if project is None:
        abort(404)
    aml_rel = (project.get('source') or {}).get('aml_path')
    if not aml_rel:
        return jsonify({"available": False, "load_cases": []})
    aml_path = store.project_dir(project_id) / aml_rel
    try:
        load_cases = list_aml_load_cases(aml_path)
    except Exception as exc:
        return jsonify({"available": False, "load_cases": [], "error": str(exc)})
    return jsonify({"available": len(load_cases) > 1, "load_cases": load_cases})


@app.route('/api/projects/<project_id>/runs-catalog', methods=['GET'])
def api_project_runs_catalog(project_id):
    """Lists every valid `(analysis_id, load_case_id)` combination for this project's
    `source/interface.json` (docs/roadmap.md, Eixo 3a) -- the generalization of
    `api_project_load_cases()` above that also works for blank/editor-created projects, not just
    `.aml`-sourced ones (every project with an interface JSON has one now, see
    `ProjectStore.create_project()`'s docstring). Feeds the two-step analysis-then-load-case
    selector in `project.html`/`preprocessor.html`'s "new run" flow.

    A project without `source/interface.json` (an XML+H5-only project with no `.aml` alongside
    it) returns `available: false` -- same degrade-gracefully contract as
    `api_project_load_cases()`."""
    project = store.get_project(project_id)
    if project is None:
        abort(404)
    interface_rel = (project.get('source') or {}).get('interface_path')
    if not interface_rel:
        return jsonify({"available": False, "runs": []})
    interface_path = store.project_dir(project_id) / interface_rel
    try:
        interface_json = json.loads(interface_path.read_text(encoding='utf-8'))
        runs = list_interface_runs(interface_json)
    except Exception as exc:
        return jsonify({"available": False, "runs": [], "error": str(exc)})
    return jsonify({"available": len(runs) > 1, "runs": runs})


@app.route('/api/projects/<project_id>/interface', methods=['GET'])
def api_project_interface(project_id):
    """Serves the project's `source/interface.json` -- the web editor's "load my draft" path
    (`?project=<id>` on `editor.html`), and generally useful for inspecting the logical model
    behind a project's compiled `input_simulation.json` (docs/roadmap.md, Eixo 3a). 404s if the
    project has no interface JSON at all (an XML+H5-only project with no `.aml`)."""
    project = store.get_project(project_id)
    if project is None:
        abort(404)
    interface_rel = (project.get('source') or {}).get('interface_path')
    if not interface_rel:
        abort(404)
    pdir = store.project_dir(project_id)
    return send_from_directory(str(pdir), interface_rel, mimetype="application/json")


@app.route('/api/projects/<project_id>/interface', methods=['PUT'])
def api_update_project_interface(project_id):
    """Saves the web editor's edited "JSON de interface" back to the project
    (`ProjectStore.update_interface()`) -- recompiles `input_simulation.json` too, and returns
    the compiler's warnings for the editor to display. Only projects created via
    `POST /api/projects/blank` (`source["interface_editable"]`) accept this -- editing/reopening
    an AML/XML-derived project is out of scope (see roadmap "Eixo 3a fora de escopo"),
    `ProjectStore.update_interface()` raises `ValueError` (400) for those."""
    body = request.get_json(force=True, silent=True) or {}
    interface_json = body.get('interface')
    if not isinstance(interface_json, dict):
        return jsonify({"error": "informe 'interface' (objeto JSON de interface)"}), 400
    try:
        warnings = store.update_interface(project_id, interface_json)
    except FileNotFoundError:
        abort(404)
    except ValueError as exc:
        return jsonify({"error": str(exc)}), 400
    except Exception as exc:
        return jsonify({"error": f"falha ao compilar o modelo: {exc}"}), 400
    return jsonify({"warnings": warnings})


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
    load_case_id = body.get('load_case_id')
    analysis_id = body.get('analysis_id')

    # Duplicate-run dedup (avoids accidentally re-running a deterministic solver against the same
    # input) and analysis/load-case config-building both moved into create_run() itself -- see
    # risersim_projects.py::create_run()'s docstring: the dedup check needs to hash the bytes THIS
    # run would actually use (which depend on analysis_id/load_case_id), not unconditionally the
    # project-level file the way this route used to do it.
    try:
        run = store.create_run(project_id, analysis_id=analysis_id, load_case_id=load_case_id, force=force)
    except FileNotFoundError as exc:
        return jsonify({"error": str(exc)}), 400
    except ValueError as exc:
        return jsonify({"error": str(exc)}), 400
    except DuplicateRunError as exc:
        return jsonify({
            "error": "já existe uma simulação terminada com o mesmo model_hash",
            "run_id": exc.run["id"],
            "status": exc.run["status"],
        }), 409
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
    (normal posprocessor/preprocessor usage via fetch()).

    `filename` must match either the run's `input_filename` or `results_filename` (both
    case-identifiable names computed by `ProjectStore.create_run()`, e.g.
    "Exemplo_01a_Far_input.json"/"Exemplo_01a_Far_results.h5") -- the frontend already knows the
    real name (it fetched `run.json` first -- see app.js::resolveResultsUrl()/
    preprocessor_app.js::resolveInputUrl()/project.js) rather than guessing a fixed one. Checking
    against `run.json` (not just "does this file exist in the run dir") also means only the two
    files this run actually produced/used are servable, not an arbitrary filename someone dropped
    in the directory."""
    run = store.get_run(project_id, run_id)
    if run is None or filename not in (run.get("input_filename"), run.get("results_filename")):
        abort(404)
    mimetype = "application/json" if filename == run.get("input_filename") else "application/octet-stream"

    run_dir = store.run_dir(project_id, run_id)
    file_path = run_dir / filename
    if not file_path.is_file():
        abort(404)
    as_attachment = request.args.get('download') is not None
    return send_from_directory(str(run_dir), filename, mimetype=mimetype, as_attachment=as_attachment)


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
