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
        nodes_dataset = f['Groups/group1/Nodes']
        print("Dataset Type:", nodes_dataset.dtype)
        print("First 5 Nodes:")
        for idx in range(5):
            node_data = nodes_dataset[idx]
            print(f"Node {idx}: Label={node_data[0].decode('utf-8').strip()}, Coords=({node_data[1]:.4f}, {node_data[2]:.4f}, {node_data[3]:.4f})")
