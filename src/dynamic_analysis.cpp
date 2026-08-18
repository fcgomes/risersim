/**
 * @file dynamic_analysis.cpp
 * @brief DynamicAnalysis: time-domain Newmark-beta integration with per-step Newton-Raphson.
 */
#include "risersim/dynamic_analysis.hpp"
#include "risersim/rotation_utils.hpp"
#include "risersim/hydrostatics.hpp"
#include "risersim/hydrodynamics.hpp"
#include "risersim/config.hpp"
#include "risersim/static_integrator.hpp"
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <cmath>
#include <map>
#include <optional>
#include <functional>

namespace risersim {

bool DynamicAnalysis::solve() {
    return solve_time_domain_dynamic(duration_s, dt_s, wave_amplitude, wave_period, alpha_rayleigh, beta_rayleigh);
}

bool DynamicAnalysis::solve_time_domain_dynamic(double duration, double dt, double amp, double period, double alpha, double beta) {
    duration_s = duration;
    dt_s = dt;
    wave_amplitude = amp;
    wave_period = period;
    alpha_rayleigh = alpha;
    beta_rayleigh = beta;

    std::cout << "\n--- Phase 3: 3D Time-Domain Dynamic Wave Response ---" << std::endl;
    std::cout << "\n=========================================================================" << std::endl;
    std::cout << "  🌊 STARTING 3D TIME-DOMAIN DYNAMIC SOLVER (Newmark-beta)" << std::endl;
    std::cout << "  Duration: " << duration_s << " s | dt: " << dt_s << " s | Wave Amp: " << wave_amplitude << " m | Wave Period: " << wave_period << " s" << std::endl;
    std::cout << "=========================================================================\n" << std::endl;

    if (!model || model->nodes().empty()) return false;

    // Resolve every line's top attachment ONCE, here -- suporte multi-linha (docs/roadmap.md,
    // backlog "múltiplas linhas com corpo flutuante compartilhado"). Um modelo single-line
    // (model->lines() vazio) resolve pra exatamente um attachment, {nodes.front(),
    // environmental.vessel_motion} -- comportamento de hoje, inalterado. `vessel_motions` (membro,
    // dynamic_analysis.hpp) só recebe uma entrada por linha cuja config resolvida tem
    // `enabled=true`; `line_runtimes` abaixo guarda, por linha, um ponteiro pra essa entrada (ou
    // nullptr, fallback senoidal) -- resolvido localmente, nesta função, então não há alinhamento
    // frágil por posição entre duas resoluções separadas.
    auto attachments = model->resolve_line_attachments();
    if (attachments.empty()) return false;

    std::map<const Node3D*, size_t> node_array_index;
    for (size_t i = 0; i < model->nodes().size(); ++i) node_array_index[model->nodes()[i].get()] = i;

    vessel_motions.clear();
    size_t enabled_count = 0;
    for (const auto& att : attachments) if (att.vessel_motion->enabled) ++enabled_count;
    vessel_motions.reserve(enabled_count); // garante que nenhum ponteiro abaixo seja invalidado por realocação

    struct LineRuntime {
        Node3D* top_node;
        size_t node_array_index;
        VesselMotion* motion; ///< nullptr -- essa linha usa o fallback senoidal em Z.
        std::vector<int> saved_eq_numbers;
        PrescribedMotion* prescribed = nullptr; ///< Preenchido logo abaixo, após popular prescribed_motions.
    };
    std::vector<LineRuntime> line_runtimes;
    for (const auto& att : attachments) {
        LineRuntime lr;
        lr.top_node = att.top_node;
        lr.node_array_index = node_array_index.at(att.top_node);
        lr.motion = nullptr;
        if (att.vessel_motion->enabled) {
            vessel_motions.emplace_back(*att.vessel_motion, wave_angle_deg, 10800.0, att.top_node->current_coords());
            lr.motion = &vessel_motions.back();
            std::cout << "  Vessel motion (RAO+JONSWAP equivalent harmonic), nó " << att.top_node->id
                      << ": freq=" << lr.motion->frequency_rad_s()
                      << " rad/s (T=" << (2.0 * std::numbers::pi / lr.motion->frequency_rad_s()) << " s)"
                      << " ramp=" << lr.motion->ramp_time_s() << " s" << std::endl;
            std::cout << "    surge=" << lr.motion->amplitude(VesselDof::Surge) << " m"
                      << " sway=" << lr.motion->amplitude(VesselDof::Sway) << " m"
                      << " heave=" << lr.motion->amplitude(VesselDof::Heave) << " m"
                      << " roll=" << lr.motion->amplitude(VesselDof::Roll) << " rad"
                      << " pitch=" << lr.motion->amplitude(VesselDof::Pitch) << " rad"
                      << " yaw=" << lr.motion->amplitude(VesselDof::Yaw) << " rad" << std::endl;
        }
        line_runtimes.push_back(std::move(lr));
    }

    // Movimento prescrito do topo via mola de penalidade (PrescribedMotion), não mais eliminação
    // de GDL + atribuição direta de disp/rot fora do sistema linear -- mesma técnica que o ANFLEX
    // real usa pra carga imposta (`cLoad`/"big number", `integrator.cpp::set_load_dofs`) e que
    // `StaticAnalysis::solve_vessel_offset` já usa pro offset estático. Confirmado contra o fonte
    // real (`domain.cpp:546-578`, `set_dof_indexes`): só GDL *restrained* (apoio fixo de verdade)
    // perdem a equação; GDL *prescribed* mantêm equação normal e participam da montagem de
    // M/C/K/U/V/A por inteiro -- é isso que garante que o acoplamento inercial do nó de topo com o
    // primeiro nó livre (M_BA·a_topo, via massa consistente do elemento) não seja descartado. A
    // versão anterior deste código eliminava o GDL do nó de topo (`eq_numbers=[-1]*6`) e escrevia
    // `disp`/`rot` direto, o que jogava fora exatamente esse termo -- diagnosticado como a causa
    // provável da divergência do Newton dinâmico mesmo com movimento de topo minúsculo (ver
    // docs/mapa_aml_exemplos_e_web_interface.md).
    //
    // Todos os nós de topo (uma por linha) precisam ter seus eq_numbers resetados ANTES de uma
    // única chamada de assign_equation_numbers() -- não uma renumeração por linha, que invalidaria
    // a numeração já atribuída às linhas processadas antes dela.
    for (auto& lr : line_runtimes) {
        lr.saved_eq_numbers = lr.top_node->eq_numbers;
        lr.top_node->eq_numbers = {0, 1, 2, 3, 4, 5};
    }
    assign_equation_numbers();

    history.clear();

    const int total_steps = static_cast<int>(duration_s / dt_s);
    const double omega = 2.0 * std::numbers::pi / wave_period;

    // Morison hydrodynamic force (drag + inertia) on the submerged line body, from a real
    // JONSWAP irregular sea state -- distinct from vessel_motion's spectral-MOMENT "equivalent
    // harmonic" (used only for the TOP's prescribed motion): this is a genuine time-domain
    // random-phase wave realization (hydrodynamics.hpp), applied directly to every element via
    // Morison's equation, matching real ANFLEX's cMorison. The module was written a while ago
    // but never wired in -- see docs/roadmap.md, Eixo 1b item 3: the Far dynamic solver's
    // negative-tension/slack-cable instability at step 109 (which the real ANFLEX does NOT have,
    // confirmed against a real WSL Fortran run of the same case) was investigated with "the line
    // has zero direct wave loading today, only current drag + the top's RAO motion" identified as
    // a plausible missing-damping/inertia cause. Gated to JONSWAP (the only wave_type with real
    // Hs/gamma spectral data to build a sea state from -- the older regular-wave fallback has
    // none). One realization for the whole run (not regenerated per step), same convention as a
    // real irregular sea state.
    std::optional<AiryWaveKinematics> wave_kinematics;
    const double wave_heading_rad = model->environmental().wave_angle_deg * std::numbers::pi / 180.0;
    if (model->environmental().wave_type == "JONSWAP") {
        double Hs = 2.0 * wave_amplitude; // wave_amplitude_m is height_m/2.0 (see model_builder.cpp)
        double depth = model->environmental().water_surface_z - current.seabed_depth();
        JONSWAPSpectrum spectrum(Hs, wave_period, depth, model->environmental().wave_gamma);
        wave_kinematics.emplace(spectrum.generate_wave_components(), depth);
    }

    // HHT-alpha (Alfa-Method) integration constants, derived from model->analysis_options().hht_alpha
    // (default 0.0 -- plain Newmark average acceleration; real ANFLEX's own default is -0.1,
    // bldanagr3.f:77/bcalfa.f:100-101, but tested directly against the Far load case and found to
    // make its known non-convergence slightly WORSE, not better -- see docs/roadmap.md, Eixo 2a,
    // Atualização 16 -- so defaulted to 0.0 instead; -0.1 remains available via JSON config).
    // gamma=0.5-alpha, beta=0.25*(1-alpha)^2 (bcalfa.f:100-101).
    const double hht_alpha = model->analysis_options().hht_alpha;
    const double gamma_newmark = 0.5 - hht_alpha;
    const double beta_newmark = 0.25 * (1.0 - hht_alpha) * (1.0 - hht_alpha);

    // Save Initial Equilibrium Displacements
    std::vector<Eigen::Vector3d> static_disps;
    std::vector<Eigen::Vector3d> static_rots;
    for (const auto& node : model->nodes()) {
        static_disps.push_back(node->disp);
        static_rots.push_back(node->rot);
    }

    // Dynamic State Vectors (System DOFs)
    Eigen::VectorXd U = Eigen::VectorXd::Zero(num_dofs);
    Eigen::VectorXd V = Eigen::VectorXd::Zero(num_dofs);
    Eigen::VectorXd A = Eigen::VectorXd::Zero(num_dofs);

    // (F_ext - F_int) from the last ACCEPTED state of the previous time step -- the Fortran
    // FORESTAT/FORALFA mechanism (badina.f:1441,1856-1863,1921). Zero initially: the dynamic
    // loop starts from static equilibrium (U=0), so the imbalance there is already ~0.
    Eigen::VectorXd F_static_prev = Eigen::VectorXd::Zero(num_dofs);

    // Uma PrescribedMotion por linha -- reserve() evita realocação enquanto guardamos ponteiros
    // pra dentro do vetor em line_runtimes (mesmo cuidado de vessel_motions acima).
    prescribed_motions.clear();
    prescribed_motions.reserve(line_runtimes.size());
    for (auto& lr : line_runtimes) {
        prescribed_motions.emplace_back(lr.top_node);
        lr.prescribed = &prescribed_motions.back();
        // Todos os 6 GDL, sempre -- inclusive no fallback sem vessel_motion real, onde X/Y/rotação
        // recebem um alvo CONSTANTE (a própria posição estática) em vez de ficarem livres sem
        // nenhuma restrição: reproduz o comportamento antigo (Dirichlet fixo nesses GDL) só que
        // agora via a mesma mola de penalidade, preservando o acoplamento de massa consistente.
        lr.prescribed->dof_active = {true, true, true, true, true, true};
    }

    // Reconciles model->nodes()[*]->disp/rot/tension_effective with a given system-DOF vector --
    // extracted so it can be called both from inside assemble_at() (every Newton trial) AND once
    // from the outer step loop after try_advance() returns, to resync node state with the FINAL
    // committed U when a step exhausted its retry budget without converging (try_advance rewinds
    // the U/V/A Eigen vectors on failure, but the node objects themselves still hold whatever the
    // last, now-superseded trial left them at -- this closes that gap before the snapshot/export
    // step below reads node state).
    auto sync_node_state = [&](const Eigen::VectorXd& U_trial) {
        for (size_t i = 0; i < model->nodes().size(); ++i) {
            auto* node = model->nodes()[i].get();

            Eigen::Vector3d dyn_rot_perturbation = Eigen::Vector3d::Zero();
            bool has_rot_dof = false;
            for (int k = 0; k < 3; ++k) {
                int eq = node->eq_numbers[k];
                if (eq >= 0) {
                    double new_disp_k = static_disps[i][k] + U_trial[eq];
                    // This iteration's increment (for the seabed friction spring).
                    if (k < 2) node->delta_disp_xy[k] = new_disp_k - node->disp[k];
                    node->disp[k] = new_disp_k;
                }

                int eq_rot = node->eq_numbers[k + 3];
                if (eq_rot >= 0) { dyn_rot_perturbation[k] = U_trial[eq_rot]; has_rot_dof = true; }
            }
            // Proper composition of the dynamic perturbation on top of the static base
            // rotation (not a linear sum). See rotation_utils.hpp.
            if (has_rot_dof) node->rot = compose_rotations(static_rots[i], dyn_rot_perturbation);
        }

        // Update Element Effective Tensions based on deformed geometry
        for (const auto& elem : model->elements()) {
            elem->update_effective_tension();
        }
    };

    // Assembles M from the model's CURRENT node state (must be called right after sync_node_state()
    // has positioned the nodes for the state being evaluated) -- extracted so both the top-of-
    // iteration assembly and each backtracking trial can share it, instead of only K/C being
    // recomputed per trial while M silently used the top-of-iteration geometry for every trial.
    // `local_mass_matrix()` (element_beam.cpp) uses `current_length()`, so M is NOT geometry-
    // independent as an earlier version of this comment claimed -- found while investigating the
    // Far dynamic case's negative-tension divergence (docs/roadmap.md, Eixo 1b, Atualização
    // 10/11): the friction-state and C-staleness bugs already fixed here follow the same pattern
    // (a quantity recomputed once per iteration and silently reused across trials whose geometry
    // has since moved).
    auto assemble_mass = [&]() -> Eigen::SparseMatrix<double> {
        Eigen::SparseMatrix<double> M(num_dofs, num_dofs);
        std::vector<Eigen::Triplet<double>> m_triplets;
        for (const auto& elem : model->elements()) {
            Eigen::MatrixXd m_elem = elem->mass_matrix(water_density_for_mass);
            int n_dof = elem->num_nodes() * 6;
            std::vector<int> eq_map(n_dof);
            for (int n = 0; n < elem->num_nodes(); ++n) {
                Node3D* nd = elem->node(n);
                for (int i = 0; i < 6; ++i) eq_map[n * 6 + i] = nd->eq_numbers[i];
            }
            for (int r = 0; r < n_dof; ++r) {
                if (eq_map[r] < 0) continue;
                for (int c = 0; c < n_dof; ++c) {
                    if (eq_map[c] < 0) continue;
                    m_triplets.push_back(Eigen::Triplet<double>(eq_map[r], eq_map[c], m_elem(r, c)));
                }
            }
        }
        M.setFromTriplets(m_triplets.begin(), m_triplets.end());
        return M;
    };

    int step = 0; // captured by reference below (assemble_at's step==1 artificial-stiffness gate)

    // Attempts to advance the dynamic state from t_start to t_start+dt_sub via one Newton solve.
    // On failure, recursively retries via two half-steps -- real ANFLEX's LSTEPITER mechanism
    // (badina.f:2163-2171: rewinds TIME/DESL/VEL/ACEL to the last converged state, halves the
    // step, reprocesses), confirmed present in the real dynamic driver but OFF in the reference
    // run used throughout this investigation (docs/roadmap.md, Eixo 2a, Atualização 16) -- so
    // unlike HHT-alpha, this isn't a replay of what made the reference run robust, it's a genuine
    // (real, not invented) robustness mechanism being tried on its own merits. Bounded by
    // model->analysis_options().dynamic_max_step_halvings (0 = no retry, immediate failure,
    // matching pre-retry behavior exactly). Returns true with U/V/A/F_static_prev committed to
    // t_start+dt_sub; false with U/V/A/F_static_prev rewound to their value on entry (state
    // unchanged from the caller's perspective, so a parent-level retry starts clean).
    std::function<bool(double, double, int)> try_advance = [&](double t_start, double dt_sub, int depth) -> bool {
        Eigen::VectorXd U_before = U, V_before = V, A_before = A, F_static_prev_before = F_static_prev;
        double time = t_start + dt_sub;

        const double c1 = 1.0 / (beta_newmark * dt_sub * dt_sub);

        // Prescribe Top Vessel Motion: real RAO+JONSWAP "equivalent harmonic" (6 DOFs) quando a
        // linha tem dado real pra isso (vessel_motion.hpp), senão o antigo fallback senoidal em Z
        // com a mesma rampa suave de 5s -- aplicado independentemente por linha, via o alvo da
        // mola de penalidade (setup acima), não uma atribuição direta de disp/rot.
        for (auto& lr : line_runtimes) {
            const Eigen::Vector3d& base_disp = static_disps[lr.node_array_index];
            const Eigen::Vector3d& base_rot = static_rots[lr.node_array_index];
            if (lr.motion) {
                Eigen::Vector3d vessel_disp, vessel_rot;
                lr.motion->get_motion(time, vessel_disp, vessel_rot);
                lr.prescribed->target_disp = base_disp + vessel_disp;
                // Componente a componente, NÃO compose_rotations() -- confirmado contra o
                // mecanismo real de movimento prescrito (`integrator.cpp::set_load_dofs`,
                // `presc_desl[i] += movements[i]` pra i=0..5, sem distinção entre translação/
                // rotação): o ANFLEX real soma o vetor de rotação do harmônico equivalente direto
                // em cima da referência estática, sem compor via Rodrigues/quaternion. Faz
                // sentido com o próprio método: "equivalent harmonic" já é linearizado do início
                // ao fim (RAO é resposta linear em frequência), então uma composição não-linear
                // aqui introduziria uma não-linearidade que o método de referência não tem.
                // `compose_rotations` continua correto pro ESTADO realmente resolvido pelo Newton
                // (abaixo, agora também pro nó de topo) -- esse é outro mecanismo do ANFLEX real
                // (`nMathUtils::pseudo_sum`, `integrator.cpp:697`), aplicado ao alvo vs. ao
                // estado, não ao mesmo lugar.
                lr.prescribed->target_rot = base_rot + vessel_rot;
            } else {
                double ramp_time = 5.0;
                double ramp_factor = (time < ramp_time) ? 0.5 * (1.0 - std::cos(std::numbers::pi * time / ramp_time)) : 1.0;
                double line_disp_z = ramp_factor * wave_amplitude * std::sin(omega * time);
                lr.prescribed->target_disp = base_disp + Eigen::Vector3d(0.0, 0.0, line_disp_z);
                lr.prescribed->target_rot = base_rot; // alvo constante -- rotação do topo nunca foi dinamicamente imposta sem vessel_motion real
            }
        }

        // Save Previous State Vectors for Newmark Integration
        const Eigen::VectorXd& U_prev = U_before;
        const Eigen::VectorXd& V_prev = V_before;
        const Eigen::VectorXd& A_prev = A_before;

        // Standard Newmark-beta Predictor step
        Eigen::VectorXd A_curr = Eigen::VectorXd::Zero(num_dofs);
        Eigen::VectorXd V_curr = V_prev + dt_sub * (1.0 - gamma_newmark) * A_prev;
        Eigen::VectorXd U_curr = U_prev + dt_sub * V_prev + 0.5 * dt_sub * dt_sub * (1.0 - 2.0 * beta_newmark) * A_prev;

        // Assembles node state + F_ext + K_global/F_int for a TRIAL dynamic-DOF vector U_trial --
        // extracted so both the main per-iteration assembly below AND the backtracking line
        // search (docs/roadmap.md item 1b) can share it instead of duplicating ~70 lines.
        struct AssembledState {
            Eigen::VectorXd F_ext;
            Eigen::SparseMatrix<double> K_global;
            Eigen::VectorXd F_int;
        };
        auto assemble_at = [&](const Eigen::VectorXd& U_trial, int iter) -> AssembledState {
            sync_node_state(U_trial);

            AssembledState st;

            // 1. External Load Vector F_ext
            st.F_ext = Eigen::VectorXd::Zero(num_dofs);
            for (const auto& elem_base : model->elements()) {
                // Weight/buoyancy/current/Morison loading is beam-specific -- see the same
                // treatment/comment in static_integrator.cpp::assemble_load_vector().
                auto* elem = dynamic_cast<CorotationalBeam3D*>(elem_base.get());
                if (!elem) continue;

                double L = elem->initial_length();
                double g = 9.81;

                double w_dry = (elem->props().rho * elem->props().A + elem->props().rho_fluid * elem->inner_area()) * g;
                // Submersion-scaled buoyancy (see hydrostatics.hpp, docs/roadmap.md item 1b) --
                // this is the fix for the divergence traced to a node crossing the water surface
                // mid-simulation: buoyancy used to be a CONSTANT per element regardless of Z,
                // with no associated tangent stiffness, so Newton had no restoring gradient right
                // at the crossing. Re-evaluated every Newton iteration (this whole F_ext block
                // already re-runs per iteration), so it tracks the node's CURRENT position, not
                // just its position at the start of the time step.
                double zc[2] = {elem->node1()->current_coords().z(), elem->node2()->current_coords().z()};
                Hydrostatics hydro(elem->props().D_outer, L, water_density);
                hydro.compute(zc, model->environmental().water_surface_z);

                int eq1_z = elem->node1()->eq_numbers[2];
                int eq2_z = elem->node2()->eq_numbers[2];

                if (eq1_z >= 0) st.F_ext[eq1_z] += hydro.end_force(0) * g - 0.5 * w_dry * L;
                if (eq2_z >= 0) st.F_ext[eq2_z] += hydro.end_force(1) * g - 0.5 * w_dry * L;

                if (enable_current) {
                    double avg_z = 0.5 * (elem->node1()->current_coords().z() + elem->node2()->current_coords().z());
                    Eigen::Vector3d cur_elem_axis = (elem->node2()->current_coords() - elem->node1()->current_coords()).normalized();
                    Eigen::Vector3d f_drag = current.get_drag_force_per_length(avg_z, elem->props().D_outer, water_density_for_mass, cur_elem_axis);

                    int eq1_x = elem->node1()->eq_numbers[0]; int eq2_x = elem->node2()->eq_numbers[0];
                    int eq1_y = elem->node1()->eq_numbers[1]; int eq2_y = elem->node2()->eq_numbers[1];

                    if (eq1_x >= 0) st.F_ext[eq1_x] += f_drag.x() * L * 0.5;
                    if (eq2_x >= 0) st.F_ext[eq2_x] += f_drag.x() * L * 0.5;
                    if (eq1_y >= 0) st.F_ext[eq1_y] += f_drag.y() * L * 0.5;
                    if (eq2_y >= 0) st.F_ext[eq2_y] += f_drag.y() * L * 0.5;
                    if (eq1_z >= 0) st.F_ext[eq1_z] += f_drag.z() * L * 0.5;
                    if (eq2_z >= 0) st.F_ext[eq2_z] += f_drag.z() * L * 0.5;
                }

                if (wave_kinematics.has_value()) {
                    Eigen::Vector3d p1 = elem->node1()->current_coords();
                    Eigen::Vector3d p2 = elem->node2()->current_coords();
                    Eigen::Vector3d elem_axis = (p2 - p1).normalized();
                    Eigen::Vector3d mid = 0.5 * (p1 + p2);

                    // Wave phase coordinate: project onto the propagation direction; z measured
                    // from the free surface (0 at surface, negative below -- AiryWaveKinematics's
                    // own convention), converted from the model's own Z frame (surface at
                    // water_surface_z, not 0). Phase origin (x=0) is the model's own coordinate
                    // origin, not tied to any real ANFLEX reference -- a simplification, doesn't
                    // affect the force's magnitude/spectral content, just its phase along the line.
                    double x_wave = mid.x() * std::cos(wave_heading_rad) + mid.y() * std::sin(wave_heading_rad);
                    double z_wave = mid.z() - model->environmental().water_surface_z;

                    Eigen::Vector3d v_fluid, a_fluid;
                    wave_kinematics->calculate_fluid_kinematics(x_wave, z_wave, time, v_fluid, a_fluid);

                    // Structural velocity at the element midpoint, averaged from both nodes'
                    // CURRENT dynamic state (V_curr, captured by reference) -- same "reused across
                    // backtracking trials" approximation already used for M_global/C_trial below,
                    // not recomputed per trial U_trial.
                    Eigen::Vector3d v_struct = Eigen::Vector3d::Zero();
                    for (int k = 0; k < 3; ++k) {
                        int eq1v = elem->node1()->eq_numbers[k];
                        int eq2v = elem->node2()->eq_numbers[k];
                        double v1 = (eq1v >= 0) ? V_curr[eq1v] : 0.0;
                        double v2 = (eq2v >= 0) ? V_curr[eq2v] : 0.0;
                        v_struct[k] = 0.5 * (v1 + v2);
                    }

                    // a_struct passed as ZERO (not V_curr/A_curr's real value) is deliberate, not
                    // an oversight: Morison's "-(Cm-1)*rho*A*a_struct" term is mathematically the
                    // SAME added-mass contribution risersim's own mass_matrix() already puts on
                    // the LEFT-hand side of the equation of motion
                    // (`m_added = rho_water*outer_area()*props.Ca`, with Ca=Cm-1) -- passing the
                    // real structural acceleration here would double-count that term (once via
                    // M_global, once via F_ext). Zeroing it makes calculate_force_per_length()
                    // return exactly `drag(v_rel) + Cm*rho*A*a_fluid`, the genuinely EXTERNAL part.
                    double Cm = 1.0 + elem->props().Ca;
                    MorisonForce morison(elem->props().Cd, Cm, elem->props().D_outer, water_density);
                    Eigen::Vector3d f_morison = morison.calculate_force_per_length(
                        v_fluid, a_fluid, v_struct, Eigen::Vector3d::Zero(), elem_axis);

                    int eq1_xm = elem->node1()->eq_numbers[0]; int eq2_xm = elem->node2()->eq_numbers[0];
                    int eq1_ym = elem->node1()->eq_numbers[1]; int eq2_ym = elem->node2()->eq_numbers[1];
                    int eq1_zm = elem->node1()->eq_numbers[2]; int eq2_zm = elem->node2()->eq_numbers[2];
                    if (eq1_xm >= 0) st.F_ext[eq1_xm] += f_morison.x() * L * 0.5;
                    if (eq2_xm >= 0) st.F_ext[eq2_xm] += f_morison.x() * L * 0.5;
                    if (eq1_ym >= 0) st.F_ext[eq1_ym] += f_morison.y() * L * 0.5;
                    if (eq2_ym >= 0) st.F_ext[eq2_ym] += f_morison.y() * L * 0.5;
                    if (eq1_zm >= 0) st.F_ext[eq1_zm] += f_morison.z() * L * 0.5;
                    if (eq2_zm >= 0) st.F_ext[eq2_zm] += f_morison.z() * L * 0.5;
                }
            }

            // 2. Assemble Global Stiffness (K) and Internal Force (F_int)
            st.K_global.resize(num_dofs, num_dofs);
            st.F_int = Eigen::VectorXd::Zero(num_dofs);
            assemble_system(st.K_global, st.F_ext, st.F_int);
            st.K_global += assemble_buoyancy_stiffness();

            // Artificial stiffness (Tikhonov regularization), gated to the FIRST time step only --
            // matches the real ANFLEX exactly (`dynamic_integrator.cpp:516-517`:
            // `if(m_have_artificial_stiffness && step == 1) apply_artificial_stiffness();`, decaying
            // by iter within that step via the same formula StaticIntegrator already ports). The
            // dynamic loop never had this before; a prior attempt applying it on EVERY step (not
            // just step 1) made things worse (docs/roadmap.md, Eixo 1b Atualização 7) -- this is a
            // different, narrower experiment matching the real gating precisely, not a retry of the
            // reverted one. `step` here is the OUTER step counter (captured by reference through
            // the try_advance/assemble_at closure chain), unaffected by dt-halving retries within it.
            if (step == 1) {
                add_artificial_stiffness(model, num_dofs, iter, st.K_global);
            }

            return st;
        };

        // Newton-Raphson Iterations per Dynamic Time Step
        int nr_converged_iter = -1;
        double res_norm_prev = 1.0e30;
        // Period-2 cycle detection (docs/roadmap.md, Eixo 2a): a node hovering right at the
        // seabed contact/friction boundary can make Newton bounce between two fixed states
        // forever (contact/friction toggling each iteration), with the entering residual for
        // iteration N reproducing iteration N-2's value EXACTLY -- a stable limit cycle, not
        // divergence or slow convergence, so neither the force-residual test nor the divergence
        // check above ever fires. Classic fixed-point-iteration degenerate case: the true root
        // lies between the two repeating iterates, so bisecting (averaging) them lands much
        // closer to it than either extreme, same idea as regula-falsi/Illinois for a bracketed
        // root. res_hist/U_hist keep the entering state from 2 iterations ago, indexed by parity.
        double res_hist[2] = {-1.0, -1.0};
        Eigen::VectorXd U_hist[2];
        // HHT-alpha bookkeeping: (F_ext-F_int) consistent with whatever U_curr this attempt's
        // Newton loop ultimately ends on -- declared outside the iter loop so it survives past
        // whichever break statement exits it.
        Eigen::VectorXd F_ext_accepted = Eigen::VectorXd::Zero(num_dofs);
        Eigen::VectorXd F_int_accepted = Eigen::VectorXd::Zero(num_dofs);
        for (int iter = 0; iter < max_nr_iters; ++iter) {
            AssembledState state = assemble_at(U_curr, iter);
            Eigen::SparseMatrix<double>& K_global = state.K_global;
            Eigen::VectorXd& F_ext = state.F_ext;
            Eigen::VectorXd& F_int = state.F_int;
            // This call didn't change U_curr, it only evaluated at it -- so state.F_ext/F_int
            // already correspond to the current U_curr. Captured unconditionally here so the
            // convergence-break and divergence-break paths below (which don't touch U_curr) are
            // covered for free; the chattering-bisection and backtracking-accepted paths further
            // down overwrite these when THEY change U_curr instead.
            F_ext_accepted = F_ext;
            F_int_accepted = F_int;

            if (!model) return false;
            // Mass at the state assemble_at(U_curr, iter) just left the nodes in -- recomputed
            // per iteration (not reused stale from a previous one), and each backtracking trial
            // below recomputes its OWN M_trial the same way, since mass_matrix() depends on
            // current element length just like K does.
            Eigen::SparseMatrix<double> M_global = assemble_mass();

            // Damping Matrix C = alpha*M + beta*K
            Eigen::SparseMatrix<double> C_global = alpha_rayleigh * M_global + beta_rayleigh * K_global;

            // Effective Dynamic Stiffness K_eff = (1+hht_alpha)*K + c1*M + (1+hht_alpha)*(gamma/(beta*dt))*C
            Eigen::SparseMatrix<double> K_eff = (1.0 + hht_alpha) * K_global + c1 * M_global
                                               + (1.0 + hht_alpha) * (gamma_newmark / (beta_newmark * dt_sub)) * C_global;

            // HHT-alpha Residual: R = (1+alpha)*(F_ext-F_int) - alpha*F_static_prev - M*A_curr - C*V_curr
            Eigen::VectorXd F_damp = C_global * V_curr;
            Eigen::VectorXd F_iner = M_global * A_curr;
            Eigen::VectorXd Residual = (1.0 + hht_alpha) * (F_ext - F_int) - hht_alpha * F_static_prev_before - F_iner - F_damp;

            double res_norm = Residual.norm();
            double f_ext_norm = F_ext.norm() + 1.0;
            double rel_res = res_norm / f_ext_norm;

            if (rel_res < nr_tolerance) {
                nr_converged_iter = iter;
                break;
            }

            {
                int parity = iter % 2;
                if (iter >= 5 && res_hist[parity] > 0.0 &&
                    std::abs(res_norm - res_hist[parity]) < 1.0e-4 * std::max(res_norm, 1.0)) {
                    Eigen::VectorXd U_avg = 0.5 * (U_curr + U_hist[parity]);
                    AssembledState avg_state = assemble_at(U_avg, iter); // leaves node->disp/rot consistent with U_avg
                    Eigen::VectorXd A_avg = c1 * (U_avg - U_prev) - (1.0 / (beta_newmark * dt_sub)) * V_prev - (1.0 / (2.0 * beta_newmark) - 1.0) * A_prev;
                    Eigen::VectorXd V_avg = V_prev + dt_sub * ((1.0 - gamma_newmark) * A_prev + gamma_newmark * A_avg);
                    std::cerr << "  ⚖️ Dynamic NR period-2 chattering detected at t=" << time
                              << "s iter " << iter << " (res=" << res_norm << " repeats iter " << (iter - 2)
                              << "'s value) -- accepting the bisected state instead." << std::endl;
                    U_curr = U_avg;
                    A_curr = A_avg;
                    V_curr = V_avg;
                    F_ext_accepted = avg_state.F_ext;
                    F_int_accepted = avg_state.F_int;
                    nr_converged_iter = iter;
                    break;
                }
                res_hist[parity] = res_norm;
                U_hist[parity] = U_curr;
            }

            // Divergence check: if the residual grew significantly, bail out of this attempt
            if (iter > 0 && res_norm > 10.0 * res_norm_prev) {
                std::cerr << "  ⚠️ Dynamic NR divergence at t=" << time
                          << "s iter " << iter << " (res=" << res_norm << " > 10x prev=" << res_norm_prev << ")" << std::endl;
                break;
            }
            res_norm_prev = res_norm;

            // Solve for Displacement Correction delta_U
            Eigen::VectorXd delta_U;
            if (linear_solver->solve(K_eff, Residual, delta_U) == LinearSolverStatus::DecompositionFailed) {
                break;
            }

            // Physical step cap, ported from StaticAnalysis::enable_step_limiting
            // (apply_newton_step_with_line_search(), static_analysis.cpp) -- the dynamic loop
            // never had an equivalent. Traced a real divergence to exactly the failure mode this
            // guards against (docs/roadmap.md item 1b): a node sitting just outside seabed
            // contact (no contact stiffness yet, since it hasn't touched) got an UNCAPPED linear
            // correction that jumped it 1.6m straight through the seabed in one iteration --
            // the contact stiffness only "switches on" after the fact, too late to have shaped
            // that correction. Scaling the whole correction down (not clamping component-wise,
            // which would distort its direction) keeps every node's translation/rotation
            // increment within a physically sane bound per iteration, so a node approaching a
            // contact boundary gets there gradually across a few iterations instead of vaulting
            // through it. Same defaults as static's (0.5 m / 0.3 rad) -- not (yet) exposed via
            // the input JSON like static's `enable_step_limiting` is; always on here, since
            // nothing previously protected the dynamic loop from this failure mode at all.
            constexpr double max_translation_step = 0.5;
            constexpr double max_rotation_step = 0.3;
            double alpha_cap = 1.0;
            for (const auto& node : model->nodes()) {
                for (int i = 0; i < 3; ++i) {
                    int eq = node->eq_numbers[i];
                    if (eq >= 0) {
                        double du = std::abs(delta_U[eq]);
                        if (du > 1.0e-12) alpha_cap = std::min(alpha_cap, max_translation_step / du);
                    }
                    int eq_rot = node->eq_numbers[i + 3];
                    if (eq_rot >= 0) {
                        double drot = std::abs(delta_U[eq_rot]);
                        if (drot > 1.0e-12) alpha_cap = std::min(alpha_cap, max_rotation_step / drot);
                    }
                }
            }

            // Backtracking safety net, ported from
            // StaticAnalysis::apply_newton_step_with_line_search() -- the step cap above alone
            // still let step 15's real repro case take 8 iterations to blow up instead of 1
            // (docs/roadmap.md item 1b), because a capped-but-still-accepted step can itself
            // land somewhere that makes the NEXT iteration's residual explode. Checks the trial
            // BEFORE committing to it, instead of applying blindly and only noticing the blowup
            // one iteration later. K, C and M are all recomputed fresh for each trial below (see
            // assemble_mass()) -- an earlier version of this comment claimed M/C were reused
            // stale from the top of the iteration as "an accepted approximation"; that stopped
            // being true for C in Atualização 6 and for M in Atualização 11 (docs/roadmap.md,
            // Eixo 1b) once both were found to matter for the same class of touchdown-zone
            // chattering.
            //
            // Threshold is 2x, NOT static's 100x (`apply_newton_step_with_line_search()`) -- tried
            // 100x first (matching static's "catastrophic-blowup-only" convention) and it did
            // nothing for the real repro case: it accepted a trial whose OWN pre-correction
            // residual was ~938k growing to ~9.78M post-correction (10.4x, comfortably under
            // 100x), which then tripped the separate iteration-to-iteration 10x divergence check
            // below anyway and still failed step 15 identically to before backtracking existed.
            // Static's 100x is calibrated against static's own catastrophic-blowup history
            // (documented in seabed.hpp) -- it isn't a universal constant, and evidently doesn't
            // transfer to this failure mode. 2x rejects the trial that used to blow up and backs
            // off instead -- confirmed against the real repro case (Exemplo_01a, real XML+H5,
            // real 20-iteration/1s/0.05s config, no artificial loosening): the dynamic solver now
            // converges clean through all 20 steps, the first time in this whole investigation
            // (docs/roadmap.md item 1b) that's happened. Not requiring strict monotonic decrease
            // (which risersim's static solver has previously found too strict in other contexts,
            // also documented in seabed.hpp) still leaves enough slack for normal iterations that
            // legitimately increase the residual a little before converging.
            // node->friction_force is PERSISTENT, incrementally-updated state (analysis.cpp:
            // `f_state += k_friction * du`, seabed.hpp::calculate_friction_1d/coupled) -- every
            // assemble_at() call mutates it in place, including trials that get REJECTED below.
            // Without snapshotting it here, a rejected trial's `du` (computed relative to the
            // PREVIOUS trial's leftover node->disp, not to U_curr) gets baked in permanently, and
            // the next trial's `du` compounds on top of that spurious increment instead of being
            // computed relative to U_curr as intended -- across several backtracks this can drift
            // the friction state far from anything physical. Static's equivalent function
            // (StaticAnalysis::apply_newton_step_with_line_search()) already guards against this
            // via its own NodeStateSnapshot/restore(); the dynamic loop never had it. Found via
            // debug instrumentation tracing exactly this failure signature: touchdown-zone "tx"
            // (axial, where friction acts) DOFs swinging to multi-MN-scale spurious tension
            // (docs/roadmap.md, Eixo 1b item 3).
            std::vector<Eigen::Vector2d> friction_snapshot(model->nodes().size());
            for (size_t k = 0; k < model->nodes().size(); ++k) friction_snapshot[k] = model->nodes()[k]->friction_force;

            constexpr int max_backtracks = 5;
            double trial_alpha = alpha_cap;
            Eigen::VectorXd U_trial, A_trial, V_trial;
            AssembledState trial_state;
            for (int bt = 0; bt <= max_backtracks; ++bt) {
                for (size_t k = 0; k < model->nodes().size(); ++k) model->nodes()[k]->friction_force = friction_snapshot[k];
                U_trial = U_curr + trial_alpha * delta_U;
                trial_state = assemble_at(U_trial, iter);
                A_trial = c1 * (U_trial - U_prev) - (1.0 / (beta_newmark * dt_sub)) * V_prev - (1.0 / (2.0 * beta_newmark) - 1.0) * A_prev;
                V_trial = V_prev + dt_sub * ((1.0 - gamma_newmark) * A_prev + gamma_newmark * A_trial);
                // Mass at THIS trial's geometry (element length moves with the nodes, same
                // rationale as recomputing K/C per trial above) -- previously reused M_global from
                // the top of the iteration for every trial, silently stale once a trial actually
                // moves a node.
                Eigen::SparseMatrix<double> M_trial = assemble_mass();
                Eigen::SparseMatrix<double> C_trial = alpha_rayleigh * M_trial + beta_rayleigh * trial_state.K_global;
                Eigen::VectorXd Residual_trial = trial_state.F_ext - trial_state.F_int - M_trial * A_trial - C_trial * V_trial;
                double norm_trial = Residual_trial.norm();

                if (norm_trial <= res_norm * 2.0 || bt == max_backtracks) break;
                trial_alpha *= 0.5;
            }

            // Increment-magnitude escape hatch, ported from the same idea as
            // ConvergenceTest's translation/rotation criteria in the static solver
            // (static_analysis.hpp:33-48): a node sitting almost exactly on the seabed contact
            // boundary can settle into a force-residual "chattering" limit cycle (contact
            // stiffness switching on/off between iterations) that never satisfies the strict
            // relative force-residual test above, EVEN THOUGH the node's actual position has
            // already converged to a physically negligible (sub-millimeter) fixed point -- traced
            // to a real repro case (Exemplo_01a, Far load case, step 79/t=3.95s): position error
            // decayed 250um -> 113um -> 43um -> 8.5um -> 4.2um -> 0.35um across consecutive
            // iterations while the force residual bounced between ~29N and ~430N. Unlike the
            // static solver, the dynamic loop had no increment-based criterion at all -- only the
            // force-residual test -- so this case looped until max_nr_iters with no escape. This
            // check is intentionally about the ACCEPTED correction's own size (independent of
            // force units/scale, so it doesn't need per-problem tuning the way an absolute force
            // tolerance would), not a special case for seabed contact specifically.
            {
                double max_transl_inc = 0.0, max_rot_inc = 0.0;
                for (const auto& node : model->nodes()) {
                    for (int i = 0; i < 3; ++i) {
                        int eq = node->eq_numbers[i];
                        if (eq >= 0) max_transl_inc = std::max(max_transl_inc, std::abs(trial_alpha * delta_U[eq]));
                        int eq_rot = node->eq_numbers[i + 3];
                        if (eq_rot >= 0) max_rot_inc = std::max(max_rot_inc, std::abs(trial_alpha * delta_U[eq_rot]));
                    }
                }
                constexpr double tiny_translation_m = 1.0e-4;
                constexpr double tiny_rotation_rad = 1.0e-4;
                if (max_transl_inc < tiny_translation_m && max_rot_inc < tiny_rotation_rad) {
                    U_curr = U_trial;
                    A_curr = A_trial;
                    V_curr = V_trial;
                    F_ext_accepted = trial_state.F_ext;
                    F_int_accepted = trial_state.F_int;
                    nr_converged_iter = iter;
                    break;
                }
            }

            // Update Displacements, Velocities and Accelerations -- the accepted trial's
            // assemble_at() call above already left node->disp/rot consistent with U_trial, so
            // nothing further to reapply here.
            U_curr = U_trial;
            A_curr = A_trial;
            V_curr = V_trial;
            F_ext_accepted = trial_state.F_ext;
            F_int_accepted = trial_state.F_int;
        }

        if (nr_converged_iter >= 0) {
            U = U_curr;
            V = V_curr;
            A = A_curr;
            // HHT-alpha bookkeeping: (F_ext-F_int) of whatever state this attempt ended on
            // becomes the next attempt's F_static_prev (badina.f's FORESTAT carried forward).
            F_static_prev = F_ext_accepted - F_int_accepted;
            return true;
        }

        if (depth >= model->analysis_options().dynamic_max_step_halvings) {
            U = U_before; V = V_before; A = A_before; F_static_prev = F_static_prev_before; // rewind, not garbage
            return false;
        }

        std::cerr << "  🔁 Dynamic step retry: halving dt at t=" << t_start << "s (depth " << (depth + 1)
                  << ", dt=" << (dt_sub / 2.0) << "s)" << std::endl;
        if (!try_advance(t_start, dt_sub / 2.0, depth + 1) ||
            !try_advance(t_start + dt_sub / 2.0, dt_sub / 2.0, depth + 1)) {
            U = U_before; V = V_before; A = A_before; F_static_prev = F_static_prev_before; // rewind
            return false;
        }
        return true;
    };

    bool all_steps_converged = true;

    for (step = 0; step <= total_steps; ++step) {
        double time = step * dt_s;

        bool step_converged = try_advance((step - 1) * dt_s, dt_s, 0);

        if (!step_converged && step > 0) {
            std::cerr << "  ⚠️ Dynamic NR did not converge at step " << step << " (t=" << time << "s)"
                       << " even after retrying with dt halved down to "
                       << (dt_s / (1 << model->analysis_options().dynamic_max_step_halvings)) << "s." << std::endl;
            all_steps_converged = false;
            if (stop_on_first_non_convergence) {
                std::cerr << "  ⏹️ Stopping (stop_on_first_non_convergence=true) instead of running "
                             "the remaining " << (total_steps - step) << " steps." << std::endl;
                break;
            }
        }

        // Resync node->disp/rot/tension_effective with the FINAL committed U (needed after a
        // fully-exhausted retry, which rewinds U/V/A but leaves node state at the last failed
        // trial -- a no-op resync when the step converged normally, since assemble_at()'s last
        // call already left node state consistent with the committed U_curr).
        sync_node_state(U);

        // Record Snapshot for Web Visualizer
        if (step % 1 == 0) {
            StepSnapshot snap;
            snap.step_index = step;
            snap.load_factor = time;

            for (const auto& node : model->nodes()) {
                snap.node_coords.push_back(node->current_coords());
            }

            for (size_t i = 0; i < model->elements().size(); ++i) {
                // Beam-specific results -- see the same treatment/comment in
                // static_analysis.cpp::capture_snapshot().
                auto* elem = dynamic_cast<CorotationalBeam3D*>(model->elements()[i].get());
                if (!elem) {
                    snap.element_tensions_kN.push_back(0.0);
                    snap.element_bending_moments_kNm.push_back(0.0);
                    snap.element_curvatures.push_back(0.0);
                    snap.element_von_mises_MPa.push_back(0.0);
                    snap.element_mbr_safety_factors.push_back(0.0);
                    continue;
                }
                // Vizinho REAL (compartilha o nó), não só adjacente no array -- com múltiplas
                // linhas, elementos de linhas diferentes ficam lado a lado no array plano sem se
                // tocar; usar i-1/i+1 cegamente misturaria curvatura/momento entre linhas
                // desconexas na fronteira. Um vizinho que não seja viga também conta como "sem
                // vizinho" pro cálculo de curvatura.
                const auto* prev = (i > 0) ? dynamic_cast<const CorotationalBeam3D*>(model->elements()[i - 1].get()) : nullptr;
                if (prev && prev->node2() != elem->node1()) prev = nullptr;
                const auto* next = (i + 1 < model->elements().size()) ? dynamic_cast<const CorotationalBeam3D*>(model->elements()[i + 1].get()) : nullptr;
                if (next && next->node1() != elem->node2()) next = nullptr;

                auto sc = elem->compute_stress_and_curvature(prev, next, 350.0);
                snap.element_tensions_kN.push_back(elem->tension_effective() / 1000.0);
                snap.element_bending_moments_kNm.push_back(sc.bending_moment_kNm);
                snap.element_curvatures.push_back(sc.curvature);
                snap.element_von_mises_MPa.push_back(sc.von_mises_MPa);
                snap.element_mbr_safety_factors.push_back(sc.mbr_safety_factor);
            }

            history.push_back(snap);
        }

        if (step % 40 == 0) {
            // Representative top Z (line 0), recomputed here only for the progress log -- doesn't
            // feed the solve, so a redundant get_motion()/ramp evaluation every 40 steps is cheap.
            double disp_z = 0.0;
            if (!line_runtimes.empty()) {
                auto& lr0 = line_runtimes[0];
                if (lr0.motion) {
                    Eigen::Vector3d vessel_disp, vessel_rot;
                    lr0.motion->get_motion(time, vessel_disp, vessel_rot);
                    disp_z = vessel_disp.z();
                } else {
                    double ramp_time = 5.0;
                    double ramp_factor = (time < ramp_time) ? 0.5 * (1.0 - std::cos(std::numbers::pi * time / ramp_time)) : 1.0;
                    disp_z = ramp_factor * wave_amplitude * std::sin(omega * time);
                }
            }
            std::cout << "  ⏱️ Dynamic Time Step " << std::setw(4) << step << "/" << total_steps
                      << " (t = " << std::fixed << std::setprecision(1) << time << " s) | Top Z: "
                      << std::setprecision(2) << disp_z << " m" << std::endl;
        }
    }

    // Restaura todos os nós de topo ao contrato de GDL original (mesmo padrão de limpeza de
    // StaticAnalysis::solve_vessel_offset) -- por simetria/higiene, mesmo esta sendo a última fase
    // da análise hoje.
    for (auto& lr : line_runtimes) lr.top_node->eq_numbers = lr.saved_eq_numbers;
    prescribed_motions.clear();
    assign_equation_numbers();

    if (!all_steps_converged) {
        std::cerr << "❌ 3D TIME-DOMAIN DYNAMIC SOLVER FINISHED WITH ONE OR MORE NON-CONVERGED STEPS!" << std::endl;
        return false;
    }

    std::cout << "🎉 3D TIME-DOMAIN DYNAMIC SOLVER CONVERGED SUCCESSFULLY!" << std::endl;
    return true;
}

} // namespace risersim
