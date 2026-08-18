#include "risersim/simulation.hpp"
#include "risersim/simulation_exporter.hpp"
#include "risersim/current_profile.hpp"

#include <algorithm>
#include <iostream>

namespace risersim {

void Simulation::load(const std::string& json_path) {
    parsed_from_json = !json_path.empty() && model_builder.load_from_json(json_path);
    model_builder.print_warnings();

    if (!parsed_from_json) {
        std::cerr << "Erro: não foi possível carregar o arquivo de entrada '" << json_path << "'." << std::endl;
    }
}

void Simulation::run() {
    RiserModel* model = model_builder.get_model();

    // Static configuration -- from here on, every parameter is read from model->environmental() /
    // model->analysis_options() (populated by ModelBuilder from the real JSON, or left at their
    // struct defaults for the synthetic fallback path), not from local variables.
    static_analysis.model = model;
    // Empuxo agora sempre subtraído uma única vez pela fórmula genérica de peso (junto com
    // section_properties.rho, sempre a densidade estrutural seca real desde a correção do
    // achado 2 em mapa_aml_exemplos_e_web_interface.md) -- antes este campo era zerado pra
    // modelos reais (JSON parseado) porque o empuxo já vinha pré-subtraído em "rho".
    static_analysis.water_density = model->environmental().water_density;
    static_analysis.water_density_for_mass = 1025.0;  // Always the real value, for added mass
    static_analysis.seabed = SeabedInteraction(
        model->environmental().seabed_depth_z, model->environmental().seabed_stiffness, model->environmental().seabed_friction);
    static_analysis.seabed.set_soil_model(model->environmental().soil_model);
    if (model->environmental().seabed_axial_friction > 0.0) static_analysis.seabed.set_axial_friction(model->environmental().seabed_axial_friction);
    if (model->environmental().seabed_lateral_friction > 0.0) static_analysis.seabed.set_lateral_friction(model->environmental().seabed_lateral_friction);
    if (model->environmental().seabed_axial_elastic_limit > 0.0) static_analysis.seabed.set_axial_elastic_deflection_limit(model->environmental().seabed_axial_elastic_limit);
    if (model->environmental().seabed_lateral_elastic_limit > 0.0) static_analysis.seabed.set_lateral_elastic_deflection_limit(model->environmental().seabed_lateral_elastic_limit);
    static_analysis.load_steps = model->analysis_options().static_steps;
    static_analysis.max_iter_per_step = model->analysis_options().static_max_iterations;
    static_analysis.tol = model->analysis_options().static_tolerance;
    static_analysis.use_assembly_phase = model->analysis_options().static_use_assembly_phase;
    static_analysis.enable_unbalanced_criteria = model->analysis_options().static_enable_unbalanced_criteria;
    static_analysis.unbalanced_force_tol = model->analysis_options().static_unbalanced_force_tol;
    static_analysis.unbalanced_moment_tol = model->analysis_options().static_unbalanced_moment_tol;
    static_analysis.enable_step_limiting = model->analysis_options().static_enable_step_limiting;
    static_analysis.max_translation_step_m = model->analysis_options().static_max_translation_step_m;
    static_analysis.max_rotation_step_rad = model->analysis_options().static_max_rotation_step_rad;
    static_analysis.offset = VesselOffset(model->analysis_options().offset_mode, model->analysis_options().offset_magnitude);
    static_analysis.enable_offset = model->analysis_options().enable_vessel_offset;

    if (model->environmental().current_enabled) {
        static_analysis.enable_current = true;
        const auto& vels = model->environmental().current_velocities_ms;
        const auto& angles = model->environmental().current_angles_deg;
        double v_surface = vels.empty() ? 1.5 : vels.front();
        double heading = angles.empty() ? 90.0 : angles.front();
        // Cd real do elemento (antes reaproveitava Ca de forma incorreta) -- todos os elementos
        // tipicamente compartilham a mesma seção num riser, então o do primeiro serve de default.
        auto* first_beam = model->elements().empty() ? nullptr : dynamic_cast<CorotationalBeam3D*>(model->elements().front().get());
        double cd = first_beam ? first_beam->props().Cd : 1.0;
        static_analysis.current = CurrentProfile(v_surface, model->environmental().seabed_depth_z, heading, 0.1428, cd);
        // Superfície real, não necessariamente Z=0: ModelBuilder alinha seabed_depth_z ao Z real
        // dos nós (model_builder.cpp), que no frame nativo do AML fica perto de 0 no leito e
        // sobe até +water_depth na superfície -- sem isso, get_velocity()/get_heading() (que
        // assumiam superfície em Z=0) nunca viam profundidade real nenhuma, sempre avaliando no
        // ponto de superfície do perfil tabulado (ver mapa_classes_anflex_estatica.md).
        static_analysis.current.set_water_surface_z(model->environmental().water_surface_z);
        // Perfil tabulado real: com 2+ pontos, CurrentProfile interpola de verdade em vez de usar
        // a lei de potência (que fica só como fallback pro caso raro de 1 ponto só).
        static_analysis.current.set_profile(
            model->environmental().current_depth_below_surface_m, model->environmental().current_velocities_ms, model->environmental().current_angles_deg);
    }

    std::cout << "\n--- Running Static Analysis ---" << std::endl;
    success_static = static_analysis.solve();

    if (success_static) {
        std::cout << "Static Analysis Converged Successfully!" << std::endl;
        // Um bloco [TOP]/[T_eff TOP] por linha (mesmo single-line, onde é só uma) -- generalizado
        // de um único model->nodes().front()/elements.front() hardcoded pra suporte multi-linha.
        for (const auto& att : model->resolve_line_attachments()) {
            std::cout << "  [TOP] X=" << att.top_node->current_coords().x()
                      << " m, Z=" << att.top_node->current_coords().z() << " m" << std::endl;
            auto top_elem = std::find_if(model->elements().begin(), model->elements().end(),
                [&](const auto& e) { return e->node(0) == att.top_node || e->node(1) == att.top_node; });
            if (top_elem != model->elements().end()) {
                if (auto* beam = dynamic_cast<CorotationalBeam3D*>(top_elem->get())) {
                    std::cout << "  [T_eff TOP] " << (beam->tension_effective() / 1000.0) << " kN" << std::endl;
                }
            }
        }
    }

    // Dynamic configuration -- constructed here, after the static solve, so it copies a fully
    // resolved model/seabed/current/num_dofs from static_analysis (see DynamicAnalysis(const
    // Analysis&) and the note on this field in simulation.hpp).
    dynamic_analysis = std::make_unique<DynamicAnalysis>(static_analysis);
    success_dynamic = true;

    if (model->analysis_options().dynamic_enabled && !success_static) {
        std::cout << "\nSkipping Dynamic Analysis: Static Analysis did not converge." << std::endl;
        success_dynamic = false;
    } else if (model->analysis_options().dynamic_enabled) {
        dynamic_analysis->duration_s = model->analysis_options().dynamic_duration_s;
        dynamic_analysis->dt_s = model->analysis_options().dynamic_dt_s;
        dynamic_analysis->wave_amplitude = model->environmental().wave_amplitude_m;
        dynamic_analysis->wave_period = model->environmental().wave_period_s;
        dynamic_analysis->wave_angle_deg = model->environmental().wave_angle_deg;
        dynamic_analysis->wave_gamma = model->environmental().wave_gamma;
        // vessel_motions (uma VesselMotion por linha, corpo rígido compartilhado -- suporte
        // multi-linha) é resolvido inteiramente dentro de
        // DynamicAnalysis::solve_time_domain_dynamic(), não aqui -- ela já tem acesso a
        // model/wave_angle_deg (setado logo acima) e é o único lugar que realmente consome essa
        // lista, então não há necessidade de pré-construir e manter alinhado por posição entre
        // dois arquivos. 10800.0 (duração de tormenta padrão de 3h da estatística de Rayleigh,
        // mesmo literal hardcoded do próprio ANFLEX real, model_builder_dat.cpp) fica hardcoded
        // lá também -- ver vessel_motion.hpp.
        dynamic_analysis->alpha_rayleigh = model->analysis_options().rayleigh_alpha;
        dynamic_analysis->beta_rayleigh = model->analysis_options().rayleigh_beta;
        dynamic_analysis->max_nr_iters = model->analysis_options().dynamic_max_iterations;
        dynamic_analysis->nr_tolerance = model->analysis_options().dynamic_tolerance;
        dynamic_analysis->stop_on_first_non_convergence = model->analysis_options().stop_on_first_non_convergence;

        std::cout << "\n--- Running Dynamic Analysis ---" << std::endl;
        std::cout << "  Wave: type=" << model->environmental().wave_type << " | angle=" << model->environmental().wave_angle_deg
                  << " deg | gamma=" << model->environmental().wave_gamma << std::endl;
        success_dynamic = dynamic_analysis->solve();
        if (success_dynamic) {
            std::cout << "Dynamic Analysis Completed Successfully!" << std::endl;
        }
    }
}

void Simulation::export_results(const std::string& output_dir, const std::string& filename) const {
    const RiserModel* model = model_builder.get_model();
    std::string h5_out = output_dir + "/" + filename;
    SimulationExporter::export_hdf5(static_analysis, *dynamic_analysis, model->environmental().seabed_depth_z, model->environmental().water_surface_z, h5_out);

    std::cout << "\nResults exported to:" << std::endl;
    std::cout << "   " << h5_out << std::endl;
}

} // namespace risersim
