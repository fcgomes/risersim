"""
risersim_runner.py
===================
Shared riserSim pipeline orchestration logic, extracted from `run_from_aml.py` (the original
manual, CLI-only workflow) so the new "run manager" (`run_server.py` / `run_worker.py`, see
docs/roadmap.md, Axis 3b) can reuse it instead of duplicating it.

Functions here:
- `find_executable()`: locates the compiled `risersim_test_main` binary.
- `discover_xml_h5_examples()`: scans `trunk/exemplos/` looking for `<name>_analysis/` folders
  with a real XML+H5 pair exported by ANFLEX (the requirement of `xml_h5_reader.py`) -- today only
  7 of the ~30 examples have this folder (see docs/mapa_aml_exemplos_e_web_interface.md).
- `discover_aml_only_examples()`: scans `trunk/exemplos/` for `.aml` files with NO matching
  XML+H5 export -- the majority of examples, previously invisible to `GET /api/examples` entirely
  (no way to create a project from them via the web, only via the CLI's `run_from_aml.py`).
- `build_config_from_xml_h5()`: compiles the simulation JSON (the schema `ModelBuilder` consumes)
  from an XML+H5 pair, reusing `xml_h5_reader.py` as-is.
- `list_aml_load_cases()`: enumerates the %LOAD_CASE bundles (e.g. "Near"/"Far"/"Transverse"/
  "Cross") a given `.aml` defines, for a caller (the web run-manager's load-case selector) to pick
  one by id -- reuses `aml_reader.py::ANFLEXAMLReader.list_load_cases()`.
- `build_config_from_aml()`: compiles the simulation JSON from a plain `.aml` (no XML+H5 needed),
  reusing `aml_reader.py` as-is -- the same role `build_config_from_xml_h5()` plays for the XML+H5
  path. Extracted from `run_from_aml.py`'s `.aml`-pure branch so both the CLI and the web run
  manager build it identically (including the MoorPy mesh-length correction, see
  `_apply_moorpy_mesh_correction()` below -- docs/roadmap.md, Eixo 2a Atualização 5).
- `run_simulation_subprocess()`: invokes the C++ binary with the same `subprocess` pattern
  `run_from_aml.py` already used (stdout/stderr redirectable to a log file, cwd in the output
  directory).
"""

import shutil
import subprocess
import platform
import sys
from pathlib import Path

_SCRIPT_DIR = Path(__file__).parent.resolve()
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))


def find_executable(custom_path: str = None) -> Path:
    """Locates the risersim_test_main executable. Same logic as always (see run_from_aml.py's
    history): custom path > typical CMake build locations > system PATH."""
    exe_name = 'risersim_test_main.exe' if platform.system() == 'Windows' else 'risersim_test_main'

    if custom_path:
        p = Path(custom_path)
        if p.is_file():
            return p

    # Typical CMake build locations
    root = _SCRIPT_DIR.parent
    candidates = [
        root / 'build' / 'bin' / exe_name,
        root / 'build' / 'bin' / 'Release' / exe_name,
        root / 'build' / 'bin' / 'Debug' / exe_name,
        root / 'build' / exe_name,
        root / 'build' / 'Release' / exe_name,
        Path('.') / 'build' / 'bin' / exe_name,
        Path('.') / 'build' / 'bin' / 'Release' / exe_name,
    ]

    for c in candidates:
        if c.is_file():
            return c

    found = shutil.which(exe_name)
    if found:
        return Path(found)

    return None


