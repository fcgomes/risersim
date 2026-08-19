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
- `build_config_from_aml_multiline()`: multi-line sibling of the above (docs/roadmap.md backlog:
  "múltiplas linhas com corpo flutuante compartilhado", driven by
  exemplos/Multilinhas/Multilinhas.aml) -- reuses `aml_reader.py::to_risersim_json_multiline()`.
- `run_simulation_subprocess()`: invokes the C++ binary with the same `subprocess` pattern
  `run_from_aml.py` already used (stdout/stderr redirectable to a log file, cwd in the output
  directory).
"""

import math
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


def list_interface_runs(interface_json):
    """Expands `analyses[].load_case_ids` (the "JSON de interface" schema, docs/roadmap.md Eixo
    3a) into the catalog of valid `(analysis_id, load_case_id)` combinations a run can be created
    from -- an analysis (solver config) applies to a LIST of load cases (current+wave scenarios),
    not the other way around (confirmed decision, see roadmap). Mirrors `list_aml_load_cases()`'s
    role for the AML path: cheap, side-effect-free, safe to call on every API request.

    Returns a flat list of `{analysis_id, analysis_name, load_case_id, load_case_name}` dicts --
    one per valid combination, in `analyses[]`/`load_case_ids[]` order.
    """
    load_cases_by_id = {lc['id']: lc for lc in interface_json.get('load_cases', [])}
    result = []
    for an in interface_json.get('analyses', []):
        for lc_id in an.get('load_case_ids', []):
            lc = load_cases_by_id.get(lc_id)
            result.append({
                'analysis_id': an['id'],
                'analysis_name': an.get('name'),
                'load_case_id': lc_id,
                'load_case_name': lc.get('name') if lc is not None else None,
            })
    return result


def build_config_from_interface(interface_json, analysis_id, load_case_id, num_elements_override=None,
                                 duration=None, dt=None, static_only=False):
    """Compiles the simulation JSON (the schema `ModelBuilder` reads) from a "JSON de interface"
    (docs/roadmap.md, Eixo 3a) -- the single compiler both `build_config_from_aml()` (below, now a
    thin wrapper around `aml_reader.py::to_interface_json()` + this function) and the planned web
    editor's "blank project" path use, so AML-derived and hand-built interface JSONs are compiled
    identically (avoids the catenary/ID-resolution logic duplication `to_risersim_json()`/
    `to_risersim_json_multiline()` used to each carry independently).

    `analysis_id`/`load_case_id`: the run to compile -- `load_case_id` must be one of
    `interface_json['analyses'][analysis_id]['load_case_ids']` (see `list_interface_runs()`).

    v1 scope (see roadmap): no vessel motion/RAO -- every line's TOP is treated as a fixed point
    in space (a `GLOBAL` reference, same as the anchor), not a moving `FLOATING` one. Multiple
    lines are independent (no shared floating body).

    Known engine limitation, not fixable here (zero C++ change in this whole effort): the
    simulation JSON's `environmental.seabed` is a SINGLE global config -- there is no per-node/
    per-segment seabed spring in the schema `ModelBuilder` reads. `segments[].soil_id` is honored
    faithfully in the interface JSON (mirrors real ANFLEX's %LINE.SEGMENT.SOIL_ID), but only the
    FIRST soil actually referenced by any segment (in line/segment order) is used to build
    `environmental.seabed` -- any other distinct soil referenced elsewhere is silently ignored
    for the seabed's own spring/friction values (a warning is returned, never raised).

    Returns `(simulation_json, warnings)` -- never raises for a resolvable business-logic issue
    (an id that isn't found elsewhere still raises `ValueError`, since there's no reasonable
    fallback for "which material/soil should this segment even use").
    """
    from catenary_geometry import (
        solve_catenary_geometry, correct_line_mesh_via_moorpy, build_section_properties,
        build_truss_properties, build_winch_properties, build_scalar_properties,
        build_buoy_properties, synthesize_mesh, compute_rayleigh_alpha, compute_rayleigh_beta,
        static_robustness_overrides,
    )
    from xml_h5_reader import SCHEMA_VERSION

    warnings = []

    soils_by_id = {s['id']: s for s in interface_json.get('soils', [])}
    materials_by_id = {m['id']: m for m in interface_json.get('materials', [])}
    buoys_by_id = {b['id']: b for b in interface_json.get('buoys', [])}
    curves_by_id = {c['id']: c for c in interface_json.get('curves', [])}
    currents_by_id = {c['id']: c for c in interface_json.get('currents', [])}
    waves_by_id = {w['id']: w for w in interface_json.get('waves', [])}
    load_cases_by_id = {lc['id']: lc for lc in interface_json.get('load_cases', [])}
    analyses_by_id = {a['id']: a for a in interface_json.get('analyses', [])}

    analysis = analyses_by_id.get(analysis_id)
    if analysis is None:
        raise ValueError(f"analysis_id={analysis_id} não encontrado na JSON de interface.")
    if load_case_id not in analysis.get('load_case_ids', []):
        raise ValueError(
            f"load_case_id={load_case_id} não está associado à análise "
            f"'{analysis.get('name')}' (id={analysis_id}).")
    load_case = load_cases_by_id.get(load_case_id)
    if load_case is None:
        raise ValueError(f"load_case_id={load_case_id} não encontrado na JSON de interface.")

    current = currents_by_id.get(load_case.get('current_id'))
    wave = waves_by_id.get(load_case.get('wave_id'))

    glb = interface_json.get('global', {})
    water_depth_m = glb.get('water_depth_m', 100.0)
    water_sp_wt_kNm3 = glb.get('water_specific_weight_kNm3', 10.0553)
    gravity = glb.get('gravity_ms2', 9.81)
    water_density_kgm3 = (water_sp_wt_kNm3 * 1000.0) / gravity if gravity > 0 else 1025.0

    def _material_props(m):
        """Derives the same E/G/A/IY/IZ/J/rho fields aml_reader.py's `beam_props` block does,
        from the interface JSON's AML-like material fields -- shared by `beam`/`truss`/`winch`
        materials, since none of this derivation is actually beam-specific (bending/torsion just
        end up unused by truss/winch, same as real ANFLEX's own `%MATERIAL.FINITE_ELEMENT`
        selecting which subset of a `%MATERIAL.FLEXIBLE_LINE` block the solver reads).

        `scalar` materials carry none of these geometry/weight fields at all (a spring has no
        cross-section) -- returns their own 6 raw stiffness fields unconverted instead, passed
        through to `build_scalar_properties()` later.

        Every return value carries `finite_element_type` (default `"beam"`, so a material written
        before this field existed is unambiguously treated as beam) alongside whichever
        type-specific fields apply.
        """
        fe_type = m.get('finite_element_type', 'beam')
        if fe_type == 'scalar':
            return {'finite_element_type': fe_type, **m}

        do, di = m['diameter_external_m'], m['diameter_internal_m']
        w_dry_kNm, w_wet_kNm = m['weight_dry_kNm'], m['weight_wet_kNm']
        ea_N = m['stiffness_axial_kN'] * 1000.0
        ei_Nm2 = m['stiffness_bending_kNm2'] * 1000.0
        gj_Nm2 = m['stiffness_torsional_kNm2'] * 1000.0
        area_outer = math.pi * do ** 2 / 4.0
        area_inner = math.pi * di ** 2 / 4.0
        area_struct = area_outer - area_inner
        j_tors = math.pi * (do ** 4 - di ** 4) / 32.0
        E = ea_N / area_struct if area_struct > 0 else 2.1e11
        # I_y from EI/E, not the geometric tube formula -- same reasoning as
        # aml_reader.py's own beam_props block (a flexible riser's real bending stiffness is much
        # lower than a solid tube of the same OD/ID). Meaningless for truss/winch (no bending
        # DOF), computed anyway since it's cheap and unused rather than branched around.
        I_y = ei_Nm2 / E if E > 0 else 5.0e-5
        G = gj_Nm2 / j_tors if j_tors > 0 else 8.0e10
        dry_mass_kgm = (w_dry_kNm * 1000.0) / gravity if gravity > 0 else 0.0
        props = {
            'finite_element_type': fe_type,
            'E_Pa': E, 'G_Pa': G, 'A_m2': area_struct, 'IY_m4': I_y, 'IZ_m4': I_y, 'J_m4': j_tors,
            'EI_Nm2': ei_Nm2, 'EA_N': ea_N,
            'weight_wet_kNm': w_wet_kNm,
            'rho_kgm3': dry_mass_kgm / area_struct if area_struct > 0 else 7850.0,
            'outer_diameter_m': do, 'inner_diameter_m': di,
            'Cd': m['morison_drag_Cd'], 'Cm': m['morison_inertia_Cm'],
            'initial_tension_kN': m.get('initial_tension_kN', 0.0),
            # Per-material Rayleigh damping (docs/roadmap.md) -- genuinely THIS material's own
            # value, not collapsed to a single model-wide material like before. `compute_rayleigh_
            # alpha/beta(0,0,0,0)` (a material that never touched these fields) returns 0.0
            # cleanly -- the whole-model floor below handles the "nobody configured anything"
            # case, so a real per-material zero (deliberately no damping) is never silently
            # overridden once at least one OTHER material in the model has real data.
            'rayleigh_alpha': compute_rayleigh_alpha(
                m.get('rayleigh_period_first_s', 0.0), m.get('rayleigh_period_second_s', 0.0),
                m.get('rayleigh_damping_first', 0.0), m.get('rayleigh_damping_second', 0.0)),
            'rayleigh_beta': compute_rayleigh_beta(
                m.get('rayleigh_period_first_s', 0.0), m.get('rayleigh_period_second_s', 0.0),
                m.get('rayleigh_damping_first', 0.0), m.get('rayleigh_damping_second', 0.0)),
        }
        if fe_type == 'winch' and m.get('winch_payout_curve_id') is not None:
            curve = curves_by_id.get(m['winch_payout_curve_id'])
            if curve is None:
                warnings.append(
                    f"Material '{m.get('name')}' (id={m['id']}): winch_payout_curve_id="
                    f"{m['winch_payout_curve_id']} não encontrado -- winch usará comprimento "
                    "constante (sem curva de recolhimento).")
            else:
                props['payout_curve'] = {'time_s': curve['time_s'], 'fraction': curve['fraction']}
        return props

    material_props_by_id = {mid: _material_props(m) for mid, m in materials_by_id.items()}

    # Whole-model safety floor: if NOT ONE beam/truss/winch material in the whole catalog
    # configured real Rayleigh damping (every one computes to alpha=beta=0.0 -- the common case
    # for a hand-built/blank-project interface JSON that never touches those fields), apply the
    # historical default (0.05/0.005 -- the same values this function used to force onto a single
    # arbitrarily-chosen material) to ALL of them, avoiding the genuinely-undamped-model dynamic
    # instability this floor was originally added for (docs/roadmap.md). Deliberately a WHOLE-
    # MODEL decision, not per-material: once at least one material has real data, every other
    # material's own real zero (e.g. `consider_damping="no"`) stays a real, undamped zero instead
    # of being silently bumped up.
    _damped_types = ('beam', 'truss', 'winch')
    _damping_candidates = [p for p in material_props_by_id.values() if p.get('finite_element_type') in _damped_types]
    if _damping_candidates and all(p['rayleigh_alpha'] == 0.0 and p['rayleigh_beta'] == 0.0 for p in _damping_candidates):
        for p in _damping_candidates:
            p['rayleigh_alpha'] = 0.05
            p['rayleigh_beta'] = 0.005

    GLOBAL_REF_ID = 0
    all_nodes, all_elements = [], []
    prescribed_nodes, restrained_nodes = [], []
    references_json = [{"id": GLOBAL_REF_ID, "type": "GLOBAL", "name": "fixed"}]
    connections_json, lines_json = [], []
    node_id_offset, elem_id_offset, next_connection_id = 0, 0, 1
    seabed_soil = None  # first non-null soil referenced by any segment -- see docstring above
    min_ei_nm2 = None

    lines = interface_json.get('lines', [])
    if not lines:
        raise ValueError("A JSON de interface não define nenhuma linha.")

    for line_num, line in enumerate(lines, start=1):
        segments = line.get('segments') or []
        if not segments:
            raise ValueError(f"Linha '{line.get('name')}' (id={line.get('id')}) não tem segmentos.")

        # Catenary BVP solve + Rayleigh damping both use the FIRST segment's material/soil as a
        # representative approximation (same simplification aml_reader.py's single-material path
        # always used) -- a real varying-EA/weight-along-the-line solve is out of scope here.
        first_seg = segments[0]
        first_mat = material_props_by_id.get(first_seg.get('material_id'))
        if first_mat is None:
            raise ValueError(
                f"Linha '{line.get('name')}': segmento referencia material_id="
                f"{first_seg.get('material_id')}, não encontrado.")
        if first_mat.get('finite_element_type') == 'scalar':
            raise ValueError(
                f"Linha '{line.get('name')}': o primeiro segmento não pode ser do tipo 'scalar' -- "
                "a solução de geometria de catenária precisa de rigidez axial e peso do primeiro "
                "segmento (use um material beam/truss/winch como primeiro segmento da linha).")
        first_soil = soils_by_id.get(first_seg.get('soil_id'))
        friction_coeff = first_soil['friction_lateral'] if first_soil else 0.5

        total_length_m = sum(s['length_m'] for s in segments)
        top_position_m = line.get('top_position_m') or [0.0, 0.0, water_depth_m]

        catenary_result = solve_catenary_geometry(
            top_position_m, line.get('catenary_angle_deg', 5.0), line.get('azimuth_deg', 0.0),
            total_length_m, first_mat['EA_N'], first_mat['weight_wet_kNm'] * 1000.0, friction_coeff,
        )
        if catenary_result is None:
            raise ValueError(
                f"Linha '{line.get('name')}': não foi possível resolver a geometria de catenária "
                "(MoorPy indisponível, ou ângulo/comprimento/profundidade incompatíveis).")
        anchor_global, _t_eff_estimate_n = catenary_result

        seg_specs = []
        for seg in segments:
            elem_len = seg.get('element_length_m') or 1.0
            seg_specs.append({
                'length_m': seg['length_m'],
                'num_elements': max(1, int(round(seg['length_m'] / elem_len))),
            })

        nodes, elements = synthesize_mesh(
            seg_specs, total_length_m, num_elements_fallback=max(1, len(seg_specs)),
            num_elements_override=num_elements_override,
            top_override=top_position_m, bottom_override=anchor_global,
            id_offset=node_id_offset, element_id_offset=elem_id_offset,
        )

        # Per-segment element_type/properties: unlike aml_reader.py's single-material path, every
        # segment here can carry its own real material (and now its own finite_element_type),
        # resolved by id -- genuinely finer fidelity than any AML-derived interface JSON produces
        # today (which only ever had one, and silently beam-only until this round -- see
        # docs/roadmap.md).
        elem_idx = 0
        buoy_elements_this_line = []
        for seg, spec in zip(segments, seg_specs):
            seg_mat = material_props_by_id.get(seg.get('material_id'))
            if seg_mat is None:
                raise ValueError(
                    f"Linha '{line.get('name')}': segmento referencia material_id="
                    f"{seg.get('material_id')}, não encontrado.")
            fe_type = seg_mat.get('finite_element_type', 'beam')
            if num_elements_override is None:
                if fe_type == 'truss':
                    props_key, props = 'truss_properties', build_truss_properties(seg_mat)
                elif fe_type == 'winch':
                    props_key, props = 'winch_properties', build_winch_properties(seg_mat)
                elif fe_type == 'scalar':
                    props_key, props = 'scalar_properties', build_scalar_properties(seg_mat)
                else:
                    props_key = 'section_properties'
                    props = build_section_properties(seg_mat, {'water_density_kgm3': water_density_kgm3})
                for _ in range(spec['num_elements']):
                    elements[elem_idx]['element_type'] = fe_type
                    elements[elem_idx][props_key] = dict(props)
                    elem_idx += 1

                # Boia anexada ao ÚLTIMO nó da subcadeia deste segmento -- BuoyElement é de 1 nó
                # só e pode compartilhar o nó com o elemento estrutural adjacente (mesmo que o
                # cBuoyElement real, ver docs/roadmap.md pra essa suposição de design). Ignorado
                # em modo num_elements_override (ver aviso abaixo) -- a subdivisão por segmento
                # não sobrevive à malha uniforme desse modo.
                buoy_id = seg.get('buoy_id')
                if buoy_id is not None:
                    buoy = buoys_by_id.get(buoy_id)
                    if buoy is None:
                        raise ValueError(
                            f"Linha '{line.get('name')}': segmento referencia buoy_id={buoy_id}, "
                            "não encontrado.")
                    last_node_id = elements[elem_idx - 1]['node2_id']
                    buoy_elements_this_line.append({
                        "id": elem_id_offset + len(elements) + len(buoy_elements_this_line) + 1,
                        "element_type": "buoy",
                        "node_id": last_node_id,
                        "buoy_properties": build_buoy_properties(buoy),
                    })
            elif seg.get('buoy_id') is not None:
                warnings.append(
                    f"Linha '{line.get('name')}': buoy_id do segmento ignorado sob "
                    "num_elements_override (malha uniforme não preserva limites de segmento).")
            if fe_type == 'beam' and (min_ei_nm2 is None or seg_mat['EI_Nm2'] < min_ei_nm2):
                min_ei_nm2 = seg_mat['EI_Nm2']

            seg_soil = soils_by_id.get(seg.get('soil_id'))
            if seg_soil is not None:
                if seabed_soil is None:
                    seabed_soil = seg_soil
                elif seabed_soil['id'] != seg_soil['id']:
                    warnings.append(
                        f"Linha '{line.get('name')}': segmento referencia soil_id={seg_soil['id']}, "
                        f"diferente do solo já em uso para o leito ({seabed_soil['id']}) -- o motor "
                        "hoje só suporta UM solo global; este segundo solo será ignorado no cálculo "
                        "de reação do leito (mas continua registrado na JSON de interface).")
        if num_elements_override is not None:
            # A single uniform mesh replaced the per-segment breakdown (synthesize_mesh's own
            # num_elements_override branch) -- every element gets the FIRST segment's material,
            # same "no per-segment boundary to preserve" fallback aml_reader.py's own
            # num_elements_override path always used.
            sp = build_section_properties(first_mat, {'water_density_kgm3': water_density_kgm3})
            for elem in elements:
                elem['element_type'] = 'beam'
                elem['section_properties'] = dict(sp)

        # Fixes the straight-chord initial mesh to MoorPy's real equilibrium shape (arc-length-
        # consistent) -- same technique aml_reader.py's multi-line path already uses per line.
        # Runs on the pure structural chain ONLY (before buoy elements are appended below) --
        # `correct_line_mesh_via_moorpy` assumes every element is a 2-node chain link
        # (`node1_id`/`node2_id`), which a 1-node buoy element isn't.
        top_id, anchor_id = nodes[0]["id"], nodes[-1]["id"]
        correct_line_mesh_via_moorpy(
            nodes, elements, top_id, anchor_id, total_length_m, {"depth_m": -abs(water_depth_m)},
        )
        elements.extend(buoy_elements_this_line)

        all_nodes.extend(nodes)
        all_elements.extend(elements)
        node_id_offset += len(nodes)
        elem_id_offset += len(elements)
        prescribed_nodes.append({"node_id": top_id, "dofs": [-1] * 6})
        restrained_nodes.append({"node_id": anchor_id, "dofs": [-1] * 6})

        # v1 scope: no vessel motion -- every line's top is ALSO just a fixed point in space (the
        # same GLOBAL reference the anchor uses), not a moving FLOATING reference.
        top_conn_id = next_connection_id; next_connection_id += 1
        connections_json.append({"id": top_conn_id, "reference_id": GLOBAL_REF_ID, "node_id": top_id})
        anchor_conn_id = next_connection_id; next_connection_id += 1
        connections_json.append({"id": anchor_conn_id, "reference_id": GLOBAL_REF_ID, "node_id": anchor_id})
        lines_json.append({
            "id": line.get('id', line_num), "name": line.get('name', f'Line{line_num}'),
            "top_connection_id": top_conn_id, "anchor_connection_id": anchor_conn_id,
        })

    if seabed_soil is None:
        raise ValueError("Nenhum segmento de nenhuma linha referencia um solo válido -- é preciso pelo menos um.")

    seabed_dict = {
        "depth_m": -abs(water_depth_m),
        "stiffness_Nm": seabed_soil['spring_stiffness_kNm'] * 1000.0,
        "friction_coeff": seabed_soil['friction_lateral'],
        "axial_friction": seabed_soil['friction_axial'],
        "lateral_friction": seabed_soil['friction_lateral'],
        "soil_model": seabed_soil.get('model', 'uncoupled'),
    }
    if seabed_soil.get('deflection_axial_m') is not None:
        seabed_dict['axial_elastic_deflection_limit'] = seabed_soil['deflection_axial_m']
    if seabed_soil.get('deflection_lateral_m') is not None:
        seabed_dict['lateral_elastic_deflection_limit'] = seabed_soil['deflection_lateral_m']

    current_dict = {
        "depth_below_surface_m": (current or {}).get('depth_below_surface_m', [0.0, water_depth_m]),
        "velocities_ms": (current or {}).get('velocities_ms', [0.0, 0.0]),
        "angles_deg": (current or {}).get('angles_deg', [0.0, 0.0]),
    }
    wave_dict = {
        "type": (wave or {}).get('type', 'REGULAR'),
        "period_s": (wave or {}).get('period_s', 10.0),
        "height_m": (wave or {}).get('height_m', 2.0),
        "amplitude_m": (wave or {}).get('height_m', 2.0) / 2.0,
        "gamma": (wave or {}).get('gamma', 3.3),
        "angle_deg": (wave or {}).get('angle_deg', 0.0),
    }

    static_opts = dict(analysis.get('static', {}))
    static_opts.setdefault('steps', 20)
    static_opts.setdefault('max_iterations', 300)
    static_opts.setdefault('tolerance', 0.01)
    static_opts.setdefault('vessel_offset', {"near_m": 0.0, "far_m": 0.0})
    robustness_overrides = static_robustness_overrides(min_ei_nm2 if min_ei_nm2 is not None else 1.0e9)
    if robustness_overrides:
        static_opts['enable_unbalanced_criteria'] = True
        static_opts.update(robustness_overrides)
        static_opts['max_iterations'] = max(static_opts.get('max_iterations', 300), 400)

    dyn_src = analysis.get('dynamic', {})
    dynamic_opts = {
        "enabled": dyn_src.get('enabled', True),
        "duration_s": dyn_src.get('duration_s', 20.0),
        "dt_s": dyn_src.get('dt_s', 0.05),
        "max_iterations": dyn_src.get('max_iterations', 20),
        "tolerance": dyn_src.get('tolerance', 1.0e-4),
    }

    # Rayleigh damping is now genuinely per-element (each element's own `section_properties`/
    # `truss_properties`/`winch_properties` carries its own material's real `rayleigh_alpha`/
    # `rayleigh_beta`, computed above in `_material_props()`, with the whole-model floor already
    # applied there too when nothing was configured anywhere) -- no single global value to derive
    # here anymore. `dynamic.rayleigh_damping` is deliberately left UNSET: the C++ engine's own
    # `AnalysisOptionsConfig` default (0.05/0.01, model.hpp) is only ever consulted as a fallback
    # for an element with no per-element data at all (a JSON predating this field), which no
    # longer applies to anything this compiler produces.

    if duration is not None:
        dynamic_opts['duration_s'] = duration
    if dt is not None:
        dynamic_opts['dt_s'] = dt
    if static_only:
        dynamic_opts['enabled'] = False

    config = {
        "title": interface_json.get('title', 'Interface Model'),
        "schema_version": SCHEMA_VERSION,
        "model": {"nodes": all_nodes, "elements": all_elements},
        "boundary_conditions": {
            "prescribed_dofs": prescribed_nodes,
            "restrained_dofs": restrained_nodes,
        },
        "references": references_json,
        "connections": connections_json,
        "lines": lines_json,
        "environmental": {
            "seabed": seabed_dict,
            "water_density": water_density_kgm3,
            "current": current_dict,
            "wave": wave_dict,
        },
        "analysis_options": {
            "static": static_opts,
            "dynamic": dynamic_opts,
        },
    }
    return config, warnings


def _build_config_from_aml_via_interface(aml_path, line_filter, num_elements_override, load_case_id,
                                          duration, dt, static_only):
    """Shared body of `build_config_from_aml()`/`build_config_from_aml_multiline()` below --
    both are now thin wrappers around `aml_reader.py::to_interface_json()` (the AML->interface
    translation) + `build_config_from_interface()` above (the single interface->simulation
    compiler, also used by the planned web editor's "blank project" path). `line_filter`, when
    not `None`, is a list of 0-based line indices (matching the old `line_index`/`line_indices`
    selection) to keep -- `None` keeps every line the `.aml` defines.

    Since `to_interface_json()` only ever produces ONE `%ANALYSIS_CASE`-derived analysis (see its
    own docstring), `analysis_id` is always that analysis's id -- there is nothing for a caller to
    choose yet on the AML path (no CLI flag added for it in this round; every `.aml` file read so
    far only defines one analysis anyway, so there is nothing to choose between).
    """
    from aml_reader import ANFLEXAMLReader

    reader = ANFLEXAMLReader(str(aml_path))
    interface_json, iface_warnings = reader.to_interface_json()
    for w in iface_warnings:
        print(f"⚠️ {w}")

    if line_filter is not None:
        interface_json = dict(interface_json)
        interface_json['lines'] = [interface_json['lines'][i] for i in line_filter]

    analysis_id = interface_json['analyses'][0]['id']
    if load_case_id is None:
        load_case_id = interface_json['analyses'][0]['load_case_ids'][0]

    config, warnings = build_config_from_interface(
        interface_json, analysis_id, load_case_id,
        num_elements_override=num_elements_override, duration=duration, dt=dt, static_only=static_only,
    )
    for w in warnings:
        print(f"⚠️ {w}")
    return config


def build_config_from_aml(aml_path, line_index=None, num_elements_override=None, load_case_id=None,
                           duration=None, dt=None, static_only=False):
    """Compiles the simulation JSON from a plain `.aml` (no XML+H5 export needed) -- the
    `.aml`-pure counterpart of `build_config_from_xml_h5()` above. Thin wrapper: translates the
    `.aml` to a "JSON de interface" (`aml_reader.py::to_interface_json()`) and hands it to
    `build_config_from_interface()`, the single compiler also used by the web editor's "blank
    project" path (docs/roadmap.md, Eixo 3a) -- both AML-derived and hand-built interface JSONs
    compile identically, including the MoorPy mesh-length correction and the EI-based robustness
    overrides (both now inside `build_config_from_interface()` itself).

    `line_index`: 0-based index of the single `%LINE` to keep -- `None` (default) keeps every
    line the `.aml` defines (for the common single-line case this is the same result as before;
    for a genuine multi-line `.aml` this now behaves like `build_config_from_aml_multiline()`
    unless a specific line is requested, since the compiler no longer has a separate single-line
    code path to default to).
    `load_case_id`: which %LOAD_CASE (current+wave pairing) to resolve, when the `.aml` defines
    more than one (e.g. "Near"/"Far") -- `None` picks the first in the file, same default as
    before.
    `duration`/`dt`/`static_only`: same override semantics as `build_config_from_xml_h5()`.
    """
    return _build_config_from_aml_via_interface(
        aml_path, [line_index] if line_index is not None else None,
        num_elements_override, load_case_id, duration, dt, static_only,
    )


def build_config_from_aml_multiline(aml_path, line_indices=None, num_elements_override=None, load_case_id=None,
                                     duration=None, dt=None, static_only=False):
    """Multi-line sibling of `build_config_from_aml()` (docs/roadmap.md backlog: "múltiplas
    linhas") -- same wrapper, `line_indices=None` (default) keeps every line, same as
    `build_config_from_aml()`'s own default now. Kept as a separate function for backward
    compatibility with existing callers (`run_from_aml.py --all-lines`); the two are otherwise
    identical after the interface-JSON reroute.
    """
    return _build_config_from_aml_via_interface(
        aml_path, line_indices, num_elements_override, load_case_id, duration, dt, static_only,
    )


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
