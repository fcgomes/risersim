"""Utilitarios de comprimento de arco compartilhados entre os scripts do
spike de validacao MoorPy (build/run/compare) e a ferramenta de warm-start
em risersim/tools/moorpy_warm_start.py.
"""
import numpy as np


def cumulative_arc_length(X, Y, Z):
    """Comprimento de arco acumulado (a partir do primeiro ponto) ao longo
    de uma polilinha 3D definida por arrays X, Y, Z."""
    d = np.sqrt(np.diff(X) ** 2 + np.diff(Y) ** 2 + np.diff(Z) ** 2)
    return np.concatenate([[0.0], np.cumsum(d)])


def resample_by_normalized_arc_length(X, Y, Z, target_s_norm):
    """Reamostra a polilinha (X, Y, Z) nas posicoes normalizadas de
    comprimento de arco dadas em `target_s_norm` (0..1), por interpolacao
    linear. Retorna (X_new, Y_new, Z_new) com o mesmo tamanho de
    `target_s_norm`.
    """
    s = cumulative_arc_length(X, Y, Z)
    s_norm = s / s[-1]
    x_new = np.interp(target_s_norm, s_norm, X)
    y_new = np.interp(target_s_norm, s_norm, Y)
    z_new = np.interp(target_s_norm, s_norm, Z)
    return x_new, y_new, z_new
