"""
catenary_geometry.py
=====================
Shared catenary-geometry building blocks, extracted from `aml_reader.py` (2026-08-17) so both the
AML importer (`ANFLEXAMLReader`) and the future interface-JSON compiler
(`risersim_runner.py::build_config_from_interface()`, docs/roadmap.md "Eixo 3a") reuse the exact
same validated logic instead of duplicating it -- this is a pure refactor, no behavior change:
every function here is byte-identical to its former `ANFLEXAMLReader` method body, just without
`self` (none of the three ever actually read instance state).

Functions:
- `solve_catenary_geometry()`: given a real top-connection position + %LINE.CATENARY.ANGLE
  boundary condition, solves the horizontal span (and resulting anchor position) via MoorPy.
- `correct_line_mesh_via_moorpy()`: overwrites a straight-chord initial mesh with MoorPy's real
  equilibrium shape (arc-length-consistent), in place.
- `build_section_properties()`: builds the `section_properties` dict shared by every element of a
  line, from resolved material + global-environment fields.
"""
import json
import math
import tempfile
from pathlib import Path


def solve_catenary_geometry(connection_global, angle_deg_from_vertical, azimuth_deg,
                             total_length_m, ea_n, w_wet_n_m, friction_coeff=0.5):
    """Solves for the real horizontal span (and resulting anchor/touchdown position) implied
    by the line's real %LINE.CATENARY.ANGLE boundary condition, given the now-known real
    connection position (see `ANFLEXAMLReader._resolve_connection_global()`) -- this is the piece
    `_synthesize_mesh()` was missing: without it, the synthetic mesh's span comes from a
    length-only heuristic (`0.70*L`) with no relation to the real angle/depth/length
    boundary-value problem, which is most of why a pure-.aml run's static T_eff (~1100+ kN
    on Exemplo_01a) is ~5x the real XML+H5 result (~217 kN) -- see docs/roadmap.md.

    The vertical drop (moorpy's `ZF`) is taken as `connection_global[2]` itself, NOT the
    nominal water depth -- the real ANFLEX global frame always has the seabed at Z=0 (see
    `_resolve_connection_global()`'s own `origin.z = seabed_depth_m - draft` derivation, and
    model_builder.cpp's generic min(Z)=seabed re-derivation), and the connection sits BELOW
    the nominal water surface by the vessel's draft, so `water_depth_m` alone overshoots the
    real vertical drop by that draft (confirmed: using water_depth_m first gave an anchor Z
    of -8m instead of the real ~0m for Exemplo_01a's Cross case -- 13m draft, 5m local Z
    offset, 8m net).

    The real ANFLEX pre-processor solves this exact problem with an external, closed-source
    library (`tec_line`, `interfaces/src/line.cpp`'s `TEC_ANGLE_B` boundary condition) not
    available in this repo. Standard catenary equations are not ANFLEX-proprietary, though:
    this uses moorpy.Catenary.catenary() (XF, ZF, L, EA, W -> tension; already validated
    against real ANFLEX in spikes/mooring_validation/, ~1% error) as the forward "given a
    span, what's the resulting angle" evaluator, wrapped in a small outer bisection on the
    span XF to match the target top angle -- moorpy itself has no direct "solve for span
    given an angle" mode (confirmed: catenary() takes XF as an input, not an unknown it
    solves for).

    The `90 - azimuth_deg` direction convention (mirroring `_resolve_connection_global()`'s
    own ship-orientation formula) was validated end-to-end against real data: applied to
    Exemplo_01a's real numbers, it reproduces the "Cross" load case's actual exported anchor
    node (XML+H5 path) almost exactly (predicted vs. real anchor position within the
    expected error margin of the ~1%-validated moorpy solver itself).

    Lazily imports moorpy (an optional runtime dependency, see Dockerfile) -- returns None
    if it isn't installed, or if the bisection can't bracket a span matching the target
    angle (e.g. physically unreachable for this length/depth combination), so callers fall
    back to the old heuristic span (see `to_risersim_json()`/`_synthesize_mesh()`).
    """
    try:
        from moorpy.Catenary import catenary
    except ImportError:
        return None

    zf = connection_global[2]
    if zf <= 0.0:
        return None

    # interfaces/src/line.cpp's own "90.0 - bc.angle" -- CATENARY.ANGLE is from vertical,
    # moorpy's catenary() tension angle (atan2(VF,HF)) is from horizontal.
    target_angle_deg = 90.0 - angle_deg_from_vertical

    def top_angle_deg(xf):
        try:
            _, _, _, _, info = catenary(xf, zf, total_length_m, ea_n, w_wet_n_m, CB=friction_coeff)
        except Exception:
            return None
        return math.degrees(math.atan2(info['VF'], info['HF']))

    # Coarse scan for a sign change (bracket), then bisection -- more robust than a raw
    # bisection from the domain's ends, since moorpy's solver can fail to converge (raises)
    # for XF values very close to 0 or to the fully-taut limit (total_length_m).
    n_scan = 60
    max_xf = total_length_m * 0.999
    prev_x, prev_err = None, None
    bracket = None
    for i in range(1, n_scan + 1):
        x = max_xf * i / n_scan
        ang = top_angle_deg(x)
        if ang is None:
            continue
        err = ang - target_angle_deg
        if prev_err is not None and prev_err * err <= 0:
            bracket = (prev_x, x)
            break
        prev_x, prev_err = x, err
    if bracket is None:
        return None

    lo, hi = bracket
    lo_err = top_angle_deg(lo) - target_angle_deg
    xf = lo
    for _ in range(60):
        mid = 0.5 * (lo + hi)
        ang = top_angle_deg(mid)
        if ang is None:
            break
        err = ang - target_angle_deg
        xf = mid
        if abs(err) < 1.0e-3:
            break
        if lo_err * err <= 0:
            hi = mid
        else:
            lo, lo_err = mid, err

    try:
        _, _, _, _, info = catenary(xf, zf, total_length_m, ea_n, w_wet_n_m, CB=friction_coeff)
    except Exception:
        return None
    t_eff_top_n = math.hypot(info['HF'], info['VF'])

    direction_rad = math.radians(90.0 - azimuth_deg)
    anchor_global = [
        connection_global[0] + xf * math.cos(direction_rad),
        connection_global[1] + xf * math.sin(direction_rad),
        0.0,  # seabed, in the real ANFLEX global frame this connection_global is also in
    ]
    return anchor_global, t_eff_top_n


