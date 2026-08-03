import os
import sys
import xml.etree.ElementTree as ET
import json
import math
import h5py

class ANFLEXXmlH5Reader:
    def __init__(self, xml_path, h5_path):
        self.xml_path = xml_path
        self.h5_path = h5_path
        self.tree = ET.parse(xml_path)
        self.root = self.tree.getroot()
        self.h5 = h5py.File(h5_path, 'r')
        
    def close(self):
        self.h5.close()

    def get_text(self, xpath, default=""):
        elem = self.root.find(xpath)
        if elem is not None and elem.text:
            return elem.text.strip().strip("'\"")
        return default

    def get_float(self, xpath, default=0.0):
        try:
            return float(self.get_text(xpath, str(default)))
        except ValueError:
            return default

    def get_int(self, xpath, default=0):
        try:
            return int(self.get_float(xpath, default))
        except ValueError:
            return default

    def extract_nodes(self):
        """Lê os nós diretamente do dataset HDF5 especificado no XML."""
        nodes = []
        # Buscando o dataset de nodes da malha principal
        nodes_elem = self.root.find(".//Groups/group1/Nodes")
        if nodes_elem is not None:
            # O nome padrão do dataset é Groups/group1/Nodes
            h5_dataset_path = "Groups/group1/Nodes"
            if h5_dataset_path in self.h5:
                ds = self.h5[h5_dataset_path]
                for idx in range(len(ds)):
                    row = ds[idx]
                    label = row[0].decode('utf-8').strip()
                    # Salva ID do nó (1-based index) e coordenadas reais
                    nodes.append({
                        "id": idx + 1,
                        "label": label,
                        "coords": [float(row[1]), float(row[2]), float(row[3])]
                    })
        return nodes

    def extract_elements(self, nodes):
        """Lê e constrói a conectividade dos elementos baseados nos segmentos do XML."""
        elements = []
        label_to_id = {node["label"]: node["id"] for node in nodes}
        
        # Procura as estruturas de linha
        lines_elem = self.root.find(".//Groups/group1/Elements")
        if lines_elem is not None:
            elem_id_counter = 1
            for line_child in lines_elem:
                line_name = line_child.tag
                # Lê os nós sequencialmente para mapear conectividade
                # No H5/XML, a lista de nós do grupo (Nodes) está na ordem exata da malha (catenária)
                for i in range(len(nodes) - 1):
                    elements.append({
                        "id": elem_id_counter,
                        "node1_id": nodes[i]["id"],
                        "node2_id": nodes[i+1]["id"],
                        "line_name": line_name
                    })
                    elem_id_counter += 1
        return elements

    def extract_material_properties(self):
        """Extrai as propriedades físicas e mecânicas da linha do XML."""
        # Encontra a tag de material flexível
        mat_elem = self.root.find(".//FlexibleLines")
        if mat_elem is None:
            # Fallback para procurar em qualquer lugar
            mat_elem = self.root.find(".//FlexibleLine")

        # Busca propriedades nos materiais flexíveis (RISE)
        # No XML estão tipicamente no ramo de materiais
        material_data = {}
        for mat in self.root.findall(".//FlexibleLine"):
            mat_id = mat.find("id")
            name = mat.tag
            
            di = float(mat.find("diameter_internal").text) if mat.find("diameter_internal") is not None else 0.2032
            do = float(mat.find("diameter_external").text) if mat.find("diameter_external") is not None else 0.2779
            dh = float(mat.find("diameter_hydrodynamic").text) if mat.find("diameter_hydrodynamic") is not None else do
            w_dry = float(mat.find("weight_dry").text) if mat.find("weight_dry") is not None else 1.0494
            w_wet = float(mat.find("weight_wet").text) if mat.find("weight_wet") is not None else 0.4395
            
            ea = float(mat.find("stiffness_axial").text) if mat.find("stiffness_axial") is not None else 3.6e5
            ei = float(mat.find("stiffness_bending").text) if mat.find("stiffness_bending") is not None else 21.7
            gj = float(mat.find("stiffness_torsional").text) if mat.find("stiffness_torsional") is not None else 3200.0
            
            cm = float(mat.find("morison_inertia").text) if mat.find("morison_inertia") is not None else 2.0
            cd = float(mat.find("morison_drag").text) if mat.find("morison_drag") is not None else 1.0
            cd_long = float(mat.find("morison_drag_longitudinal").text) if mat.find("morison_drag_longitudinal") is not None else 0.0

            # Conversão para SI
            ea_N = ea * 1000.0
            ei_Nm2 = ei * 1000.0
            gj_Nm2 = gj * 1000.0
            area_struct = (math.pi * (do**2 - di**2) / 4.0)
            
            # Estimativa de massa e E/G equivalente
            dry_mass_kgm = w_dry * 1000.0 / 9.81
            E = ea_N / area_struct if area_struct > 0 else 2.1e11
            G = gj_Nm2 / (2.0 * (E * (math.pi * (do**4 - di**4) / 64.0))) if area_struct > 0 else 8.0e10
            
            material_data = {
                "name": name,
                "inner_diameter_m": di,
                "outer_diameter_m": do,
                "hydro_diameter_m": dh,
                "weight_dry_kNm": w_dry,
                "weight_wet_kNm": w_wet,
                "dry_mass_kgm": dry_mass_kgm,
                "EA_N": ea_N,
                "EI_Nm2": ei_Nm2,
                "GJ_Nm2": gj_Nm2,
                "E_Pa": E,
                "G_Pa": G,
                "A_m2": area_struct,
                "Cd": cd,
                "Cm": cm,
                "Cd_longitudinal": cd_long
            }
            break # Pega o primeiro material disponível
            
        if not material_data:
            # Fallback para propriedades padrão do RISE
            material_data = {
                "name": "RISE",
                "inner_diameter_m": 0.2032,
                "outer_diameter_m": 0.2779,
                "hydro_diameter_m": 0.2779,
                "weight_dry_kNm": 1.0494,
                "weight_wet_kNm": 0.4395,
                "dry_mass_kgm": 1.0494 * 1000.0 / 9.81,
                "EA_N": 3.6e8,
                "EI_Nm2": 21700.0,
                "GJ_Nm2": 3200000.0,
                "E_Pa": 12.75e9,
                "G_Pa": 7.65e9,
                "A_m2": 0.0282,
                "Cd": 1.0,
                "Cm": 2.0,
                "Cd_longitudinal": 0.0
            }
        return material_data

    def to_risersim_json(self):
        """Converte a modelagem XML+H5 para o novo formato JSON estruturado."""
        title = self.get_text("ModelName", "ANFLEX XML Simulation")
        
        # 1. Nós e elementos
        nodes = self.extract_nodes()
        elements = self.extract_elements(nodes)
        material = self.extract_material_properties()
        
        # Associa as propriedades do material aos elementos
        for elem in elements:
            elem["section_properties"] = {
                "E": material["E_Pa"],
                "G": material["G_Pa"],
                "A": material["A_m2"],
                "IY": material["EI_Nm2"] / material["E_Pa"] if material["E_Pa"] > 0 else 5e-5,
                "IZ": material["EI_Nm2"] / material["E_Pa"] if material["E_Pa"] > 0 else 5e-5,
                "J": material["GJ_Nm2"] / material["G_Pa"] if material["G_Pa"] > 0 else 1e-4,
                "EI": material["EI_Nm2"],
                "rho": material["dry_mass_kgm"] / material["A_m2"] if material["A_m2"] > 0 else 7850.0,
                "D_outer": material["outer_diameter_m"],
                "D_inner": material["inner_diameter_m"],
                "rho_fluid": 1025.0, # Fluido interno padrão
                "Ca": material["Cm"] - 1.0
            }

        # 2. Condições de Contorno
        # Nós de topo e fundo são identificados por restrições no XML
        prescribed_nodes = []
        restrained_nodes = []
        
        # FPSO C1 (geralmente nó 1) tem movimentos prescritos
        prescribed_nodes.append({
            "node_id": 1,
            "dofs": [-1, -1, -1, -1, -1, -1] # Fixos / controlados
        })
        # Âncora de fundo (geralmente nó final)
        restrained_nodes.append({
            "node_id": len(nodes),
            "dofs": [-1, -1, -1, -1, -1, -1]
        })
        
        # 3. Solo
        soil_stiff_kNm = self.get_float(".//Soils/Solo/spring/stiffness", 800.0)
        friction_lat = self.get_float(".//Soils/Solo/friction/lateral", 0.95)
        seabed_depth = self.get_float(".//GlobalData/seabed/depth", 265.0)
        
        # 4. Caso de Análise Estática / Dinâmica
        static_total_time = self.get_float(".//AnalysisData/Static/total_time", 11.0)
        static_time_step = self.get_float(".//AnalysisData/Static/time_step", 1.0)
        static_max_iter = self.get_int(".//AnalysisData/Static/max_num_iteration", 40)
        static_tol = self.get_float(".//AnalysisData/Static/tolerance", 1.0e-3)
        
        dyn_total_time = self.get_float(".//AnalysisData/Dynamic/total_time", 1.0)
        dyn_time_step = self.get_float(".//AnalysisData/Dynamic/time_step", 0.05)
        dyn_max_iter = self.get_int(".//AnalysisData/Dynamic/max_num_iteration", 20)
        dyn_tol = self.get_float(".//AnalysisData/Dynamic/tolerance", 1.0e-3)
        
        static_steps = max(1, int(static_total_time / (static_time_step if static_time_step > 0 else 1.0)))

        # 5. Onda (JONSWAP)
        wave_height = self.get_float(".//Waves/ON/height", 6.3)
        wave_period = self.get_float(".//Waves/ON/period", 10.0)
        wave_gamma = self.get_float(".//Waves/ON/gamma", 2.07)
        wave_angle = self.get_float(".//Waves/ON/angle", 0.0)

        # 6. Corrente
        # Fallback de perfil de corrente
        curr_depths = [0.0, seabed_depth]
        curr_vels = [1.02, 0.27]
        curr_angles = [90.0, 90.0]

        # Montagem do JSON estruturado
        data = {
            "title": title,
            "model": {
                "nodes": [{"id": n["id"], "coords": n["coords"]} for n in nodes],
                "elements": elements
            },
            "boundary_conditions": {
                "prescribed_dofs": prescribed_nodes,
                "restrained_dofs": restrained_nodes
            },
            "environmental": {
                "seabed": {
                    "depth_m": -abs(seabed_depth),
                    "stiffness_Nm": soil_stiff_kNm * 1000.0,
                    "friction_coeff": friction_lat
                },
                "current": {
                    "depths_m": curr_depths,
                    "velocities_ms": curr_vels,
                    "angles_deg": curr_angles
                },
                "wave": {
                    "type": "JONSWAP",
                    "period_s": wave_period,
                    "height_m": wave_height,
                    "amplitude_m": wave_height / 2.0,
                    "gamma": wave_gamma,
                    "angle_deg": wave_angle
                }
            },
            "analysis_options": {
                "static": {
                    "steps": static_steps,
                    "max_iterations": static_max_iter,
                    "tolerance": static_tol,
                    "vessel_offset": {
                        "near_m": 0.0, # Sempre nominal zero para catenária pura
                        "far_m": 0.0
                    }
                },
                "dynamic": {
                    "enabled": True,
                    "duration_s": dyn_total_time,
                    "dt_s": dyn_time_step,
                    "max_iterations": dyn_max_iter,
                    "tolerance": dyn_tol,
                    "rayleigh_damping": {
                        "alpha": 0.05,
                        "beta": 0.005
                    }
                }
            }
        }
        return data

