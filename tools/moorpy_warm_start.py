"""
Generates a "warm start" variant of risersim's input JSON: solves the
quasi-static equilibrium with MoorPy (a library validated in
risersim/spikes/mooring_validation/, ~1% error vs. ANFLEX's real result on
Exemplo_01a) and adds a `warm_start` section to the JSON with each node's
position in that equilibrium geometry.

`risersim::ModelBuilder` (risersim/src/model_builder.cpp) consumes this section (if present) to use
as the initial guess for its own static solver, instead of the raw reference
geometry read from the H5 -- which today makes risersim's Newton-Raphson
diverge on the real Exemplo_01a. Without the `warm_start` section, risersim's
behavior is identical to today's (a purely additive change).

Usage:
    python moorpy_warm_start.py <input.json> <output_com_warm_start.json>
"""
import json
import os
import sys

# Reuses the spike's already-validated code, instead of duplicating it.
_SPIKE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "spikes", "mooring_validation")
sys.path.insert(0, os.path.normpath(_SPIKE_DIR))

from build_from_risersim_json import build_system_from_json  # noqa: E402
from arc_length_utils import resample_by_normalized_arc_length  # noqa: E402


def compute_warm_start_positions(input_json_path):
    """Solves the equilibrium in MoorPy and returns a list of
    {"node_id": ..., "coords": [x, y, z]} in the same order/normalized
    arc-length as the input JSON's nodes (JSON's reference frame: Z=0 at
    the seabed, increasing upward -- the usual reference frame).
    """
    ms, line, meta = build_system_from_json(input_json_path)
    ms.initialize()
    ms.solveEquilibrium(DOFtype="free", tol=0.05, maxIter=500)

    Xs, Ys, Zs, _ = line.getLineCoords(0.0)
    water_depth = meta["water_depth"]
    Zs_json_frame = [z + water_depth for z in Zs]

    # line.getLineCoords goes from pointA (anchor, metadata["anchor_node_id"])
    # to pointB (top, metadata["top_node_id"]). The JSON's node order
    # (node_order below) follows the element chain starting from the
    # first element's node1_id, which can be either the top or the anchor --
    # if it's the top, the node list goes TOP -> ANCHOR, reversed relative
    # to MoorPy, so we reverse MoorPy's profile to match.
    with open(input_json_path, "r", encoding="utf-8") as f:
        data = json.load(f)

    nodes_json = data["model"]["nodes"]
    elements_json = data["model"]["elements"]
    coords_by_id = {n["id"]: n["coords"] for n in nodes_json}

    # Cumulative arc length of the input nodes, in the same order as the
    # elements (node1_id -> node2_id in sequence) -- the same notion of
    # "position along the line" used to resample MoorPy.
    node_order = [elements_json[0]["node1_id"]] + [e["node2_id"] for e in elements_json]
    s = [0.0]
    for i in range(1, len(node_order)):
        c1 = coords_by_id[node_order[i - 1]]
        c2 = coords_by_id[node_order[i]]
        dx, dy, dz = c2[0] - c1[0], c2[1] - c1[1], c2[2] - c1[2]
        s.append(s[-1] + (dx ** 2 + dy ** 2 + dz ** 2) ** 0.5)
    s_norm = [v / s[-1] for v in s]

    if node_order[0] == meta["top_node_id"]:
        Xs, Ys, Zs_json_frame = Xs[::-1], Ys[::-1], Zs_json_frame[::-1]

    x_new, y_new, z_new = resample_by_normalized_arc_length(Xs, Ys, Zs_json_frame, s_norm)

    return [
        {"node_id": node_order[i], "coords": [float(x_new[i]), float(y_new[i]), float(z_new[i])]}
        for i in range(len(node_order))
    ], meta


def main(input_path, output_path):
    node_positions, meta = compute_warm_start_positions(input_path)

    with open(input_path, "r", encoding="utf-8") as f:
        data = json.load(f)

    data["warm_start"] = {
        "source": "moorpy",
        "note": "Geometria de equilibrio quasi-estatico calculada pelo MoorPy "
                "(risersim/spikes/mooring_validation/), usada como chute "
                "inicial do solver estatico proprio.",
        "node_positions": node_positions,
    }

    with open(output_path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)

    print("=" * 60)
    print("  moorpy_warm_start - gera geometria inicial via MoorPy")
    print("=" * 60)
    print(f"Entrada:  {input_path}")
    print(f"Saida:    {output_path}")
    print(f"Nos com warm start: {len(node_positions)}")
    print(f"Tracao no topo (MoorPy): {meta['weight_wet_N_m'] * meta['L_unstretched'] / 1000.0:.2f} kN (peso total, referencia)")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Uso: python moorpy_warm_start.py <input.json> <output_com_warm_start.json>")
        sys.exit(1)
    main(sys.argv[1], sys.argv[2])
