#include <iostream>
#include <iomanip>
#include <fstream>
#include <string>
#include <cmath>
#include <nlohmann/json.hpp>

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
    double total_span_x = 120.0;
    double seabed_stiffness = 1.0e5;
    double seabed_friction = 0.5;
    double water_density = 1025.0;

    bool enable_offset = true;
    risersim::OffsetMode offset_mode = risersim::OffsetMode::Far;
    double offset_mag = 10.0;

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

    // Tenta carregar JSON se fornecido
    if (!input_json_path.empty()) {
        std::ifstream ifs(input_json_path);
        if (ifs.is_open()) {
            try {
                json j;
                ifs >> j;
                std::cout << "📄 Carregando configuração de entrada: " << input_json_path << std::endl;

                if (j.contains("beam_props")) {
                    auto bp = j["beam_props"];
                    props.E = bp.value("E", props.E);
                    props.G = bp.value("G", props.G);
                    props.A = bp.value("A", props.A);
                    props.D_outer = bp.value("D_outer", props.D_outer);
                    props.D_inner = bp.value("D_inner", props.D_inner);
                    props.rho = bp.value("rho", props.rho);
                    props.rho_fluid = bp.value("rho_fluid", props.rho_fluid);
                    props.Ca = bp.value("Ca", props.Ca);

                    // Rigidez fletora EI real do AML (N.m²)
                    props.EI = bp.value("EI", 21700.0);
                    if (props.EI <= 0.0) props.EI = 21700.0;

                    // IY e IZ geométricos reais da seção
                    double do_m = props.D_outer;
                    double di_m = props.D_inner;
                    double I_geom = M_PI * (std::pow(do_m, 4) - std::pow(di_m, 4)) / 64.0;
                    props.IY = I_geom;
                    props.IZ = I_geom;
                    props.J = 2.0 * I_geom;
                }

                if (j.contains("geometry")) {
                    auto geo = j["geometry"];
                    num_elements = geo.value("num_elements", num_elements);
                    total_length = geo.value("total_length_m", total_length);
                    double water_depth = geo.value("water_depth_m", 100.0);
                    total_depth_z = -std::abs(water_depth);
                    total_span_x = geo.value("span_x_m", total_length * 0.5);
                }

                if (j.contains("seabed")) {
                    auto sb = j["seabed"];
                    total_depth_z = sb.value("depth_m", total_depth_z);
                    seabed_stiffness = sb.value("stiffness_Nm", seabed_stiffness);
                    seabed_friction = sb.value("friction_coeff", seabed_friction);
                }

                water_density = j.value("water_density_kgm3", water_density);

                if (j.contains("offsets")) {
                    auto off = j["offsets"];
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

                if (j.contains("current")) {
                    auto curr = j["current"];
                    auto vels = curr.value("velocities_ms", std::vector<double>{});
                    if (!vels.empty() && vels[0] > 0.0) {
                        enable_current = true;
                        curr_v_surface = vels[0];
                        auto angles = curr.value("angles_deg", std::vector<double>{90.0});
                        if (!angles.empty()) curr_heading = angles[0];
                    }
                }

                if (j.contains("wave")) {
                    auto wave = j["wave"];
                    dyn_wave_period = wave.value("period_s", dyn_wave_period);
                    dyn_wave_amp = wave.value("amplitude_m", wave.value("height_m", 5.0) / 2.0);
                }

                if (j.contains("rayleigh")) {
                    auto ray = j["rayleigh"];
                    dyn_alpha_rayleigh = ray.value("alpha", dyn_alpha_rayleigh);
                    dyn_beta_rayleigh = ray.value("beta", dyn_beta_rayleigh);
                }

                if (j.contains("simulation_options")) {
                    auto opts = j["simulation_options"];
                    run_dynamic = !opts.value("static_only", false);
                    dyn_duration = opts.value("duration_s", dyn_duration);
                    dyn_dt = opts.value("dt_s", dyn_dt);
                }

            } catch (const std::exception& e) {
                std::cerr << "⚠️ Erro ao parsear JSON de entrada: " << e.what() << ". Usando valores padrão." << std::endl;
            }
        } else {
            std::cerr << "⚠️ Não foi possível abrir o arquivo JSON: " << input_json_path << ". Usando configuração de teste padrão." << std::endl;
        }
    }

    // Discretização do riser com trecho suspenso + trecho apoiado no solo (TDZ)
    const int num_nodes = num_elements + 1;
    std::vector<risersim::Node3D*> nodes;
    std::vector<risersim::CorotationalBeam3D*> elements;

    double h_water = std::abs(total_depth_z); // 265.0 m
    double L_total = total_length;            // 500.0 m

    // Comprimento suspenso teórico da catenária no ar S_susp (~310m para h=265m)
    double S_susp = std::min(L_total * 0.70, 310.0);
    double X_tdp = std::sqrt(std::max(1.0, S_susp * S_susp - h_water * h_water));

    for (int i = 0; i < num_nodes; ++i) {
        double s = (static_cast<double>(i) / static_cast<double>(num_elements)) * L_total;
        double x = 0.0;
        double z = 0.0;

        if (s <= S_susp) {
            // Trecho suspenso no ar (curva catenária suave de Z = 0 até Z = -265m)
            double ratio = s / S_susp;
            x = ratio * X_tdp;
            // Equação parabólica inicial suave de catenária suspensa
            z = -h_water * (2.0 * ratio - ratio * ratio);
        } else {
            // Trecho apoiado deitado no solo (Touchdown Zone em Z = -265m)
            double s_seabed = s - S_susp;
            x = X_tdp + s_seabed;
            z = -h_water; // Deitado exatamente sobre o solo
        }

        nodes.push_back(new risersim::Node3D(i + 1, x, 0.0, z));
    }

    nodes.front()->eq_numbers = std::vector<int>(6, -1);
    nodes.back()->eq_numbers = std::vector<int>(6, -1);

    for (size_t i = 1; i < nodes.size() - 1; ++i) {
        // Graus de liberdade ativados para X, Y, Z
        nodes[i]->eq_numbers = {0, 1, 2, -1, -1, -1};
    }

    const double L_unstretched = total_length / static_cast<double>(num_elements);
    for (int i = 0; i < num_elements; ++i) {
        auto* elem = new risersim::CorotationalBeam3D(i + 1, nodes[i], nodes[i + 1], props, L_unstretched);
        elements.push_back(elem);
    }

    // Configuração Estática com contato rígido no solo (Stiffness Penalty = 1e6 N/m^2)
    risersim::StaticAnalysis static_analysis;
    static_analysis.nodes = nodes;
    static_analysis.elements = elements;
    static_analysis.water_density = water_density;
    double penalty_seabed_stiffness = std::max(seabed_stiffness, 1.0e6);
    static_analysis.seabed = risersim::SeabedInteraction(total_depth_z, penalty_seabed_stiffness, seabed_friction);
    static_analysis.load_steps = 20;
    static_analysis.max_iter_per_step = 300;
    static_analysis.tol = 100.0;
    static_analysis.offset = risersim::VesselOffset(offset_mode, offset_mag);
    static_analysis.enable_offset = enable_offset;

    if (enable_current) {
        static_analysis.enable_current = true;
        static_analysis.current = risersim::CurrentProfile(curr_v_surface, total_depth_z, curr_heading, curr_alpha, props.Ca);
    }

    std::cout << "\n--- Executando Análise Estática ---" << std::endl;
    bool success_static = static_analysis.solve();

    if (success_static) {
        std::cout << "✅ Análise Estática Convergida com Sucesso!" << std::endl;
        std::cout << "  [TOPO] X=" << nodes.front()->current_coords().x()
                  << " m, Z=" << nodes.front()->current_coords().z() << " m" << std::endl;
        std::cout << "  [T_eff TOPO] " << (elements.front()->tension_effective / 1000.0) << " kN" << std::endl;
    }

    // Configuração Dinâmica
    risersim::DynamicAnalysis dynamic_analysis(static_analysis);
    bool success_dynamic = true;

    if (run_dynamic) {
        dynamic_analysis.duration_s = dyn_duration;
        dynamic_analysis.dt_s = dyn_dt;
        dynamic_analysis.wave_amplitude = dyn_wave_amp;
        dynamic_analysis.wave_period = dyn_wave_period;
        dynamic_analysis.alpha_rayleigh = dyn_alpha_rayleigh;
        dynamic_analysis.beta_rayleigh = dyn_beta_rayleigh;

        std::cout << "\n--- Executando Análise Dinâmica ---" << std::endl;
        success_dynamic = dynamic_analysis.solve();
        if (success_dynamic) {
            std::cout << "✅ Análise Dinâmica Concluída com Sucesso!" << std::endl;
        }
    }

    // Exportação dos Resultados
    std::string json_out = output_dir + "/catenary_results.json";
    std::string h5_out = output_dir + "/catenary_results.h5";
    risersim::SimulationExporter::export_json(static_analysis, dynamic_analysis, json_out);
    risersim::SimulationExporter::export_hdf5(static_analysis, dynamic_analysis, h5_out);

    std::cout << "\n💾 Resultados exportados para:" << std::endl;
    std::cout << "   " << json_out << std::endl;

    for (auto* elem : elements) delete elem;
    for (auto* node : nodes) delete node;

    return (success_static && success_dynamic) ? 0 : 1;
}
