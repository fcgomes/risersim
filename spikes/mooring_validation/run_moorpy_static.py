"""
Roda o equilibrio quasi-estatico do MoorPy para o modelo montado a partir do
JSON do risersim, e exporta o resultado (convergiu?, tracao, perfil da
linha) para comparacao com o risersim e com o resultado real do ANFLEX.

Uso:
    python run_moorpy_static.py [input_simulation.json] [saida.json] [--exact-top]

--exact-top: fixa o topo na posicao absoluta final real do ANFLEX (lida do
resultado do curso, incluindo o offset estatico da FPSO), em vez da
geometria de referencia crua do H5 -- permite uma comparacao no-a-no exata
com compare_results.py.
"""
import json
import sys

from build_from_risersim_json import build_system_from_json
from anflex_reference import anflex_top_xyz


def run(json_path, output_path, use_exact_top=False):
    top_override = anflex_top_xyz() if use_exact_top else None
    ms, line, meta = build_system_from_json(json_path, top_xyz_override=top_override)

    ms.initialize()

    converged = True
    error_message = None
    try:
        ms.solveEquilibrium(DOFtype="free", tol=0.05, maxIter=500)
    except Exception as exc:  # noqa: BLE001 - spike: queremos capturar qualquer falha do solver
        converged = False
        error_message = str(exc)

    result = {
        "converged": converged,
        "error_message": error_message,
        "metadata": meta,
    }

    if converged:
        Xs, Ys, Zs, Ts = line.getLineCoords(0.0)
        # O MoorPy trabalha no referencial "z=0 no nivel do mar, seabed em
        # -water_depth". O JSON do risersim (e o resultado do ANFLEX) usa
        # "z=0 no leito marinho, cresce para cima". Converte de volta aqui
        # para o perfil exportado ficar no mesmo referencial de todo o
        # resto (comparacao em compare_results.py depende disso).
        water_depth = meta["water_depth"]
        Zs_json_frame = [z + water_depth for z in Zs]
        result["profile"] = {
            "X": list(Xs),
            "Y": list(Ys),
            "Z": Zs_json_frame,
            "tension_N": list(Ts),
        }
        result["tension_anchor_N"] = float(Ts[0])
        result["tension_top_N"] = float(Ts[-1])
        result["tension_top_kN"] = float(Ts[-1]) / 1000.0

    with open(output_path, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=2)

    print("=" * 60)
    print("  MoorPy - validacao de equilibrio quasi-estatico")
    print("=" * 60)
    print(f"Entrada:  {json_path}")
    print(f"Saida:    {output_path}")
    print(f"Convergiu: {converged}")
    if not converged:
        print(f"Erro: {error_message}")
    else:
        print(f"Tracao na ancora (fundo): {result['tension_anchor_N'] / 1000.0:.2f} kN")
        print(f"Tracao no topo:           {result['tension_top_kN']:.2f} kN")
        print(f"Peso total submerso da linha: {meta['weight_wet_N_m'] * meta['L_unstretched'] / 1000.0:.2f} kN")

    return result


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    exact_top = "--exact-top" in sys.argv

    json_in = args[0] if len(args) > 0 else "risersim_results/input_simulation.json"
    default_out = (
        "risersim/spikes/mooring_validation/results/moorpy_result_exact_top.json"
        if exact_top
        else "risersim/spikes/mooring_validation/results/moorpy_result.json"
    )
    json_out = args[1] if len(args) > 1 else default_out

    import os
    os.makedirs(os.path.dirname(json_out), exist_ok=True)

    run(json_in, json_out, use_exact_top=exact_top)
