/**
 * @file main_test.cpp
 * @brief Standalone CLI entry point: loads a structured model JSON (or a synthetic fallback), runs static + dynamic analysis, and exports results.
 */
#include <iostream>
#include <iomanip>
#include <fstream>
#include <string>
#include <cmath>
#include <map>
#include <nlohmann/json.hpp>

#include "risersim/config.hpp"
#include "risersim/node.hpp"
#include "risersim/element_beam.hpp"
#include "risersim/static_analysis.hpp"
#include "risersim/dynamic_analysis.hpp"
#include "risersim/simulation_exporter.hpp"
#include "risersim/hydrodynamics.hpp"
#include "risersim/seabed.hpp"
#include "risersim/buoyancy_and_restrictor.hpp"
#include "risersim/current_profile.hpp"
#include "risersim/vessel_offset.hpp"

using json = nlohmann::json;

int main(int argc, char* argv[]) {
    std::cout << "=========================================================" << std::endl;
    std::cout << "  riserSim - Simulation Engine" << std::endl;
    std::cout << "=========================================================" << std::endl;

    std::string input_json_path = "";
    std::string output_dir = ".";

    if (argc > 1) {
        input_json_path = argv[1];
    }
    if (argc > 2) {
        output_dir = argv[2];
    }

    // Parâmetros relevantes só pro modelo sintético de fallback (usado quando não há JSON
    // válido) -- tudo que vem de um JSON real vive em model->environmental/model->analysis_options
    // (risersim/include/risersim/model.hpp), não em variáveis soltas aqui. Isso é o que permite
    // as outras classes (StaticAnalysis/DynamicAnalysis/SeabedInteraction/CurrentProfile) serem
    // alimentadas a partir de UM lugar só, em vez de ~35 variáveis espalhadas sendo atribuídas
    // uma a uma depois de construídas.
    risersim::BeamMaterialProps props;
    int num_elements = 40;
    double total_length = 180.0;
    double total_span_x = 120.0;

    // Load a structured JSON model, if one was provided
    auto* model = new risersim::RiserModel();
    bool parsed_from_json = false;

    if (!input_json_path.empty()) {
        std::ifstream ifs(input_json_path);
        if (ifs.is_open()) {
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
                        model->add_node(id, coords[0], coords[1], coords[2]);
                    }
                }

                // 2. Load elements
                if (j.contains("model") && j["model"].contains("elements") && !model->nodes.empty()) {
                    auto elems_json = j["model"]["elements"];
                    for (auto& e_j : elems_json) {
                        int id = e_j["id"];
                        int n1_idx = e_j["node1_id"].get<int>() - 1;
                        int n2_idx = e_j["node2_id"].get<int>() - 1;

                        risersim::BeamMaterialProps elem_props;
                        if (e_j.contains("section_properties")) {
                            auto sp = e_j["section_properties"];
                            elem_props.E = sp.value("E", elem_props.E);
                            elem_props.G = sp.value("G", elem_props.G);
                            elem_props.A = sp.value("A", elem_props.A);
                            elem_props.D_outer = sp.value("D_outer", elem_props.D_outer);
                            elem_props.D_inner = sp.value("D_inner", elem_props.D_inner);
                            elem_props.Ca = sp.value("Ca", elem_props.Ca);
                            elem_props.Cd = sp.value("Cd", elem_props.Cd);
                            elem_props.EI = sp.value("EI", 21700.0);

                            // Maps density directly from the XML's real submerged weight, for pure physical consistency
                            double weight_wet_N = 439.5; // Default of 0.4395 kN/m in Newtons
                            if (sp.contains("weight_wet_kNm")) {
                                weight_wet_N = sp.value("weight_wet_kNm", 0.4395) * 1000.0;
                            } else if (sp.contains("rho") && sp.contains("A")) {
                                weight_wet_N = sp.value("rho", elem_props.rho) * sp.value("A", elem_props.A) * 9.81;
                            }

                            // rho: usa o valor real do JSON quando presente (os dois conversores
                            // Python já calculam com a mesma fórmula abaixo, então isso não muda
                            // nada pra JSONs existentes -- só evita re-derivar à toa, e respeita
                            // um `rho` divergente se algum conversor futuro vier a calcular
                            // diferente).
                            double rho_derived = (weight_wet_N / 9.81) / (elem_props.A > 0.0 ? elem_props.A : 0.0282);
                            elem_props.rho = sp.value("rho", rho_derived);

                            // rho_fluid: peso próprio do fluido interno (bore) -- termo real,
                            // distinto do empuxo externo (que já vem líquido embutido em `rho`/
                            // `weight_wet_kNm`, ver water_density abaixo). Antes ficava sempre
                            // zerado; sem esse campo no JSON, mantém 0.0 (sem fluido interno
                            // assumido), mas usa o valor real quando presente.
                            elem_props.rho_fluid = sp.value("rho_fluid", 0.0);

                            // Deriva IY/IZ/J geometricamente só como fallback -- fontes reais via
                            // XML+H5 (xml_h5_reader.py) já carregam IY/IZ possivelmente
                            // assimétricos e J real; fontes AML não têm eixo assimétrico real
                            // (seção sempre tratada como tubo axissimétrico) e caem no mesmo
                            // fallback derivado de antes.
                            double I_eff = elem_props.EI / elem_props.E;
                            elem_props.IY = sp.value("IY", I_eff);
                            elem_props.IZ = sp.value("IZ", I_eff);

                            double do_m = elem_props.D_outer;
                            double di_m = elem_props.D_inner;
                            double J_geom = 2.0 * (std::numbers::pi * (std::pow(do_m, 4) - std::pow(di_m, 4)) / 64.0);
                            elem_props.J = sp.value("J", J_geom);
                        }

                        double L_unstretched = (model->nodes[n2_idx]->coords - model->nodes[n1_idx]->coords).norm();
                        model->add_element(id, model->nodes[n1_idx].get(), model->nodes[n2_idx].get(), elem_props, L_unstretched);
                    }
                }

                // 3. Boundary conditions
                std::vector<bool> is_constrained(model->nodes.size(), false);
                if (j.contains("boundary_conditions")) {
                    auto bc = j["boundary_conditions"];

                    // Prescribed
                    if (bc.contains("prescribed_dofs")) {
                        for (auto& p_d : bc["prescribed_dofs"]) {
                            int n_id = p_d["node_id"].get<int>() - 1;
                            if (n_id >= 0 && n_id < model->nodes.size()) {
                                model->nodes[n_id]->eq_numbers = std::vector<int>(6, -1);
                                is_constrained[n_id] = true;
                            }
                        }
                    }
                    // Restrained
                    if (bc.contains("restrained_dofs")) {
                        for (auto& r_d : bc["restrained_dofs"]) {
                            int n_id = r_d["node_id"].get<int>() - 1;
                            if (n_id >= 0 && n_id < model->nodes.size()) {
                                model->nodes[n_id]->eq_numbers = std::vector<int>(6, -1);
                                is_constrained[n_id] = true;
                            }
                        }
                    }
                }

                // Sets DOFs for free (intermediate) nodes.
                // Node3D always initializes eq_numbers with 6 entries (never empty), so DOF
                // freedom must be tracked via is_constrained, not via .empty()
                for (size_t i = 0; i < model->nodes.size(); ++i) {
                    if (!is_constrained[i]) {
                        model->nodes[i]->eq_numbers = {0, 1, 2, 3, 4, 5}; // Free translations and rotations
                    }
                }

                // 3.5. Warm start (equilibrium geometry computed externally,
                // e.g. MoorPy via risersim/tools/moorpy_warm_start.py) --
                // optional; without this section, behavior is unchanged.
                // Only adjusts `disp` (never `coords`), preserving each
                // element's unstretched length computed above.
                if (j.contains("warm_start") && j["warm_start"].contains("node_positions")) {
                    std::map<int, risersim::Node3D*> node_by_id;
                    for (const auto& node : model->nodes) node_by_id[node->id] = node.get();

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

                // 4. Environmental parameters -> model->environmental (single place other classes
                // pull from below, instead of local variables scattered through this function).
                if (j.contains("environmental")) {
                    auto env = j["environmental"];
                    auto& ec = model->environmental;

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
                        ec.seabed_stiffness = sb.value("stiffness_Nm", ec.seabed_stiffness);
                        ec.seabed_friction = sb.value("friction_coeff", ec.seabed_friction);
                        ec.seabed_axial_friction = sb.value("axial_friction", ec.seabed_axial_friction);
                        ec.seabed_lateral_friction = sb.value("lateral_friction", ec.seabed_lateral_friction);
                        ec.seabed_axial_elastic_limit = sb.value("axial_elastic_deflection_limit", ec.seabed_axial_elastic_limit);
                        ec.seabed_lateral_elastic_limit = sb.value("lateral_elastic_deflection_limit", ec.seabed_lateral_elastic_limit);
                        std::string soil_model_str = sb.value("soil_model", std::string("uncoupled"));
                        ec.soil_model = (soil_model_str == "coupled") ? risersim::SoilModel::Coupled : risersim::SoilModel::Uncoupled;
                        // "depth_m" here comes from the AML's own Z origin, which the min_z
                        // override below deliberately does NOT trust for *position* (see that
                        // comment) -- but its *magnitude* (the total water depth) is still
                        // meaningful, and is what locates the water-surface plane relative to
                        // the real seabed position.
                        water_depth_magnitude = std::abs(sb.value("depth_m", -water_depth_magnitude));
                    }

                    double max_z = -1e9;
                    for (const auto& node : model->nodes) {
                        if (node->coords.z() > max_z) max_z = node->coords.z();
                    }

                    if (ec.seabed_enabled) {
                        // Aligns the seabed with the real minimum Z of the nodes read from the H5
                        double min_z = 1e9;
                        for (const auto& node : model->nodes) {
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
                            ec.current_depths_m = curr.value("depths_m", std::vector<double>{});
                            ec.current_velocities_ms = vels;
                            ec.current_angles_deg = curr.value("angles_deg", std::vector<double>{90.0});
                        }
                    }
                    if (env.contains("wave")) {
                        auto wave = env["wave"];
                        ec.wave_period_s = wave.value("period_s", ec.wave_period_s);
                        ec.wave_amplitude_m = wave.value("amplitude_m", wave.value("height_m", 5.0) / 2.0);
                        ec.wave_angle_deg = wave.value("angle_deg", ec.wave_angle_deg);
                        ec.wave_gamma = wave.value("gamma", ec.wave_gamma);
                        ec.wave_type = wave.value("type", ec.wave_type);
                    }
                }

                // 5. Solver parameters (static / dynamic options) -> model->analysis_options
                if (j.contains("analysis_options")) {
                    auto opts = j["analysis_options"];
                    auto& ao = model->analysis_options;
                    if (opts.contains("static")) {
                        auto st = opts["static"];
                        ao.static_steps = st.value("steps", ao.static_steps);
                        ao.static_max_iterations = st.value("max_iterations", ao.static_max_iterations);
                        ao.static_tolerance = st.value("tolerance", ao.static_tolerance);

                        if (st.contains("vessel_offset")) {
                            auto off = st["vessel_offset"];
                            double near_m = off.value("near_m", 0.0);
                            double far_m = off.value("far_m", 0.0);
                            if (far_m > 0.0) {
                                ao.enable_vessel_offset = true;
                                ao.offset_mode = risersim::OffsetMode::Far;
                                ao.offset_magnitude = far_m;
                            } else if (near_m > 0.0) {
                                ao.enable_vessel_offset = true;
                                ao.offset_mode = risersim::OffsetMode::Near;
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

                parsed_from_json = true;

            } catch (const std::exception& e) {
                std::cerr << "Error parsing structured JSON: " << e.what() << ". Falling back to the default synthetic model." << std::endl;
            }
        }
    }

    // If nothing was loaded from JSON, build a synthetic analytical catenary test geometry (fallback)
    if (!parsed_from_json) {
        std::cout << "Generating default parabolic test geometry..." << std::endl;
        const int num_nodes = num_elements + 1;
        double h_water = std::abs(model->environmental.seabed_depth_z); // 100.0 m (EnvironmentalConfig default)
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
            model->add_node(i + 1, x, 0.0, z);
        }

        model->nodes.front()->eq_numbers = std::vector<int>(6, -1);
        model->nodes.back()->eq_numbers = std::vector<int>(6, -1);

        for (size_t i = 1; i < model->nodes.size() - 1; ++i) {
            model->nodes[i]->eq_numbers = {0, 1, 2, -1, -1, -1};
        }

        const double L_unstretched = total_length / static_cast<double>(num_elements);
        for (int i = 0; i < num_elements; ++i) {
            model->add_element(i + 1, model->nodes[i].get(), model->nodes[i + 1].get(), props, L_unstretched);
        }
    }

    // Static configuration -- from here on, every parameter is read from model->environmental /
    // model->analysis_options (populated above from the real JSON, or left at their struct
    // defaults for the synthetic fallback path), not from local variables.
    risersim::StaticAnalysis static_analysis;
    static_analysis.model = model;
    static_analysis.water_density = parsed_from_json ? 0.0 : model->environmental.water_density;
    static_analysis.water_density_for_mass = 1025.0;  // Always the real value, for added mass
    static_analysis.seabed = risersim::SeabedInteraction(
        model->environmental.seabed_depth_z, model->environmental.seabed_stiffness, model->environmental.seabed_friction);
    static_analysis.seabed.soil_model = model->environmental.soil_model;
    if (model->environmental.seabed_axial_friction > 0.0) static_analysis.seabed.axial_friction = model->environmental.seabed_axial_friction;
    if (model->environmental.seabed_lateral_friction > 0.0) static_analysis.seabed.lateral_friction = model->environmental.seabed_lateral_friction;
    if (model->environmental.seabed_axial_elastic_limit > 0.0) static_analysis.seabed.axial_elastic_deflection_limit = model->environmental.seabed_axial_elastic_limit;
    if (model->environmental.seabed_lateral_elastic_limit > 0.0) static_analysis.seabed.lateral_elastic_deflection_limit = model->environmental.seabed_lateral_elastic_limit;
    static_analysis.load_steps = model->analysis_options.static_steps;
    static_analysis.max_iter_per_step = model->analysis_options.static_max_iterations;
    static_analysis.tol = model->analysis_options.static_tolerance;
    static_analysis.offset = risersim::VesselOffset(model->analysis_options.offset_mode, model->analysis_options.offset_magnitude);
    static_analysis.enable_offset = model->analysis_options.enable_vessel_offset;

    if (model->environmental.current_enabled) {
        static_analysis.enable_current = true;
        const auto& vels = model->environmental.current_velocities_ms;
        const auto& angles = model->environmental.current_angles_deg;
        double v_surface = vels.empty() ? 1.5 : vels.front();
        double heading = angles.empty() ? 90.0 : angles.front();
        // Cd real do elemento (antes reaproveitava Ca de forma incorreta) -- todos os elementos
        // tipicamente compartilham a mesma seção num riser, então o do primeiro serve de default.
        double cd = model->elements.empty() ? 1.0 : model->elements.front()->props.Cd;
        static_analysis.current = risersim::CurrentProfile(v_surface, model->environmental.seabed_depth_z, heading, 0.1428, cd);
        // Perfil tabulado real: com 2+ pontos, CurrentProfile interpola de verdade em vez de usar
        // a lei de potência (que fica só como fallback pro caso raro de 1 ponto só).
        static_analysis.current.set_profile(
            model->environmental.current_depths_m, model->environmental.current_velocities_ms, model->environmental.current_angles_deg);
    }

    std::cout << "\n--- Running Static Analysis ---" << std::endl;
    bool success_static = static_analysis.solve();

    if (success_static) {
        std::cout << "Static Analysis Converged Successfully!" << std::endl;
        std::cout << "  [TOP] X=" << model->nodes.front()->current_coords().x()
                  << " m, Z=" << model->nodes.front()->current_coords().z() << " m" << std::endl;
        std::cout << "  [T_eff TOP] " << (model->elements.front()->tension_effective / 1000.0) << " kN" << std::endl;
    }

    // Dynamic configuration
    risersim::DynamicAnalysis dynamic_analysis(static_analysis);
    bool success_dynamic = true;

    if (model->analysis_options.dynamic_enabled && !success_static) {
        std::cout << "\nSkipping Dynamic Analysis: Static Analysis did not converge." << std::endl;
        success_dynamic = false;
    } else if (model->analysis_options.dynamic_enabled) {
        dynamic_analysis.duration_s = model->analysis_options.dynamic_duration_s;
        dynamic_analysis.dt_s = model->analysis_options.dynamic_dt_s;
        dynamic_analysis.wave_amplitude = model->environmental.wave_amplitude_m;
        dynamic_analysis.wave_period = model->environmental.wave_period_s;
        dynamic_analysis.wave_angle_deg = model->environmental.wave_angle_deg;
        dynamic_analysis.wave_gamma = model->environmental.wave_gamma;
        dynamic_analysis.alpha_rayleigh = model->analysis_options.rayleigh_alpha;
        dynamic_analysis.beta_rayleigh = model->analysis_options.rayleigh_beta;
        dynamic_analysis.max_nr_iters = model->analysis_options.dynamic_max_iterations;
        dynamic_analysis.nr_tolerance = model->analysis_options.dynamic_tolerance;
        dynamic_analysis.stop_on_first_non_convergence = model->analysis_options.stop_on_first_non_convergence;

        std::cout << "\n--- Running Dynamic Analysis ---" << std::endl;
        std::cout << "  Wave: type=" << model->environmental.wave_type << " | angle=" << model->environmental.wave_angle_deg
                  << " deg | gamma=" << model->environmental.wave_gamma << std::endl;
        success_dynamic = dynamic_analysis.solve();
        if (success_dynamic) {
            std::cout << "Dynamic Analysis Completed Successfully!" << std::endl;
        }
    }

    // Results export
    std::string json_out = output_dir + "/catenary_results.json";
    std::string h5_out = output_dir + "/catenary_results.h5";
    risersim::SimulationExporter::export_json(static_analysis, dynamic_analysis, model->environmental.seabed_depth_z, model->environmental.water_surface_z, json_out);
    risersim::SimulationExporter::export_hdf5(static_analysis, dynamic_analysis, model->environmental.seabed_depth_z, model->environmental.water_surface_z, h5_out);

    std::cout << "\nResults exported to:" << std::endl;
    std::cout << "   " << json_out << std::endl;

    delete model;

    return (success_static && success_dynamic) ? 0 : 1;
}
