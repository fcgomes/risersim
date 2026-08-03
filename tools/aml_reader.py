import os
import sys
import json
import math

class ANFLEXAMLReader:
    """
    ANFLEX AML Reader & Model Extractor (v2)
    =========================================
    Parseia arquivos de entrada .aml do ANFLEX e converte em configurações
    físicas prontas para o riserSim.

    Formato AML:
      - Tags iniciam com '%'
      - Linhas sem '%' são dados da tag anterior
      - Primeira linha de dados após a tag é a 1ª linha de valor
      - Correntes: 1ª linha de dados = N, seguido de N valores (um por linha)
      - Seção %LINE.SEGMENT.MESH: 1ª linha = N_segmentos,
        próximas N linhas = "length factor_start factor_end"

    Unidades nativas do AML:
      - Comprimentos: metros
      - Pesos/forças: kN
      - Rigidez EA: kN
      - Rigidez EI: kN.m²
      - Rigidez GJ: kN.m²
      - Velocidade: m/s
      - Ângulos: graus
    """

    def __init__(self, aml_filepath):
        self.filepath = aml_filepath
        # raw_sections: dict[tag_str] -> list[str] (linhas de dados)
        self.raw_sections = {}
        # Para tags que aparecem múltiplas vezes (ex: %CURRENT), lista de ocorrências
        self.raw_sections_multi = {}
        self.model_data = {}
        self._parse_file()
        self._extract_all()

    # -------------------------------------------------------------------------
    # PARSING BRUTO
    # -------------------------------------------------------------------------
    def _parse_file(self):
        if not os.path.exists(self.filepath):
            raise FileNotFoundError(f"Arquivo AML não encontrado: {self.filepath}")

        with open(self.filepath, 'r', encoding='latin-1', errors='ignore') as f:
            lines = [line.rstrip('\r\n') for line in f]

        current_tag = None
        tag_lines = []

        for line in lines:
            stripped = line.strip()
            if not stripped or stripped.startswith('#'):
                continue

            if stripped.startswith('%'):
                if current_tag is not None:
                    # Armazena última ocorrência em raw_sections
                    self.raw_sections[current_tag] = tag_lines
                    # Armazena TODAS as ocorrências em raw_sections_multi
                    self.raw_sections_multi.setdefault(current_tag, []).append(tag_lines)
                current_tag = stripped
                tag_lines = []
            else:
                tag_lines.append(stripped)

        if current_tag is not None:
            self.raw_sections[current_tag] = tag_lines
            self.raw_sections_multi.setdefault(current_tag, []).append(tag_lines)

    # -------------------------------------------------------------------------
    # HELPERS DE ACESSO
    # -------------------------------------------------------------------------
    def _get_float(self, tag, default=0.0, line_idx=0, col_idx=0):
        """Retorna um float do dado de uma tag."""
        if tag in self.raw_sections and len(self.raw_sections[tag]) > line_idx:
            try:
                parts = self.raw_sections[tag][line_idx].split()
                return float(parts[col_idx])
            except (ValueError, IndexError):
                pass
        return default

    def _get_str(self, tag, default='', line_idx=0):
        """Retorna uma string do dado de uma tag (remove aspas)."""
        if tag in self.raw_sections and len(self.raw_sections[tag]) > line_idx:
            return self.raw_sections[tag][line_idx].strip().strip("'\"")
        return default

    def _get_profile(self, tag):
        """
        Lê um perfil de N valores onde:
          - 1ª linha de dados = N (contagem)
          - próximas N linhas = 1 valor cada
        Retorna list[float].
        """
        if tag not in self.raw_sections:
            return []
        data = self.raw_sections[tag]
        if not data:
            return []
        try:
            n = int(data[0].split()[0])
        except (ValueError, IndexError):
            return []
        result = []
        for i in range(1, min(n + 1, len(data))):
            try:
                result.append(float(data[i].split()[0]))
            except (ValueError, IndexError):
                pass
        return result

    def _get_all_profiles(self, tag):
        """Como _get_profile mas retorna lista de listas (todas as ocorrências da tag)."""
        result = []
        for occurrence_data in self.raw_sections_multi.get(tag, []):
            if not occurrence_data:
                continue
            try:
                n = int(occurrence_data[0].split()[0])
            except (ValueError, IndexError):
                continue
            profile = []
            for i in range(1, min(n + 1, len(occurrence_data))):
                try:
                    profile.append(float(occurrence_data[i].split()[0]))
                except (ValueError, IndexError):
                    pass
            result.append(profile)
        return result

    # -------------------------------------------------------------------------
    # EXTRAÇÃO DOS DADOS DO MODELO
    # -------------------------------------------------------------------------
    def _extract_all(self):
        g = 9.81  # vai ser sobrescrito pelo valor real do AML

        # 1. Título
        title = self._get_str('%TITLE', 'ANFLEX Model')

        # 2. Parâmetros Globais
        seabed_depth_m = self._get_float('%GLOBAL.SEABED.DEPTH', 265.0)
        gravity        = self._get_float('%GLOBAL.GRAVITY', 9.81)
        water_sp_wt_kNm3 = self._get_float('%GLOBAL.WATER_SPECIFIC_WEIGHT', 10.0553)
        steel_sp_wt_kNm3 = self._get_float('%GLOBAL.STEEL_SPECIFIC_WEIGHT', 78.5)
        g = gravity
        water_density_kgm3 = (water_sp_wt_kNm3 * 1000.0) / g if g > 0 else 1025.0

        # 3. Solo
        soil_name     = self._get_str('%SOIL', 'Solo')
        soil_stiff_kNm = self._get_float('%SOIL.SPRING.STIFFNESS', 800.0)
        soil_damping  = self._get_float('%SOIL.SPRING.DAMPING', 0.0)
        friction_axial   = self._get_float('%SOIL.FRICTION.AXIAL', 0.92)
        friction_lateral = self._get_float('%SOIL.FRICTION.LATERAL', 0.95)

        # 4. Material da linha flexível
        mat_name = self._get_str('%MATERIAL.FLEXIBLE_LINE', 'RISE')
        di = self._get_float('%MATERIAL.DIAMETER.INTERNAL', 0.2032)
        do = self._get_float('%MATERIAL.DIAMETER.EXTERNAL', 0.2779)
        dh = self._get_float('%MATERIAL.DIAMETER.HIDRODYNAMIC', do)
        w_dry_kNm = self._get_float('%MATERIAL.WEIGHT.DRY', 1.0494)   # kN/m
        w_wet_kNm = self._get_float('%MATERIAL.WEIGHT.WET', 0.4395)   # kN/m
        ea_kN    = self._get_float('%MATERIAL.STIFFNESS.AXIAL', 360000.0)     # kN
        ei_kNm2  = self._get_float('%MATERIAL.STIFFNESS.BENDING', 21.7)       # kN.m²
        gj_kNm2  = self._get_float('%MATERIAL.STIFFNESS.TORSIONAL', 3200.0)   # kN.m²
        cd       = self._get_float('%MATERIAL.MORISON.DRAG', 1.0)
        cm       = self._get_float('%MATERIAL.MORISON.INERTIA', 2.0)
        cd_long  = self._get_float('%MATERIAL.MORISON.DRAG.LONGITUDINAL', 0.0)

        # Módulos de flutuação embutidos no material
        floater_weight_kNm  = self._get_float('%MATERIAL.FLOATER.WEIGHT', 0.0)
        floater_buoy_kNm    = self._get_float('%MATERIAL.FLOATER.BUOYANCY', 0.0)
        floater_diam_m      = self._get_float('%MATERIAL.FLOATER.DIAMETER', 0.0)
        floater_spacing_m   = self._get_float('%MATERIAL.FLOATER.SPACING', 0.0)
        floater_sp_wt       = self._get_float('%MATERIAL.FLOATER.SPECIFIC_WEIGHT', 0.0)
        floater_width       = self._get_float('%MATERIAL.FLOATER.WIDTH', 1.0)
        floater_eff         = self._get_float('%MATERIAL.FLOATER.EFFICIENCY_FACTOR', 1.0)

        # Amortecimento de Rayleigh
        rayleigh_T1  = self._get_float('%MATERIAL.RAYLEIGH.PERIOD.FIRST', 0.0)
        rayleigh_T2  = self._get_float('%MATERIAL.RAYLEIGH.PERIOD.SECOND', 0.0)
        rayleigh_xi1 = self._get_float('%MATERIAL.RAYLEIGH.DAMPING.FIRST', 0.0)
        rayleigh_xi2 = self._get_float('%MATERIAL.RAYLEIGH.DAMPING.SECOND', 0.0)

        # Conversão para SI
        ea_N   = ea_kN   * 1000.0
        ei_Nm2 = ei_kNm2 * 1000.0
        gj_Nm2 = gj_kNm2 * 1000.0
        w_dry_N_per_m = w_dry_kNm * 1000.0  # N/m

        # Derivar E, A, I, G, J a partir de EA, EI, GJ, diâmetros
        area_outer = math.pi * do**2 / 4.0
        area_inner = math.pi * di**2 / 4.0
        area_struct = area_outer - area_inner   # área anular (m²)
        I_y = math.pi * (do**4 - di**4) / 64.0
        J_tors = math.pi * (do**4 - di**4) / 32.0

        E = ea_N / area_struct if area_struct > 0 else 2.1e11
        # EI / I => E; cross check
        E_from_EI = ei_Nm2 / I_y if I_y > 0 else E
        # Use E from EA (primary structural stiffness)
        G = gj_Nm2 / J_tors if J_tors > 0 else 8.0e10

        # Massa linear seca (kg/m)
        dry_mass_kgm = w_dry_N_per_m / g if g > 0 else 0.0

        # 5. Linhas (múltiplas possíveis)
        lines_data = []
        for i, name_data in enumerate(self.raw_sections_multi.get('%LINE', [])):
            line_name = name_data[0].strip("'\"") if name_data else f'L{i+1}'

            # Ângulo de catenária, azimute, offsets
            # Essas tags aparecem dentro da seção de cada linha — extraímos a
            # i-ésima ocorrência de cada tag para corresponder à i-ésima linha
            def get_nth(tag, default, n=i, col=0, line=0):
                occ = self.raw_sections_multi.get(tag, [])
                if n < len(occ) and occ[n]:
                    try:
                        return float(occ[n][line].split()[col])
                    except (ValueError, IndexError):
                        pass
                return default

            cat_angle = get_nth('%LINE.CATENARY.ANGLE', 5.0)
            azimuth   = get_nth('%LINE.AZIMUTH', 0.0)
            offset_near = get_nth('%LINE.OFFSET.NEAR', 0.0)
            offset_far  = get_nth('%LINE.OFFSET.FAR',  0.0)
            conn_id   = get_nth('%LINE.CONNECTION_ID', -1)
            top_press_static  = get_nth('%LINE.TOP_PRESSURE', 0.0, col=0)
            top_press_content = get_nth('%LINE.TOP_PRESSURE', 0.0, col=1)

            # Segmentos da linha
            segments = []
            mesh_occ = self.raw_sections_multi.get('%LINE.SEGMENT.MESH', [])
            mat_id_occ = self.raw_sections_multi.get('%LINE.SEGMENT.MATERIAL_ID', [])
            buoy_id_occ = self.raw_sections_multi.get('%LINE.SEGMENT.BUOY_ID', [])

            total_length_m = 0.0
            if i < len(mesh_occ):
                mesh_data = mesh_occ[i]
                try:
                    n_segs = int(mesh_data[0]) if mesh_data else 0
                    for s in range(1, n_segs + 1):
                        if s < len(mesh_data):
                            parts = mesh_data[s].split()
                            seg_len = float(parts[0]) if parts else 0.0
                            elem_len = float(parts[1]) if len(parts) > 1 else 5.0
                            total_length_m += seg_len
                            # Calcular número de elementos para o segmento
                            seg_elements = max(1, int(round(seg_len / (elem_len if elem_len > 0.0 else 5.0))))
                            
                            # Material e buoy IDs para este segmento
                            mat_id = -1
                            buoy_id = -1
                            if i < len(mat_id_occ) and len(mat_id_occ[i]) > s:
                                try:
                                    mat_id = int(mat_id_occ[i][s])
                                except ValueError:
                                    pass
                            if i < len(buoy_id_occ) and len(buoy_id_occ[i]) > s:
                                try:
                                    buoy_id = int(buoy_id_occ[i][s])
                                except ValueError:
                                    pass
                            segments.append({
                                'length_m': seg_len,
                                'material_id': mat_id,
                                'buoy_id': buoy_id,
                                'num_elements': seg_elements,
                            })
                except (ValueError, IndexError):
                    pass

            lines_data.append({
                'name': line_name,
                'catenary_angle_deg': cat_angle,
                'azimuth_deg': azimuth,
                'offset_near_m': offset_near,
                'offset_far_m': offset_far,
                'connection_id': int(conn_id),
                'top_pressure_static_kPa': top_press_static,
                'top_pressure_content_kPa': top_press_content,
                'total_length_m': total_length_m if total_length_m > 0 else 500.0,
                'segments': segments,
            })

        # 6. Correntes (múltiplas, identificadas por %CURRENT + %CURRENT.ID)
        currents = []
        current_names   = [d[0].strip("'\"") for d in self.raw_sections_multi.get('%CURRENT', []) if d]
        current_ids     = [d[0] if d else '-1' for d in self.raw_sections_multi.get('%CURRENT.ID', [])]
        depth_profiles  = self._get_all_profiles('%CURRENT.DEPTH')
        vel_profiles    = self._get_all_profiles('%CURRENT.VELOCITY')
        angle_profiles  = self._get_all_profiles('%CURRENT.ANGLE')

        for i in range(len(current_names)):
            currents.append({
                'name': current_names[i] if i < len(current_names) else f'Cor_{i+1}',
                'id': int(current_ids[i]) if i < len(current_ids) else -1,
                'depths_m': depth_profiles[i] if i < len(depth_profiles) else [0.0, seabed_depth_m],
                'velocities_ms': vel_profiles[i] if i < len(vel_profiles) else [0.0, 0.0],
                'angles_deg': angle_profiles[i] if i < len(angle_profiles) else [0.0, 0.0],
            })

        # 7. Ondas (JONSWAP)
        waves = []
        wave_ids    = [d[0] if d else '-1' for d in self.raw_sections_multi.get('%WAVE.ID', [])]
        wave_jonswap = [d[0].strip("'\"").upper() if d else 'OFF' for d in self.raw_sections_multi.get('%WAVE.JONSWAP', [])]
        wave_periods = [float(d[0]) if d else 10.0 for d in self.raw_sections_multi.get('%WAVE.PERIOD', [])]
        wave_heights = [float(d[0]) if d else 2.0 for d in self.raw_sections_multi.get('%WAVE.HEIGHT', [])]
        wave_gammas  = [float(d[0]) if d else 3.3 for d in self.raw_sections_multi.get('%WAVE.GAMMA', [])]
        wave_angles  = [float(d[0]) if d else 0.0 for d in self.raw_sections_multi.get('%WAVE.ANGLE', [])]

        for i in range(len(wave_ids)):
            waves.append({
                'id': int(wave_ids[i]) if i < len(wave_ids) else -1,
                'jonswap': wave_jonswap[i].upper() == 'ON' if i < len(wave_jonswap) else True,
                'period_s': wave_periods[i] if i < len(wave_periods) else 10.0,
                'height_m': wave_heights[i] if i < len(wave_heights) else 2.0,
                'gamma': wave_gammas[i] if i < len(wave_gammas) else 3.3,
                'angle_deg': wave_angles[i] if i < len(wave_angles) else 0.0,
            })

        # Monta o dicionário principal do modelo
        self.model_data = {
            'title': title,
            'global': {
                'seabed_depth_m': seabed_depth_m,
                'gravity_ms2': gravity,
                'water_specific_weight_kNm3': water_sp_wt_kNm3,
                'water_density_kgm3': water_density_kgm3,
                'steel_specific_weight_kNm3': steel_sp_wt_kNm3,
            },
            'soil': {
                'name': soil_name,
                'stiffness_kNm': soil_stiff_kNm,
                'stiffness_Nm': soil_stiff_kNm * 1000.0,
                'damping': soil_damping,
                'friction_axial': friction_axial,
                'friction_lateral': friction_lateral,
            },
            'material': {
                'name': mat_name,
                'inner_diameter_m': di,
                'outer_diameter_m': do,
                'hydro_diameter_m': dh,
                'weight_dry_kNm': w_dry_kNm,
                'weight_wet_kNm': w_wet_kNm,
                'dry_mass_kgm': dry_mass_kgm,
                'EA_N': ea_N,
                'EI_Nm2': ei_Nm2,
                'GJ_Nm2': gj_Nm2,
                # Propriedades elementares derivadas
                'E_Pa': E,
                'G_Pa': G,
                'A_m2': area_struct,
                'IY_m4': I_y,
                'IZ_m4': I_y,    # tubo axissimétrico
                'J_m4': J_tors,
                'rho_kgm3': dry_mass_kgm / area_struct if area_struct > 0 else 7850.0,
                'Cd': cd,
                'Cm': cm,
                'Cd_longitudinal': cd_long,
                'floater': {
                    'weight_kNm': floater_weight_kNm,
                    'buoyancy_kNm': floater_buoy_kNm,
                    'diameter_m': floater_diam_m,
                    'spacing_m': floater_spacing_m,
                    'specific_weight': floater_sp_wt,
                    'width': floater_width,
                    'efficiency_factor': floater_eff,
                },
                'rayleigh': {
                    'period_1_s': rayleigh_T1,
                    'period_2_s': rayleigh_T2,
                    'damping_ratio_1': rayleigh_xi1,
                    'damping_ratio_2': rayleigh_xi2,
                    # Coeficientes alpha/beta se os períodos estiverem definidos
                    'alpha': self._compute_rayleigh_alpha(rayleigh_T1, rayleigh_T2, rayleigh_xi1, rayleigh_xi2),
                    'beta':  self._compute_rayleigh_beta(rayleigh_T1, rayleigh_T2, rayleigh_xi1, rayleigh_xi2),
                },
            },
            'lines': lines_data,
            'currents': currents,
            'waves': waves,
            'analysis': {
                'static': {
                    'total_time': self._get_float('%ANALYSIS_CASE.STATIC.TOTAL_TIME', self._get_float('%ASSEMBLY.TOTAL_TIME', 11.0)),
                    'time_step':  self._get_float('%ANALYSIS_CASE.STATIC.TIME_STEP',  self._get_float('%ASSEMBLY.TIME_STEP',  1.0)),
                    'max_iter':   int(self._get_float('%ANALYSIS_CASE.STATIC.MAX_NUM_ITERATION', self._get_float('%ASSEMBLY.MAX_NUM_ITERATION', 40.0))),
                    'tolerance':  self._get_float('%ANALYSIS_CASE.STATIC.TOLERANCE',  self._get_float('%ASSEMBLY.TOLERANCE',  1.0e-3)),
                },
                'dynamic': {
                    'total_time': self._get_float('%ANALYSIS_CASE.TIME_DOMAIN.TOTAL_TIME', 1.0),
                    'time_step':  self._get_float('%ANALYSIS_CASE.TIME_DOMAIN.TIME_STEP',  0.05),
                    'max_iter':   int(self._get_float('%ANALYSIS_CASE.TIME_DOMAIN.MAX_NUM_ITERATION', 20.0)),
                    'tolerance':  self._get_float('%ANALYSIS_CASE.TIME_DOMAIN.TOLERANCE',  1.0e-3),
                }
            }
        }

    @staticmethod
    def _compute_rayleigh_alpha(T1, T2, xi1, xi2):
        """Coeficiente α do amortecimento de Rayleigh: C = α*M + β*K."""
        if T1 > 0 and T2 > 0 and T1 != T2:
            w1 = 2 * math.pi / T1
            w2 = 2 * math.pi / T2
            # [xi1; xi2] = (1/2) * [[1/w1, w1], [1/w2, w2]] * [alpha; beta]
            det = (1/w1) * w2 - (1/w2) * w1
            if abs(det) > 1e-12:
                alpha = 2.0 * (xi1 * w2 - xi2 * w1) / (w2**2 - w1**2) * w1 * w2
                return alpha
        return 0.0

    @staticmethod
    def _compute_rayleigh_beta(T1, T2, xi1, xi2):
        """Coeficiente β do amortecimento de Rayleigh: C = α*M + β*K."""
        if T1 > 0 and T2 > 0 and T1 != T2:
            w1 = 2 * math.pi / T1
            w2 = 2 * math.pi / T2
            if abs(w2**2 - w1**2) > 1e-12:
                beta = 2.0 * (xi2 * w2 - xi1 * w1) / (w2**2 - w1**2)
                return beta
        return 0.0

    # -------------------------------------------------------------------------
    # MÉTODO DE CONFIGURAÇÃO PARA O riserSim
    # -------------------------------------------------------------------------
    def to_risersim_config(self, line_index=0):
        """
        Retorna um dicionário de configuração pronto para instanciar uma
        StaticAnalysis + DynamicAnalysis do riserSim.

        Parâmetros:
          line_index: índice da linha a simular (0 = primeira, default)

        Retorna dict com:
          'beam_props'   : BeamMaterialProps em SI
          'geometry'     : num_elements, total_length, depth, span_x
          'seabed'       : SeabedInteraction params
          'wave'         : DynamicAnalysis wave params (1ª onda disponível)
          'current'      : 1ª corrente disponível
          'rayleigh'     : alpha, beta
          'offsets'      : near, far
        """
        mat = self.model_data['material']
        glb = self.model_data['global']
        soil = self.model_data['soil']

        line = self.model_data['lines'][line_index] if self.model_data['lines'] else {}
        wave = self.model_data['waves'][0] if self.model_data['waves'] else {}
        curr = self.model_data['currents'][0] if self.model_data['currents'] else {}

        total_length = line.get('total_length_m', 500.0)
        depth = glb['seabed_depth_m']

        # Soma o número de elementos de todos os segmentos da linha definidos no AML
        if 'segments' in line and line['segments']:
            num_elements = sum(seg.get('num_elements', 0) for seg in line['segments'])
        else:
            num_elements = max(20, min(200, int(total_length / 5)))
        if num_elements <= 0:
            num_elements = 100

        # Alcance horizontal real X_span da catenária (com trecho repousando no solo TDZ):
        # Para um riser flexível suspenso de comprimento L e profundidade h,
        # o alcance horizontal típico da âncora é ~0.70 * L (ex: 350m para L=500m, h=265m)
        span_x = min(total_length * 0.70, math.sqrt(max(0.0, total_length**2 - depth**2)) * 0.85)

        config = {
            'title': self.model_data['title'],
            'beam_props': {
                'E':         mat['E_Pa'],
                'G':         mat['G_Pa'],
                'A':         mat['A_m2'],
                'IY':        mat['IY_m4'],
                'IZ':        mat['IZ_m4'],
                'J':         mat['J_m4'],
                'EI':        mat['EI_Nm2'],  # Rigidez fletora real EI (N.m²)
                'rho':       mat['rho_kgm3'],
                'D_outer':   mat['outer_diameter_m'],
                'D_inner':   mat['inner_diameter_m'],
                'rho_fluid': glb['water_density_kgm3'],  # Assume fluido interno = água (conservador)
                'Ca':        mat['Cm'] - 1.0,            # Cm = 1 + Ca
            },
            'geometry': {
                'num_elements': num_elements,
                'total_length_m': total_length,
                'water_depth_m': depth,
                'catenary_angle_deg': line.get('catenary_angle_deg', 5.0),
                'azimuth_deg': line.get('azimuth_deg', 0.0),
                'span_x_m': span_x,
                # Convenção de Coordenadas:
                # ANFLEX: Seabed em Z = 0.0, Superfície em Z = +water_depth
                # risersim: Superfície em Z = 0.0, Seabed em Z = -water_depth
                'anflex_z_seabed_m': 0.0,
                'anflex_z_surface_m': depth,
            },
            'seabed': {
                'depth_m': -depth,  # riserSim usa Z negativo
                'stiffness_Nm': soil['stiffness_Nm'],
                'friction_coeff': soil['friction_lateral'],
            },
            'offsets': {
                'near_m': 0.0,
                'far_m':  0.0,
            },
            'wave': {
                'period_s':   wave.get('period_s', 10.0),
                'height_m':   wave.get('height_m', 2.0),
                'amplitude_m': wave.get('height_m', 2.0) / 2.0,
                'gamma':      wave.get('gamma', 3.3),
                'jonswap':    wave.get('jonswap', True),
                'angle_deg':  wave.get('angle_deg', 0.0),
            },
            'current': {
                'depths_m':      curr.get('depths_m', [0.0, depth]),
                'velocities_ms': curr.get('velocities_ms', [0.0, 0.0]),
                'angles_deg':    curr.get('angles_deg', [0.0, 0.0]),
            },
            'rayleigh': {
                'alpha': mat['rayleigh']['alpha'],
                'beta':  mat['rayleigh']['beta'],
            },
            'water_density_kgm3': glb['water_density_kgm3'],
            'simulation_options': {
                # Configurações de passos e solvers extraídas do AML
                'static_steps': max(1, int(self.model_data['analysis']['static']['total_time'] / 
                                          (self.model_data['analysis']['static']['time_step'] if self.model_data['analysis']['static']['time_step'] > 0 else 1.0))),
                'static_max_iter': self.model_data['analysis']['static']['max_iter'],
                'static_tolerance': self.model_data['analysis']['static']['tolerance'],
                'duration_s': self.model_data['analysis']['dynamic']['total_time'],
                'dt_s': self.model_data['analysis']['dynamic']['time_step'],
                'dynamic_max_iter': self.model_data['analysis']['dynamic']['max_iter'],
                'dynamic_tolerance': self.model_data['analysis']['dynamic']['tolerance'],
            }
        }
        return config

    # -------------------------------------------------------------------------
    # SAÍDA
    # -------------------------------------------------------------------------
    def summary(self):
        d = self.model_data
        g = d['global']
        m = d['material']
        s = d['soil']
        line = d['lines'][0] if d['lines'] else {}
        print("==========================================================")
        print(f"  📦 ANFLEX AML: {d['title']}")
        print("==========================================================")
        print(f"🌊 Lâmina d'água (seabed depth): {g['seabed_depth_m']:.1f} m")
        print(f"💧 Densidade da água:            {g['water_density_kgm3']:.1f} kg/m³")
        print(f"📏 Comprimento total da linha:   {line.get('total_length_m', 0):.1f} m")
        print(f"📐 Ângulo catenária / Azimute:   {line.get('catenary_angle_deg', 0):.1f}° / {line.get('azimuth_deg', 0):.1f}°")
        print(f"🔵 Diâmetros ID / OD:            {m['inner_diameter_m']*1000:.1f} mm / {m['outer_diameter_m']*1000:.1f} mm")
        print(f"⚖️  Peso Seco / Úmido:            {m['weight_dry_kNm']:.4f} kN/m / {m['weight_wet_kNm']:.4f} kN/m")
        print(f"💪 EA:                           {m['EA_N']/1e6:.1f} MN")
        print(f"🌀 EI:                           {m['EI_Nm2']/1e3:.2f} kN.m²")
        print(f"🔩 GJ:                           {m['GJ_Nm2']/1e3:.2f} kN.m²")
        print(f"🏗️  E derivado / G derivado:      {m['E_Pa']/1e9:.2f} GPa / {m['G_Pa']/1e9:.2f} GPa")
        print(f"📏 Área estrutural:              {m['A_m2']*1e4:.2f} cm²")
        print(f"⚓ Solo Kz:                      {s['stiffness_kNm']:.0f} kN/m  |  µ_lat={s['friction_lateral']}")
        if d['currents']:
            c = d['currents'][0]
            vmax = max(c['velocities_ms']) if c['velocities_ms'] else 0
            vmin = min(c['velocities_ms']) if c['velocities_ms'] else 0
            print(f"🌊 Corrente ({c['name']}):           {vmax:.2f} m/s (sup.) → {vmin:.2f} m/s (fundo)")
        if d['waves']:
            w = d['waves'][0]
            print(f"🌊 Onda JONSWAP:                 Hs={w['height_m']:.1f}m  Tp={w['period_s']:.1f}s  γ={w['gamma']:.2f}")
        print(f"🔢 Linhas: {len(d['lines'])}  |  Correntes: {len(d['currents'])}  |  Ondas: {len(d['waves'])}")
        print("==========================================================")

    def to_json(self, json_path=None):
        """Salva model_data em JSON. Retorna string JSON se json_path=None."""
        text = json.dumps(self.model_data, indent=2, ensure_ascii=False)
        if json_path:
            with open(json_path, 'w', encoding='utf-8') as f:
                f.write(text)
            print(f"✅ Configurações salvas em JSON: {json_path}")
        return text


# =============================================================================
# CLI
# =============================================================================
if __name__ == "__main__":
    filepath = sys.argv[1] if len(sys.argv) > 1 else \
        r"exemplos/Curso/Exemplo_01/Exemplo_01a/Exemplo_01a.aml"
    reader = ANFLEXAMLReader(filepath)
    reader.summary()

    if len(sys.argv) > 2:
        reader.to_json(sys.argv[2])
    else:
        # Imprime a configuração riserSim no stdout
        cfg = reader.to_risersim_config()
        print("\n--- Configuração riserSim ---")
        print(json.dumps(cfg, indent=2, ensure_ascii=False))