def discover_xml_h5_examples(exemplos_root):
    """Scans `exemplos_root` looking for `<name>_analysis/` folders with a real exported XML+H5
    pair (the only input format supported in Phase 1 of the run manager -- see docs/roadmap.md,
    Axis 3b, and docs/mapa_aml_exemplos_e_web_interface.md).

    Content-based discovery, not a fixed naming convention: most examples use
    `<name>_A1.xml`/`.h5`, but at least one (`Manifold`) uses `<name>_An1.xml`/`.h5` -- instead of
    assuming the "_A1" suffix (like `run_from_aml.py` does for the single-file CLI use case), here
    we look for the folder's single "main" XML (excluding the secondary
    `..._results_static/dynamic.xml` files, which are ANFLEX post-processing outputs, not model
    inputs) and pair it with the H5 of the same base name.

    Returns a list of dicts `{id, name, xml_path, h5_path, aml_path, source_dir}`, sorted by id.
    `id` is the example folder's path relative to `exemplos_root` (with "/" even on Windows) --
    unique even when the base name collides between different examples (e.g.
    "Sombra/EfeitoSombra" vs. "SemSombra/EfeitoSombra"). `aml_path` is `None` when the
    corresponding `.aml` isn't next to the `_analysis/` folder (shouldn't happen in the real
    examples, but the `.aml` is only used for two optional metadata fields -- see
    `build_config_from_xml_h5` -- it's not strictly required).
    """
    exemplos_root = Path(exemplos_root)
    results = []
    if not exemplos_root.is_dir():
        return results

    for analysis_dir in sorted(exemplos_root.rglob('*_analysis')):
        if not analysis_dir.is_dir():
            continue
        stem = analysis_dir.name[: -len('_analysis')]

        xml_candidates = [p for p in analysis_dir.glob('*.xml') if '_results_' not in p.name.lower()]
        if len(xml_candidates) != 1:
            continue
        xml_path = xml_candidates[0]
        h5_path = xml_path.with_suffix('.h5')
        if not h5_path.is_file():
            continue

        aml_path = analysis_dir.parent / f"{stem}.aml"

        rel_dir = analysis_dir.parent.relative_to(exemplos_root)
        example_id = "/".join(rel_dir.parts)

        results.append({
            "id": example_id,
            "name": stem,
            "xml_path": str(xml_path),
            "h5_path": str(h5_path),
            "aml_path": str(aml_path) if aml_path.is_file() else None,
            "source_dir": str(analysis_dir.parent),
        })

    results.sort(key=lambda e: e["id"])
    return results


def discover_aml_only_examples(exemplos_root):
    """Scans `exemplos_root` for `.aml` files with NO matching XML+H5 export next to them (no
    `<stem>_analysis/` folder with a resolvable pair, same convention `discover_xml_h5_examples()`
    itself uses) -- the majority of `trunk/exemplos/` (today only 7 of ~30 examples have a real
    XML+H5 export). Complements `discover_xml_h5_examples()`: together, `GET /api/examples` can
    offer every example under `trunk/exemplos/`, not just the minority with a real ANFLEX export --
    previously these were only reachable via the CLI (`run_from_aml.py`), never the web.

    Skips any `.aml` that DOES have a real XML+H5 export (already covered by
    `discover_xml_h5_examples()` under the same `id`) -- avoids listing the same physical model
    twice under two different flows.

    Returns a list of dicts `{id, name, xml_path: None, h5_path: None, aml_path, source_dir,
    load_cases}`, sorted by id. `id` is the `.aml` file's path relative to `exemplos_root`
    (not just its parent dir -- a folder could plausibly hold more than one `.aml`, though today's
    examples don't). `load_cases` is the same summary `list_aml_load_cases()` returns, so the
    frontend can show which cases exist BEFORE the user even creates the project (an XML+H5
    example doesn't need this -- it's already resolved to one case by the time it was exported).
    `xml_path`/`h5_path` are always `None` here -- present so a caller can treat entries from both
    discovery functions with the same dict shape.
    """
    exemplos_root = Path(exemplos_root)
    results = []
    if not exemplos_root.is_dir():
        return results

    for aml_path in sorted(exemplos_root.rglob('*.aml')):
        analysis_dir = aml_path.parent / f"{aml_path.stem}_analysis"
        if analysis_dir.is_dir():
            xml_candidates = [p for p in analysis_dir.glob('*.xml') if '_results_' not in p.name.lower()]
            if len(xml_candidates) == 1 and xml_candidates[0].with_suffix('.h5').is_file():
                continue  # already covered by discover_xml_h5_examples()

        rel = aml_path.relative_to(exemplos_root)
        example_id = "/".join(rel.parts)

        try:
            load_cases = list_aml_load_cases(aml_path)
        except Exception:
            load_cases = []

        results.append({
            "id": example_id,
            "name": aml_path.stem,
            "xml_path": None,
            "h5_path": None,
            "aml_path": str(aml_path),
            "source_dir": str(aml_path.parent),
            "load_cases": load_cases,
        })

    results.sort(key=lambda e: e["id"])
    return results


