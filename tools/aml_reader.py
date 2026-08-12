import os
import sys
import json
import math

class ANFLEXAMLReader:
    """
    ANFLEX AML Reader & Model Extractor (v2)
    =========================================
    Parses ANFLEX .aml input files and converts them into physical
    configurations ready for riserSim.

    AML format:
      - Tags start with '%'
      - Lines without '%' are data for the preceding tag
      - The first data line after the tag is the 1st value line
      - Currents: 1st data line = N, followed by N values (one per line)
      - %LINE.SEGMENT.MESH section: 1st line = N_segments,
        next N lines = "length factor_start factor_end"

    AML's native units:
      - Lengths: meters
      - Weights/forces: kN
      - EA stiffness: kN
      - EI stiffness: kN.m²
      - GJ stiffness: kN.m²
      - Velocity: m/s
      - Angles: degrees
    """

    def __init__(self, aml_filepath):
        self.filepath = aml_filepath
        # raw_sections: dict[tag_str] -> list[str] (data lines)
        self.raw_sections = {}
        # For tags that appear multiple times (e.g. %CURRENT), a list of occurrences
        self.raw_sections_multi = {}
        self.model_data = {}
        self._parse_file()
        self._extract_all()

    # -------------------------------------------------------------------------
    # RAW PARSING
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
                    # Stores the last occurrence in raw_sections
                    self.raw_sections[current_tag] = tag_lines
                    # Stores ALL occurrences in raw_sections_multi
                    self.raw_sections_multi.setdefault(current_tag, []).append(tag_lines)
                current_tag = stripped
                tag_lines = []
            else:
                tag_lines.append(stripped)

        if current_tag is not None:
            self.raw_sections[current_tag] = tag_lines
            self.raw_sections_multi.setdefault(current_tag, []).append(tag_lines)

    # -------------------------------------------------------------------------
    # ACCESS HELPERS
    # -------------------------------------------------------------------------
    def _get_float(self, tag, default=0.0, line_idx=0, col_idx=0):
        """Returns a float from a tag's data."""
        if tag in self.raw_sections and len(self.raw_sections[tag]) > line_idx:
            try:
                parts = self.raw_sections[tag][line_idx].split()
                return float(parts[col_idx])
            except (ValueError, IndexError):
                pass
        return default

    def _get_str(self, tag, default='', line_idx=0):
        """Returns a string from a tag's data (strips quotes)."""
        if tag in self.raw_sections and len(self.raw_sections[tag]) > line_idx:
            return self.raw_sections[tag][line_idx].strip().strip("'\"")
        return default

    def _get_float_at(self, tag, occ_idx, default=0.0, line_idx=0, col_idx=0):
        """Like _get_float, but reads a specific occurrence (occ_idx) of a multi-occurrence tag
        (e.g. the i-th %SOIL block's %SOIL.SPRING.STIFFNESS) instead of only the tag's single
        stored occurrence (_get_float always reads raw_sections, which _parse_file overwrites
        with the LAST occurrence -- see raw_sections_multi's own comment in __init__)."""
        occ = self.raw_sections_multi.get(tag, [])
        if occ_idx < len(occ) and len(occ[occ_idx]) > line_idx:
            try:
                parts = occ[occ_idx][line_idx].split()
                return float(parts[col_idx])
            except (ValueError, IndexError):
                pass
        return default

    def _get_profile(self, tag):
        """
        Reads a profile of N values where:
          - 1st data line = N (count)
          - next N lines = 1 value each
        Returns list[float].
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
        """Like _get_profile but returns a list of lists (every occurrence of the tag)."""
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

    def _scan_wave_spectrum_types(self):
        """Returns a list of "jonswap"/"regular" markers, one per %WAVE block, in real file
        order -- see the call site's comment (in _extract_all()) for why this needs a dedicated
        re-scan of the raw lines instead of the usual raw_sections_multi[tag] lookup: the type
        marker IS the block-opening tag itself (%WAVE.JONSWAP vs %WAVE.REGULAR), not something
        inside a fixed %WAVE block, so the two tags' occurrence lists need to be interleaved back
        into real file order to line up with %WAVE.ID's own occurrence order.
        """
        types = []
        try:
            with open(self.filepath, 'r', encoding='latin-1', errors='ignore') as f:
                for line in f:
                    stripped = line.strip()
                    if stripped == '%WAVE.JONSWAP':
                        types.append('jonswap')
                    elif stripped == '%WAVE.REGULAR':
                        types.append('regular')
        except OSError:
            pass
        return types

    # -------------------------------------------------------------------------
    # MODEL DATA EXTRACTION
    # -------------------------------------------------------------------------
    def _extract_all(self):
        g = 9.81  # will be overwritten by the AML's real value

        # 1. Title
        title = self._get_str('%TITLE', 'ANFLEX Model')

        # 2. Global Parameters
        seabed_depth_m = self._get_float('%GLOBAL.SEABED.DEPTH', 265.0)
        gravity        = self._get_float('%GLOBAL.GRAVITY', 9.81)
        water_sp_wt_kNm3 = self._get_float('%GLOBAL.WATER_SPECIFIC_WEIGHT', 10.0553)
        steel_sp_wt_kNm3 = self._get_float('%GLOBAL.STEEL_SPECIFIC_WEIGHT', 78.5)
        g = gravity
        water_density_kgm3 = (water_sp_wt_kNm3 * 1000.0) / g if g > 0 else 1025.0

        # 3. Soil (legacy single global soil -- "last %SOIL block in the file wins", no ID
        # matching at all; kept as-is since to_risersim_config() below still uses it, and it
        # also doubles as the fallback in _resolve_soil_for_line() when a line's segment has no
        # SOIL_ID or it doesn't match any parsed %SOIL.ID block).
        soil_name     = self._get_str('%SOIL', 'Solo')
        soil_stiff_kNm = self._get_float('%SOIL.SPRING.STIFFNESS', 800.0)
        soil_damping  = self._get_float('%SOIL.SPRING.DAMPING', 0.0)
        friction_axial   = self._get_float('%SOIL.FRICTION.AXIAL', 0.92)
        friction_lateral = self._get_float('%SOIL.FRICTION.LATERAL', 0.95)

        # 3b. Every %SOIL block, keyed by its real %SOIL.ID -- lets a line's segment resolve
        # its real soil by ID (%LINE.SEGMENT.SOIL_ID, read below) instead of always taking
        # whichever %SOIL block happened to appear last in the file. Used by
        # _resolve_soil_for_line()/to_risersim_json() -- see docs/roadmap.md item 2a. Confirmed
        # real data: exemplos/DNV/DNV_Check.aml has %SOIL.ID 112 matching a segment's
        # %LINE.SEGMENT.SOIL_ID 112; exemplos/ESDV/ESDV.aml has two distinct %SOIL blocks
        # (916, 917) with real line segments referencing 917.
        soils = []
        soil_names_multi = [d[0].strip("'\"") for d in self.raw_sections_multi.get('%SOIL', []) if d]
        soil_ids_multi    = [d[0] if d else '-1' for d in self.raw_sections_multi.get('%SOIL.ID', [])]
        for i in range(len(soil_names_multi)):
            try:
                s_id = int(float(soil_ids_multi[i])) if i < len(soil_ids_multi) else -1
            except ValueError:
                s_id = -1
            soils.append({
                'name': soil_names_multi[i],
                'id': s_id,
                'stiffness_kNm': self._get_float_at('%SOIL.SPRING.STIFFNESS', i, 800.0),
                'stiffness_Nm': self._get_float_at('%SOIL.SPRING.STIFFNESS', i, 800.0) * 1000.0,
                'damping': self._get_float_at('%SOIL.SPRING.DAMPING', i, 0.0),
                'friction_axial': self._get_float_at('%SOIL.FRICTION.AXIAL', i, 0.92),
                'friction_lateral': self._get_float_at('%SOIL.FRICTION.LATERAL', i, 0.95),
                # None (not a numeric default) when absent -- these should only enter the JSON
                # when the AML actually has them, same "optional field" philosophy as
                # xml_h5_reader.py's get_optional_float() for the same two fields.
                'axial_elastic_deflection_limit': self._get_float_at('%SOIL.DEFLECTION.AXIAL', i, None),
                'lateral_elastic_deflection_limit': self._get_float_at('%SOIL.DEFLECTION.LATERAL', i, None),
            })

        # Global %OPTION.SOIL.COUPLED/.UNCOUPLED flag -- self-describing tag (the keyword
        # itself carries the value, no separate data line -- same convention as
        # %ASSEMBLY.USING.TRUE/FALSE in xml_h5_reader.py's extract_assembly_flag()). Applies to
        # the whole model, not per-soil, so a plain membership check against raw_sections
        # (which _parse_file already populates -- even for a tag with zero data lines) is
        # enough, no regex needed.
        if '%OPTION.SOIL.COUPLED' in self.raw_sections:
            soil_model = 'coupled'
        elif '%OPTION.SOIL.UNCOUPLED' in self.raw_sections:
            soil_model = 'uncoupled'
        else:
            soil_model = None

        # 4. Flexible line material
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

        # Buoyancy modules embedded in the material
        floater_weight_kNm  = self._get_float('%MATERIAL.FLOATER.WEIGHT', 0.0)
        floater_buoy_kNm    = self._get_float('%MATERIAL.FLOATER.BUOYANCY', 0.0)
        floater_diam_m      = self._get_float('%MATERIAL.FLOATER.DIAMETER', 0.0)
        floater_spacing_m   = self._get_float('%MATERIAL.FLOATER.SPACING', 0.0)
        floater_sp_wt       = self._get_float('%MATERIAL.FLOATER.SPECIFIC_WEIGHT', 0.0)
        floater_width       = self._get_float('%MATERIAL.FLOATER.WIDTH', 1.0)
        floater_eff         = self._get_float('%MATERIAL.FLOATER.EFFICIENCY_FACTOR', 1.0)

        # Rayleigh damping
        rayleigh_T1  = self._get_float('%MATERIAL.RAYLEIGH.PERIOD.FIRST', 0.0)
        rayleigh_T2  = self._get_float('%MATERIAL.RAYLEIGH.PERIOD.SECOND', 0.0)
        rayleigh_xi1 = self._get_float('%MATERIAL.RAYLEIGH.DAMPING.FIRST', 0.0)
        rayleigh_xi2 = self._get_float('%MATERIAL.RAYLEIGH.DAMPING.SECOND', 0.0)

        # Conversion to SI
        ea_N   = ea_kN   * 1000.0
        ei_Nm2 = ei_kNm2 * 1000.0
        gj_Nm2 = gj_kNm2 * 1000.0
        w_dry_N_per_m = w_dry_kNm * 1000.0  # N/m

        # Derive E, A, I, G, J from EA, EI, GJ, diameters
        area_outer = math.pi * do**2 / 4.0
        area_inner = math.pi * di**2 / 4.0
        area_struct = area_outer - area_inner   # annular area (m²)
        I_y = math.pi * (do**4 - di**4) / 64.0
        J_tors = math.pi * (do**4 - di**4) / 32.0

        E = ea_N / area_struct if area_struct > 0 else 2.1e11
        # EI / I => E; cross check
        E_from_EI = ei_Nm2 / I_y if I_y > 0 else E
        # Use E from EA (primary structural stiffness)
        G = gj_Nm2 / J_tors if J_tors > 0 else 8.0e10

        # Dry linear mass (kg/m)
        dry_mass_kgm = w_dry_N_per_m / g if g > 0 else 0.0

        # 5. Lines (multiple possible)
        lines_data = []
        for i, name_data in enumerate(self.raw_sections_multi.get('%LINE', [])):
            line_name = name_data[0].strip("'\"") if name_data else f'L{i+1}'

            # Catenary angle, azimuth, offsets
            # These tags appear inside each line's section -- we extract the
            # i-th occurrence of each tag to correspond to the i-th line
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

            # Line segments
            segments = []
            mesh_occ = self.raw_sections_multi.get('%LINE.SEGMENT.MESH', [])
            mat_id_occ = self.raw_sections_multi.get('%LINE.SEGMENT.MATERIAL_ID', [])
            buoy_id_occ = self.raw_sections_multi.get('%LINE.SEGMENT.BUOY_ID', [])
            # %LINE.SEGMENT.SOIL_ID -- same multi-occurrence "count then N values" pattern as
            # MATERIAL_ID/BUOY_ID above; previously never read at all. Feeds
            # _resolve_soil_for_line()/to_risersim_json() (docs/roadmap.md item 2a).
            soil_id_occ = self.raw_sections_multi.get('%LINE.SEGMENT.SOIL_ID', [])

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
                            # Compute number of elements for the segment
                            seg_elements = max(1, int(round(seg_len / (elem_len if elem_len > 0.0 else 5.0))))

                            # Material and buoy IDs for this segment
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
                            soil_id = None
                            if i < len(soil_id_occ) and len(soil_id_occ[i]) > s:
                                try:
                                    soil_id = int(soil_id_occ[i][s])
                                except ValueError:
                                    pass
                            segments.append({
                                'length_m': seg_len,
                                'material_id': mat_id,
                                'buoy_id': buoy_id,
                                'soil_id': soil_id,
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

        # 6. Currents (multiple, identified by %CURRENT + %CURRENT.ID)
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

        # 7. Waves (JONSWAP)
        waves = []
        wave_ids    = [d[0] if d else '-1' for d in self.raw_sections_multi.get('%WAVE.ID', [])]
        # %WAVE.JONSWAP/%WAVE.REGULAR are the two real block-opening keywords for a wave --
        # confirmed against interfaces/src/wave.cpp:805 (cRegular::save, "%WAVE.REGULAR") vs.
        # :1073 ("%WAVE.JONSWAP"): the quoted string right after either is just the wave's NAME
        # (e.g. 'ON'/'ONE'/'ONW'/'OSW' in Exemplo_01a.aml -- Onda Norte/Nordeste/Noroeste/
        # Sudoeste, same naming convention as %CURRENT's own name), NOT an on/off toggle.
        # raw_sections_multi keys each tag's own occurrences separately, losing the relative
        # order between DIFFERENT tags needed to know which real type each %WAVE.ID entry
        # belongs to -- _scan_wave_spectrum_types() recovers that by walking the file directly.
        wave_spectrum_types = self._scan_wave_spectrum_types()
        wave_periods = [float(d[0]) if d else 10.0 for d in self.raw_sections_multi.get('%WAVE.PERIOD', [])]
        wave_heights = [float(d[0]) if d else 2.0 for d in self.raw_sections_multi.get('%WAVE.HEIGHT', [])]
        wave_gammas  = [float(d[0]) if d else 3.3 for d in self.raw_sections_multi.get('%WAVE.GAMMA', [])]
        wave_angles  = [float(d[0]) if d else 0.0 for d in self.raw_sections_multi.get('%WAVE.ANGLE', [])]
        # Full real JONSWAP spectrum parameters -- needed by VesselMotion's own spectral
        # computation (see _resolve_vessel_motion()), not just the "wave" block's metadata
        # (period/gamma above). Keyword names and defaults confirmed against
        # xml_h5_reader.py::extract_vessel_motion_raw()'s own defaults (0.2/3.0/100), which
        # match exactly -- same real source (ANFLEX's %WAVE.FIRST_FREQUENCY/LAST_FREQUENCY/
        # SPECTRUM_SUBDIVISION), just read from the .aml text here instead of the XML.
        wave_alphas = [float(d[0]) if d else 0.0 for d in self.raw_sections_multi.get('%WAVE.ALPHA', [])]
        wave_wini   = [float(d[0]) if d else 0.2 for d in self.raw_sections_multi.get('%WAVE.FIRST_FREQUENCY', [])]
        wave_wfin   = [float(d[0]) if d else 3.0 for d in self.raw_sections_multi.get('%WAVE.LAST_FREQUENCY', [])]
        wave_nwave  = [int(float(d[0])) if d else 100 for d in self.raw_sections_multi.get('%WAVE.SPECTRUM_SUBDIVISION', [])]

        for i in range(len(wave_ids)):
            waves.append({
                'id': int(wave_ids[i]) if i < len(wave_ids) else -1,
                'jonswap': (wave_spectrum_types[i] == 'jonswap') if i < len(wave_spectrum_types) else True,
                'period_s': wave_periods[i] if i < len(wave_periods) else 10.0,
                'height_m': wave_heights[i] if i < len(wave_heights) else 2.0,
                'gamma': wave_gammas[i] if i < len(wave_gammas) else 3.3,
                'angle_deg': wave_angles[i] if i < len(wave_angles) else 0.0,
                'alpha': wave_alphas[i] if i < len(wave_alphas) else 0.0,
                'wini_rad_s': wave_wini[i] if i < len(wave_wini) else 0.2,
                'wfin_rad_s': wave_wfin[i] if i < len(wave_wfin) else 3.0,
                'nwave': wave_nwave[i] if i < len(wave_nwave) else 100,
            })

        # 8. Load cases (multiple possible) -- each %LOAD_CASE bundles a CHOSEN current+wave (by
        # ID) for one specific loading scenario, e.g. ANFLEX's usual 'Near'/'Far' offset pair. A
        # real XML+H5 export always corresponds to exactly ONE already-resolved load case, so
        # xml_h5_reader.py never needs this; a plain .aml carries all of them, and until now
        # aml_reader.py had no notion of "load case" at all -- _resolve_current_for_load_case()
        # (see below) used to just grab the FIRST %LOAD_CASE.CURRENT_ID in the file, with no way
        # to pick a different one and no awareness that %LOAD_CASE.WAVE_ID exists at all (wave
        # selection stayed on the old waves[0] pick). Same name+ID parsing pattern as
        # soils/currents above -- the bare %LOAD_CASE tag's own data line is its name (e.g.
        # 'Near'/'Far'), same convention as %SOIL/%CURRENT.
        def _int_or_none(v):
            try:
                return int(float(v)) if v is not None else None
            except ValueError:
                return None

        load_cases = []
        lc_names = [d[0].strip("'\"") for d in self.raw_sections_multi.get('%LOAD_CASE', []) if d]
        lc_ids = [d[0] if d else None for d in self.raw_sections_multi.get('%LOAD_CASE.ID', [])]
        lc_current_ids = [d[0] if d else None for d in self.raw_sections_multi.get('%LOAD_CASE.CURRENT_ID', [])]
        lc_wave_ids = [d[0] if d else None for d in self.raw_sections_multi.get('%LOAD_CASE.WAVE_ID', [])]
        for i in range(len(lc_names)):
            load_cases.append({
                'name': lc_names[i],
                'id': _int_or_none(lc_ids[i]) if i < len(lc_ids) else None,
                'current_id': _int_or_none(lc_current_ids[i]) if i < len(lc_current_ids) else None,
                'wave_id': _int_or_none(lc_wave_ids[i]) if i < len(lc_wave_ids) else None,
            })

        # Assembles the model's main dictionary
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
            # Every %SOIL block, keyed by ID -- see the "3b" comment above where `soils` is
            # built. `soil_model` is the global %OPTION.SOIL.COUPLED/.UNCOUPLED flag (or None).
            'soils': soils,
            'soil_model': soil_model,
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
                # Derived elementary properties
                'E_Pa': E,
                'G_Pa': G,
                'A_m2': area_struct,
                'IY_m4': I_y,
                'IZ_m4': I_y,    # axisymmetric tube
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
                    # Alpha/beta coefficients if the periods are defined
                    'alpha': self._compute_rayleigh_alpha(rayleigh_T1, rayleigh_T2, rayleigh_xi1, rayleigh_xi2),
                    'beta':  self._compute_rayleigh_beta(rayleigh_T1, rayleigh_T2, rayleigh_xi1, rayleigh_xi2),
                },
            },
            'lines': lines_data,
            'currents': currents,
            'waves': waves,
            'load_cases': load_cases,
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
        """Rayleigh damping coefficient α: C = α*M + β*K."""
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
        """Rayleigh damping coefficient β: C = α*M + β*K."""
        if T1 > 0 and T2 > 0 and T1 != T2:
            w1 = 2 * math.pi / T1
            w2 = 2 * math.pi / T2
            if abs(w2**2 - w1**2) > 1e-12:
                beta = 2.0 * (xi2 * w2 - xi1 * w1) / (w2**2 - w1**2)
                return beta
        return 0.0

    # -------------------------------------------------------------------------
    # CONFIGURATION METHOD FOR riserSim
    # -------------------------------------------------------------------------
    def to_risersim_config(self, line_index=0):
        """
        Returns a configuration dict ready to instantiate a riserSim
        StaticAnalysis + DynamicAnalysis.

        Parameters:
          line_index: index of the line to simulate (0 = first, default)

        Returns a dict with:
          'beam_props'   : BeamMaterialProps in SI
          'geometry'     : num_elements, total_length, depth, span_x
          'seabed'       : SeabedInteraction params
          'wave'         : DynamicAnalysis wave params (1st wave available)
          'current'      : 1st current available
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

        # Sums the number of elements across every line segment defined in the AML
        if 'segments' in line and line['segments']:
            num_elements = sum(seg.get('num_elements', 0) for seg in line['segments'])
        else:
            num_elements = max(20, min(200, int(total_length / 5)))
        if num_elements <= 0:
            num_elements = 100

        # Catenary's real horizontal reach X_span (with a stretch resting on the seabed TDZ):
        # For a suspended flexible riser of length L and depth h,
        # the anchor's typical horizontal reach is ~0.70 * L (e.g. 350m for L=500m, h=265m)
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
                'EI':        mat['EI_Nm2'],  # Real bending stiffness EI (N.m²)
                'rho':       mat['rho_kgm3'],
                'D_outer':   mat['outer_diameter_m'],
                'D_inner':   mat['inner_diameter_m'],
                'rho_fluid': glb['water_density_kgm3'],  # Assumes internal fluid = water (conservative)
                'Ca':        mat['Cm'] - 1.0,            # Cm = 1 + Ca
                'Cd':        mat['Cd'],
            },
            'geometry': {
                'num_elements': num_elements,
                'total_length_m': total_length,
                'water_depth_m': depth,
                'catenary_angle_deg': line.get('catenary_angle_deg', 5.0),
                'azimuth_deg': line.get('azimuth_deg', 0.0),
                'span_x_m': span_x,
                # Coordinate Convention:
                # ANFLEX: Seabed at Z = 0.0, Surface at Z = +water_depth
                # risersim: Surface at Z = 0.0, Seabed at Z = -water_depth
                'anflex_z_seabed_m': 0.0,
                'anflex_z_surface_m': depth,
            },
            'seabed': {
                'depth_m': -depth,  # riserSim uses negative Z
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
                # Step and solver settings extracted from the AML
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
    # BY-ID RESOLUTION (soil, current) -- see docs/roadmap.md item 2a
    # -------------------------------------------------------------------------
    def _resolve_soil_for_line(self, line):
        """Global single-soil-by-ID resolution -- same simplification level xml_h5_reader.py
        already uses for current (_find_chosen_current): one winning soil for the whole line,
        matched by ID, not a per-position lookup. Only the line's FIRST segment's
        %LINE.SEGMENT.SOIL_ID is consulted -- a line crossing several distinct soil zones along
        its own length (seen in Boiao/P52_Boiao.aml) is explicitly out of scope, see
        docs/roadmap.md item 2b.

        Falls back to the legacy last-%SOIL-block-wins pick (self.model_data['soil']) when
        there's no segment SOIL_ID, or it doesn't match any parsed %SOIL.ID block -- so this
        never raises on older/simpler examples that don't use SOIL_ID at all. The fallback dict
        is reshaped to carry the same keys as a real `soils[]` entry so callers don't need to
        special-case which one they got.
        """
        soils = self.model_data.get('soils', [])
        segments = line.get('segments') or []
        if soils and segments:
            soil_id = segments[0].get('soil_id')
            if soil_id is not None:
                for s in soils:
                    if s.get('id') == soil_id:
                        return s
        legacy = self.model_data['soil']
        return {
            'name': legacy['name'],
            'id': None,
            'stiffness_kNm': legacy['stiffness_kNm'],
            'stiffness_Nm': legacy['stiffness_Nm'],
            'damping': legacy['damping'],
            'friction_axial': legacy['friction_axial'],
            'friction_lateral': legacy['friction_lateral'],
            'axial_elastic_deflection_limit': None,
            'lateral_elastic_deflection_limit': None,
        }

    def _resolve_load_case(self, load_case_id=None):
        """Picks which %LOAD_CASE bundle (a specific current+wave pairing for one loading
        scenario, e.g. ANFLEX's usual 'Near'/'Far' offset pair) to use for the rest of
        resolution. A real XML+H5 export always corresponds to exactly ONE already-resolved
        load case -- a plain .aml can define several, and there was previously no way to tell
        aml_reader.py which one to use (it silently always took the first).

        `load_case_id=None` (default) picks the first %LOAD_CASE in the file, same "index 0"
        default philosophy as `line_index=0` -- but unlike line_index this is matched by the
        real %LOAD_CASE.ID when one IS given, not by position, since a load case's identity is
        its ID (position in the file is incidental). Falls back to the first load case if the
        given id doesn't match any parsed one (e.g. a typo), and returns {} (current/wave
        resolution then each fall back to their own [0] pick) if the file has no %LOAD_CASE
        block at all -- so this never raises on older/simpler examples.
        """
        load_cases = self.model_data.get('load_cases', [])
        if not load_cases:
            return {}
        if load_case_id is None:
            return load_cases[0]
        for lc in load_cases:
            if lc.get('id') == load_case_id:
                return lc
        return load_cases[0]

    def _resolve_current(self, load_case):
        """Mirrors xml_h5_reader.py::_find_chosen_current(): resolves the real current used by
        the given load case's CURRENT_ID against the parsed %CURRENT.ID list, falling back to
        currents[0] (the old, ID-blind pick to_risersim_config() still uses) when there's no
        load case / no CURRENT_ID / no match -- so this never raises on files that don't use
        %LOAD_CASE at all (older/simpler examples).

        Real tag confirmed by grepping exemplos/*.aml: exemplos/DNV/DNV_Check.aml has
        %LOAD_CASE.CURRENT_ID 1815 matching %CURRENT.ID 1815 (only one current in that file, so
        it doesn't disambiguate anything by itself); a real divergence from the naive
        currents[0] pick is exercised by exemplos/Curso/Exemplo_01/Exemplo_01a/Exemplo_01a.aml
        (4 currents, IDs 125-128 in file order -- so currents[0] would be ID 125 -- but its
        'Near' load case's CURRENT_ID is 128).
        """
        currents = self.model_data.get('currents', [])
        if not currents:
            return {}
        chosen_id = load_case.get('current_id')
        if chosen_id is not None:
            for c in currents:
                if c.get('id') == chosen_id:
                    return c
        return currents[0]

    def _resolve_wave(self, load_case):
        """Same pattern as _resolve_current(), for %LOAD_CASE.WAVE_ID against the parsed
        %WAVE.ID list -- previously wave selection wasn't touched by the item-2a fix at all and
        stayed on the old waves[0] pick (to_risersim_config()'s 'wave' key), the same class of
        bug _resolve_current() fixes for current. Falls back to waves[0] when there's no load
        case / no WAVE_ID / no match.
        """
        waves = self.model_data.get('waves', [])
        if not waves:
            return {}
        chosen_id = load_case.get('wave_id')
        if chosen_id is not None:
            for w in waves:
                if w.get('id') == chosen_id:
                    return w
        return waves[0]

    # -------------------------------------------------------------------------
    # VESSEL MOTION (RAO + JONSWAP "equivalent harmonic" top motion, see docs/roadmap.md item
    # 2a follow-up -- ported from the real ANFLEX pre-processor, interfaces/src, since none of
    # this %KEYWORD-level parsing exists in trunk/src/trunk/libs (those only consume the
    # already-translated .dat/XML). Mirrors xml_h5_reader.py::extract_vessel_motion_raw()'s
    # return shape exactly, so _resolve_vessel_motion() below can feed model_builder.cpp's
    # existing environmental.vessel_motion parser (model_builder.cpp:333-377) unchanged.
    # -------------------------------------------------------------------------
    def _parse_floating_ships(self):
        """Parses every %FLOATING.SHIP block, keyed by its real %FLOATING.ID. Layout confirmed
        against interfaces/src/ship.cpp (cShip::read) and the real text (e.g.
        Exemplo_01a.aml:481-486): label line, then "origin.x origin.y", "length beam height"
        (unused here), "draft", "azimuth" -- 5 data lines per block, in that fixed order, no
        further %-tags in between (the %FLOATING.ID that identifies the block is a SEPARATE
        top-level tag emitted right after, matched here by occurrence index -- same convention
        already used elsewhere in this file for %SOIL/%SOIL.ID etc.).
        """
        ships = []
        blocks = self.raw_sections_multi.get('%FLOATING.SHIP', [])
        ids = [d[0] if d else None for d in self.raw_sections_multi.get('%FLOATING.ID', [])]
        for i, block in enumerate(blocks):
            if len(block) < 5:
                continue
            try:
                fid = int(float(ids[i])) if i < len(ids) and ids[i] is not None else -1
            except ValueError:
                fid = -1
            try:
                origin_x, origin_y = [float(v) for v in block[1].split()[:2]]
                draft = float(block[3].split()[0])
                azimuth_deg = float(block[4].split()[0])
            except (ValueError, IndexError):
                continue
            ships.append({
                'id': fid,
                'name': block[0].strip("'\""),
                'origin_x': origin_x,
                'origin_y': origin_y,
                'draft': draft,
                'azimuth_deg': azimuth_deg,
            })
        return ships

    def _parse_connections(self):
        """Parses every %CONNECTION.BEGIN block, keyed by its real %CONNECTION.ID --
        %CONNECTION.LOCAL_COORDINATES (3-vector) and %CONNECTION.REFERENCE_ID (points at a
        %FLOATING.ID) are separate top-level tags, matched by occurrence index like
        _parse_floating_ships() above.
        """
        connections = []
        names = [d[0].strip("'\"") if d else '' for d in self.raw_sections_multi.get('%CONNECTION.BEGIN', [])]
        ids = [d[0] if d else None for d in self.raw_sections_multi.get('%CONNECTION.ID', [])]
        coords_occ = self.raw_sections_multi.get('%CONNECTION.LOCAL_COORDINATES', [])
        ref_ids = [d[0] if d else None for d in self.raw_sections_multi.get('%CONNECTION.REFERENCE_ID', [])]
        for i in range(len(names)):
            try:
                cid = int(float(ids[i])) if i < len(ids) and ids[i] is not None else -1
            except ValueError:
                cid = -1
            local_coords = [0.0, 0.0, 0.0]
            if i < len(coords_occ) and coords_occ[i]:
                try:
                    local_coords = [float(v) for v in coords_occ[i][0].split()[:3]]
                except (ValueError, IndexError):
                    pass
            try:
                rid = int(float(ref_ids[i])) if i < len(ref_ids) and ref_ids[i] is not None else -1
            except ValueError:
                rid = -1
            connections.append({'id': cid, 'name': names[i], 'local_coords': local_coords, 'reference_id': rid})
        return connections

    def _resolve_connection_global(self, line):
        """Resolves the real global position of the line's vessel-attachment (connection)
        point, mirroring cConnection::global_coord (interfaces/src/connection.cpp:189-281) for
        the %FLOATING.SHIP case: rotation about X (floating's own "rotation") and Y
        ("inclination") are always 0 for a %FLOATING.SHIP block (the .aml never exposes those
        fields for a ship, only azimuth) -- so this reduces to rotating the connection's local
        coordinates about Z by (90-azimuth) around the floating's origin, then translating by
        that origin. `origin = (ship.origin_x, ship.origin_y, seabed_depth_m - ship.draft)`,
        confirmed against cFloating::get_origin (interfaces/src/floating.cpp:175-180).

        Validated by hand against this session's own real data before writing this code: for
        Exemplo_01a's FPSO (origin=(0,0), draft=13, azimuth=195) and its riser's connection
        (local coords 65,-32,5), this formula gives global X=-47.73 m -- exactly the [TOP] X
        already observed for the real "Cross" load case (XML+H5 path, post-static-solve).

        Returns `(global_coords, ship_dict)`, or `None` if the line has no real
        %CONNECTION/%FLOATING data (plain riser-only .aml) -- callers then skip vessel_motion
        entirely (unchanged fallback, see _resolve_vessel_motion()).
        """
        conn_id = line.get('connection_id')
        if conn_id is None or conn_id < 0:
            return None
        conn = next((c for c in self._parse_connections() if c['id'] == conn_id), None)
        if conn is None:
            return None
        ship = next((s for s in self._parse_floating_ships() if s['id'] == conn['reference_id']), None)
        if ship is None:
            return None

        seabed_depth_m = self.model_data['global']['seabed_depth_m']
        origin = (ship['origin_x'], ship['origin_y'], seabed_depth_m - ship['draft'])
        ang = math.radians(90.0 - ship['azimuth_deg'])
        lx, ly, lz = conn['local_coords']
        rx = lx * math.cos(ang) - ly * math.sin(ang)
        ry = lx * math.sin(ang) + ly * math.cos(ang)
        global_coords = [origin[0] + rx, origin[1] + ry, origin[2] + lz]
        return global_coords, ship

    def _solve_catenary_geometry(self, connection_global, angle_deg_from_vertical, azimuth_deg,
                                  total_length_m, ea_n, w_wet_n_m, friction_coeff=0.5):
        """Solves for the real horizontal span (and resulting anchor/touchdown position) implied
        by the line's real %LINE.CATENARY.ANGLE boundary condition, given the now-known real
        connection position (see _resolve_connection_global()) -- this is the piece
        _synthesize_mesh() was missing: without it, the synthetic mesh's span comes from a
        length-only heuristic (`0.70*L`) with no relation to the real angle/depth/length
        boundary-value problem, which is most of why a pure-.aml run's static T_eff (~1100+ kN
        on Exemplo_01a) is ~5x the real XML+H5 result (~217 kN) -- see docs/roadmap.md.

        The vertical drop (moorpy's `ZF`) is taken as `connection_global[2]` itself, NOT the
        nominal water depth -- the real ANFLEX global frame always has the seabed at Z=0 (see
        _resolve_connection_global()'s own `origin.z = seabed_depth_m - draft` derivation, and
        model_builder.cpp's generic min(Z)=seabed re-derivation), and the connection sits BELOW
        the nominal water surface by the vessel's draft, so `water_depth_m` alone overshoots the
        real vertical drop by that draft (confirmed: using water_depth_m first gave an anchor Z
        of -8m instead of the real ~0m for Exemplo_01a's Cross case -- 13m draft, 5m local Z
        offset, 8m net).

        The real ANFLEX pre-processor solves this exact problem with an external, closed-source
        library (`tec_line`, `interfaces/src/line.cpp`'s `TEC_ANGLE_B` boundary condition) not
        available in this repo. Standard catenary equations are not ANFLEX-proprietary, though:
        this uses moorpy.Catenary.catenary() (XF, ZF, L, EA, W -> tension; already validated
        against real ANFLEX in spikes/mooring_validation/, ~1% error) as the forward "given a
        span, what's the resulting angle" evaluator, wrapped in a small outer bisection on the
        span XF to match the target top angle -- moorpy itself has no direct "solve for span
        given an angle" mode (confirmed: catenary() takes XF as an input, not an unknown it
        solves for).

        The `90 - azimuth_deg` direction convention (mirroring _resolve_connection_global()'s
        own ship-orientation formula) was validated end-to-end against real data: applied to
        Exemplo_01a's real numbers, it reproduces the "Cross" load case's actual exported anchor
        node (XML+H5 path) almost exactly (predicted vs. real anchor position within the
        expected error margin of the ~1%-validated moorpy solver itself).

        Lazily imports moorpy (an optional runtime dependency, see Dockerfile) -- returns None
        if it isn't installed, or if the bisection can't bracket a span matching the target
        angle (e.g. physically unreachable for this length/depth combination), so callers fall
        back to the old heuristic span (see to_risersim_json()/_synthesize_mesh()).
        """
        try:
            from moorpy.Catenary import catenary
        except ImportError:
            return None

        zf = connection_global[2]
        if zf <= 0.0:
            return None

        # interfaces/src/line.cpp's own "90.0 - bc.angle" -- CATENARY.ANGLE is from vertical,
        # moorpy's catenary() tension angle (atan2(VF,HF)) is from horizontal.
        target_angle_deg = 90.0 - angle_deg_from_vertical

        def top_angle_deg(xf):
            try:
                _, _, _, _, info = catenary(xf, zf, total_length_m, ea_n, w_wet_n_m, CB=friction_coeff)
            except Exception:
                return None
            return math.degrees(math.atan2(info['VF'], info['HF']))

        # Coarse scan for a sign change (bracket), then bisection -- more robust than a raw
        # bisection from the domain's ends, since moorpy's solver can fail to converge (raises)
        # for XF values very close to 0 or to the fully-taut limit (total_length_m).
        n_scan = 60
        max_xf = total_length_m * 0.999
        prev_x, prev_err = None, None
        bracket = None
        for i in range(1, n_scan + 1):
            x = max_xf * i / n_scan
            ang = top_angle_deg(x)
            if ang is None:
                continue
            err = ang - target_angle_deg
            if prev_err is not None and prev_err * err <= 0:
                bracket = (prev_x, x)
                break
            prev_x, prev_err = x, err
        if bracket is None:
            return None

        lo, hi = bracket
        lo_err = top_angle_deg(lo) - target_angle_deg
        xf = lo
        for _ in range(60):
            mid = 0.5 * (lo + hi)
            ang = top_angle_deg(mid)
            if ang is None:
                break
            err = ang - target_angle_deg
            xf = mid
            if abs(err) < 1.0e-3:
                break
            if lo_err * err <= 0:
                hi = mid
            else:
                lo, lo_err = mid, err

        try:
            _, _, _, _, info = catenary(xf, zf, total_length_m, ea_n, w_wet_n_m, CB=friction_coeff)
        except Exception:
            return None
        t_eff_top_n = math.hypot(info['HF'], info['VF'])

        direction_rad = math.radians(90.0 - azimuth_deg)
        anchor_global = [
            connection_global[0] + xf * math.cos(direction_rad),
            connection_global[1] + xf * math.sin(direction_rad),
            0.0,  # seabed, in the real ANFLEX global frame this connection_global is also in
        ]
        return anchor_global, t_eff_top_n

    def _parse_function_tables(self):
        """Parses every %TIME_FUNCTION/%DISPLACEMENT_FUNCTION/%ROTATIONAL_FUNCTION block into a
        dict keyed by its real ID, each value a list of (x, y) points -- used to evaluate the
        ramp function backing %TIME_SERIES_LOAD.FUNCTIONS (see _resolve_static_offset()). All
        three share the same "N then N 'x y' lines" POINTS layout.
        """
        tables = {}
        for tag in ('%TIME_FUNCTION', '%DISPLACEMENT_FUNCTION', '%ROTATIONAL_FUNCTION'):
            ids = [d[0] if d else None for d in self.raw_sections_multi.get(f'{tag}.ID', [])]
            points_occ = self.raw_sections_multi.get(f'{tag}.POINTS', [])
            for i, fid_raw in enumerate(ids):
                if fid_raw is None or i >= len(points_occ) or not points_occ[i]:
                    continue
                try:
                    fid = int(float(fid_raw))
                    n = int(float(points_occ[i][0].split()[0]))
                except (ValueError, IndexError):
                    continue
                pts = []
                for j in range(1, min(n + 1, len(points_occ[i]))):
                    parts = points_occ[i][j].split()
                    if len(parts) >= 2:
                        try:
                            pts.append((float(parts[0]), float(parts[1])))
                        except ValueError:
                            pass
                if pts:
                    tables[fid] = pts
        return tables

    @staticmethod
    def _eval_function_table(points, x):
        """Linear interpolation over a (x,y) point list, clamped at the domain's ends -- same
        convention as ANFLEX's own ramp functions (ramp-then-hold, e.g. 'StaTfDef')."""
        if not points:
            return 1.0
        pts = sorted(points, key=lambda p: p[0])
        if x <= pts[0][0]:
            return pts[0][1]
        if x >= pts[-1][0]:
            return pts[-1][1]
        for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
            if x0 <= x <= x1:
                if x1 == x0:
                    return y0
                t = (x - x0) / (x1 - x0)
                return y0 + t * (y1 - y0)
        return pts[-1][1]

    def _resolve_static_offset(self, load_case_id, line):
        """Resolves the real static mean-offset load ("static_offset", part of the RAO's CM
        position -- see vessel_motion.hpp's offset_m doc comment) for one load case, mirroring
        cSaveDatUtil::get_global_static_offset() (interfaces/src/save-dat-util.cpp:1941-2010):
        %FLOATING.LOADS.STATIC's per-load-case %TIME_SERIES_LOAD.AMPLITUDES (6 values, DX/DY/DZ
        + 3 rotational, unused here), each scaled by its own %TIME_SERIES_LOAD.FUNCTIONS ramp
        function evaluated at the real static analysis's total_time (%ANALYSIS_CASE.STATIC.
        TOTAL_TIME, already parsed into self.model_data['analysis']['static']['total_time']),
        then rotated into the global frame by (90 - reference azimuth) if the load's own
        REFERENCE_SYSTEM is LOCAL_FLOATING/LOCAL_LINE (interfaces/src/load.cpp:372-414 -- same
        formula for both, confirmed).

        ASSUMES %FLOATING.LOADS.STATIC's %LOAD_CONTROLLER.LOAD.LCASE_ID blocks come before
        %FLOATING.LOADS.DYNAMIC's own (same tag, reused for both) in the file -- true for every
        real .aml seen so far (fixed STATIC-then-DYNAMIC block order) -- so only the first
        len(%TIME_SERIES_LOAD occurrences) entries of the flat LCASE_ID list are considered.
        Returns [0,0,0] if no matching block is found (e.g. a .aml with no static offset load at
        all) -- never raises.
        """
        ts_occ = self.raw_sections_multi.get('%TIME_SERIES_LOAD', [])
        if not ts_occ:
            return [0.0, 0.0, 0.0]
        lc_id_occ = self.raw_sections_multi.get('%LOAD_CONTROLLER.LOAD.LCASE_ID', [])[:len(ts_occ)]
        idx = None
        for i, d in enumerate(lc_id_occ):
            try:
                if d and int(float(d[0])) == load_case_id:
                    idx = i
                    break
            except (ValueError, IndexError):
                continue
        if idx is None:
            return [0.0, 0.0, 0.0]

        amp_occ = self.raw_sections_multi.get('%TIME_SERIES_LOAD.AMPLITUDES', [])
        func_occ = self.raw_sections_multi.get('%TIME_SERIES_LOAD.FUNCTIONS', [])
        refsys_occ = self.raw_sections_multi.get('%TIME_SERIES_LOAD.REFERENCE_SYSTEM', [])
        refsys_obj_occ = self.raw_sections_multi.get('%TIME_SERIES_LOAD.REFERENCE_SYSTEM.OBJECT', [])
        if idx >= len(amp_occ) or idx >= len(func_occ) or len(amp_occ[idx]) < 6 or len(func_occ[idx]) < 6:
            return [0.0, 0.0, 0.0]

        try:
            amps = [float(v) for v in amp_occ[idx][:6]]
        except ValueError:
            return [0.0, 0.0, 0.0]
        func_ids = []
        for v in func_occ[idx][:6]:
            try:
                func_ids.append(int(float(v.split()[0])))
            except (ValueError, IndexError):
                func_ids.append(None)

        tables = self._parse_function_tables()
        total_time = self.model_data['analysis']['static']['total_time']
        resolved = []
        for amp, fid in zip(amps, func_ids):
            factor = self._eval_function_table(tables.get(fid, []), total_time) if fid is not None else 1.0
            resolved.append(amp * factor)
        dx, dy, dz = resolved[0], resolved[1], resolved[2]

        refsys = refsys_occ[idx][0].strip() if idx < len(refsys_occ) and refsys_occ[idx] else ''
        if refsys in ('LOCAL_FLOATING', 'LOCAL_LINE'):
            obj_id = None
            if idx < len(refsys_obj_occ) and refsys_obj_occ[idx]:
                try:
                    obj_id = int(float(refsys_obj_occ[idx][0]))
                except ValueError:
                    pass
            if refsys == 'LOCAL_LINE':
                ref_azimuth = line.get('azimuth_deg', 0.0)
            else:  # LOCAL_FLOATING
                ship = next((s for s in self._parse_floating_ships() if s['id'] == obj_id), None)
                ref_azimuth = ship['azimuth_deg'] if ship else 0.0
            ang = math.radians(90.0 - ref_azimuth)
            dx, dy = dx * math.cos(ang) - dy * math.sin(ang), dx * math.sin(ang) + dy * math.cos(ang)

        return [dx, dy, dz]

    def _parse_rao_table(self, name=None):
        """Parses the embedded %RAO table (NOT an external file -- '<name>' after the tag is
        just a label, e.g. 'FPSO.RAO'; the whole table is inline in the .aml text). Format
        confirmed against interfaces/src/rao.cpp::read_rao_file and
        libs/anf_movements/include/rao_table.h: a "n_headings n_freq" line, then n_headings
        blocks of "H E A D I N G :   <value>" followed by n_freq rows of 13 numbers each --
        frequency (rad/s), then (amplitude, phase) pairs for surge/sway/heave/roll/pitch/yaw in
        that fixed order. Phases (deg) and rotational amplitudes (deg/m) are kept in their raw
        file units here, unconverted -- same "unit conversion stays in C++ at the point of use"
        philosophy xml_h5_reader.py already follows for phase_deg.

        Returns None if there's no %RAO block (or `name` doesn't match any) -- caller then skips
        vessel_motion entirely.
        """
        dof_names = ['surge', 'sway', 'heave', 'roll', 'pitch', 'yaw']
        for data in self.raw_sections_multi.get('%RAO', []):
            if not data:
                continue
            table_name = data[0].strip("'\"")
            if name is not None and table_name != name:
                continue
            try:
                n_dir, n_wave = [int(v) for v in data[1].split()[:2]]
            except (ValueError, IndexError):
                continue

            headings_deg = []
            frequencies_rad_s = None
            amplitude = {d: [] for d in dof_names}
            phase_deg = {d: [] for d in dof_names}
            idx = 2
            for _ in range(n_dir):
                if idx >= len(data):
                    break
                try:
                    heading_val = float(data[idx].split(':')[-1].strip())
                except (ValueError, IndexError):
                    heading_val = 0.0
                idx += 1
                headings_deg.append(heading_val)
                freqs = []
                row_amp = {d: [] for d in dof_names}
                row_phase = {d: [] for d in dof_names}
                for _ in range(n_wave):
                    if idx >= len(data):
                        break
                    parts = data[idx].split()
                    idx += 1
                    if len(parts) < 13:
                        continue
                    try:
                        vals = [float(v) for v in parts[:13]]
                    except ValueError:
                        continue
                    freqs.append(vals[0])
                    for k, dname in enumerate(dof_names):
                        row_amp[dname].append(vals[1 + 2 * k])
                        row_phase[dname].append(vals[2 + 2 * k])
                if frequencies_rad_s is None:
                    frequencies_rad_s = freqs
                for d in dof_names:
                    amplitude[d].append(row_amp[d])
                    phase_deg[d].append(row_phase[d])

            if not headings_deg or frequencies_rad_s is None:
                continue
            return {
                'headings_deg': headings_deg,
                'frequencies_rad_s': frequencies_rad_s,
                'amplitude': amplitude,
                'phase_deg': phase_deg,
            }
        return None

    def _resolve_vessel_motion(self, load_case, line, top_node_position=(0.0, 0.0, 0.0)):
        """Orchestrates the real RAO+JONSWAP top-motion resolution for one load case, returning
        the SAME shape xml_h5_reader.py::extract_vessel_motion_raw() does (consumed identically
        by model_builder.cpp:333-377) -- or None when the .aml doesn't have real
        %CONNECTION/%FLOATING/%RAO data for this line (e.g. a plain riser-only .aml, or a
        vessel-less example): vessel_motion is then simply left unpopulated, same fallback as
        before this method existed (regular wave, Z only -- model.hpp's doc comment on
        EnvironmentalConfig::vessel_motion).

        `movement_center` is written relative to the GAP between the real connection position
        (`connection_global`) and wherever node 1 (the riser's top) actually ended up in the
        JSON's "model.nodes" (`top_node_position` -- the real global connection position when
        _solve_catenary_geometry() converges, or the synthetic (0,0,0) origin otherwise, the
        default here), NOT relative to `top_node_position` alone -- VesselMotion (C++) computes
        the lever arm as `node->current_coords() - (movement_center + offset)`; for that to
        equal the real physical lever arm `connection_global - (movement_center_real + offset)`
        regardless of where node 1 actually sits, `movement_center` must absorb exactly the gap
        `connection_global - top_node_position` (zero when node 1 already sits at the real
        connection position -- then movement_center is just its real, unmodified value; the
        earlier version of this subtracted `top_node_position` directly instead of the gap,
        which happened to cancel out correctly only in the (0,0,0)-fallback case and produced
        absurd amplitudes (100+m heave) once node 1 started sitting at the real connection
        position -- caught by eyeballing the printed "Vessel motion" line against Cross's
        already-validated 30m heave, not by convergence alone).
        """
        if load_case.get('id') is None:
            return None
        conn_result = self._resolve_connection_global(line)
        if conn_result is None:
            return None
        connection_global, ship = conn_result

        rao = self._parse_rao_table()
        if rao is None:
            return None

        # Real ANFLEX default (REF_GLOBAL, mov_center=(0,0,swl)) -- %MOV_CENTER.COORDINATES.*
        # override not implemented (not present in any real .aml seen so far); see plan's "fora
        # de escopo" note. Confirmed against the real Cross load case's own exported
        # vessel_motion (XML+H5 path, extract_vessel_motion_raw()): movement_center = [0,0,265]
        # -- Z defaults to the still-water level (the real water depth magnitude, this model's
        # global frame having the seabed at Z=0), NOT literally 0 -- an earlier version of this
        # assumed [0,0,0], confirmed wrong by that same real value.
        water_depth_m = self.model_data['global']['seabed_depth_m']
        movement_center_global = [0.0, 0.0, water_depth_m]
        gap = [connection_global[k] - top_node_position[k] for k in range(3)]
        movement_center_rel = [movement_center_global[k] - gap[k] for k in range(3)]

        offset = self._resolve_static_offset(load_case['id'], line)
        # NOT "90 - azimuth" (that formula is real, but for a DIFFERENT real transform --
        # cConnection::global_coord's own rotation, see _resolve_connection_global(), validated
        # separately). The RAO/vessel-motion refsys angle goes through an extra real
        # write-then-reread round trip (interfaces/src/save-dat.cpp writes `90-azimuth`;
        # model_builder_dat.cpp reads it back as `(360-that)`, which nets out to `azimuth-90` --
        # confirmed exactly against the real Cross load case's own exported refsys_angle_deg:
        # 105.0 == 195.0(azimuth) - 90.0, NOT 90-195. An earlier version of this used 90-azimuth
        # here too (wrongly re-using the connection-transform formula for this different real
        # quantity), confirmed wrong by that same real value.
        refsys_angle_deg = ship['azimuth_deg'] - 90.0

        max_dof = 'heave'
        lc_index = next((i for i, lc in enumerate(self.model_data.get('load_cases', []))
                          if lc.get('id') == load_case['id']), None)
        max_dof_occ = self.raw_sections_multi.get('%EQUIVALENT_HARMONIC.MAXIMIZATION', [])
        if lc_index is not None and lc_index < len(max_dof_occ) and max_dof_occ[lc_index]:
            max_dof = max_dof_occ[lc_index][0].strip("'\"").lower()

        wave = self._resolve_wave(load_case)

        print(f"⚓ Movimento de topo real: conexão global ≈ "
              f"({connection_global[0]:.2f}, {connection_global[1]:.2f}, {connection_global[2]:.2f}) m "
              f"| offset estático ≈ ({offset[0]:.2f}, {offset[1]:.2f}, {offset[2]:.2f}) m "
              f"| refsys={refsys_angle_deg:.1f}°")

        return {
            'movement_center': movement_center_rel,
            'offset': offset,
            'refsys_angle_deg': refsys_angle_deg,
            'maximization_dof': max_dof,
            'headings_deg': rao['headings_deg'],
            'frequencies_rad_s': rao['frequencies_rad_s'],
            'amplitude': rao['amplitude'],
            'phase_deg': rao['phase_deg'],
            'jonswap': {
                'alpha': wave.get('alpha', 0.0),
                'gamma': wave.get('gamma', 3.3),
                'period_s': wave.get('period_s', 10.0),
                'wini_rad_s': wave.get('wini_rad_s', 0.2),
                'wfin_rad_s': wave.get('wfin_rad_s', 3.0),
                'nwave': wave.get('nwave', 100),
            },
        }

    # -------------------------------------------------------------------------
    # MESH SYNTHESIS (no pre-solved geometry in a plain .aml, unlike xml_h5_reader.py)
    # -------------------------------------------------------------------------
    def _synthesize_mesh(self, line, depth, span_x, num_elements_fallback, num_elements_override=None,
                          top_override=None, bottom_override=None):
        """Synthesizes an initial straight-line node/element mesh from the line's segment
        lengths -- there's no pre-solved geometry to copy out of a plain .aml the way
        xml_h5_reader.py copies real solved coordinates out of the H5 (see extract_nodes()
        there). Confirmed by reading static_analysis.cpp: the solver treats whatever
        `nodes[i].coords` are given as an initial/reference configuration and deforms it via
        load-stepping Newton-Raphson (an "assembly phase" that ramps up load) to reach the real
        equilibrium catenary shape -- a straight line between anchor and touchdown is an
        adequate starting guess, it does not need to be physically accurate.

        Top anchor at (0, 0, 0) (surface), bottom point at (span_x, 0, -depth) -- the same
        "Surface at Z=0, Seabed at Z=-water_depth" convention already documented in
        to_risersim_config()'s 'geometry' block. `depth` must be the real water depth (positive
        magnitude): model_builder.cpp re-derives the seabed's actual Z-position as the true
        min(Z) among the loaded nodes, so the very last node is forced to land exactly at
        Z=-depth (not just approximately, in case of float drift from the per-segment stepping
        below) for that re-derivation to be correct.

        `top_override`/`bottom_override` (from to_risersim_json(), when a real
        %CONNECTION/%FLOATING + a converged moorpy catenary solve are available -- see
        _resolve_connection_global()/_solve_catenary_geometry()) replace the synthetic
        (0,0,0)/(span_x,0,-depth) endpoints with the REAL global connection and anchor
        positions instead, in the SAME real ANFLEX global frame the XML/H5 path already uses
        (seabed Z~0, surface Z~+depth) -- still just an initial guess for the Newton-Raphson
        solver, but a much better one (real span, not a length-only heuristic), and the one
        _resolve_vessel_motion() needs node 1's position to actually agree with (see its own
        docstring on why movement_center is expressed relative to node 1's position).

        When `line` has real segments (the common case), nodes are distributed by walking each
        segment's own element count in turn (cumulative arc-length), so a segment's elements
        land contiguously instead of being smeared uniformly across the whole line --
        mirrors xml_h5_reader.py's extract_elements() building sequential connectivity, just
        computing the node positions here too since there's no H5 to read them from.
        `num_elements_override` (from run_from_aml.py's --num-elements) replaces the segment
        breakdown with a single uniform mesh of that many elements instead -- there's nothing
        riding on preserving per-segment boundaries today since aml_reader.py only resolves one
        global material for the whole line, not a per-segment one by %LINE.SEGMENT.MATERIAL_ID.
        """
        segments = line.get('segments') or []
        total_length = line.get('total_length_m', 500.0)
        if total_length <= 0:
            total_length = 500.0

        if num_elements_override:
            seg_lengths = [total_length]
            seg_elem_counts = [max(1, int(num_elements_override))]
        elif segments:
            seg_lengths = [s.get('length_m', 0.0) for s in segments]
            seg_elem_counts = [max(1, s.get('num_elements', 1)) for s in segments]
        else:
            seg_lengths = [total_length]
            seg_elem_counts = [max(1, int(num_elements_fallback))]

        top = tuple(top_override) if top_override is not None else (0.0, 0.0, 0.0)
        bottom = tuple(bottom_override) if bottom_override is not None else (span_x, 0.0, -depth)

        nodes = [{"id": 1, "coords": list(top)}]
        node_id = 1
        cum_length = 0.0
        for seg_len, seg_n in zip(seg_lengths, seg_elem_counts):
            step = seg_len / seg_n
            for _ in range(seg_n):
                cum_length += step
                t = min(1.0, cum_length / total_length)
                coords = [top[j] + t * (bottom[j] - top[j]) for j in range(3)]
                node_id += 1
                nodes.append({"id": node_id, "coords": coords})

        nodes[-1]["coords"] = list(bottom)  # exact touchdown/anchor point, no float drift

        elements = []
        for i in range(len(nodes) - 1):
            elements.append({
                "id": i + 1,
                "node1_id": nodes[i]["id"],
                "node2_id": nodes[i + 1]["id"],
            })
        return nodes, elements

    # -------------------------------------------------------------------------
    # STRUCTURED JSON (the schema ModelBuilder::load_from_json actually reads)
    # -------------------------------------------------------------------------
    def to_risersim_json(self, line_index=0, num_elements_override=None, load_case_id=None):
        """Converts the .aml model into the SAME structured JSON schema xml_h5_reader.py's
        to_risersim_json() produces -- the schema ModelBuilder::load_from_json
        (src/model_builder.cpp) actually reads. to_risersim_config() above produces a different,
        older flat schema with no "model"/"nodes"/"elements" at all; ModelBuilder silently
        ignores it (every `j["model"]...contains(...)` check just fails) and the real riser
        model is never loaded -- 23 of the ~30 example .aml files went through exactly this
        silent-failure path before this method existed. See docs/roadmap.md item 2a.

        to_risersim_config() is kept, unmodified, and reused here as an internal building block:
        beam_props, geometry's span_x heuristic, rayleigh and the analysis step counts all carry
        over verbatim (none of that is affected by load-case resolution). "seabed", "current"
        AND "wave" are recomputed here with real ID-matched values instead of
        to_risersim_config()'s own last-occurrence-wins / currents[0] / waves[0] picks --
        current and wave both come from the SAME resolved %LOAD_CASE (_resolve_load_case()),
        since a load case bundles a specific current+wave pairing, not two independent choices.

        `load_case_id` selects which %LOAD_CASE (by its real %LOAD_CASE.ID, e.g. ANFLEX's usual
        'Near'/'Far' pair) to resolve current/wave from -- defaults to the first one in the file
        when not given, same as a plain .aml with no %LOAD_CASE at all (nothing to choose from).
        """
        # Lazy import: xml_h5_reader.py unconditionally imports h5py, which aml_reader.py has
        # otherwise never required (stdlib only -- os/sys/json/math, see the class docstring).
        # Deferring the import to inside this method, mirroring risersim_runner.py's own lazy
        # import of xml_h5_reader inside build_config_from_xml_h5(), keeps a plain `.aml`-only
        # workflow free of the h5py dependency unless this specific method is actually called.
        from xml_h5_reader import SCHEMA_VERSION, extract_assembly_flag, extract_static_convergence_criterium

        flat = self.to_risersim_config(line_index=line_index)
        line = self.model_data['lines'][line_index] if self.model_data['lines'] else {}
        mat = self.model_data['material']
        glb = self.model_data['global']
        depth = flat['geometry']['water_depth_m']
        span_x = flat['geometry']['span_x_m']
        soil = self._resolve_soil_for_line(line)
        load_case = self._resolve_load_case(load_case_id)

        # Real initial mesh endpoints, when resolvable: the connection's real global position
        # (see _resolve_connection_global()) for the top, and a moorpy-solved real anchor
        # position (see _solve_catenary_geometry()) for the bottom -- instead of the synthetic
        # (0,0,0)/heuristic-span_x placeholder _synthesize_mesh() falls back to otherwise. None
        # for any .aml without real %CONNECTION/%FLOATING data, or if moorpy isn't installed/
        # can't converge -- same graceful fallback philosophy as vessel_motion below.
        top_override = None
        bottom_override = None
        conn_result = self._resolve_connection_global(line)
        if conn_result is not None:
            connection_global, _ship = conn_result
            catenary_result = self._solve_catenary_geometry(
                connection_global, line.get('catenary_angle_deg', 5.0), line.get('azimuth_deg', 0.0),
                line.get('total_length_m', 500.0), mat['EA_N'], mat['weight_wet_kNm'] * 1000.0,
                soil.get('friction_lateral', 0.5),
            )
            if catenary_result is not None:
                anchor_global, t_eff_estimate_n = catenary_result
                top_override = connection_global
                bottom_override = anchor_global
                print(f"⚓ Malha inicial: catenária real resolvida (moorpy) -- topo "
                      f"({top_override[0]:.2f}, {top_override[1]:.2f}, {top_override[2]:.2f}) m, "
                      f"âncora ({bottom_override[0]:.2f}, {bottom_override[1]:.2f}, {bottom_override[2]:.2f}) m, "
                      f"T_eff estimado {t_eff_estimate_n / 1000.0:.1f} kN")

        nodes, elements = self._synthesize_mesh(
            line, depth, span_x, flat['geometry']['num_elements'], num_elements_override,
            top_override=top_override, bottom_override=bottom_override,
        )

        section_properties = {
            "E": mat['E_Pa'], "G": mat['G_Pa'], "A": mat['A_m2'],
            "IY": mat['IY_m4'], "IZ": mat['IZ_m4'], "J": mat['J_m4'],
            "EI": mat['EI_Nm2'],
            "weight_wet_kNm": mat['weight_wet_kNm'],
            "rho": mat['rho_kgm3'],
            "D_outer": mat['outer_diameter_m'], "D_inner": mat['inner_diameter_m'],
            "rho_fluid": glb['water_density_kgm3'],
            "Ca": mat['Cm'] - 1.0,
            "Cd": mat['Cd'],
            # "rho_structural" deliberately omitted: the .aml only exposes
            # %MATERIAL.WEIGHT.DRY/.WET (already net of/inclusive of buoyancy), not a distinct
            # raw structural/dry density independent of the wet weight the way the XML's
            # material <density> tag is (xml_h5_reader.extract_material_properties()'s
            # density_kNm3) -- mat['rho_kgm3'] here (dry_mass_kgm/area_struct) is already the
            # right "rho", but there's no second, independently-sourced number to put in
            # "rho_structural" that wouldn't just be relabeling the same one under a different
            # key. Leaving it out and letting the C++ side's documented rho-derived fallback
            # (model_builder.cpp) apply is more honest than passing a value with false
            # provenance.
        }
        for elem in elements:
            elem["section_properties"] = dict(section_properties)

        prescribed_nodes = [{"node_id": 1, "dofs": [-1, -1, -1, -1, -1, -1]}]
        restrained_nodes = [{"node_id": len(nodes), "dofs": [-1, -1, -1, -1, -1, -1]}]

        seabed_dict = {
            "depth_m": -abs(depth),
            "stiffness_Nm": soil['stiffness_Nm'],
            "friction_coeff": soil['friction_lateral'],
            "axial_friction": soil['friction_axial'],
            "lateral_friction": soil['friction_lateral'],
        }
        if soil.get('axial_elastic_deflection_limit') is not None:
            seabed_dict['axial_elastic_deflection_limit'] = soil['axial_elastic_deflection_limit']
        if soil.get('lateral_elastic_deflection_limit') is not None:
            seabed_dict['lateral_elastic_deflection_limit'] = soil['lateral_elastic_deflection_limit']
        if self.model_data.get('soil_model') is not None:
            seabed_dict['soil_model'] = self.model_data['soil_model']

        curr = self._resolve_current(load_case)
        wave = self._resolve_wave(load_case)
        # Real RAO+JONSWAP top motion (vessel + connection offset) -- None for any .aml without
        # a real %CONNECTION/%FLOATING/%RAO for this line (e.g. a plain riser-only file), in
        # which case vessel_motion is simply left out of the JSON below (unchanged fallback:
        # regular wave, Z only). See _resolve_vessel_motion()'s own docstring. top_override (if
        # resolved above) is node 1's REAL coordinate in "model.nodes" -- must match here too,
        # see _resolve_vessel_motion()'s own docstring on why.
        top_node_position = top_override if top_override is not None else (0.0, 0.0, 0.0)
        vessel_motion = self._resolve_vessel_motion(load_case, line, top_node_position=top_node_position)

        def _label(d, id_key='id', name_key='name'):
            name = d.get(name_key)
            ident = d.get(id_key)
            if name:
                return f"'{name}'" + (f" (ID {ident})" if ident is not None else "")
            return f"ID {ident}" if ident is not None else "(padrão, sem ID)"

        print(f"🎯 Caso de carregamento: {_label(load_case)} "
              f"→ Corrente: {_label(curr)} | Onda: {_label(wave)} | Solo: {_label(soil)}")

        current_dict = {
            # AML's %CURRENT.DEPTH is already tabulated ascending-from-surface (0=surface,
            # increasing toward the seabed -- confirmed on real data: Exemplo_01a.aml's current
            # has its highest velocity, 1.02 m/s, at depth 0, decreasing to 0.36 m/s at depth
            # 265 == that file's real water depth), i.e. already the exact convention
            # CurrentProfile::depth_below_surface_m expects (see current_profile.hpp and the
            # long comment in xml_h5_reader.py's extract_current_profile() about how easy this
            # is to get backwards) -- unlike the XML/H5 path, no reordering/inversion is needed
            # here, this is a straight passthrough.
            "depth_below_surface_m": curr.get('depths_m', [0.0, depth]),
            "velocities_ms": curr.get('velocities_ms', [0.0, 0.0]),
            "angles_deg": curr.get('angles_deg', [0.0, 0.0]),
        }

        wave_dict = {
            "type": "JONSWAP" if wave.get('jonswap', True) else "REGULAR",
            "period_s": wave.get('period_s', 10.0),
            "height_m": wave.get('height_m', 2.0),
            "amplitude_m": wave.get('height_m', 2.0) / 2.0,
            "gamma": wave.get('gamma', 3.3),
            "angle_deg": wave.get('angle_deg', 0.0),
        }

        sim_opts = flat['simulation_options']
        static_opts = {
            "steps": sim_opts['static_steps'],
            "max_iterations": sim_opts['static_max_iter'],
            "tolerance": sim_opts['static_tolerance'],
            "vessel_offset": {"near_m": 0.0, "far_m": 0.0},  # nominally zero for a pure catenary
        }
        # extract_assembly_flag/extract_static_convergence_criterium read straight from the
        # .aml/.pml text via regex, independent of ANFLEXXmlH5Reader -- already used for the
        # XML+H5 path (risersim_runner.py::build_config_from_xml_h5()) but not previously wired
        # into the .aml-only path; cheap to add here since self.filepath is already known.
        assembly_flag = extract_assembly_flag(self.filepath)
        if assembly_flag is not None:
            static_opts['use_assembly_phase'] = assembly_flag
        enable_unbalanced, max_unbalanced = extract_static_convergence_criterium(self.filepath)
        if enable_unbalanced:
            static_opts['enable_unbalanced_criteria'] = True
            if max_unbalanced is not None:
                static_opts['unbalanced_force_tol'] = max_unbalanced
                static_opts['unbalanced_moment_tol'] = max_unbalanced

        dynamic_opts = {
            "enabled": True,
            "duration_s": sim_opts['duration_s'],
            "dt_s": sim_opts['dt_s'],
            "max_iterations": sim_opts['dynamic_max_iter'],
            "tolerance": sim_opts['dynamic_tolerance'],
            "rayleigh_damping": {"alpha": flat['rayleigh']['alpha'], "beta": flat['rayleigh']['beta']},
        }

        return {
            "title": self.model_data['title'],
            "schema_version": SCHEMA_VERSION,
            "model": {"nodes": nodes, "elements": elements},
            "boundary_conditions": {
                "prescribed_dofs": prescribed_nodes,
                "restrained_dofs": restrained_nodes,
            },
            "environmental": {
                "seabed": seabed_dict,
                "water_density": glb['water_density_kgm3'],
                "current": current_dict,
                "wave": wave_dict,
            } | ({"vessel_motion": {"enabled": True, **vessel_motion}} if vessel_motion is not None else {}),
            "analysis_options": {
                "static": static_opts,
                "dynamic": dynamic_opts,
            },
        }

    # -------------------------------------------------------------------------
    # OUTPUT
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
        """Saves model_data as JSON. Returns the JSON string if json_path=None."""
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
        # Prints the riserSim configuration to stdout
        cfg = reader.to_risersim_config()
        print("\n--- Configuração riserSim ---")
        print(json.dumps(cfg, indent=2, ensure_ascii=False))
