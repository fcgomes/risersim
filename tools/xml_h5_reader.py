import os
import re
import sys
import xml.etree.ElementTree as ET
import json
import math
import h5py

# Version of the compiled JSON's schema (what to_risersim_json() produces, consumed by
# ModelBuilder::load_from_json in C++) -- bump by hand when the format changes in a way
# ModelBuilder needs to distinguish. Written into the JSON itself below; risersim_projects.py
# reads this field back from the snapshot already copied into the run's directory and writes it
# into run.json, alongside model_hash and web_version -- see docs/roadmap.md, Axis 3b (version
# provenance).
SCHEMA_VERSION = 1


def extract_assembly_flag(aml_path):
    """Reads the real `%ASSEMBLY.USING.TRUE`/`%ASSEMBLY.USING.FALSE` flag from the `.aml`/`.pml`
    text.

    Self-describing AML convention (the keyword already carries the boolean value, no separate
    value line -- e.g. `%ASSEMBLY.USING.FALSE` alone on a line), unlike the two-line key/value
    pattern used by other directives (`%ASSEMBLY.TOTAL_TIME`, etc.).

    This flag only exists in the `.aml`/`.pml`, not in the XML/H5 -- it controls whether the real
    ANFLEX runs an "assembly" phase (artificial stiffness at every step, followed by a separate
    "static" phase) before the real static analysis, or just a single smooth ramp (see
    docs/mapa_classes_anflex_estatica.md). Returns True/False, or None if not found (the JSON is
    left without the field, risersim uses its own safe default).
    """
    if not aml_path or not os.path.isfile(aml_path):
        return None
    with open(aml_path, "r", encoding="latin-1") as f:
        text = f.read()
    m = re.search(r"^%ASSEMBLY\.USING\.(TRUE|FALSE)\s*$", text, re.IGNORECASE | re.MULTILINE)
    if not m:
        return None
    return m.group(1).upper() == "TRUE"


def extract_static_convergence_criterium(aml_path):
    """Reads the real `%ANALYSIS_CASE.STATIC.CONVERGENCE_CRITERIUM`/
    `%ANALYSIS_CASE.STATIC.MAX_UNBALANCED` from the `.aml`/`.pml` text (two-line key/value
    pattern, unlike `extract_assembly_flag`'s self-describing convention).

    The real ANFLEX supports combining the displacement-ratio criterion (always active) with a
    maximum unbalanced force/moment criterion (`'DISP_AND_FORCE'`, with a cap in
    `MAX_UNBALANCED`) -- risersim already has this escape valve implemented
    (`StaticAnalysis::enable_unbalanced_criteria`/`unbalanced_force_tol`/`unbalanced_moment_tol`,
    see docs/mapa_classes_anflex_estatica.md), it just had never been read from the real AML
    before.

    Returns `(enable_unbalanced: bool, max_unbalanced: float | None)`. `enable_unbalanced` is
    `True` when the real criterion includes force (contains `'FORCE'`, covers
    `'DISP_AND_FORCE'` and variants), `False` otherwise or if not found.
    """
    if not aml_path or not os.path.isfile(aml_path):
        return False, None
    with open(aml_path, "r", encoding="latin-1") as f:
        text = f.read()

    crit_m = re.search(r"%ANALYSIS_CASE\.STATIC\.CONVERGENCE_CRITERIUM\s*\n\s*'([^']*)'", text)
    enable_unbalanced = bool(crit_m) and "FORCE" in crit_m.group(1).upper()

    unb_m = re.search(r"%ANALYSIS_CASE\.STATIC\.MAX_UNBALANCED\s*\n\s*([-\d.eE+]+)", text)
    max_unbalanced = float(unb_m.group(1)) if unb_m else None

    return enable_unbalanced, max_unbalanced


