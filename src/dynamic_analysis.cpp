/**
 * @file dynamic_analysis.cpp
 * @brief DynamicAnalysis: time-domain Newmark-beta integration with per-step Newton-Raphson.
 */
#include "risersim/dynamic_analysis.hpp"
#include "risersim/rotation_utils.hpp"
#include "risersim/hydrostatics.hpp"
#include "risersim/config.hpp"
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <cmath>

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

    if (vessel_motion.has_value()) {
        std::cout << "  Vessel motion (RAO+JONSWAP equivalent harmonic): freq=" << vessel_motion->frequency_rad_s()
                  << " rad/s (T=" << (2.0 * std::numbers::pi / vessel_motion->frequency_rad_s()) << " s)" << std::endl;
        std::cout << "    surge=" << vessel_motion->amplitude(VesselDof::Surge) << " m"
                  << " sway=" << vessel_motion->amplitude(VesselDof::Sway) << " m"
                  << " heave=" << vessel_motion->amplitude(VesselDof::Heave) << " m"
                  << " roll=" << vessel_motion->amplitude(VesselDof::Roll) << " rad"
                  << " pitch=" << vessel_motion->amplitude(VesselDof::Pitch) << " rad"
                  << " yaw=" << vessel_motion->amplitude(VesselDof::Yaw) << " rad" << std::endl;
    }

    if (!model || model->nodes.empty()) return false;

    Node3D* top_node = model->nodes.front().get();
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
    std::vector<int> top_node_saved_eq_numbers = top_node->eq_numbers;
    top_node->eq_numbers = {0, 1, 2, 3, 4, 5};
    assign_equation_numbers();

    history.clear();

    const int total_steps = static_cast<int>(duration_s / dt_s);
    const double omega = 2.0 * std::numbers::pi / wave_period;

    // Newmark-beta Constants (Average Acceleration Method)
    const double gamma_newmark = 0.55;
    const double beta_newmark = 0.28;

    const double c1 = 1.0 / (beta_newmark * dt_s * dt_s);

    // Save Initial Equilibrium Displacements
    std::vector<Eigen::Vector3d> static_disps;
    std::vector<Eigen::Vector3d> static_rots;
    for (const auto& node : model->nodes) {
        static_disps.push_back(node->disp);
        static_rots.push_back(node->rot);
    }

    // Dynamic State Vectors (System DOFs)
    Eigen::VectorXd U = Eigen::VectorXd::Zero(num_dofs);
    Eigen::VectorXd V = Eigen::VectorXd::Zero(num_dofs);
    Eigen::VectorXd A = Eigen::VectorXd::Zero(num_dofs);

    prescribed_motions.clear();
    prescribed_motions.emplace_back(top_node);
    PrescribedMotion& top_motion = prescribed_motions.back();
    // Todos os 6 GDL, sempre -- inclusive no fallback sem vessel_motion real, onde X/Y/rotação
    // recebem um alvo CONSTANTE (a própria posição estática) em vez de ficarem livres sem
    // nenhuma restrição: reproduz o comportamento antigo (Dirichlet fixo nesses GDL) só que agora
    // via a mesma mola de penalidade, preservando o acoplamento de massa consistente.
    top_motion.dof_active = {true, true, true, true, true, true};

    bool all_steps_converged = true;

    for (int step = 0; step <= total_steps; ++step) {
        double time = step * dt_s;

        // Prescribe Top Vessel Motion: real RAO+JONSWAP "equivalent harmonic" (6 DOFs) when the
        // input JSON has real data for it (vessel_motion.hpp), else the old single-Z regular-
        // wave sinusoid with the same 5s smooth ramp -- now via top_motion's penalty-spring
        // target (see setup above), not a direct disp/rot assignment.
        double disp_z = 0.0; // só usado no log de progresso abaixo
        if (vessel_motion.has_value()) {
            Eigen::Vector3d vessel_disp, vessel_rot;
            vessel_motion->get_motion(time, vessel_disp, vessel_rot);
            top_motion.target_disp = static_disps.front() + vessel_disp;
            // Componente a componente, NÃO compose_rotations() -- confirmado contra o mecanismo
            // real de movimento prescrito (`integrator.cpp::set_load_dofs`, `presc_desl[i] +=
            // movements[i]` pra i=0..5, sem distinção entre translação/rotação): o ANFLEX real
            // soma o vetor de rotação do harmônico equivalente direto em cima da referência
            // estática, sem compor via Rodrigues/quaternion. Faz sentido com o próprio método:
            // "equivalent harmonic" já é linearizado do início ao fim (RAO é resposta linear em
            // frequência), então uma composição não-linear aqui introduziria uma não-linearidade
            // que o método de referência não tem. `compose_rotations` continua correto pro ESTADO
            // realmente resolvido pelo Newton (linha ~155, abaixo, agora também pro nó de topo) --
            // esse é outro mecanismo do ANFLEX real (`nMathUtils::pseudo_sum`,
            // `integrator.cpp:697`), aplicado ao alvo vs. ao estado, não ao mesmo lugar.
            top_motion.target_rot = static_rots.front() + vessel_rot;
            disp_z = vessel_disp.z();
        } else {
            double ramp_time = 5.0;
            double ramp_factor = (time < ramp_time) ? 0.5 * (1.0 - std::cos(std::numbers::pi * time / ramp_time)) : 1.0;
            disp_z = ramp_factor * wave_amplitude * std::sin(omega * time);
            top_motion.target_disp = static_disps.front() + Eigen::Vector3d(0.0, 0.0, disp_z);
            top_motion.target_rot = static_rots.front(); // alvo constante -- rotação do topo nunca foi dinamicamente imposta sem vessel_motion real
        }

        // Save Previous State Vectors for Newmark Integration
        Eigen::VectorXd U_prev = U;
        Eigen::VectorXd V_prev = V;
        Eigen::VectorXd A_prev = A;

        // Standard Newmark-beta Predictor step
        Eigen::VectorXd A_curr = Eigen::VectorXd::Zero(num_dofs);
        Eigen::VectorXd V_curr = V_prev + dt_s * (1.0 - gamma_newmark) * A_prev;
        Eigen::VectorXd U_curr = U_prev + dt_s * V_prev + 0.5 * dt_s * dt_s * (1.0 - 2.0 * beta_newmark) * A_prev;

        // Assembles node state + F_ext + K_global/F_int for a TRIAL dynamic-DOF vector U_trial --
        // extracted so both the main per-iteration assembly below AND the backtracking line
        // search (docs/roadmap.md item 1b) can share it instead of duplicating ~70 lines. Sets
        // node->disp/rot ABSOLUTELY from static_disps/static_rots + U_trial (not incrementally),
        // so repeated calls with different U_trial candidates are self-consistent -- no manual
        // snapshot/restore needed between backtracking attempts, each call simply overwrites.
        struct AssembledState {
            Eigen::VectorXd F_ext;
            Eigen::SparseMatrix<double> K_global;
            Eigen::VectorXd F_int;
        };
        auto assemble_at = [&](const Eigen::VectorXd& U_trial) -> AssembledState {
            // Update Node Displacements (Static + Current Dynamic Perturbation) -- top_node
            // included now: it's a genuine free DOF held by the penalty spring (top_motion), not
            // excluded by direct assignment, so it must receive Newton corrections like any other
            // free node (this is what lets its mass properly couple to its neighbor via M_BA).
            for (size_t i = 0; i < model->nodes.size(); ++i) {
                auto* node = model->nodes[i].get();

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
            for (const auto& elem : model->elements) {
                elem->update_effective_tension();
            }

            AssembledState st;

            // 1. External Load Vector F_ext
            st.F_ext = Eigen::VectorXd::Zero(num_dofs);
            for (const auto& elem : model->elements) {
                double L = elem->initial_length;
                double g = 9.81;

                double w_dry = (elem->props.rho * elem->props.A + elem->props.rho_fluid * elem->inner_area()) * g;
                // Submersion-scaled buoyancy (see hydrostatics.hpp, docs/roadmap.md item 1b) --
                // this is the fix for the divergence traced to a node crossing the water surface
                // mid-simulation: buoyancy used to be a CONSTANT per element regardless of Z,
                // with no associated tangent stiffness, so Newton had no restoring gradient right
                // at the crossing. Re-evaluated every Newton iteration (this whole F_ext block
                // already re-runs per iteration), so it tracks the node's CURRENT position, not
                // just its position at the start of the time step.
                double zc[2] = {elem->node1->current_coords().z(), elem->node2->current_coords().z()};
                Hydrostatics hydro(elem->props.D_outer, L, water_density);
                hydro.compute(zc, model->environmental.water_surface_z);

                int eq1_z = elem->node1->eq_numbers[2];
                int eq2_z = elem->node2->eq_numbers[2];

                if (eq1_z >= 0) st.F_ext[eq1_z] += hydro.end_force(0) * g - 0.5 * w_dry * L;
                if (eq2_z >= 0) st.F_ext[eq2_z] += hydro.end_force(1) * g - 0.5 * w_dry * L;

                if (enable_current) {
                    double avg_z = 0.5 * (elem->node1->current_coords().z() + elem->node2->current_coords().z());
                    double f_drag_x = 0.0, f_drag_y = 0.0;
                    current.get_drag_force_per_meter(avg_z, elem->props.D_outer, water_density_for_mass, f_drag_x, f_drag_y);

                    int eq1_x = elem->node1->eq_numbers[0]; int eq2_x = elem->node2->eq_numbers[0];
                    int eq1_y = elem->node1->eq_numbers[1]; int eq2_y = elem->node2->eq_numbers[1];

                    if (eq1_x >= 0) st.F_ext[eq1_x] += f_drag_x * L * 0.5;
                    if (eq2_x >= 0) st.F_ext[eq2_x] += f_drag_x * L * 0.5;
                    if (eq1_y >= 0) st.F_ext[eq1_y] += f_drag_y * L * 0.5;
                    if (eq2_y >= 0) st.F_ext[eq2_y] += f_drag_y * L * 0.5;
                }
            }

            // 2. Assemble Global Stiffness (K) and Internal Force (F_int)
            st.K_global.resize(num_dofs, num_dofs);
            st.F_int = Eigen::VectorXd::Zero(num_dofs);
            assemble_system(st.K_global, st.F_ext, st.F_int);
            st.K_global += assemble_buoyancy_stiffness();

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
        for (int iter = 0; iter < max_nr_iters; ++iter) {
            AssembledState state = assemble_at(U_curr);
            Eigen::SparseMatrix<double>& K_global = state.K_global;
            Eigen::VectorXd& F_ext = state.F_ext;
            Eigen::VectorXd& F_int = state.F_int;

            if (!model) return false;
            Eigen::SparseMatrix<double> M_global(num_dofs, num_dofs);
            std::vector<Eigen::Triplet<double>> m_triplets;
            // Goes through the Element interface, same rationale as Analysis::assemble_system()
            // (see analysis.cpp) -- works unchanged for any future element type. Not Z-dependent
            // (unlike K_global) -- safe to reuse as-is across this iteration's backtracking
            // trials below without recomputing.
            for (const auto& elem : model->elements) {
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
            M_global.setFromTriplets(m_triplets.begin(), m_triplets.end());

            // Damping Matrix C = alpha*M + beta*K
            Eigen::SparseMatrix<double> C_global = alpha_rayleigh * M_global + beta_rayleigh * K_global;

            // Effective Dynamic Stiffness K_eff = K + c1*M + (gamma / (beta * dt))*C
            Eigen::SparseMatrix<double> K_eff = K_global + c1 * M_global + (gamma_newmark / (beta_newmark * dt_s)) * C_global;

            // Dynamic Residual Force Vector: R = F_ext - F_int - M*A_curr - C*V_curr
            Eigen::VectorXd F_damp = C_global * V_curr;
            Eigen::VectorXd F_iner = M_global * A_curr;
            Eigen::VectorXd Residual = F_ext - F_int - F_iner - F_damp;

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
                    assemble_at(U_avg); // leaves node->disp/rot consistent with U_avg (return value unused)
                    Eigen::VectorXd A_avg = c1 * (U_avg - U_prev) - (1.0 / (beta_newmark * dt_s)) * V_prev - (1.0 / (2.0 * beta_newmark) - 1.0) * A_prev;
                    Eigen::VectorXd V_avg = V_prev + dt_s * ((1.0 - gamma_newmark) * A_prev + gamma_newmark * A_avg);
                    std::cerr << "  ⚖️ Dynamic NR period-2 chattering detected at step " << step
                              << " iter " << iter << " (res=" << res_norm << " repeats iter " << (iter - 2)
                              << "'s value) -- accepting the bisected state instead." << std::endl;
                    U_curr = U_avg;
                    A_curr = A_avg;
                    V_curr = V_avg;
                    nr_converged_iter = iter;
                    break;
                }
                res_hist[parity] = res_norm;
                U_hist[parity] = U_curr;
            }

            // Divergence check: if the residual grew significantly, bail out of this step's correction
            if (iter > 0 && res_norm > 10.0 * res_norm_prev) {
                std::cerr << "  ⚠️ Dynamic NR divergence at step " << step
                          << " iter " << iter << " (res=" << res_norm << " > 10x prev=" << res_norm_prev << ")" << std::endl;
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
            for (const auto& node : model->nodes) {
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
            // one iteration later. Reuses this iteration's M_global/C_global for every trial (not
            // recomputed per trial -- M isn't Z-dependent at all, and re-deriving C from a
            // slightly stale K_global is an accepted approximation for this accept/reject check).
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
            constexpr int max_backtracks = 5;
            double trial_alpha = alpha_cap;
            Eigen::VectorXd U_trial, A_trial, V_trial;
            AssembledState trial_state;
            for (int bt = 0; bt <= max_backtracks; ++bt) {
                U_trial = U_curr + trial_alpha * delta_U;
                trial_state = assemble_at(U_trial);
                A_trial = c1 * (U_trial - U_prev) - (1.0 / (beta_newmark * dt_s)) * V_prev - (1.0 / (2.0 * beta_newmark) - 1.0) * A_prev;
                V_trial = V_prev + dt_s * ((1.0 - gamma_newmark) * A_prev + gamma_newmark * A_trial);
                Eigen::SparseMatrix<double> C_trial = alpha_rayleigh * M_global + beta_rayleigh * trial_state.K_global;
                Eigen::VectorXd Residual_trial = trial_state.F_ext - trial_state.F_int - M_global * A_trial - C_trial * V_trial;
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
                for (const auto& node : model->nodes) {
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
        }

        U = U_curr;
        V = V_curr;
        A = A_curr;

        if (nr_converged_iter < 0 && step > 0) {
            std::cerr << "  ⚠️ Dynamic NR did not converge at step " << step
                      << " (t=" << time << "s, last res_norm=" << res_norm_prev << ")" << std::endl;
            all_steps_converged = false;
            if (stop_on_first_non_convergence) {
                std::cerr << "  ⏹️ Stopping (stop_on_first_non_convergence=true) instead of running "
                             "the remaining " << (total_steps - step) << " steps." << std::endl;
                break;
            }
        }

        // Record Snapshot for Web Visualizer
        if (step % 1 == 0) {
            StepSnapshot snap;
            snap.step_index = step;
            snap.load_factor = time;

            for (const auto& node : model->nodes) {
                snap.node_coords.push_back(node->current_coords());
            }

            for (size_t i = 0; i < model->elements.size(); ++i) {
                auto* elem = model->elements[i].get();
                const auto* prev = (i > 0) ? model->elements[i - 1].get() : nullptr;
                const auto* next = (i + 1 < model->elements.size()) ? model->elements[i + 1].get() : nullptr;

                auto sc = elem->compute_stress_and_curvature(prev, next, 350.0);
                snap.element_tensions_kN.push_back(elem->tension_effective / 1000.0);
                snap.element_bending_moments_kNm.push_back(sc.bending_moment_kNm);
                snap.element_curvatures.push_back(sc.curvature);
                snap.element_von_mises_MPa.push_back(sc.von_mises_MPa);
                snap.element_mbr_safety_factors.push_back(sc.mbr_safety_factor);
            }

            history.push_back(snap);
        }

        if (step % 40 == 0) {
            std::cout << "  ⏱️ Dynamic Time Step " << std::setw(4) << step << "/" << total_steps
                      << " (t = " << std::fixed << std::setprecision(1) << time << " s) | Top Z: "
                      << std::setprecision(2) << disp_z << " m" << std::endl;
        }
    }

    // Restaura o nó de topo ao contrato de GDL original (mesmo padrão de limpeza de
    // StaticAnalysis::solve_vessel_offset) -- por simetria/higiene, mesmo esta sendo a última fase
    // da análise hoje.
    top_node->eq_numbers = top_node_saved_eq_numbers;
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
