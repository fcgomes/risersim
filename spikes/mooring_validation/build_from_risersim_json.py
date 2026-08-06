"""
Converte o JSON estruturado do risersim (o mesmo lido por risersim::ModelBuilder,
risersim/src/model_builder.cpp) em um moorpy.System pronto para resolver o equilibrio
quasi-estatico.

Nao mexe no motor C++ do risersim -- e um script isolado, so leitura do JSON.
"""
import json
import math

import moorpy as mp


def load_risersim_json(json_path):
    with open(json_path, "r", encoding="utf-8") as f:
        return json.load(f)


def build_system_from_json(json_path, rho_water_override=None, g=9.81, top_xyz_override=None):
    """Le o JSON do risersim e monta um moorpy.System com uma unica linha
    entre os dois nos com condicao de contorno fixa (topo e ancora).

    top_xyz_override: se fornecido (X, Y, Z no mesmo referencial do JSON,
        Z=0 no leito marinho), usa essa posicao para o ponto do topo em vez
        da geometria de referencia lida do H5. Serve para reproduzir a
        posicao final real do ANFLEX (que aplica o offset estatico da FPSO
        nesse caso de carga) e permitir uma comparacao no-a-no exata.

    Retorna (system, line, metadata) onde metadata traz os valores usados
    (uteis para o script de comparacao).
    """
    data = load_risersim_json(json_path)

    nodes_json = data["model"]["nodes"]
    elements_json = data["model"]["elements"]

    coords_by_id = {n["id"]: n["coords"] for n in nodes_json}

    # 1. Nos fixos (topo e ancora) a partir das condicoes de contorno.
    fixed_ids = []
    bc = data.get("boundary_conditions", {})
    for entry in bc.get("prescribed_dofs", []):
        fixed_ids.append(entry["node_id"])
    for entry in bc.get("restrained_dofs", []):
        fixed_ids.append(entry["node_id"])
    if len(fixed_ids) < 2:
        raise ValueError(
            f"Esperava pelo menos 2 nos fixos (prescribed + restrained), achei {len(fixed_ids)}"
        )

    # Convencao observada nos dados do Exemplo_01a: Z=0 esta no leito marinho
    # (o no mais baixo tem Z ~ 0), Z cresce para cima. O no de maior Z entre
    # os fixos e o topo (conexao com a unidade flutuante); o de menor Z e a
    # ancora/PLET no fundo.
    fixed_ids_sorted = sorted(fixed_ids, key=lambda nid: coords_by_id[nid][2])
    anchor_id = fixed_ids_sorted[0]
    top_id = fixed_ids_sorted[-1]

    anchor_xyz = coords_by_id[anchor_id]
    top_xyz = list(top_xyz_override) if top_xyz_override is not None else coords_by_id[top_id]

    # 2. Propriedades de secao/material -- assume uma unica linha homogenea
    #    (Exemplo_01a so tem "L1"); usa a media entre elementos por robustez.
    E_list, A_list, w_wet_list, d_out_list = [], [], [], []
    rho_fluid_list = []
    for e in elements_json:
        sp = e["section_properties"]
        E_list.append(sp["E"])
        A_list.append(sp["A"])
        w_wet_list.append(sp["weight_wet_kNm"] * 1000.0)  # kN/m -> N/m
        d_out_list.append(sp["D_outer"])
        rho_fluid_list.append(sp.get("rho_fluid", 1025.0))

    def avg(lst):
        return sum(lst) / len(lst)

    EA = avg(E_list) * avg(A_list)
    w_wet_N_m = avg(w_wet_list)
    d_vol = avg(d_out_list)
    rho_water = rho_water_override if rho_water_override is not None else avg(rho_fluid_list)

    # 3. Comprimento nao-esticado: soma das distancias no->no ao longo da
    #    linha, na mesma ordem dos elementos (mesma logica que
    #    ModelBuilder usa por elemento, so que somada para a linha toda).
    #    As coordenadas do JSON ja sao a geometria "instalada" (lida do H5),
    #    entao essa soma e uma boa aproximacao do comprimento real da linha.
    L_unstretched = 0.0
    for e in elements_json:
        c1 = coords_by_id[e["node1_id"]]
        c2 = coords_by_id[e["node2_id"]]
        dx = c2[0] - c1[0]
        dy = c2[1] - c1[1]
        dz = c2[2] - c1[2]
        L_unstretched += math.sqrt(dx * dx + dy * dy + dz * dz)

    # 4. Profundidade / seabed. environmental.seabed.depth_m documenta a
    #    lamina d'agua; a convencao de Z dos nos (seabed em Z=0) e a fonte
    #    de verdade para onde a ancora realmente esta.
    env = data.get("environmental", {})
    seabed = env.get("seabed", {})
    water_depth = abs(seabed.get("depth_m", anchor_xyz[2]))
    friction_coeff = seabed.get("friction_coeff", 0.5)

    # 5. Monta o moorpy.System.
    #    IMPORTANTE (achado durante a validacao): moorpy.Line.staticSolve()
    #    RECALCULA o peso submerso internamente a partir de `mass` (kg/m) e
    #    do diametro (`d_vol`), como (mass - pi/4*d_vol^2*rho_water)*g --
    #    ele IGNORA o `w` passado em setLineType() na hora de resolver.
    #    Por isso precisamos "engenheirar reverso" a massa que, combinada
    #    com o empuxo do diametro externo, reproduz o peso submerso real
    #    (weight_wet_kNm) que ja validamos contra a documentacao do curso.
    #    Sem essa correcao, a linha fica com peso submerso NEGATIVO (boiante)
    #    e o solver encontra uma forma sem sentido fisico (linha flutuando
    #    contra a superficie em vez de descansar no leito marinho).
    buoyant_mass_equiv = (math.pi / 4.0) * d_vol ** 2 * rho_water
    mass_for_moorpy = w_wet_N_m / g + buoyant_mass_equiv

    ms = mp.System(depth=water_depth, rho=rho_water, g=g)
    ms.setLineType(
        name="riser",
        dnommm=d_vol * 1000.0,
        d_vol=d_vol,
        w=w_wet_N_m,
        EA=EA,
        mass=mass_for_moorpy,
    )

    z_anchor_moorpy = anchor_xyz[2] - water_depth
    z_top_moorpy = top_xyz[2] - water_depth

    pA = ms.addPoint(1, [anchor_xyz[0], anchor_xyz[1], z_anchor_moorpy])
    pB = ms.addPoint(1, [top_xyz[0], top_xyz[1], z_top_moorpy])

    line = ms.addLine(L_unstretched, "riser", pointA=pA.number, pointB=pB.number, cb=friction_coeff)

    metadata = {
        "anchor_node_id": anchor_id,
        "top_node_id": top_id,
        "anchor_xyz_json": anchor_xyz,
        "top_xyz_json": top_xyz,
        "water_depth": water_depth,
        "L_unstretched": L_unstretched,
        "EA_N": EA,
        "weight_wet_N_m": w_wet_N_m,
        "d_outer_m": d_vol,
        "rho_water": rho_water,
        "friction_coeff": friction_coeff,
        "mass_for_moorpy_kg_m": mass_for_moorpy,
        "top_xyz_override_used": list(top_xyz_override) if top_xyz_override is not None else None,
    }

    return ms, line, metadata


if __name__ == "__main__":
    import sys

    json_path = sys.argv[1] if len(sys.argv) > 1 else "risersim_results/input_simulation.json"
    ms, line, meta = build_system_from_json(json_path)
    print("Modelo MoorPy montado a partir de:", json_path)
    for k, v in meta.items():
        print(f"  {k}: {v}")