def correct_line_mesh_via_moorpy(nodes, elements, top_id, anchor_id, total_length_m, seabed_dict):
    """Overwrites `nodes[i]["coords"]` in place with MoorPy's own solved equilibrium shape for
    ONE line's own nodes/elements -- the per-line counterpart of
    `risersim_runner.py::_apply_moorpy_mesh_correction()` (which assumes the WHOLE
    `model.elements` array forms one continuous chain, true for a single-line JSON but not for a
    multi-line JSON's flat merged element list).

    Why this exists: a straight-chord initial mesh is always shorter than the real (sagging)
    catenary arc it approximates, and `model_builder.cpp` derives each element's `L_unstretched`
    (used for axial strain) from the Euclidean distance between the input node coordinates, not
    from the line's declared segment length -- an under-length reference produces several times
    too much static top tension. Fix: replace the straight-line coordinates with MoorPy's own
    equilibrium shape (arc-length-consistent) before `model_builder.cpp` ever computes
    `L_unstretched` from them.

    Same best-effort contract as `_apply_moorpy_mesh_correction()`: any exception (MoorPy
    unavailable, solve failure, ...) leaves `nodes` unchanged (the straight chord -- length may be
    off) and prints a warning, never raises.
    """
    try:
        from moorpy_warm_start import compute_warm_start_positions
    except Exception as exc:
        print(f"⚠️ MoorPy indisponível ({exc}) -- linha mantém a malha reta original (comprimento pode ficar incorreto).")
        return

    line_doc = {
        "model": {"nodes": nodes, "elements": elements, "total_length_m": total_length_m},
        "boundary_conditions": {
            "prescribed_dofs": [{"node_id": top_id, "dofs": [-1] * 6}],
            "restrained_dofs": [{"node_id": anchor_id, "dofs": [-1] * 6}],
        },
        "environmental": {"seabed": seabed_dict},
    }
    try:
        with tempfile.TemporaryDirectory() as tmp:
            scratch_path = Path(tmp) / "input_simulation.json"
            with open(scratch_path, "w", encoding="utf-8") as f:
                json.dump(line_doc, f)
            node_positions, _meta = compute_warm_start_positions(str(scratch_path))
        coords_by_id = {p["node_id"]: p["coords"] for p in node_positions}
        for n in nodes:
            if n["id"] in coords_by_id:
                n["coords"] = coords_by_id[n["id"]]
    except Exception as exc:
        print(f"⚠️ Não foi possível corrigir a malha desta linha via MoorPy ({exc}) -- "
              "mantém a malha reta original (comprimento pode ficar incorreto).")


