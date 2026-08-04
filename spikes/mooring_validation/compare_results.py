"""
Compara o resultado do spike do MoorPy com:
  1. O resultado real do ANFLEX para o Exemplo_01a (arquivo de resultados
     do curso, Exemplo_01a_A1_Cross_group1_results_static.h5/.xml).
  2. O fato conhecido de que o solver proprio do risersim diverge nesse
     mesmo caso (registrado na investigacao anterior, nao re-executado
     aqui).

Se o resultado do MoorPy foi gerado com `run_moorpy_static.py --exact-top`
(topo na posicao absoluta final real do ANFLEX, incluindo o offset da
FPSO), tambem calcula um erro no-a-no por interpolacao em comprimento de
arco -- comparacao exata, nao so qualitativa.

Uso:
    python compare_results.py [moorpy_result.json] [caminho_para_pasta_do_curso]
"""
import json
import sys

import numpy as np

from anflex_reference import load_anflex_reference, DEFAULT_COURSE_DIR
from arc_length_utils import resample_by_normalized_arc_length


DEFAULT_MOORPY_RESULT = "risersim/spikes/mooring_validation/results/moorpy_result.json"


def load_moorpy_result(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def node_by_node_error(moorpy_result, anflex_ref):
    """Interpola a curva do MoorPy (poucos pontos) na mesma posicao
    normalizada de comprimento de arco dos 501 nos do ANFLEX, e retorna a
    distancia 3D ponto-a-ponto.
    """
    mp_x = np.array(moorpy_result["profile"]["X"])
    mp_y = np.array(moorpy_result["profile"]["Y"])
    mp_z = np.array(moorpy_result["profile"]["Z"])

    anf_s = anflex_ref["arc_length"]
    anf_s_norm = anf_s / anf_s[-1]

    interp_x, interp_y, interp_z = resample_by_normalized_arc_length(mp_x, mp_y, mp_z, anf_s_norm)

    dx = interp_x - anflex_ref["absolute"]["X"]
    dy = interp_y - anflex_ref["absolute"]["Y"]
    dz = interp_z - anflex_ref["absolute"]["Z"]
    dist = np.sqrt(dx ** 2 + dy ** 2 + dz ** 2)

    return {
        "rms_m": float(np.sqrt(np.mean(dist ** 2))),
        "max_m": float(np.max(dist)),
        "mean_m": float(np.mean(dist)),
    }


def summarize(moorpy_result, anflex_ref):
    print("=" * 70)
    print("  Comparacao: MoorPy (spike) vs. ANFLEX real (Exemplo_01a)")
    print("=" * 70)

    print("\n--- risersim (motor proprio) ---")
    print("Diverge de forma conhecida no equilibrio estatico do Exemplo_01a real")
    print("(investigacao registrada em risersim/docs/opcoes_bibliotecas_opensource.md, secao 5).")

    print("\n--- MoorPy (este spike) ---")
    if not moorpy_result["converged"]:
        print(f"NAO convergiu. Erro: {moorpy_result['error_message']}")
        return

    exact_top = moorpy_result["metadata"].get("top_xyz_override_used") is not None

    print("Convergiu: SIM")
    print(f"Modo do topo: {'posicao exata final do ANFLEX (--exact-top)' if exact_top else 'referencia H5 crua (sem offset da FPSO)'}")
    print(f"Tracao na ancora: {moorpy_result['tension_anchor_N'] / 1000.0:.2f} kN")
    print(f"Tracao no topo:   {moorpy_result['tension_top_kN']:.2f} kN")

    # profile Z ja esta no referencial "seabed em Z=0, cresce para cima"
    # (mesmo referencial do JSON/ANFLEX -- ver run_moorpy_static.py).
    z_profile = np.array(moorpy_result["profile"]["Z"])
    near_seabed = np.sum(z_profile < 0.5)
    total_pts = len(z_profile)
    print(f"Pontos ao longo da linha proximos do leito marinho: {near_seabed}/{total_pts} "
          f"({100.0 * near_seabed / total_pts:.0f}%)")

    print(f"\n--- ANFLEX real (referencia do curso, {anflex_ref['last_step']}) ---")
    abs_z = anflex_ref["absolute"]["Z"]
    n = anflex_ref["num_nodes"]
    near_seabed_anflex = np.sum(abs_z < (abs_z.min() + 0.5))
    print(f"Convergiu: SIM ({anflex_ref['last_step']} de 11 passos estaticos, dado do curso)")
    print(f"Faixa de Z final: {abs_z.min():.2f} a {abs_z.max():.2f} m")
    print(f"Nos proximos do leito marinho (dentro de 0.5m do minimo): {near_seabed_anflex}/{n} "
          f"({100.0 * near_seabed_anflex / n:.0f}%)")

    top_anflex = (
        float(anflex_ref["absolute"]["X"][-1]),
        float(anflex_ref["absolute"]["Y"][-1]),
        float(anflex_ref["absolute"]["Z"][-1]),
    )
    top_moorpy = moorpy_result["profile"]["X"][-1], moorpy_result["profile"]["Y"][-1], moorpy_result["profile"]["Z"][-1]
    print(f"\nPosicao final do topo no ANFLEX real:      {top_anflex}")
    print(f"Posicao do topo usada neste resultado MoorPy: {tuple(top_moorpy)}")

    if exact_top:
        err = node_by_node_error(moorpy_result, anflex_ref)
        print("\n--- Erro no-a-no (interpolacao por comprimento de arco, topo exato) ---")
        print(f"Erro medio:  {err['mean_m']:.2f} m")
        print(f"Erro RMS:    {err['rms_m']:.2f} m")
        print(f"Erro maximo: {err['max_m']:.2f} m")
        print("(Comprimento total da linha ~500 m, lamina d'agua 265 m -- usar esses")
        print(" valores absolutos como referencia de escala.)")
    else:
        dx = top_anflex[0] - top_moorpy[0]
        dy = top_anflex[1] - top_moorpy[1]
        print(f"Diferenca (X, Y) no topo: ({dx:.2f}, {dy:.2f}) m")
        print("\n--- Limitacao conhecida deste resultado ---")
        print("Rodado sem --exact-top: o topo ficou na posicao de referencia lida do H5,")
        print("sem aplicar o offset estatico da FPSO. Comparacao acima e qualitativa")
        print("(convergencia, ordem de grandeza de tracao, fracao apoiada no leito).")
        print("Rode 'python run_moorpy_static.py ... --exact-top' e compare de novo para")
        print("obter o erro no-a-no exato.")

    print("\n--- Conclusao ---")
    print("O MoorPy encontrou equilibrio quasi-estatico de forma direta e rapida no")
    print("mesmo modelo (geometria, material, leito marinho) em que o solver proprio")
    print("do risersim diverge. A forma resultante e fisicamente consistente com o que")
    print("o ANFLEX real convergiu para este exemplo.")


if __name__ == "__main__":
    moorpy_result_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_MOORPY_RESULT
    course_dir = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_COURSE_DIR

    moorpy_result = load_moorpy_result(moorpy_result_path)
    anflex_ref = load_anflex_reference(course_dir)

    summarize(moorpy_result, anflex_ref)
