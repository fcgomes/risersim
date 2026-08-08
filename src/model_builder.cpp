#include "risersim/model_builder.hpp"
#include "risersim/config.hpp"

#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>

using json = nlohmann::json;

namespace risersim {

namespace {

/// Reads `obj[key]`, falling back to `default_val` and recording a warning when the key is
/// absent. Only for scalar/string fields -- vector-typed and multi-key-fallback defaults are
/// handled by hand at their call sites (their "missing" phrasing doesn't fit this generic form).
template <typename T>
T value_warn(const json& obj, const std::string& key, T default_val, const std::string& field_path,
             std::vector<ModelWarning>& warnings) {
    if (obj.contains(key)) {
        return obj.value(key, default_val);
    }
    std::ostringstream oss;
    oss << field_path << " ausente no JSON -- assumindo " << default_val << ".";
    warnings.push_back({"warning", oss.str()});
    return default_val;
}

} // namespace

BeamMaterialProps parse_beam_section_properties(const json& sp, std::map<std::string, int>* missing_field_counts) {
    auto note_missing = [&](const std::string& field) {
        if (missing_field_counts) (*missing_field_counts)[field]++;
    };

    BeamMaterialProps elem_props;
    if (!sp.contains("E")) note_missing("E");
    elem_props.E = sp.value("E", elem_props.E);
    if (!sp.contains("G")) note_missing("G");
    elem_props.G = sp.value("G", elem_props.G);
    if (!sp.contains("A")) note_missing("A");
    elem_props.A = sp.value("A", elem_props.A);
    if (!sp.contains("D_outer")) note_missing("D_outer");
    elem_props.D_outer = sp.value("D_outer", elem_props.D_outer);
    if (!sp.contains("D_inner")) note_missing("D_inner");
    elem_props.D_inner = sp.value("D_inner", elem_props.D_inner);
    if (!sp.contains("Ca")) note_missing("Ca");
    elem_props.Ca = sp.value("Ca", elem_props.Ca);
    if (!sp.contains("Cd")) note_missing("Cd");
    elem_props.Cd = sp.value("Cd", elem_props.Cd);
    if (!sp.contains("EI")) note_missing("EI");
    elem_props.EI = sp.value("EI", 21700.0);

    // Maps density directly from the XML's real submerged weight, for pure physical consistency
    double weight_wet_N = 439.5; // Default of 0.4395 kN/m in Newtons
    if (sp.contains("weight_wet_kNm")) {
        weight_wet_N = sp.value("weight_wet_kNm", 0.4395) * 1000.0;
    } else if (sp.contains("rho") && sp.contains("A")) {
        weight_wet_N = sp.value("rho", elem_props.rho) * sp.value("A", elem_props.A) * 9.81;
    } else {
        note_missing("weight_wet_kNm");
    }

    // rho: usa o valor real do JSON quando presente (os dois conversores
    // Python já calculam com a mesma fórmula abaixo, então isso não muda
    // nada pra JSONs existentes -- só evita re-derivar à toa, e respeita
    // um `rho` divergente se algum conversor futuro vier a calcular
    // diferente).
    double rho_derived = (weight_wet_N / 9.81) / (elem_props.A > 0.0 ? elem_props.A : 0.0282);
    if (!sp.contains("rho")) note_missing("rho");
    elem_props.rho = sp.value("rho", rho_derived);

    // rho_structural: densidade real de massa estrutural (kg/m^3), para inércia/matriz de massa
    // -- distinta de `rho` acima (equivalente-de-peso-submerso, só para a fórmula de peso
    // estático). Bug real encontrado e corrigido (ver mapa_classes_anflex_estatica.md):
    // total_linear_mass() reaproveitava `rho` (peso submerso já líquido de empuxo) como se fosse
    // massa estrutural, subestimando a massa dinâmica real em ~2,4x pro Exemplo_01a (empuxo
    // reduz peso líquido de verdade, mas nunca reduz massa inercial). Sem esse campo no JSON
    // (formato antigo, antes desta correção), cai de volta em `elem_props.rho` -- preserva o
    // comportamento anterior (com o mesmo bug) em vez de piorar, até o JSON ser regenerado.
    if (!sp.contains("rho_structural")) note_missing("rho_structural");
    elem_props.rho_structural = sp.value("rho_structural", elem_props.rho);

    // rho_fluid: peso próprio do fluido interno (bore) -- termo real,
    // distinto do empuxo externo (que já vem líquido embutido em `rho`/
    // `weight_wet_kNm`, ver water_density no chamador). Antes ficava sempre
    // zerado; sem esse campo no JSON, mantém 0.0 (sem fluido interno
    // assumido), mas usa o valor real quando presente.
    if (!sp.contains("rho_fluid")) note_missing("rho_fluid");
    elem_props.rho_fluid = sp.value("rho_fluid", 0.0);

    // Deriva IY/IZ/J geometricamente só como fallback -- fontes reais via
    // XML+H5 (xml_h5_reader.py) já carregam IY/IZ possivelmente
    // assimétricos e J real; fontes AML não têm eixo assimétrico real
    // (seção sempre tratada como tubo axissimétrico) e caem no mesmo
    // fallback derivado de antes.
    double I_eff = elem_props.EI / elem_props.E;
    if (!sp.contains("IY")) note_missing("IY");
    elem_props.IY = sp.value("IY", I_eff);
    if (!sp.contains("IZ")) note_missing("IZ");
    elem_props.IZ = sp.value("IZ", I_eff);

    double do_m = elem_props.D_outer;
    double di_m = elem_props.D_inner;
    double J_geom = 2.0 * (std::numbers::pi * (std::pow(do_m, 4) - std::pow(di_m, 4)) / 64.0);
    if (!sp.contains("J")) note_missing("J");
    elem_props.J = sp.value("J", J_geom);

    return elem_props;
}

bool ModelBuilder::load_from_json(const std::string& input_json_path) {
    std::ifstream ifs(input_json_path);
    if (!ifs.is_open()) return false;

    try {
        json j;
        ifs >> j;
        std::cout << "Loading structured input configuration: " << input_json_path << std::endl;

        // 1. Load nodes
        if (j.contains("model") && j["model"].contains("nodes")) {
            auto nodes_json = j["model"]["nodes"];
            for (auto& n_j : nodes_json) {
                int id = n_j["id"];
                auto coords = n_j["coords"].get<std::vector<double>>();
                model.add_node(id, coords[0], coords[1], coords[2]);
            }
        }

        // 2. Load elements
        std::map<std::string, int> missing_field_counts;
        int elements_missing_section_properties = 0;
        if (j.contains("model") && j["model"].contains("elements") && !model.nodes.empty()) {
            auto elems_json = j["model"]["elements"];
            for (auto& e_j : elems_json) {
                int id = e_j["id"];
                int n1_idx = e_j["node1_id"].get<int>() - 1;
                int n2_idx = e_j["node2_id"].get<int>() - 1;

                BeamMaterialProps elem_props;
                if (e_j.contains("section_properties")) {
                    elem_props = parse_beam_section_properties(e_j["section_properties"], &missing_field_counts);
                } else {
                    elements_missing_section_properties++;
                }

                double L_unstretched = (model.nodes[n2_idx]->coords - model.nodes[n1_idx]->coords).norm();
                model.add_element(id, model.nodes[n1_idx].get(), model.nodes[n2_idx].get(), elem_props, L_unstretched);
            }
        }
        if (elements_missing_section_properties > 0) {
            std::ostringstream oss;
            oss << elements_missing_section_properties << " elemento(s) sem 'section_properties' no JSON -- usando material genérico padrão (aço, D_outer=0.25m).";
            warnings.push_back({"warning", oss.str()});
        }
        for (const auto& [field, count] : missing_field_counts) {
            std::ostringstream oss;
            oss << count << " elemento(s) com 'section_properties." << field << "' ausente -- assumindo o valor padrão/derivado.";
            warnings.push_back({"warning", oss.str()});
        }

        // 3. Boundary conditions
        std::vector<bool> is_constrained(model.nodes.size(), false);
        if (j.contains("boundary_conditions")) {
            auto bc = j["boundary_conditions"];

            // Prescribed
            if (bc.contains("prescribed_dofs")) {
                for (auto& p_d : bc["prescribed_dofs"]) {
                    int n_id = p_d["node_id"].get<int>() - 1;
                    if (n_id >= 0 && n_id < static_cast<int>(model.nodes.size())) {
                        model.nodes[n_id]->eq_numbers = std::vector<int>(6, -1);
                        is_constrained[n_id] = true;
                    }
                }
            }
            // Restrained
            if (bc.contains("restrained_dofs")) {
                for (auto& r_d : bc["restrained_dofs"]) {
                    int n_id = r_d["node_id"].get<int>() - 1;
                    if (n_id >= 0 && n_id < static_cast<int>(model.nodes.size())) {
                        model.nodes[n_id]->eq_numbers = std::vector<int>(6, -1);
                        is_constrained[n_id] = true;
                    }
                }
            }
        }

        // Sets DOFs for free (intermediate) nodes.
        // Node3D always initializes eq_numbers with 6 entries (never empty), so DOF
        // freedom must be tracked via is_constrained, not via .empty()
        for (size_t i = 0; i < model.nodes.size(); ++i) {
            if (!is_constrained[i]) {
                model.nodes[i]->eq_numbers = {0, 1, 2, 3, 4, 5}; // Free translations and rotations
            }
        }

        // 3.5. Warm start (equilibrium geometry computed externally,
        // e.g. MoorPy via risersim/tools/moorpy_warm_start.py) --
        // optional; without this section, behavior is unchanged.
        // Only adjusts `disp` (never `coords`), preserving each
        // element's unstretched length computed above.
        if (j.contains("warm_start") && j["warm_start"].contains("node_positions")) {
            std::map<int, Node3D*> node_by_id;
            for (const auto& node : model.nodes) node_by_id[node->id] = node.get();

            int warm_applied = 0;
            for (auto& wp : j["warm_start"]["node_positions"]) {
                int n_id = wp["node_id"].get<int>();
                auto coords_w = wp["coords"].get<std::vector<double>>();
                auto it = node_by_id.find(n_id);
                if (it != node_by_id.end()) {
                    Eigen::Vector3d warm_pos(coords_w[0], coords_w[1], coords_w[2]);
                    it->second->disp = warm_pos - it->second->coords;
                    warm_applied++;
                }
            }
            std::cout << "Warm start applied: " << warm_applied << " nodes (source: "
                      << j["warm_start"].value("source", "?") << ")" << std::endl;
        }

        // 4. Environmental parameters -> model.environmental (single place other classes
        // pull from below, instead of local variables scattered through this function).
        if (j.contains("environmental")) {
            auto env = j["environmental"];
            auto& ec = model.environmental;

            // "enabled": false (diagnostic-only escape hatch, matching
            // diag_isolated_segment.cpp's seabed_mode=0) pushes the seabed far below any
            // real node instead of aligning it to the model's real minimum Z -- lets a
            // JSON model be re-run with contact effectively disabled, to isolate whether
            // the seabed is contributing to a convergence problem (see
            // mapa_classes_anflex_estatica.md).
            double water_depth_magnitude = 100.0; // fallback if "seabed" sub-object is absent
            if (env.contains("seabed")) {
                auto sb = env["seabed"];
                ec.seabed_enabled = sb.value("enabled", true);
                ec.seabed_stiffness = value_warn(sb, "stiffness_Nm", ec.seabed_stiffness, "environmental.seabed.stiffness_Nm", warnings);
                ec.seabed_friction = value_warn(sb, "friction_coeff", ec.seabed_friction, "environmental.seabed.friction_coeff", warnings);
                ec.seabed_axial_friction = sb.value("axial_friction", ec.seabed_axial_friction);
                ec.seabed_lateral_friction = sb.value("lateral_friction", ec.seabed_lateral_friction);
                ec.seabed_axial_elastic_limit = sb.value("axial_elastic_deflection_limit", ec.seabed_axial_elastic_limit);
                ec.seabed_lateral_elastic_limit = sb.value("lateral_elastic_deflection_limit", ec.seabed_lateral_elastic_limit);
                std::string soil_model_str = value_warn(sb, "soil_model", std::string("uncoupled"), "environmental.seabed.soil_model", warnings);
                ec.soil_model = (soil_model_str == "coupled") ? SoilModel::Coupled : SoilModel::Uncoupled;
                // "depth_m" here comes from the AML's own Z origin, which the min_z
                // override below deliberately does NOT trust for *position* (see that
                // comment) -- but its *magnitude* (the total water depth) is still
                // meaningful, and is what locates the water-surface plane relative to
                // the real seabed position.
                if (!sb.contains("depth_m")) {
                    warnings.push_back({"warning", "environmental.seabed.depth_m ausente -- assumindo profundidade de água de 100 m."});
                }
                water_depth_magnitude = std::abs(sb.value("depth_m", -water_depth_magnitude));
            } else {
                warnings.push_back({"warning",
                    "environmental.seabed ausente -- usando configuração padrão (habilitado, profundidade=100m, rigidez=1e5 N/m, atrito=0.5)."});
            }

            // Densidade real da água externa, usada por StaticAnalysis::water_density pra
            // subtrair o empuxo da fórmula genérica de peso (junto com section_properties.rho,
            // agora sempre a densidade estrutural seca real -- ver
            // docs/mapa_aml_exemplos_e_web_interface.md, achado 2). Antes este campo nem existia
            // no JSON e o empuxo já vinha pré-subtraído em "rho" pelos conversores Python, exigindo
            // que Simulation::run() zerasse water_density como caso especial pra modelos reais.
            ec.water_density = value_warn(env, "water_density", ec.water_density, "environmental.water_density", warnings);

            double max_z = -1e9;
            for (const auto& node : model.nodes) {
                if (node->coords.z() > max_z) max_z = node->coords.z();
            }

            if (ec.seabed_enabled) {
                // Aligns the seabed with the real minimum Z of the nodes read from the H5
                double min_z = 1e9;
                for (const auto& node : model.nodes) {
                    if (node->coords.z() < min_z) min_z = node->coords.z();
                }
                ec.seabed_depth_z = min_z;
                ec.water_surface_z = ec.seabed_depth_z + water_depth_magnitude;
                std::cout << "Seabed positioned at the nodes' real Z: " << ec.seabed_depth_z
                          << " m | Water surface at: " << ec.water_surface_z << " m" << std::endl;
            } else {
                ec.seabed_depth_z = -1.0e6;
                // No real seabed reference left to add the water depth to -- approximate
                // the surface as the model's highest node (a riser's top end is normally
                // at/near the water surface).
                ec.water_surface_z = max_z;
                std::cout << "Seabed DISABLED (environmental.seabed.enabled=false) -- pushed to "
                          << ec.seabed_depth_z << " m, no contact possible | Water surface approximated at: "
                          << ec.water_surface_z << " m" << std::endl;
            }
            if (env.contains("current")) {
                auto curr = env["current"];
                auto vels = curr.value("velocities_ms", std::vector<double>{});
                if (!vels.empty() && vels[0] > 0.0) {
                    ec.current_enabled = true;
                    // Perfil tabulado completo (não só o primeiro ponto) -- CurrentProfile
                    // interpola de verdade quando há 2+ pontos, ver current_profile.hpp.
                    ec.current_depth_below_surface_m = curr.value("depth_below_surface_m", std::vector<double>{});
                    ec.current_velocities_ms = vels;
                    if (!curr.contains("angles_deg")) {
                        warnings.push_back({"warning", "environmental.current.angles_deg ausente -- assumindo heading de 90 graus."});
                    }
                    ec.current_angles_deg = curr.value("angles_deg", std::vector<double>{90.0});
                }
                // Rampa de carga real da corrente (independente de current_enabled --
                // StaticAnalysis só a consome quando current_enabled é verdadeiro, mas não
                // custa nada guardar se veio no JSON).
                ec.current_ramp_x = curr.value("ramp_x", std::vector<double>{});
                ec.current_ramp_y = curr.value("ramp_y", std::vector<double>{});
            }
            if (env.contains("wave")) {
                auto wave = env["wave"];
                ec.wave_period_s = value_warn(wave, "period_s", ec.wave_period_s, "environmental.wave.period_s", warnings);
                if (wave.contains("amplitude_m")) {
                    ec.wave_amplitude_m = wave.value("amplitude_m", ec.wave_amplitude_m);
                } else if (wave.contains("height_m")) {
                    ec.wave_amplitude_m = wave.value("height_m", 5.0) / 2.0;
                } else {
                    warnings.push_back({"warning", "environmental.wave.amplitude_m e height_m ausentes -- assumindo amplitude de 2.5 m."});
                    ec.wave_amplitude_m = wave.value("height_m", 5.0) / 2.0;
                }
                ec.wave_angle_deg = value_warn(wave, "angle_deg", ec.wave_angle_deg, "environmental.wave.angle_deg", warnings);
                ec.wave_gamma = value_warn(wave, "gamma", ec.wave_gamma, "environmental.wave.gamma", warnings);
                ec.wave_type = value_warn(wave, "type", ec.wave_type, "environmental.wave.type", warnings);
            }
            // Movimento real de topo (RAO + JONSWAP "equivalent harmonic") -- recurso novo e
            // opcional (`.value()` simples, sem value_warn); ausente = fallback já existente
            // (onda regular só em Z), ver dynamic_analysis.cpp.
            if (env.contains("vessel_motion") && env["vessel_motion"].value("enabled", false)) {
                auto vm = env["vessel_motion"];
                auto& vmc = ec.vessel_motion;
                vmc.enabled = true;

                auto center = vm.value("movement_center", std::vector<double>{0.0, 0.0, 0.0});
                auto offset = vm.value("offset", std::vector<double>{0.0, 0.0, 0.0});
                for (int i = 0; i < 3 && i < static_cast<int>(center.size()); ++i) {
                    vmc.cm_position_m[i] = center[i];
                }
                for (int i = 0; i < 3 && i < static_cast<int>(offset.size()); ++i) {
                    vmc.offset_m[i] = offset[i];
                }
                vmc.refsys_angle_deg = vm.value("refsys_angle_deg", 0.0);

                static const std::map<std::string, VesselDof> dof_map = {
                    {"surge", VesselDof::Surge}, {"sway", VesselDof::Sway}, {"heave", VesselDof::Heave},
                    {"roll", VesselDof::Roll}, {"pitch", VesselDof::Pitch}, {"yaw", VesselDof::Yaw},
                };
                std::string max_dof_str = vm.value("maximization_dof", std::string("heave"));
                auto it = dof_map.find(max_dof_str);
                vmc.maximization_dof = (it != dof_map.end()) ? it->second : VesselDof::Heave;

                vmc.headings_deg = vm.value("headings_deg", std::vector<double>{});
                vmc.frequencies_rad_s = vm.value("frequencies_rad_s", std::vector<double>{});

                if (vm.contains("amplitude") && vm.contains("phase_deg")) {
                    auto amp = vm["amplitude"];
                    auto phase = vm["phase_deg"];
                    for (const auto& [name, dof] : dof_map) {
                        vmc.amplitude[static_cast<int>(dof)] = amp.value(name, std::vector<std::vector<double>>{});
                        vmc.phase_deg[static_cast<int>(dof)] = phase.value(name, std::vector<std::vector<double>>{});
                    }
                }

                if (vm.contains("jonswap")) {
                    auto js = vm["jonswap"];
                    vmc.jonswap_alpha = js.value("alpha", 0.0);
                    vmc.jonswap_gamma = js.value("gamma", vmc.jonswap_gamma);
                    vmc.jonswap_period_s = js.value("period_s", vmc.jonswap_period_s);
                    vmc.jonswap_wini_rad_s = js.value("wini_rad_s", vmc.jonswap_wini_rad_s);
                    vmc.jonswap_wfin_rad_s = js.value("wfin_rad_s", vmc.jonswap_wfin_rad_s);
                    vmc.jonswap_nwave = js.value("nwave", vmc.jonswap_nwave);
                }
            }
        }

        // 5. Solver parameters (static / dynamic options) -> model.analysis_options
        // (Solver tuning knobs, not physical/environmental values -- deliberately not
        // warned-about, see the rationale in model_builder.hpp.)
        if (j.contains("analysis_options")) {
            auto opts = j["analysis_options"];
            auto& ao = model.analysis_options;
            if (opts.contains("static")) {
                auto st = opts["static"];
                ao.static_steps = st.value("steps", ao.static_steps);
                ao.static_max_iterations = st.value("max_iterations", ao.static_max_iterations);
                ao.static_tolerance = st.value("tolerance", ao.static_tolerance);
                ao.static_use_assembly_phase = st.value("use_assembly_phase", ao.static_use_assembly_phase);
                ao.static_enable_unbalanced_criteria = st.value("enable_unbalanced_criteria", ao.static_enable_unbalanced_criteria);
                ao.static_unbalanced_force_tol = st.value("unbalanced_force_tol", ao.static_unbalanced_force_tol);
                ao.static_unbalanced_moment_tol = st.value("unbalanced_moment_tol", ao.static_unbalanced_moment_tol);
                ao.static_enable_step_limiting = st.value("enable_step_limiting", ao.static_enable_step_limiting);
                ao.static_max_translation_step_m = st.value("max_translation_step_m", ao.static_max_translation_step_m);
                ao.static_max_rotation_step_rad = st.value("max_rotation_step_rad", ao.static_max_rotation_step_rad);

                if (st.contains("vessel_offset")) {
                    auto off = st["vessel_offset"];
                    double near_m = off.value("near_m", 0.0);
                    double far_m = off.value("far_m", 0.0);
                    if (far_m > 0.0) {
                        ao.enable_vessel_offset = true;
                        ao.offset_mode = OffsetMode::Far;
                        ao.offset_magnitude = far_m;
                    } else if (near_m > 0.0) {
                        ao.enable_vessel_offset = true;
                        ao.offset_mode = OffsetMode::Near;
                        ao.offset_magnitude = near_m;
                    } else {
                        ao.enable_vessel_offset = false;
                    }
                }
            }
            if (opts.contains("dynamic")) {
                auto dy = opts["dynamic"];
                ao.dynamic_enabled = dy.value("enabled", true);
                ao.dynamic_duration_s = dy.value("duration_s", ao.dynamic_duration_s);
                ao.dynamic_dt_s = dy.value("dt_s", ao.dynamic_dt_s);
                ao.dynamic_max_iterations = dy.value("max_iterations", ao.dynamic_max_iterations);
                ao.dynamic_tolerance = dy.value("tolerance", ao.dynamic_tolerance);
                ao.stop_on_first_non_convergence = dy.value("stop_on_first_non_convergence", ao.stop_on_first_non_convergence);

                if (dy.contains("rayleigh_damping")) {
                    auto ray = dy["rayleigh_damping"];
                    ao.rayleigh_alpha = ray.value("alpha", ao.rayleigh_alpha);
                    ao.rayleigh_beta = ray.value("beta", ao.rayleigh_beta);
                }
            }
        }

        return true;

    } catch (const std::exception& e) {
        std::cerr << "Error parsing structured JSON: " << e.what() << ". Falling back to the default synthetic model." << std::endl;
        return false;
    }
}

void ModelBuilder::build_synthetic_fallback(const BeamMaterialProps& props, int num_elements, double total_length) {
    const int num_nodes = num_elements + 1;
    double h_water = std::abs(model.environmental.seabed_depth_z); // 100.0 m (EnvironmentalConfig default)
    double L_total = total_length;            // 180.0 m

    double S_susp = std::min(L_total * 0.70, 310.0);
    double X_tdp = std::sqrt(std::max(1.0, S_susp * S_susp - h_water * h_water));

    for (int i = 0; i < num_nodes; ++i) {
        double s = (static_cast<double>(i) / static_cast<double>(num_elements)) * L_total;
        double x = 0.0;
        double z = 0.0;

        if (s <= S_susp) {
            double ratio = s / S_susp;
            x = ratio * X_tdp;
            z = -h_water * (2.0 * ratio - ratio * ratio);
        } else {
            double s_seabed = s - S_susp;
            x = X_tdp + s_seabed;
            z = -h_water;
        }
        model.add_node(i + 1, x, 0.0, z);
    }

    model.nodes.front()->eq_numbers = std::vector<int>(6, -1);
    model.nodes.back()->eq_numbers = std::vector<int>(6, -1);

    for (size_t i = 1; i < model.nodes.size() - 1; ++i) {
        model.nodes[i]->eq_numbers = {0, 1, 2, -1, -1, -1};
    }

    const double L_unstretched = total_length / static_cast<double>(num_elements);
    for (int i = 0; i < num_elements; ++i) {
        model.add_element(i + 1, model.nodes[i].get(), model.nodes[i + 1].get(), props, L_unstretched);
    }
}

void ModelBuilder::print_warnings() const {
    for (const auto& w : warnings) {
        std::cout << "[" << w.level << "] " << w.message << std::endl;
    }
}

} // namespace risersim
