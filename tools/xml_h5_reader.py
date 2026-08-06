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
        # O nome do grupo sob <Groups> varia de caixa entre exportações do ANFLEX
        # (ex.: Exemplo_01a usa "group1", Exemplo_02a usa "Group1") -- descoberto
        # dinamicamente em vez de hardcoded, tanto no XML quanto no dataset HDF5
        # correspondente (a mesma inconsistência de caixa existe nos dois arquivos).
        # Um valor hardcoded errado falha silenciosamente (find() retorna None) e
        # produz um modelo com 0 nós/elementos, sem nenhum erro visível até o
        # binário C++ tentar processá-lo.
        self.group_name = self._detect_group_name()

    def _detect_group_name(self, default="group1"):
        groups_elem = self.root.find(".//Groups")
        if groups_elem is not None and len(groups_elem) > 0:
            return groups_elem[0].tag
        return default

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
        nodes_elem = self.root.find(f".//Groups/{self.group_name}/Nodes")
        if nodes_elem is not None:
            h5_dataset_path = f"Groups/{self.group_name}/Nodes"
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
        lines_elem = self.root.find(f".//Groups/{self.group_name}/Elements")
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
        """Extrai as propriedades físicas e mecânicas da linha do XML.

        O material real fica em Groups/<nome_do_grupo>/Materials/<NomeDoSegmento>
        (nome dinâmico por segmento, ex. "RISE_L1_seg001"), não em uma tag fixa
        "FlexibleLine". Os campos do ANFLEX usam unidades de engenharia
        (kN, kN/m², kN/m³), não SI puro — a conversão é feita explicitamente
        abaixo.
        """
        mat = None
        materials_group = self.root.find(f".//Groups/{self.group_name}/Materials")
        if materials_group is not None and len(materials_group) > 0:
            mat = materials_group[0]  # Pega o primeiro material do grupo

        material_data = {}
        if mat is not None:
            def mf(tag, default):
                el = mat.find(tag)
                try:
                    return float(el.text) if el is not None and el.text else default
                except ValueError:
                    return default

            di = mf("internal_diameter", 0.2032)
            do = mf("external_diameter", 0.2779)
            dh = mf("hidro_diameter", do)
            area = mf("area", math.pi * (do**2 - di**2) / 4.0)
            hidro_area = mf("hidrostatic_area", math.pi * do**2 / 4.0)

            density_kNm3 = mf("density", 37.18)                 # peso específico estrutural (kN/m3)
            ext_fluid_kNm3 = mf("external_fluid_density", 10.055)
            int_fluid_kNm3 = mf("internal_fluid_density", ext_fluid_kNm3)

            elasticity_kNm2 = mf("elasticity", 1.27543e7)        # E em kN/m2
            poisson = mf("poisson", 0.3)
            xi = mf("xi", 6.5233e-4)   # J  (m4)
            yi = mf("yi", 1.7014e-6)   # IY (m4)
            zi = mf("zi", yi)          # IZ (m4)

            cm = mf("inertia_coef", 2.0)
            cd = mf("drag_normal_coef", 1.0)
            cd_long = mf("drag_longitudinal_coef", 0.0)

            # Conversão para SI: ANFLEX usa kN / kN/m2 / kN/m3 internamente
            E = elasticity_kNm2 * 1000.0                # Pa
            G = E / (2.0 * (1.0 + poisson))              # Pa
            ea_N = E * area
            ei_Nm2 = E * yi
            gj_Nm2 = G * xi

            weight_dry_kNm = density_kNm3 * area
            # Peso submerso = peso seco - empuxo (peso específico da água externa * área hidrostática)
            weight_wet_kNm = weight_dry_kNm - ext_fluid_kNm3 * hidro_area

            material_data = {
                "name": mat.tag,
                "inner_diameter_m": di,
                "outer_diameter_m": do,
                "hydro_diameter_m": dh,
                "weight_dry_kNm": weight_dry_kNm,
                "weight_wet_kNm": weight_wet_kNm,
                "EA_N": ea_N,
                "EI_Nm2": ei_Nm2,
                "GJ_Nm2": gj_Nm2,
                "E_Pa": E,
                "G_Pa": G,
                "A_m2": area,
                "IY_m4": yi,
                "IZ_m4": zi,
                "J_m4": xi,
                "Cd": cd,
                "Cm": cm,
                "Cd_longitudinal": cd_long,
                "internal_fluid_density_kgm3": int_fluid_kNm3 * 1000.0 / 9.81,
            }

        if not material_data:
            # Fallback só quando o XML não tem nenhum material (não deveria ocorrer)
            material_data = {
                "name": "RISE",
                "inner_diameter_m": 0.2032,
                "outer_diameter_m": 0.2779,
                "hydro_diameter_m": 0.2779,
                "weight_dry_kNm": 1.0494,
                "weight_wet_kNm": 0.4395,
                "EA_N": 3.6e8,
                "EI_Nm2": 21700.0,
                "GJ_Nm2": 3200000.0,
                "E_Pa": 12.75e9,
                "G_Pa": 7.65e9,
                "A_m2": 0.0282,
                "IY_m4": 1.7014e-6,
                "IZ_m4": 1.7014e-6,
                "J_m4": 6.5233e-4,
                "Cd": 1.0,
                "Cm": 2.0,
                "Cd_longitudinal": 0.0,
                "internal_fluid_density_kgm3": 1025.0,
            }
        return material_data

    def _find_chosen_current(self):
        """Localiza o elemento Currents/<Nome> associado ao primeiro caso de
        carregamento (LoadingCases/<Caso>/current_id), com fallback pro primeiro
        caso de corrente do XML se não achar/não houver current_id. Compartilhado
        por extract_current_profile() e extract_current_ramp() -- mesmo caso de
        corrente alimenta tanto o perfil quanto a curva de rampa de carga.
        """
        current_id = None
        loading_cases = self.root.find(".//LoadingCases")
        if loading_cases is not None and len(loading_cases) > 0:
            first_case = loading_cases[0]
            cid_el = first_case.find("current_id")
            if cid_el is not None and cid_el.text:
                try:
                    current_id = int(float(cid_el.text))
                except ValueError:
                    current_id = None

        currents_root = self.root.find(".//Currents")
        chosen = None
        if currents_root is not None and len(currents_root) > 0:
            for child in currents_root:
                id_el = child.find("id")
                if id_el is not None and id_el.text and current_id is not None:
                    try:
                        if int(float(id_el.text)) == current_id:
                            chosen = child
                            break
                    except ValueError:
                        pass
            if chosen is None:
                chosen = currents_root[0]
        return chosen

    def extract_current_profile(self):
        """Lê o perfil de corrente real associado ao caso de carregamento do XML.

        O perfil fica em Currents/<Nome>/profile/values, como linhas
        "profundidade|ângulo|velocidade" (profundidade medida a partir do leito
        marinho, crescente em direção à superfície).
        """
        chosen = self._find_chosen_current()

        depths, angles, vels = [], [], []
        if chosen is not None:
            values_el = chosen.find("profile/values")
            if values_el is not None and values_el.text:
                for line in values_el.text.strip().splitlines():
                    parts = line.strip().split("|")
                    if len(parts) >= 3:
                        try:
                            depths.append(float(parts[0]))
                            angles.append(float(parts[1]))
                            vels.append(float(parts[2]))
                        except ValueError:
                            continue

        if not depths:
            # Fallback só quando o XML não tem nenhum perfil de corrente
            return [0.0, 265.0], [90.0, 90.0], [1.02, 0.27]

        # "Depth" no XML é medido a partir do leito marinho (0=fundo, crescente
        # até a superfície) — mesma convenção dos nós do modelo. main_test.cpp
        # só consome o índice [0] como valor de superfície, então inverte-se a
        # ordem para que o ponto mais raso (perto da superfície) fique primeiro.
        order = sorted(range(len(depths)), key=lambda i: depths[i], reverse=True)
        depths = [depths[i] for i in order]
        angles = [angles[i] for i in order]
        vels = [vels[i] for i in order]

        return depths, angles, vels

    def extract_current_ramp(self):
        """Lê a curva real de rampa de carga da corrente (Currents/<caso>/static_function_id
        -> Functions/<Tag>/id -> Functions/<Tag>/points no HDF5).

        No ANFLEX real, o peso próprio nunca é rampeado (m_has_gravitational_load nunca é
        atribuído no código-fonte -- fica sempre com magnitude total), mas a corrente é
        rampeada por uma curva própria e independente, tipicamente mantendo a corrente
        praticamente zerada no primeiro passo de carga e só crescendo gradualmente até o
        valor total no último passo -- ver mapa_classes_anflex_estatica.md. O eixo X é
        normalizado pelo seu próprio máximo (domínio vira fração [0,1] do total de passos
        daquela curva), pra poder ser reamostrado com qualquer número de passos de carga
        configurado no risersim, sem depender do static_steps bater exatamente com o X
        original do XML.

        Retorna (ramp_x, ramp_y); listas vazias se o XML não tiver static_function_id ou a
        função correspondente (fallback: risersim usa a mesma rampa do peso, comportamento
        anterior a esta mudança).
        """
        chosen = self._find_chosen_current()
        if chosen is None:
            return [], []

        func_id_el = chosen.find("static_function_id")
        if func_id_el is None or not func_id_el.text:
            return [], []
        try:
            func_id = int(float(func_id_el.text))
        except ValueError:
            return [], []

        # Filho direto da raiz -- só o catálogo real de funções (TempFunc/StaTfDef/DispFunc/...).
        # ".//Functions" pegaria o primeiro na ordem do documento, que na prática é outro elemento
        # de mesmo nome em LoadingCases/.../Loads/Functions (X/Y/Z/RX/RY/RZ, sem relação nenhuma
        # com static_function_id de corrente) -- confirmado lendo a árvore real do Exemplo_01a.
        functions_root = self.root.find("Functions")
        if functions_root is None:
            return [], []
        func_elem = None
        for child in functions_root:
            id_el = child.find("id")
            if id_el is not None and id_el.text:
                try:
                    if int(float(id_el.text)) == func_id:
                        func_elem = child
                        break
                except ValueError:
                    pass
        if func_elem is None:
            return [], []

        dataset_path = f"Functions/{func_elem.tag}/points"
        if dataset_path not in self.h5:
            return [], []
        ds = self.h5[dataset_path]
        xs = [float(row[0]) for row in ds]
        ys = [float(row[1]) for row in ds]
        if not xs or max(xs) <= 0.0:
            return [], []

        x_max = max(xs)
        ramp_x = [x / x_max for x in xs]
        return ramp_x, ys

    def to_risersim_json(self):
        """Converte a modelagem XML+H5 para o novo formato JSON estruturado."""
        title = self.get_text("ModelName", "ANFLEX XML Simulation")
        
        # 1. Nós e elementos
        nodes = self.extract_nodes()
        elements = self.extract_elements(nodes)
        material = self.extract_material_properties()
        
        # Associa as propriedades do material aos elementos
        weight_wet_kNm = material["weight_wet_kNm"]
        rho_equivalent = (weight_wet_kNm * 1000.0 / 9.81) / material["A_m2"] if material["A_m2"] > 0 else 3793.35
        for elem in elements:
            elem["section_properties"] = {
                "E": material["E_Pa"],
                "G": material["G_Pa"],
                "A": material["A_m2"],
                "IY": material.get("IY_m4", material["EI_Nm2"] / material["E_Pa"] if material["E_Pa"] > 0 else 5e-5),
                "IZ": material.get("IZ_m4", material["EI_Nm2"] / material["E_Pa"] if material["E_Pa"] > 0 else 5e-5),
                "J": material.get("J_m4", material["GJ_Nm2"] / material["G_Pa"] if material["G_Pa"] > 0 else 1e-4),
                "EI": material["EI_Nm2"],
                "weight_wet_kNm": weight_wet_kNm,
                "rho": rho_equivalent,
                "D_outer": material["outer_diameter_m"],
                "D_inner": material["inner_diameter_m"],
                "rho_fluid": material.get("internal_fluid_density_kgm3", 1025.0),
                "Ca": material["Cm"] - 1.0,
                "Cd": material.get("Cd", 1.0),
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
        
        # 3. Solo (Soils/Solo/vertical_stiffness e lateral_friction são campos diretos,
        # não aninhados sob "spring"/"friction" como no XPath antigo)
        soil_stiff_kNm = self.get_float(".//Soils/Solo/vertical_stiffness", 800.0)
        friction_lat = self.get_float(".//Soils/Solo/lateral_friction", 0.95)
        # GlobalData/seabed é um escalar de referência (não um branch com "depth");
        # a lâmina d'água real é GlobalData/seawater_level
        seabed_depth = self.get_float(".//GlobalData/seawater_level", 265.0)

        # 4. Caso de Análise Estática / Dinâmica
        # (num_max_iter, não max_num_iteration; x_tol é a tolerância relativa de
        # deslocamento, compatível com o critério rel_R < tolerance do solver)
        static_total_time = self.get_float(".//AnalysisData/Static/total_time", 11.0)
        static_time_step = self.get_float(".//AnalysisData/Static/time_step", 1.0)
        static_max_iter = self.get_int(".//AnalysisData/Static/num_max_iter", 40)
        static_tol = self.get_float(".//AnalysisData/Static/x_tol", 1.0e-3)

        dyn_total_time = self.get_float(".//AnalysisData/Dynamic/total_time", 1.0)
        dyn_time_step = self.get_float(".//AnalysisData/Dynamic/time_step", 0.05)
        dyn_max_iter = self.get_int(".//AnalysisData/Dynamic/num_max_iter", 20)
        dyn_tol = self.get_float(".//AnalysisData/Dynamic/x_tol", 1.0e-3)

        static_steps = max(1, int(static_total_time / (static_time_step if static_time_step > 0 else 1.0)))

        # 5. Onda (JONSWAP)
        wave_height = self.get_float(".//Waves/ON/height", 6.3)
        wave_period = self.get_float(".//Waves/ON/period", 10.0)
        wave_gamma = self.get_float(".//Waves/ON/gamma", 2.07)
        wave_angle = self.get_float(".//Waves/ON/angle", 0.0)

        # 6. Corrente (perfil real lido de Currents/<id>/profile, associado ao
        # current_id do primeiro caso de carregamento)
        curr_depths, curr_angles, curr_vels = self.extract_current_profile()
        curr_ramp_x, curr_ramp_y = self.extract_current_ramp()

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
                    "angles_deg": curr_angles,
                    "ramp_x": curr_ramp_x,
                    "ramp_y": curr_ramp_y
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
