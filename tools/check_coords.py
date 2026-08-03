import json

with open('/anflex/risersim/risersim_results/catenary_results.json') as f:
    data = json.load(f)

steps = data.get('static_steps', [])
print("Número de passos estáticos:", len(steps))
for idx, s in enumerate(steps):
    coords = s.get('nodes', [])
    print(f"Passo {idx}: index={s.get('step_index')}, load={s.get('load_factor')}, num_coords={len(coords)}")
    if coords:
        c0 = coords[0]
        c_mid = coords[len(coords)//2]
        c_end = coords[-1]
        print(f"  Topo (Nó 1): Z = {c0['z']:.2f} m")
        print(f"  Meio (Nó {len(coords)//2}): X = {c_mid['x']:.2f} m, Z = {c_mid['z']:.2f} m")
        print(f"  Âncora (Nó {len(coords)}): X = {c_end['x']:.2f} m, Z = {c_end['z']:.2f} m")

