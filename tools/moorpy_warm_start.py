"""
Gera uma variante "warm start" do JSON de entrada do risersim: resolve o
equilibrio quasi-estatico com o MoorPy (biblioteca validada em
risersim/spikes/mooring_validation/, ~1% de erro vs. o resultado real do
ANFLEX no Exemplo_01a) e adiciona ao JSON uma secao `warm_start` com a
posicao de cada no nessa geometria de equilibrio.

`risersim::ModelBuilder` (risersim/src/model_builder.cpp) consome essa secao (se presente) para usar
como chute inicial do solver estatico proprio, em vez da geometria de
referencia crua lida do H5 -- que hoje faz o Newton-Raphson do risersim
divergir no Exemplo_01a real. Sem a secao `warm_start`, o comportamento do
risersim e identico ao de hoje (mudanca puramente aditiva).

Uso:
    python moorpy_warm_start.py <input.json> <output_com_warm_start.json>
"""
import json
import os
import sys

# Reaproveita o codigo ja validado do spike, em vez de duplicar.
_SPIKE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "spikes", "mooring_validation")
sys.path.insert(0, os.path.normpath(_SPIKE_DIR))

from build_from_risersim_json import build_system_from_json  # noqa: E402
from arc_length_utils import resample_by_normalized_arc_length  # noqa: E402


def compute_warm_start_positions(input_json_path):
    """Resolve o equilibrio no MoorPy e retorna uma lista de
    {"node_id": ..., "coords": [x, y, z]} na mesma ordem/arco-comprimento
    normalizado dos nos do JSON de entrada (referencial do JSON: Z=0 no
    leito marinho, cresce para cima -- mesmo referencial de sempre).
    """
    ms, line, meta = build_system_from_json(input_json_path)
    ms.initialize()
    ms.solveEquilibrium(DOFtype="free", tol=0.05, maxIter=500)

    Xs, Ys, Zs, _ = line.getLineCoords(0.0)
    water_depth = meta["water_depth"]
    Zs_json_frame = [z + water_depth for z in Zs]

    # line.getLineCoords vai de pointA (ancora, metadata["anchor_node_id"])
    # para pointB (topo, metadata["top_node_id"]). A ordem dos nos do JSON
    # (node_order abaixo) segue a cadeia de elementos a partir do
    # node1_id do primeiro elemento, que pode ser o topo ou a ancora --
    # se for o topo, a lista de nos vai TOPO -> ANCORA, invertido em
    # relacao ao MoorPy, entao invertemos o perfil do MoorPy para casar.
    with open(input_json_path, "r", encoding="utf-8") as f:
        data = json.load(f)

    nodes_json = data["model"]["nodes"]
    elements_json = data["model"]["elements"]
    coords_by_id = {n["id"]: n["coords"] for n in nodes_json}

    # Comprimento de arco acumulado dos nos de entrada, na mesma ordem dos
    # elementos (node1_id -> node2_id em sequencia) -- mesma nocao de
    # "posicao ao longo da linha" usada para reamostrar o MoorPy.
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