def synthesize_mesh(segments, total_length, num_elements_fallback=100, num_elements_override=None,
                     top_override=None, bottom_override=None, depth=0.0, span_x=0.0,
                     id_offset=0, element_id_offset=None):
    """Synthesizes an initial straight-line node/element mesh from a line's segment lengths --
    there's no pre-solved geometry to copy out of a plain .aml (or a hand-built interface JSON)
    the way `xml_h5_reader.py` copies real solved coordinates out of the H5. Confirmed by reading
    `static_analysis.cpp`: the solver treats whatever `nodes[i].coords` are given as an initial/
    reference configuration and deforms it via load-stepping Newton-Raphson to reach the real
    equilibrium catenary shape -- a straight line between anchor and touchdown is an adequate
    starting guess, it does not need to be physically accurate.

    `top_override`/`bottom_override` (from a real %CONNECTION/%FLOATING + a converged moorpy
    catenary solve, see `solve_catenary_geometry()`) replace the synthetic `(0,0,0)`/
    `(span_x,0,-depth)` endpoints with the real global connection and anchor positions instead --
    still just an initial guess for the Newton-Raphson solver, but a much better one (real span,
    not a length-only heuristic).

    `segments`: list of `{"length_m", "num_elements"}` dicts (element count per segment, already
    resolved by the caller from e.g. `element_length_m`) -- when `num_elements_override` is given,
    the segment breakdown is replaced with a single uniform mesh of that many elements instead.

    `id_offset`/`element_id_offset` (default 0/None -- None means "same as id_offset"): shifts
    this line's node/element ids so multiple lines can be merged into one flat
    `model.nodes`/`model.elements` list with globally-unique ids -- `ModelBuilder` resolves every
    id by lookup, not by assuming a dense 1..N sequence, so ids only need to be unique across the
    WHOLE model, not contiguous per line.

    Returns `(nodes, elements)`, each element a plain dict ready to drop into `model.nodes`/
    `model.elements` (elements still need `section_properties` attached by the caller).
    """
    if element_id_offset is None:
        element_id_offset = id_offset

    total_length = total_length if total_length and total_length > 0 else 500.0

    if num_elements_override:
        seg_lengths = [total_length]
        seg_elem_counts = [max(1, int(num_elements_override))]
    elif segments:
        seg_lengths = [s.get('length_m', 0.0) for s in segments]
        seg_elem_counts = [max(1, s.get('num_elements', 1)) for s in segments]
    else:
        seg_lengths = [total_length]
        seg_elem_counts = [max(1, int(num_elements_fallback))]

    top = tuple(top_override) if top_override is not None else (0.0, 0.0, 0.0)
    bottom = tuple(bottom_override) if bottom_override is not None else (span_x, 0.0, -depth)

    nodes = [{"id": id_offset + 1, "coords": list(top)}]
    node_id = id_offset + 1
    cum_length = 0.0
    for seg_len, seg_n in zip(seg_lengths, seg_elem_counts):
        step = seg_len / seg_n
        for _ in range(seg_n):
            cum_length += step
            t = min(1.0, cum_length / total_length)
            coords = [top[j] + t * (bottom[j] - top[j]) for j in range(3)]
            node_id += 1
            nodes.append({"id": node_id, "coords": coords})

    nodes[-1]["coords"] = list(bottom)  # exact touchdown/anchor point, no float drift

    elements = []
    for i in range(len(nodes) - 1):
        elements.append({
            "id": element_id_offset + i + 1,
            "node1_id": nodes[i]["id"],
            "node2_id": nodes[i + 1]["id"],
        })
    return nodes, elements


def compute_rayleigh_alpha(T1, T2, xi1, xi2):
    """Rayleigh damping coefficient alpha: C = alpha*M + beta*K."""
    if T1 > 0 and T2 > 0 and T1 != T2:
        w1 = 2 * math.pi / T1
        w2 = 2 * math.pi / T2
        det = (1 / w1) * w2 - (1 / w2) * w1
        if abs(det) > 1e-12:
            return 2.0 * (xi1 * w2 - xi2 * w1) / (w2 ** 2 - w1 ** 2) * w1 * w2
    return 0.0