# Para rodar direto do terminal e exportar o JSON estruturado
if __name__ == "__main__":
    if len(sys.argv) < 3:
        # Defaults para teste
        xml_in = r"exemplos/Curso/Exemplo_01/Exemplo_01a/Exemplo_01a_analysis/Exemplo_01a_A1.xml"
        h5_in = r"exemplos/Curso/Exemplo_01/Exemplo_01a/Exemplo_01a_analysis/Exemplo_01a_A1.h5"
        json_out = r"risersim_results/input_simulation_structured.json"
    else:
        xml_in = sys.argv[1]
        h5_in = sys.argv[2]
        json_out = sys.argv[3] if len(sys.argv) > 3 else "input_simulation_structured.json"

    print(f"📖 Lendo XML: {xml_in}")
    print(f"📦 Lendo H5: {h5_in}")
    
    reader = ANFLEXXmlH5Reader(xml_in, h5_in)
    out_data = reader.to_risersim_json()
    reader.close()
    
    os.makedirs(os.path.dirname(json_out) if os.path.dirname(json_out) else ".", exist_ok=True)
    with open(json_out, 'w', encoding='utf-8') as f:
        json.dump(out_data, f, indent=2, ensure_ascii=False)
        
    print(f"✅ JSON estruturado gerado com sucesso: {json_out}")
