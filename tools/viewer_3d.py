import json
import sys
import os

def visualize_riser_3d(json_path="catenary_results.json"):
    """Visualizador Tridimensional 3D Desacoplado do riserSim"""
    if not os.path.exists(json_path):
        print(f"❌ Arquivo de resultados não encontrado: {json_path}")
        return

    with open(json_path, "r") as f:
        data = json.load(f)

    if "steps" in data and len(data["steps"]) > 0:
        latest_step = data["steps"][-1]
        nodes = latest_step["nodes"]
        elements = latest_step["elements"]
    else:
        nodes = data["nodes"]
        elements = data["elements"]

    x_coords = [n["x"] for n in nodes]
    y_coords = [n["y"] for n in nodes]
    z_coords = [n["z"] for n in nodes]
    tensions = [e["tension_effective_kN"] for e in elements]

    print("=========================================================")
    print("  🌊 riserSim Separate 3D Visualizer                    ")
    print("=========================================================")
    print(f"✅ Nós carregados: {len(nodes)}")
    print(f"✅ Elementos de Viga: {len(elements)}")
    print(f"📈 Tração no Topo: {tensions[0]:.2f} kN")
    print(f"📉 Tração no Fundo: {tensions[-1]:.2f} kN")
    print("=========================================================")

    try:
        import pyvista as pv
        import numpy as np

        points = np.column_stack((x_coords, y_coords, z_coords))
        n_points = len(points)
        lines = np.hstack(([n_points], np.arange(n_points)))

        grid = pv.PolyData(points, lines=lines)
        node_tensions = [tensions[0]] + tensions
        grid.point_data["Tensão Efetiva (kN)"] = node_tensions

        tube = grid.tube(radius=0.5)

        plotter = pv.Plotter(title="riserSim 3D Catenary Riser Visualization")
        plotter.add_mesh(tube, cmap="plasma", scalar_bar_args={"title": "Tração Efetiva (kN)"})
        plotter.add_axes()
        plotter.show_grid()
        plotter.set_background("slategray")

        seabed_z = min(z_coords) - 1.0
        seabed = pv.Plane(center=(50, 0, seabed_z), direction=(0, 0, 1), i_size=150, j_size=50)
        plotter.add_mesh(seabed, color="sandybrown", opacity=0.6)

        plotter.show()

    except ImportError:
        try:
            import matplotlib.pyplot as plt

            fig = plt.figure(figsize=(10, 7))
            ax = fig.add_subplot(111, projection='3d')

            scatter = ax.scatter(x_coords, y_coords, z_coords, c=[tensions[0]] + tensions, cmap='plasma', s=30)
            ax.plot(x_coords, y_coords, z_coords, color='navy', linewidth=2, label='Riser Catenary')

            fig.colorbar(scatter, ax=ax, label='Tração Efetiva (kN)')
            ax.set_xlabel('X (m)')
            ax.set_ylabel('Y (m)')
            ax.set_zlabel('Z (m)')
            ax.set_title('riserSim 3D Catenary Riser Profile')
            plt.legend()
            plt.show()
        except ImportError:
            print("💡 Matplotlib/PyVista não encontrados no container.")
            print("💡 Iniciando servidor Web 3D HTML5 na porta 8080...")
            import http.server
            import socketserver

            PORT = 8080
            Handler = http.server.SimpleHTTPRequestHandler
            print(f"🌐 Abra seu navegador em: http://localhost:{PORT}/risersim/tools/viewer_3d.html")
            with socketserver.TCPServer(("", PORT), Handler) as httpd:
                httpd.serve_forever()

if __name__ == "__main__":
    file_path = sys.argv[1] if len(sys.argv) > 1 else "catenary_results.json"
    visualize_riser_3d(file_path)
