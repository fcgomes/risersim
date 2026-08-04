"""
Leitura do resultado real do ANFLEX para o Exemplo_01a (arquivo de
resultados do curso), usado tanto para montar um caso "topo exato" no
MoorPy quanto para comparar resultados.
"""
import os

import h5py
import numpy as np


DEFAULT_COURSE_DIR = "exemplos/Curso/Exemplo_01/Exemplo_01a/Exemplo_01a_analysis"
RESULTS_H5_NAME = "Exemplo_01a_A1_Cross_group1_results_static.h5"


def load_anflex_reference(course_dir=DEFAULT_COURSE_DIR):
    """Le a geometria de referencia + deslocamentos do ultimo passo estatico
    do resultado real do ANFLEX e retorna as posicoes absolutas finais dos
    501 nos (geo + deformed), que e o que o ANFLEX de fato convergiu para
    o Exemplo_01a.
    """
    h5_path = os.path.join(course_dir, RESULTS_H5_NAME)
    with h5py.File(h5_path, "r") as f:
        geo = f["group1/geometry/nodes"][:]
        step_keys = sorted(k for k in f["group1/deformed"].keys() if k.startswith("step_"))
        last_step = step_keys[-1]
        disp = f[f"group1/deformed/{last_step}"][:]

    abs_x = geo["X"] + disp["X"]
    abs_y = geo["Y"] + disp["Y"]
    abs_z = geo["Z"] + disp["Z"]

    # SL (arc length ao longo da linha) vem com um valor sentinela invalido
    # em alguns exports do ANFLEX (ex.: -6.277e+66); nesse caso recalculamos
    # o comprimento de arco acumulado a partir das proprias posicoes X,Y,Z.
    sl = disp["SL"].astype(float)
    if np.any(sl < -1e30) or np.any(~np.isfinite(sl)):
        d = np.sqrt(np.diff(abs_x) ** 2 + np.diff(abs_y) ** 2 + np.diff(abs_z) ** 2)
        sl = np.concatenate([[0.0], np.cumsum(d)])

    return {
        "last_step": last_step,
        "num_nodes": len(geo),
        "ref_geometry": {"X": geo["X"], "Y": geo["Y"], "Z": geo["Z"]},
        "absolute": {"X": abs_x, "Y": abs_y, "Z": abs_z},
        "arc_length": sl,
    }


def anflex_top_xyz(course_dir=DEFAULT_COURSE_DIR):
    """Posicao absoluta final do no do topo (ultimo no da lista, 'C1'),
    ja incluindo o offset estatico da FPSO aplicado nesse caso de carga.
    """
    ref = load_anflex_reference(course_dir)
    return (
        float(ref["absolute"]["X"][-1]),
        float(ref["absolute"]["Y"][-1]),
        float(ref["absolute"]["Z"][-1]),
    )
