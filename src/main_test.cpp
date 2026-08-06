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

    risersim::BeamMaterialProps props;
    int num_elements = 40;
    double total_length = 180.0;
    double total_depth_z = -100.0;
    double water_surface_z = 0.0; ///< Z of the sea surface, for the viewer's water-surface plane. Default 0.0 matches the synthetic fallback geometry's convention (top node at z=0).
    double total_span_x = 120.0;
    double seabed_stiffness = 1.0e5;
    double seabed_friction = 0.5;
    // Axial/lateral overrides: absent from most exported JSONs today (they only carry an
    // isotropic "friction_coeff"), so these fall back to seabed_friction/0.05 (the
    // SeabedInteraction default) unless a JSON explicitly provides them -- see
    // environmental.seabed.axial_friction etc below.
    double seabed_axial_friction = -1.0;
    double seabed_lateral_friction = -1.0;
    double seabed_axial_elastic_limit = -1.0;
    double seabed_lateral_elastic_limit = -1.0;
    // "uncoupled" (independent per-axis Coulomb caps, ANFLEX's %SOIL.UNCOUPLED) or "coupled"
    // (combined axial+lateral yield surface, ANFLEX's %OPTION.SOIL.COUPLED) -- see
    // seabed.hpp:SoilModel. Real models pick one explicitly and they are physically different,
    // not interchangeable defaults.
    risersim::SoilModel soil_model = risersim::SoilModel::Uncoupled;
    double water_density = 1025.0;

    int static_steps = 20;
    int static_max_iter = 300;
    double static_tolerance = 0.01;

    bool enable_offset = false;
    risersim::OffsetMode offset_mode = risersim::OffsetMode::Far;
    double offset_mag = 0.0;

    bool enable_current = false;
    double curr_v_surface = 1.5;
    double curr_heading = 90.0;
    double curr_alpha = 0.1428;

    bool run_dynamic = true;
    double dyn_duration = 20.0;
    double dyn_dt = 0.05;
    double dyn_wave_amp = 2.5;
    double dyn_wave_period = 10.0;
    double dyn_alpha_rayleigh = 0.05;
    double dyn_beta_rayleigh = 0.01;
    int dyn_max_nr_iters = 20;
    double dyn_nr_tolerance = 1.0e-4;
    double dyn_wave_angle_deg = 0.0;
    double dyn_wave_gamma = 3.3;
    std::string dyn_wave_type = "regular";
    bool dyn_stop_on_first_non_convergence = false;

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
                            elem_props.EI = sp.value("EI", 21700.0);

                            // Maps density directly from the XML's real submerged weight, for pure physical consistency
                            double weight_wet_N = 439.5; // Default of 0.4395 kN/m in Newtons
                            if (sp.contains("weight_wet_kNm")) {
                                weight_wet_N = sp.value("weight_wet_kNm", 0.4395) * 1000.0;
                            } else if (sp.contains("rho") && sp.contains("A")) {
                                weight_wet_N = sp.value("rho", elem_props.rho) * sp.value("A", elem_props.A) * 9.81;
                            }

                            elem_props.rho = (weight_wet_N / 9.81) / (elem_props.A > 0.0 ? elem_props.A : 0.0282);
                            elem_props.rho_fluid = 0.0; // Net weight already embedded in the submerged weight

                            // Derives effective IY/IZ from the AML's real bending stiffness EI
                            double I_eff = elem_props.EI / elem_props.E;
                            elem_props.IY = I_eff;
                            elem_props.IZ = I_eff;
                            
                            double do_m = elem_props.D_outer;
                            double di_m = elem_props.D_inner;
                            double I_geom = std::numbers::pi * (std::pow(do_m, 4) - std::pow(di_m, 4)) / 64.0;
                            elem_props.J = 2.0 * I_geom;
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

                // 4. Environmental parameters
                if (j.contains("environmental")) {
                    auto env = j["environmental"];
                    // "enabled": false (diagnostic-only escape hatch, matching
                    // diag_isolated_segment.cpp's seabed_mode=0) pushes the seabed far below any
                    // real node instead of aligning it to the model's real minimum Z -- lets a
                    // JSON model be re-run with contact effectively disabled, to isolate whether
                    // the seabed is contributing to a convergence problem (see
                    // mapa_classes_anflex_estatica.md).
                    bool seabed_enabled = true;
                    double water_depth_magnitude = 100.0; // fallback if "seabed" sub-object is absent
                    if (env.contains("seabed")) {
                        auto sb = env["seabed"];
                        seabed_enabled = sb.value("enabled", true);
                        seabed_stiffness = sb.value("stiffness_Nm", seabed_stiffness);
                        seabed_friction = sb.value("friction_coeff", seabed_friction);
                        seabed_axial_friction = sb.value("axial_friction", seabed_axial_friction);
                        seabed_lateral_friction = sb.value("lateral_friction", seabed_lateral_friction);
                        seabed_axial_elastic_limit = sb.value("axial_elastic_deflection_limit", seabed_axial_elastic_limit);
                        seabed_lateral_elastic_limit = sb.value("lateral_elastic_deflection_limit", seabed_lateral_elastic_limit);
                        std::string soil_model_str = sb.value("soil_model", std::string("uncoupled"));
                        soil_model = (soil_model_str == "coupled") ? risersim::SoilModel::Coupled : risersim::SoilModel::Uncoupled;
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

                    if (seabed_enabled) {
                        // Aligns the seabed with the real minimum Z of the nodes read from the H5
                        double min_z = 1e9;
                        for (const auto& node : model->nodes) {
                            if (node->coords.z() < min_z) min_z = node->coords.z();
                        }
                        total_depth_z = min_z;
                        water_surface_z = total_depth_z + water_depth_magnitude;
                        std::cout << "Seabed positioned at the nodes' real Z: " << total_depth_z
                                  << " m | Water surface at: " << water_surface_z << " m" << std::endl;
                    } else {
                        total_depth_z = -1.0e6;
                        // No real seabed reference left to add the water depth to -- approximate
                        // the surface as the model's highest node (a riser's top end is normally
                        // at/near the water surface).
                        water_surface_z = max_z;
                        std::cout << "Seabed DISABLED (environmental.seabed.enabled=false) -- pushed to "
                                  << total_depth_z << " m, no contact possible | Water surface approximated at: "
                                  << water_surface_z << " m" << std::endl;
                    }
                    if (env.contains("current")) {
                        auto curr = env["current"];
                        auto vels = curr.value("velocities_ms", std::vector<double>{});
                        if (!vels.empty() && vels[0] > 0.0) {
                            enable_current = true;
                            curr_v_surface = vels[0];
                            auto angles = curr.value("angles_deg", std::vector<double>{90.0});
                            if (!angles.empty()) curr_heading = angles[0];
                        }
                    }
                    if (env.contains("wave")) {
                        auto wave = env["wave"];
                        dyn_wave_period = wave.value("period_s", dyn_wave_period);
                        dyn_wave_amp = wave.value("amplitude_m", wave.value("height_m", 5.0) / 2.0);
                        dyn_wave_angle_deg = wave.value("angle_deg", dyn_wave_angle_deg);
                        dyn_wave_gamma = wave.value("gamma", dyn_wave_gamma);
                        dyn_wave_type = wave.value("type", dyn_wave_type);
                    }
                }

                // 5. Solver parameters (static / dynamic options)
                if (j.contains("analysis_options")) {
                    auto opts = j["analysis_options"];
                    if (opts.contains("static")) {
                        auto st = opts["static"];
                        static_steps = st.value("steps", static_steps);
                        static_max_iter = st.value("max_iterations", static_max_iter);
                        static_tolerance = st.value("tolerance", static_tolerance);
                        
                        if (st.contains("vessel_offset")) {
                            auto off = st["vessel_offset"];
                            double near_m = off.value("near_m", 0.0);
                            double far_m = off.value("far_m", 0.0);
                            if (far_m > 0.0) {
                                enable_offset = true;
                                offset_mode = risersim::OffsetMode::Far;
                                offset_mag = far_m;
                            } else if (near_m > 0.0) {
                                enable_offset = true;
                                offset_mode = risersim::OffsetMode::Near;
                                offset_mag = near_m;
                            } else {
                                enable_offset = false;
                            }
                        }
                    }
                    if (opts.contains("dynamic")) {
                        auto dy = opts["dynamic"];
                        run_dynamic = dy.value("enabled", true);
                        dyn_duration = dy.value("duration_s", dyn_duration);
                        dyn_dt = dy.value("dt_s", dyn_dt);
                        dyn_max_nr_iters = dy.value("max_iterations", dyn_max_nr_iters);
                        dyn_nr_tolerance = dy.value("tolerance", dyn_nr_tolerance);
                        dyn_stop_on_first_non_convergence = dy.value("stop_on_first_non_convergence", dyn_stop_on_first_non_convergence);
                        
                        if (dy.contains("rayleigh_damping")) {
                            auto ray = dy["rayleigh_damping"];
                            dyn_alpha_rayleigh = ray.value("alpha", dyn_alpha_rayleigh);
                            dyn_beta_rayleigh = ray.value("beta", dyn_beta_rayleigh);
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
        double h_water = std::abs(total_depth_z); // 265.0 m
        double L_total = total_length;            // 500.0 m

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

    // Static configuration
    risersim::StaticAnalysis static_analysis;
    static_analysis.model = model;
    static_analysis.water_density = parsed_from_json ? 0.0 : water_density;
    static_analysis.water_density_for_mass = 1025.0;  // Always the real value, for added mass
    static_analysis.seabed = risersim::SeabedInteraction(total_depth_z, seabed_stiffness, seabed_friction);
    static_analysis.seabed.soil_model = soil_model;
    if (seabed_axial_friction > 0.0) static_analysis.seabed.axial_friction = seabed_axial_friction;
    if (seabed_lateral_friction > 0.0) static_analysis.seabed.lateral_friction = seabed_lateral_friction;
    if (seabed_axial_elastic_limit > 0.0) static_analysis.seabed.axial_elastic_deflection_limit = seabed_axial_elastic_limit;
    if (seabed_lateral_elastic_limit > 0.0) static_analysis.seabed.lateral_elastic_deflection_limit = seabed_lateral_elastic_limit;
    static_analysis.load_steps = static_steps;
    static_analysis.max_iter_per_step = static_max_iter;
    static_analysis.tol = static_tolerance;
    static_analysis.offset = risersim::VesselOffset(offset_mode, offset_mag);
    static_analysis.enable_offset = enable_offset;

    if (enable_current) {
        static_analysis.enable_current = true;
        static_analysis.current = risersim::CurrentProfile(curr_v_surface, total_depth_z, curr_heading, curr_alpha, model->elements.front()->props.Ca);
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

    if (run_dynamic && !success_static) {
        std::cout << "\nSkipping Dynamic Analysis: Static Analysis did not converge." << std::endl;
        success_dynamic = false;
    } else if (run_dynamic) {
        dynamic_analysis.duration_s = dyn_duration;
        dynamic_analysis.dt_s = dyn_dt;
        dynamic_analysis.wave_amplitude = dyn_wave_amp;
        dynamic_analysis.wave_period = dyn_wave_period;
        dynamic_analysis.wave_angle_deg = dyn_wave_angle_deg;
        dynamic_analysis.wave_gamma = dyn_wave_gamma;
        dynamic_analysis.alpha_rayleigh = dyn_alpha_rayleigh;
        dynamic_analysis.beta_rayleigh = dyn_beta_rayleigh;
        dynamic_analysis.max_nr_iters = dyn_max_nr_iters;
        dynamic_analysis.nr_tolerance = dyn_nr_tolerance;
        dynamic_analysis.stop_on_first_non_convergence = dyn_stop_on_first_non_convergence;

        std::cout << "\n--- Running Dynamic Analysis ---" << std::endl;
        std::cout << "  Wave: type=" << dyn_wave_type << " | angle=" << dyn_wave_angle_deg
                  << " deg | gamma=" << dyn_wave_gamma << std::endl;
        success_dynamic = dynamic_analysis.solve();
        if (success_dynamic) {
            std::cout << "Dynamic Analysis Completed Successfully!" << std::endl;
        }
    }

    // Results export
    std::string json_out = output_dir + "/catenary_results.json";
    std::string h5_out = output_dir + "/catenary_results.h5";
    risersim::SimulationExporter::export_json(static_analysis, dynamic_analysis, total_depth_z, water_surface_z, json_out);
    risersim::SimulationExporter::export_hdf5(static_analysis, dynamic_analysis, total_depth_z, water_surface_z, h5_out);

    std::cout << "\nResults exported to:" << std::endl;
    std::cout << "   " << json_out << std::endl;

    delete model;

    return (success_static && success_dynamic) ? 0 : 1;
}