def build_config_from_xml_h5(xml_path, h5_path, aml_path=None, duration=None, dt=None, static_only=False):
    """Compiles the simulation JSON (the schema `ModelBuilder::load_from_json` consumes) from a
    real XML+H5 pair, using `xml_h5_reader.py` as-is (not reimplemented here).

    Extracted from `run_from_aml.py`'s `use_xml_h5` branch (the original CLI pipeline) so both
    the CLI and the new run manager (`run_server.py`) build the config in exactly the same way.

    `aml_path`, if given, feeds two metadata fields that only exist in the `.aml`/`.pml` text, not
    in the XML/H5 (see `xml_h5_reader.extract_assembly_flag`/`extract_static_convergence_criterium`):
    the real `%ASSEMBLY.USING` flag and the real static convergence criterion
    (`%ANALYSIS_CASE.STATIC.CONVERGENCE_CRITERIUM`/`MAX_UNBALANCED`). Without `aml_path`, the
    config is left without these fields and risersim uses its own safe defaults (identical
    behavior to before this extraction).

    `duration`/`dt`, if given (not `None`), override the dynamic duration/timestep read from the
    XML -- the same semantics as `--duration`/`--dt` in the original CLI (only overrides when the
    user actually asked for it, so as not to mask the XML's real values with a fixed default).
    `static_only=True` turns off the entire dynamic analysis (same semantics as `--static-only`).
    """
    from xml_h5_reader import ANFLEXXmlH5Reader, extract_assembly_flag, extract_static_convergence_criterium

    reader = ANFLEXXmlH5Reader(str(xml_path), str(h5_path))
    try:
        config = reader.to_risersim_json()
    finally:
        reader.close()

    if aml_path:
        assembly_flag = extract_assembly_flag(str(aml_path))
        if assembly_flag is not None:
            config['analysis_options']['static']['use_assembly_phase'] = assembly_flag

        enable_unbalanced, max_unbalanced = extract_static_convergence_criterium(str(aml_path))
        if enable_unbalanced:
            config['analysis_options']['static']['enable_unbalanced_criteria'] = True
            if max_unbalanced is not None:
                config['analysis_options']['static']['unbalanced_force_tol'] = max_unbalanced
                config['analysis_options']['static']['unbalanced_moment_tol'] = max_unbalanced

    if duration is not None:
        config['analysis_options']['dynamic']['duration_s'] = duration
    if dt is not None:
        config['analysis_options']['dynamic']['dt_s'] = dt
    if static_only:
        config['analysis_options']['dynamic']['enabled'] = False

    return config


def list_aml_load_cases(aml_path):
    """Enumerates the %LOAD_CASE bundles a given `.aml` defines (e.g. "Near"/"Far"/"Transverse"/
    "Cross" for `Exemplo_01a.aml`) -- thin wrapper so callers (the web run-manager's
    `GET .../load-cases` route) don't need to construct/import `ANFLEXAMLReader` themselves, same
    reasoning as why this module wraps `xml_h5_reader.py` for the XML+H5 path above.

    Returns `[]` (never raises on a missing %LOAD_CASE block) for a `.aml` with none -- see
    `ANFLEXAMLReader.list_load_cases()`'s own docstring.
    """
    from aml_reader import ANFLEXAMLReader
    return ANFLEXAMLReader(str(aml_path)).list_load_cases()


def _apply_moorpy_mesh_correction(config):
    """Overwrites `config['model']['nodes'][i]['coords']` in place with MoorPy's own solved
    equilibrium shape, extracted from `run_from_aml.py`'s original inline mesh-correction block
    (docs/roadmap.md, Eixo 2a Atualização 5).

    Why this exists: `_synthesize_mesh()` (aml_reader.py) places the initial nodes on the STRAIGHT
    LINE (chord) between the real top/anchor points -- fine as a Newton-Raphson starting guess, but
    `model_builder.cpp:149` derives each element's `L_unstretched` (unstressed reference length,
    used for axial strain) from the EUCLIDEAN distance between a pair of input node coordinates,
    not from the line's real declared segment length. A chord is always shorter than the real
    (sagging) catenary arc length it connects -- this under-length reference produced several times
    too much static top tension (994 kN vs. the real, XML+H5-validated 217 kN for Exemplo_01a/
    Cross). Fix: replace the straight-line coordinates with MoorPy's own equilibrium shape (arc-
    length-consistent) BEFORE `model_builder.cpp` ever computes `L_unstretched` from them.

    Unlike the original inline version (which read/rewrote the real `input_simulation.json` output
    file directly, since it ran after that file was already written to disk), this takes/returns an
    in-memory `config` dict -- `compute_warm_start_positions()` (moorpy_warm_start.py) only accepts
    a file PATH, not a dict, so this round-trips through a throwaway `tempfile.TemporaryDirectory()`
    instead of the caller's real output path (which may not exist yet, e.g. at web run-creation
    time, before any run directory has been allocated).

    Best-effort: any exception (MoorPy unavailable, solve failure, ...) falls back to the original
    straight-line mesh unchanged, with a warning -- never fails the whole config-build over this.
    """
    import json
    import tempfile

    try:
        from moorpy_warm_start import compute_warm_start_positions
    except Exception as exc:
        print(f"⚠️ MoorPy indisponível ({exc}) -- usando a malha reta original (comprimento pode ficar incorreto).")
        return config

    try:
        with tempfile.TemporaryDirectory() as tmp:
            scratch_path = Path(tmp) / 'input_simulation.json'
            with open(scratch_path, 'w', encoding='utf-8') as f:
                json.dump(config, f)
            node_positions, _meta = compute_warm_start_positions(str(scratch_path))
        coords_by_id = {p['node_id']: p['coords'] for p in node_positions}
        for n in config['model']['nodes']:
            if n['id'] in coords_by_id:
                n['coords'] = coords_by_id[n['id']]
        print("⚓ Malha inicial corrigida via MoorPy: comprimento real do cabo preservado "
              "(evita o encurtamento artificial da corda reta topo-âncora).")
    except Exception as exc:
        print(f"⚠️ Não foi possível corrigir a malha inicial via MoorPy ({exc}) -- "
              f"usando a malha reta original (comprimento pode ficar incorreto).")
    return config


