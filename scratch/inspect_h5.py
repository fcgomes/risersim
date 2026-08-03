import h5py
import os

possible_paths = [
    r"exemplos/Curso/Exemplo_01/Exemplo_01a/Exemplo_01a_analysis/Exemplo_01a_A1.h5",
    r"/workspace/exemplos/Curso/Exemplo_01/Exemplo_01a/Exemplo_01a_analysis/Exemplo_01a_A1.h5"
]

h5_path = None
for p in possible_paths:
    if os.path.exists(p):
        h5_path = p
        break

if h5_path:
    with h5py.File(h5_path, 'r') as f:
        print("--- Groups in Root ---")
        for key in f.keys():
            print(f"Group: {key}")
            # Se for um grupo, mostra o que tem dentro (nível 1)
            if isinstance(f[key], h5py.Group):
                for subkey in f[key].keys():
                    item = f[key][subkey]
                    if isinstance(item, h5py.Dataset):
                        print(f"  Dataset: {subkey} | Shape: {item.shape} | Type: {item.dtype}")
                    else:
                        print(f"  Subgroup: {subkey}")