class ANFLEXXmlH5Reader:
    def __init__(self, xml_path, h5_path):
        self.xml_path = xml_path
        self.h5_path = h5_path
        self.tree = ET.parse(xml_path)
        self.root = self.tree.getroot()
        self.h5 = h5py.File(h5_path, 'r')
        # The group name under <Groups> varies in case between ANFLEX exports (e.g. Exemplo_01a
        # uses "group1", Exemplo_02a uses "Group1") -- discovered dynamically instead of
        # hardcoded, both in the XML and in the corresponding HDF5 dataset (the same case
        # inconsistency exists in both files). A wrong hardcoded value fails silently (find()
        # returns None) and produces a model with 0 nodes/elements, with no visible error until
        # the C++ binary tries to process it.
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

    def get_optional_float(self, xpath):
        """Like get_float, but returns None (instead of a default) when the XML doesn't have the
        element -- used for fields that should only enter the JSON when actually present in the
        real source, so as not to mask their absence with a "fake" value (see usage in
        extract_data() for the soil's axial/lateral friction)."""
        elem = self.root.find(xpath)
        if elem is None or not elem.text:
            return None
        try:
            return float(elem.text.strip())
        except ValueError:
            return None

    def extract_nodes(self):
        """Reads the nodes directly from the HDF5 dataset specified in the XML."""
        nodes = []
        # Looking up the main mesh's nodes dataset
        nodes_elem = self.root.find(f".//Groups/{self.group_name}/Nodes")
        if nodes_elem is not None:
            h5_dataset_path = f"Groups/{self.group_name}/Nodes"
            if h5_dataset_path in self.h5:
                ds = self.h5[h5_dataset_path]
                for idx in range(len(ds)):
                    row = ds[idx]
                    label = row[0].decode('utf-8').strip()
                    # Stores the node ID (1-based index) and its real coordinates
                    nodes.append({
                        "id": idx + 1,
                        "label": label,
                        "coords": [float(row[1]), float(row[2]), float(row[3])]
                    })
        return nodes

    def extract_elements(self, nodes):
        """Reads and builds element connectivity based on the XML's line segments."""
        elements = []
        label_to_id = {node["label"]: node["id"] for node in nodes}

        # Looks for the line structures
        lines_elem = self.root.find(f".//Groups/{self.group_name}/Elements")
        if lines_elem is not None:
            elem_id_counter = 1
            for line_child in lines_elem:
                line_name = line_child.tag
                # Reads the nodes sequentially to map connectivity
                # In the H5/XML, the group's node list (Nodes) is in the mesh's exact order (catenary)
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
        """Extracts the line's physical and mechanical properties from the XML.

        The real material lives at Groups/<group_name>/Materials/<SegmentName>
        (a dynamic name per segment, e.g. "RISE_L1_seg001"), not under a fixed
        "FlexibleLine" tag. ANFLEX's fields use engineering units
        (kN, kN/m², kN/m³), not pure SI — the conversion is done explicitly
        below.
        """
        mat = None
        materials_group = self.root.find(f".//Groups/{self.group_name}/Materials")
        if materials_group is not None and len(materials_group) > 0:
            mat = materials_group[0]  # Takes the group's first material

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

            density_kNm3 = mf("density", 37.18)                 # structural specific weight (kN/m3)
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

            # Real per-material Rayleigh damping (mass_damping/stiffness_damping already computed
            # by the real ANFLEX from %MATERIAL.RAYLEIGH.PERIOD/DAMPING.FIRST/SECOND in the AML --
            # names that match the real C++'s own, m_mass_damping/m_stiff_damping, see beamSD.cpp).
            # consider_damping="no" (seen in every example available) means the element applies no
            # damping at all, even if the coefficients aren't zeroed out.
            consider_damping_el = mat.find("consider_damping")
            consider_damping = (consider_damping_el is not None and consider_damping_el.text
                                 and consider_damping_el.text.strip().lower() == "yes")
            mass_damping = mf("mass_damping", 0.0)
            stiffness_damping = mf("stiffness_damping", 0.0)

            # Conversion to SI: ANFLEX uses kN / kN/m2 / kN/m3 internally
            E = elasticity_kNm2 * 1000.0                # Pa
            G = E / (2.0 * (1.0 + poisson))              # Pa
            ea_N = E * area
            ei_Nm2 = E * yi
            gj_Nm2 = G * xi

            weight_dry_kNm = density_kNm3 * area
            # Submerged weight = dry weight - buoyancy (external water's specific weight * hydrostatic area)
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
                "external_fluid_density_kgm3": ext_fluid_kNm3 * 1000.0 / 9.81,
                "density_kNm3": density_kNm3,
                "consider_damping": consider_damping,
                "mass_damping": mass_damping,
                "stiffness_damping": stiffness_damping,
            }

        if not material_data:
            # Fallback only for when the XML has no material at all (shouldn't happen)
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
        """Locates the Currents/<Name> element associated with the first loading case
        (LoadingCases/<Case>/current_id), falling back to the XML's first current case if not
        found/no current_id. Shared by extract_current_profile() and extract_current_ramp() --
        the same current case feeds both the profile and the load ramp curve.
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
        """Reads the real current profile associated with the XML's loading case.

        The profile lives at Currents/<Name>/profile/values, as
        "depth|angle|velocity" lines (depth measured from the seabed,
        increasing toward the surface).
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
            # Fallback only for when the XML has no current profile at all
            return [0.0, 265.0], [90.0, 90.0], [1.02, 0.27]

        # "Depth" in the XML is measured from the seabed (0=bottom, increasing toward the
        # surface) -- but CurrentProfile::depth_below_surface_m (current_profile.hpp) expects the
        # OPPOSITE convention: depth from the SURFACE (0=surface), ascending (required by
        # interp1(), which doesn't support a descending table -- see the bug described below).
        # Transforms each point into this convention before storing it, instead of just
        # reordering the indices like the previous version did.
        #
        # Real bug found and fixed (see mapa_classes_anflex_estatica.md): the previous version
        # only reversed the ORDER of the indices (shallowest first), but kept the depth VALUES as
        # "height above the seabed" (0=bottom) -- ending up descending (e.g. [265, 225, ..., 0])
        # instead of ascending. `interp1()` (current_profile.hpp) requires an ascending x_table,
        # and its first check (`x <= x_table.front()`) turns into a short-circuit for ANY query
        # depth when the table is descending -- it always returned the surface point (the
        # profile's real highest velocity), including for nodes resting on the seabed. Since drag
        # force scales with v², this applied up to ~27x more lateral load than the real value
        # right in the touchdown zone (Exemplo_01a: 1.78 m/s vs. the real 0.34 m/s there) -- the
        # same region where soil+friction are already the most delicate.
        total_water_depth = max(depths)  # the tabulated profile's top corresponds to the surface
        order = sorted(range(len(depths)), key=lambda i: depths[i], reverse=True)
        depths = [total_water_depth - depths[i] for i in order]
        angles = [angles[i] for i in order]
        vels = [vels[i] for i in order]

        return depths, angles, vels

    def extract_current_ramp(self):
        """Reads the real current load ramp curve (Currents/<case>/static_function_id
        -> Functions/<Tag>/id -> Functions/<Tag>/points in the HDF5).

        In the real ANFLEX, self-weight is never ramped (m_has_gravitational_load is never
        assigned in the source code -- it always stays at full magnitude), but the current is
        ramped by its own independent curve, typically keeping the current practically at zero on
        the first load step and only growing gradually to the full value on the last step -- see
        mapa_classes_anflex_estatica.md. The X axis is normalized by its own maximum (the domain
        becomes a [0,1] fraction of that curve's total steps), so it can be resampled with
        whatever number of load steps is configured in risersim, without depending on
        static_steps matching the XML's original X exactly.

        Returns (ramp_x, ramp_y); empty lists if the XML has no static_function_id or the
        corresponding function (fallback: risersim uses the same ramp as the weight, the behavior
        before this change).
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

        # Direct child of the root -- only the real function catalog (TempFunc/StaTfDef/DispFunc/...).
        # ".//Functions" would grab the first one in document order, which in practice is a
        # different element with the same name under LoadingCases/.../Loads/Functions
        # (X/Y/Z/RX/RY/RZ, with no relation at all to the current's static_function_id) --
        # confirmed by reading Exemplo_01a's real tree.
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

    def extract_vessel_motion_raw(self):
        """Reads the real (raw, no physics computed) data for the real top movement: the loading
        case's dynamic movement type, RAO->node geometry, the full RAO table (H5), and JONSWAP
        parameters.

        Faithful to the same pattern as `extract_current_profile()`: Python only extracts/
        restructures the real data, without interpolating or computing anything -- the one doing
        the physics (JONSWAP, RAO table interpolation, spectral moments, Rayleigh extremes,
        CM->point geometric transfer) is the C++ (`VesselMotion`, `vessel_motion.hpp`/`.cpp`), in
        the same spirit as `CurrentProfile::get_velocity()` already interpolating the current
        profile there, not here.

        Only supports `dynamic_movement_type == "equivalent_harmonic"` (the real mode used in
        Exemplo_01a and the only one with an algorithm replicated in risersim today -- full
        irregular spectral synthesis, another real ANFLEX mode, is out of scope; see
        docs/mapa_aml_exemplos_e_web_interface.md). Returns `None` when the XML doesn't use this
        mode, doesn't have the `Dynamic`/`Loads` section, or doesn't have a real RAO table
        associated with it -- in these cases the caller falls back to the existing fallback
        (regular wave in Z only).
        """
        loading_cases = self.root.find(".//LoadingCases")
        if loading_cases is None or len(loading_cases) == 0:
            return None
        case = loading_cases[0]  # the same single case used by _find_chosen_current()

        dyn = case.find("Dynamic")
        if dyn is None:
            return None
        movement_type = (dyn.findtext("dynamic_movement_type") or "").strip().lower()
        if movement_type != "equivalent_harmonic":
            return None
        maximization_dof = (dyn.findtext("maximization_dof") or "heave").strip().lower()

        loads = dyn.find("Loads")
        if loads is None or len(loads) == 0:
            return None
        load_node = loads[0]  # first (and only, in the available examples) dynamic load node

        rao_node = load_node.find("Rao")
        if rao_node is None:
            return None

        def read_xyz(parent_tag, elem):
            branch = elem.find(parent_tag)
            if branch is None:
                return [0.0, 0.0, 0.0]
            return [
                float(branch.findtext("X") or 0.0),
                float(branch.findtext("Y") or 0.0),
                float(branch.findtext("Z") or 0.0),
            ]

        movement_center = read_xyz("movement_center", rao_node)
        offset = read_xyz("offset", rao_node)
        refsys_angle_deg = float(load_node.findtext("refsys_angle") or 0.0)
        consider_low_frequency = (load_node.findtext("consider_low_frequency") or "no").strip().lower() == "yes"

        rao_id_text = rao_node.findtext("id")
        if not rao_id_text:
            return None
        try:
            rao_id = int(float(rao_id_text))
        except ValueError:
            return None

        # Finds the real RAO table (RAOs/<Name>) whose <id> matches the load node's Rao/id --
        # the same by-ID resolution pattern already used for the current (_find_chosen_current)
        # and the ramp function (extract_current_ramp).
        raos_root = self.root.find("RAOs")
        if raos_root is None or len(raos_root) == 0:
            return None
        rao_group = None
        for child in raos_root:
            id_el = child.find("id")
            if id_el is not None and id_el.text:
                try:
                    if int(float(id_el.text)) == rao_id:
                        rao_group = child
                        break
                except ValueError:
                    continue
        if rao_group is None:
            return None

        dof_names = ["Surge", "Sway", "Heave", "Roll", "Pitch", "Yaw"]
        headings_deg = []
        frequencies_rad_s = None
        amplitude = {name.lower(): [] for name in dof_names}
        phase_deg = {name.lower(): [] for name in dof_names}

        # Sorts by numeric heading (the child's name, "Heading_015.00", already carries the
        # value; the XML's document order isn't guaranteed to be increasing).
        heading_children = []
        for child in rao_group:
            if not child.tag.startswith("Heading_"):
                continue
            try:
                heading_val = float(child.tag.replace("Heading_", ""))
            except ValueError:
                continue
            heading_children.append((heading_val, child))
        heading_children.sort(key=lambda t: t[0])

        for heading_val, child in heading_children:
            h5_path = f"RAOs/{rao_group.tag}/{child.tag}/amps_phases"
            if h5_path not in self.h5:
                continue
            ds = self.h5[h5_path]
            if frequencies_rad_s is None:
                frequencies_rad_s = [float(v) for v in ds["Wave_Freq"]]
            headings_deg.append(heading_val)
            for name in dof_names:
                amplitude[name.lower()].append([float(v) for v in ds[f"{name}_Amp"]])
                # The H5's real phase comes in DEGREES (verified by reading the file directly) --
                # kept in degrees here on purpose, same philosophy as finding 3
                # (depth_below_surface_m): pure unit conversion stays in the C++ at the point of
                # use, not in Python.
                phase_deg[name.lower()].append([float(v) for v in ds[f"{name}_Phase"]])

        if not headings_deg or frequencies_rad_s is None:
            return None

        # Full real JONSWAP parameters -- period/gamma are also already read in to_risersim_json()
        # for the "wave" block (metadata), but VesselMotion (C++) needs the full JONSWAP spectrum
        # to do its own computation, so they're duplicated here on purpose (same real source, two
        # consumers).
        alpha = self.get_float(".//Waves/ON/alpha", 0.0)
        gamma = self.get_float(".//Waves/ON/gamma", 2.07)
        period_s = self.get_float(".//Waves/ON/period", 10.0)
        wini = self.get_float(".//Waves/ON/wini", 0.2)
        wfin = self.get_float(".//Waves/ON/wfin", 3.0)
        nwave = self.get_int(".//Waves/ON/nwave", 100)

        return {
            "movement_center": movement_center,
            "offset": offset,
            "refsys_angle_deg": refsys_angle_deg,
            "maximization_dof": maximization_dof,
            "consider_low_frequency": consider_low_frequency,
            "headings_deg": headings_deg,
            "frequencies_rad_s": frequencies_rad_s,
            "amplitude": amplitude,
            "phase_deg": phase_deg,
            "jonswap": {
                "alpha": alpha,
                "gamma": gamma,
                "period_s": period_s,
                "wini_rad_s": wini,
                "wfin_rad_s": wfin,
                "nwave": nwave,
            },
        }

    def to_risersim_json(self):
        """Converts the XML+H5 model into the new structured JSON format."""
        title = self.get_text("ModelName", "ANFLEX XML Simulation")

        # 1. Nodes and elements
        nodes = self.extract_nodes()
        elements = self.extract_elements(nodes)
        material = self.extract_material_properties()

        # Associates the material properties with the elements
        weight_wet_kNm = material["weight_wet_kNm"]
        weight_dry_kNm = material["weight_dry_kNm"]
        # rho_structural is the real structural specific weight (`density`, kN/m3) converted to
        # kg/m3 -- a direct unit conversion, without going through weight/area (which would cancel
        # out algebraically anyway: density*area/area). When the XML has no material (synthetic
        # fallback, no "density_kNm3"), falls back to the old value derived from the weight.
        if "density_kNm3" in material:
            rho_structural = material["density_kNm3"] * 1000.0 / 9.81
        else:
            rho_structural = (weight_dry_kNm * 1000.0 / 9.81) / material["A_m2"] if material["A_m2"] > 0 else 3793.35
        # "rho" (used by the static weight formula `w_dry = rho*A*g`) is now the SAME real density
        # as rho_structural, no longer an "equivalent density" fabricated from the weight already
        # net of buoyancy. Previously, `rho` baked in buoyancy (via weight_wet_kNm) and
        # `environmental.water_density` was zeroed out in the C++ (`simulation.cpp`) to avoid
        # subtracting buoyancy twice -- an architecture bug fixed in this session (see
        # mapa_aml_exemplos_e_web_interface.md, finding 2): now buoyancy is always subtracted
        # exactly once by the already-existing generic formula
        # (`w_dry - water_density*outer_area*g`), same as the synthetic path, with the real
        # `water_density` (below) instead of zeroed out.
        rho_dry = rho_structural
        water_density_real = material.get("external_fluid_density_kgm3", 1025.0)

        # Rayleigh damping: uses the material's real values (mass_damping/stiffness_damping,
        # already computed by the real ANFLEX -- see extract_material_properties()) when the XML
        # has consider_damping="yes"; otherwise (seen in every example available:
        # consider_damping="no"), the real model applies no damping at all -- zero is the faithful
        # value, not a "fake" fallback. Without material (synthetic fallback, no
        # "consider_damping" key), keeps the old hardcoded values.
        if "consider_damping" in material:
            if material["consider_damping"]:
                alpha_rayleigh = material["mass_damping"]
                beta_rayleigh = material["stiffness_damping"]
            else:
                alpha_rayleigh = 0.0
                beta_rayleigh = 0.0
        else:
            alpha_rayleigh = 0.05
            beta_rayleigh = 0.005
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
                "rho": rho_dry,
                "rho_structural": rho_structural,
                "D_outer": material["outer_diameter_m"],
                "D_inner": material["inner_diameter_m"],
                "rho_fluid": material.get("internal_fluid_density_kgm3", 1025.0),
                "Ca": material["Cm"] - 1.0,
                "Cd": material.get("Cd", 1.0),
            }

        # 2. Boundary Conditions
        # Top and bottom nodes are identified by restraints in the XML
        prescribed_nodes = []
        restrained_nodes = []

        # The FPSO C1 (usually node 1) has prescribed movements
        prescribed_nodes.append({
            "node_id": 1,
            "dofs": [-1, -1, -1, -1, -1, -1] # Fixed / controlled
        })
        # Bottom anchor (usually the last node)
        restrained_nodes.append({
            "node_id": len(nodes),
            "dofs": [-1, -1, -1, -1, -1, -1]
        })

        # 3. Soil (Soils/Solo/vertical_stiffness and lateral_friction are direct fields,
        # not nested under "spring"/"friction" like in the old XPath)
        soil_stiff_kNm = self.get_float(".//Soils/Solo/vertical_stiffness", 800.0)
        friction_lat = self.get_float(".//Soils/Solo/lateral_friction", 0.95)
        # Real axial/lateral friction and distinct elastic limits (Soils/Solo/axial_friction
        # etc., confirmed in Exemplo_01a_A1.xml:784-800 -- axial and lateral diverge quite a bit
        # from each other, e.g. axial_friction=0.92/axial_elastic_deflection_limit=0.03m vs.
        # lateral_friction=0.95/lateral_elastic_deflection_limit=0.278m). Only written into the
        # JSON if the XML actually has them -- when absent, ModelBuilder already falls back to
        # the isotropic friction_coeff (sentinel -1.0 in EnvironmentalConfig, see model.hpp),
        # exactly the behavior before this change. See
        # docs/mapa_classes_anflex_interface.md.
        soil_axial_friction = self.get_optional_float(".//Soils/Solo/axial_friction")
        soil_lateral_friction = self.get_optional_float(".//Soils/Solo/lateral_friction")
        soil_axial_elastic_limit = self.get_optional_float(".//Soils/Solo/axial_elastic_deflection_limit")
        soil_lateral_elastic_limit = self.get_optional_float(".//Soils/Solo/lateral_elastic_deflection_limit")
        # <coupled>yes|no</coupled> -- the real tag confirmed in the XML (not "soil_model"),
        # mapped to the values ModelBuilder already expects in environmental.seabed.soil_model.
        soil_coupled_text = self.get_text(".//Soils/Solo/coupled", "").strip().lower()
        soil_model = {"yes": "coupled", "no": "uncoupled"}.get(soil_coupled_text)
        # GlobalData/seabed is a reference scalar (not a branch with "depth");
        # the real water depth is GlobalData/seawater_level
        seabed_depth = self.get_float(".//GlobalData/seawater_level", 265.0)

        seabed_dict = {
            "depth_m": -abs(seabed_depth),
            "stiffness_Nm": soil_stiff_kNm * 1000.0,
            "friction_coeff": friction_lat,
        }
        if soil_axial_friction is not None:
            seabed_dict["axial_friction"] = soil_axial_friction
        if soil_lateral_friction is not None:
            seabed_dict["lateral_friction"] = soil_lateral_friction
        if soil_axial_elastic_limit is not None:
            seabed_dict["axial_elastic_deflection_limit"] = soil_axial_elastic_limit
        if soil_lateral_elastic_limit is not None:
            seabed_dict["lateral_elastic_deflection_limit"] = soil_lateral_elastic_limit
        if soil_model is not None:
            seabed_dict["soil_model"] = soil_model

        # 4. Static / Dynamic Analysis Case
        # (num_max_iter, not max_num_iteration; x_tol is the relative displacement
        # tolerance, compatible with the solver's rel_R < tolerance criterion)
        static_total_time = self.get_float(".//AnalysisData/Static/total_time", 11.0)
        static_time_step = self.get_float(".//AnalysisData/Static/time_step", 1.0)
        static_max_iter = self.get_int(".//AnalysisData/Static/num_max_iter", 40)
        static_tol = self.get_float(".//AnalysisData/Static/x_tol", 1.0e-3)

        dyn_total_time = self.get_float(".//AnalysisData/Dynamic/total_time", 1.0)
        dyn_time_step = self.get_float(".//AnalysisData/Dynamic/time_step", 0.05)
        dyn_max_iter = self.get_int(".//AnalysisData/Dynamic/num_max_iter", 20)
        dyn_tol = self.get_float(".//AnalysisData/Dynamic/x_tol", 1.0e-3)

        static_steps = max(1, int(static_total_time / (static_time_step if static_time_step > 0 else 1.0)))

        # 5. Wave (JONSWAP)
        wave_height = self.get_float(".//Waves/ON/height", 6.3)
        wave_period = self.get_float(".//Waves/ON/period", 10.0)
        wave_gamma = self.get_float(".//Waves/ON/gamma", 2.07)
        wave_angle = self.get_float(".//Waves/ON/angle", 0.0)

        # 6. Current (real profile read from Currents/<id>/profile, associated with the
        # first loading case's current_id)
        curr_depths, curr_angles, curr_vels = self.extract_current_profile()
        curr_ramp_x, curr_ramp_y = self.extract_current_ramp()

        # 7. Real top movement (RAO + "equivalent harmonic" JONSWAP) -- None when the XML
        # doesn't use this mode or doesn't have a real RAO table associated with it; in that
        # case "vessel_motion" simply isn't included in the JSON, and the C++ falls back to the
        # existing fallback (regular wave in Z only). See docs/mapa_aml_exemplos_e_web_interface.md.
        vessel_motion_raw = self.extract_vessel_motion_raw()

        # Assembly of the structured JSON
        data = {
            "title": title,
            "schema_version": SCHEMA_VERSION,
            "model": {
                "nodes": [{"id": n["id"], "coords": n["coords"]} for n in nodes],
                "elements": elements
            },
            "boundary_conditions": {
                "prescribed_dofs": prescribed_nodes,
                "restrained_dofs": restrained_nodes
            },
            "environmental": {
                "seabed": seabed_dict,
                # Real external water density (`external_fluid_density` from the material,
                # kg/m3) -- consumed by StaticAnalysis::water_density to subtract buoyancy
                # exactly once from the generic weight formula (see "rho"/"rho_structural"
                # above); this field didn't even exist in the JSON before, and the C++ zeroed
                # out water_density for real JSON as a special case (finding 2 in
                # mapa_aml_exemplos_e_web_interface.md).
                "water_density": water_density_real,
                "current": {
                    "depth_below_surface_m": curr_depths,
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
            } | ({"vessel_motion": {"enabled": True, **vessel_motion_raw}} if vessel_motion_raw is not None else {}),
            "analysis_options": {
                "static": {
                    "steps": static_steps,
                    "max_iterations": static_max_iter,
                    "tolerance": static_tol,
                    "vessel_offset": {
                        "near_m": 0.0, # Always nominally zero for a pure catenary
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
                        "alpha": alpha_rayleigh,
                        "beta": beta_rayleigh
                    }
                }
            }
        }
        return data

# To run directly from the terminal and export the structured JSON
if __name__ == "__main__":
    if len(sys.argv) < 3:
        # Defaults for testing
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