def build_config_from_aml(aml_path, line_index=0, num_elements_override=None, load_case_id=None,
                           duration=None, dt=None, static_only=False, apply_moorpy_correction=True):
    """Compiles the simulation JSON from a plain `.aml` (no XML+H5 export needed), using
    `aml_reader.py` as-is -- the `.aml`-pure counterpart of `build_config_from_xml_h5()` above.

    Extracted from `run_from_aml.py`'s original `.aml`-pure branch (the `else:` of its
    `use_xml_h5` check) so both the CLI and the web run manager build this config identically,
    including the MoorPy mesh-length correction (see `_apply_moorpy_mesh_correction()`) -- without
    it, a caller would silently reintroduce the ~4x static tension over-prediction bug it fixes.

    `load_case_id`: which %LOAD_CASE (current+wave pairing) to resolve, when the `.aml` defines
    more than one (e.g. "Near"/"Far") -- `None` picks the first in the file, same default as
    `ANFLEXAMLReader.to_risersim_json()` itself.
    `duration`/`dt`/`static_only`: same override semantics as `build_config_from_xml_h5()` (only
    applied when not `None`/`False`, so as not to mask the `.aml`'s own real values).
    `apply_moorpy_correction`: set `False` only for diagnostics that want the raw, uncorrected
    straight-chord mesh -- every normal caller (CLI, web) should leave this at its default.
    """
    from aml_reader import ANFLEXAMLReader

    reader = ANFLEXAMLReader(str(aml_path))
    config = reader.to_risersim_json(
        line_index=line_index, num_elements_override=num_elements_override,
        load_case_id=load_case_id,
    )

    if duration is not None:
        config['analysis_options']['dynamic']['duration_s'] = duration
    if dt is not None:
        config['analysis_options']['dynamic']['dt_s'] = dt
    if static_only:
        config['analysis_options']['dynamic']['enabled'] = False

    # Safety-net Rayleigh damping if the AML computed zero (avoids divergence in the dynamic
    # phase) -- same floor `run_from_aml.py` always applied on this path.
    ray = config['analysis_options']['dynamic']['rayleigh_damping']
    ray['alpha'] = max(ray.get('alpha', 0.0), 0.05)
    ray['beta'] = max(ray.get('beta', 0.0), 0.005)

    if apply_moorpy_correction:
        config = _apply_moorpy_mesh_correction(config)

    return config


def run_simulation_subprocess(exe_path, input_json_path, out_dir, stdout_file=None):
    """Invokes the `risersim_test_main` binary -- the same `subprocess` pattern `run_from_aml.py`
    already used (`[exe, input_json]`, cwd=out_dir), with the optional addition of redirecting
    stdout/stderr to a file (used by `run_worker.py`: `stdout.log` is the source of truth for a
    run's live progress, since the C++ binary already prints per iteration via `std::cout`/
    `std::endl`, with no change needed in the C++).

    No output-dir CLI argument -- the binary always writes results next to `input_json_path`'s own
    directory (main.cpp derives it from argv[1]), which must already be `out_dir`.

    `stdout_file`: path (str/Path) to redirect combined stdout+stderr to, or `None` to inherit
    the calling Python process's stdout/stderr (the original CLI's behavior).

    Returns the process's exit code.
    """
    cmd = [str(exe_path), str(input_json_path)]
    if stdout_file is not None:
        with open(stdout_file, 'w', encoding='utf-8') as log_f:
            result = subprocess.run(cmd, cwd=str(out_dir), stdout=log_f, stderr=subprocess.STDOUT)
    else:
        result = subprocess.run(cmd, cwd=str(out_dir))
    return result.returncode