def compute_rayleigh_beta(T1, T2, xi1, xi2):
    """Rayleigh damping coefficient beta: C = alpha*M + beta*K."""
    if T1 > 0 and T2 > 0 and T1 != T2:
        w1 = 2 * math.pi / T1
        w2 = 2 * math.pi / T2
        if abs(w2 ** 2 - w1 ** 2) > 1e-12:
            return 2.0 * (xi2 * w2 - xi1 * w1) / (w2 ** 2 - w1 ** 2)
    return 0.0


def static_robustness_overrides(ei_nm2: float) -> dict:
    """Escape hatch for a real static-solver instability found on
    exemplos/Multilinhas/Multilinhas.aml (EI=6.27 kN.m^2, 2070 elements/line, 2200m water
    depth): full-step Newton blows up in ROTATIONAL DOFs (not the already-known seabed
    contact/friction chattering) for very bendable, finely-meshed lines. Root cause: with
    EI this low, the rotational stiffness terms are near-singular, so a full Newton step
    overcorrects rotation wildly instead of converging.

    Fix requires zero C++ change: `enable_step_limiting` (already implemented, off by
    default) with a MUCH tighter rotation cap than its own default (0.01 rad vs 0.3 rad)
    resolves it completely -- confirmed on all 7 Multilinhas.aml lines individually and on
    the combined multi-line model, all converging to the identical T_eff (2361 kN).
    `unbalanced_force_tol`/`unbalanced_moment_tol` also loosened (5->30 N) -- with the
    tighter rotation cap alone, convergence was real but needed hundreds of iterations per
    step (chasing a residual that was already physically negligible); loosening these too
    cut that back down to ~15-40 iterations/step. This deliberately overrides any real
    %ANALYSIS_CASE.STATIC.CONVERGENCE_CRITERIUM value already extracted -- a documented
    departure from fidelity, justified because the alternative is non-convergence for this
    class of case.

    Verified harmless on an already-validated normal case too (Exemplo_01a/Cross, EI=21.7
    kN.m^2, 500 elements): same T_eff (217.3 kN) as the unmodified baseline, converging in
    FEWER iterations -- so the EI threshold below only needs to separate the two real data
    points found so far, not be exact; erring toward triggering more often costs a little
    compute, not correctness. See docs/roadmap.md Eixo 2c.
    """
    EI_THRESHOLD_NM2 = 15_000.0  # between the two real data points: 6 270 (unstable) / 21 700 (fine)
    if ei_nm2 >= EI_THRESHOLD_NM2:
        return {}
    return {
        "enable_step_limiting": True,
        "max_translation_step_m": 0.2,
        "max_rotation_step_rad": 0.01,
        "unbalanced_force_tol": 30.0,
        "unbalanced_moment_tol": 30.0,
    }


def build_section_properties(mat, glb):
    """Builds the `section_properties` dict shared by every element of a line, from the resolved
    %MATERIAL.FLEXIBLE_LINE fields (`mat`, as produced by
    `ANFLEXAMLReader.to_risersim_config()`'s `beam_props`) and global environment fields (`glb`).
    Extracted so every producer of this dict (single-line, multi-line, and the future
    interface-JSON compiler) builds it identically.

    `rho_structural` is deliberately omitted: the .aml only exposes %MATERIAL.WEIGHT.DRY/.WET
    (already net of/inclusive of buoyancy), not a distinct raw structural/dry density independent
    of the wet weight the way the XML's material <density> tag is
    (`xml_h5_reader.extract_material_properties()`'s `density_kNm3`) -- `mat['rho_kgm3']` here
    (dry_mass_kgm/area_struct) is already the right "rho", but there's no second,
    independently-sourced number to put in "rho_structural" that wouldn't just be relabeling the
    same one under a different key. Leaving it out and letting the C++ side's documented
    rho-derived fallback (model_builder.cpp) apply is more honest than passing a value with false
    provenance.
    """
    return {
        "E": mat['E_Pa'], "G": mat['G_Pa'], "A": mat['A_m2'],
        "IY": mat['IY_m4'], "IZ": mat['IZ_m4'], "J": mat['J_m4'],
        "EI": mat['EI_Nm2'],
        "weight_wet_kNm": mat['weight_wet_kNm'],
        "rho": mat['rho_kgm3'],
        "D_outer": mat['outer_diameter_m'], "D_inner": mat['inner_diameter_m'],
        "rho_fluid": glb['water_density_kgm3'],
        "Ca": mat['Cm'] - 1.0,
        "Cd": mat['Cd'],
    }
