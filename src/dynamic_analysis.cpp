/**
 * @file dynamic_analysis.cpp
 * @brief DynamicAnalysis: time-domain Newmark-beta integration with per-step Newton-Raphson.
 */
#include "risersim/dynamic_analysis.hpp"
#include "risersim/rotation_utils.hpp"
#include "risersim/config.hpp"
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

    if (!model || model->nodes.empty()) return false;
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

    Node3D* top_node = model->nodes.front().get();

    // Dynamic State Vectors (System DOFs)
    Eigen::VectorXd U = Eigen::VectorXd::Zero(num_dofs);
    Eigen::VectorXd V = Eigen::VectorXd::Zero(num_dofs);
    Eigen::VectorXd A = Eigen::VectorXd::Zero(num_dofs);

    bool all_steps_converged = true;

    for (int step = 0; step <= total_steps; ++step) {
        double time = step * dt_s;

        // Prescribe Top Vessel Harmonic Motion with 5s Smooth Ramp
        double ramp_time = 5.0;
        double ramp_factor = (time < ramp_time) ? 0.5 * (1.0 - std::cos(std::numbers::pi * time / ramp_time)) : 1.0;
        double disp_z = ramp_factor * wave_amplitude * std::sin(omega * time);
        top_node->disp = static_disps.front() + Eigen::Vector3d(0.0, 0.0, disp_z);

        // Save Previous State Vectors for Newmark Integration
        Eigen::VectorXd U_prev = U;
        Eigen::VectorXd V_prev = V;
        Eigen::VectorXd A_prev = A;

        // Standard Newmark-beta Predictor step
        Eigen::VectorXd A_curr = Eigen::VectorXd::Zero(num_dofs);
        Eigen::VectorXd V_curr = V_prev + dt_s * (1.0 - gamma_newmark) * A_prev;
        Eigen::VectorXd U_curr = U_prev + dt_s * V_prev + 0.5 * dt_s * dt_s * (1.0 - 2.0 * beta_newmark) * A_prev;

        // Newton-Raphson Iterations per Dynamic Time Step
        int nr_converged_iter = -1;
        double res_norm_prev = 1.0e30;
        for (int iter = 0; iter < max_nr_iters; ++iter) {
            // Update Node Displacements (Static + Current Dynamic Perturbation)
            for (size_t i = 0; i < model->nodes.size(); ++i) {
                auto* node = model->nodes[i].get();
                if (node == top_node) continue;

                Eigen::Vector3d dyn_rot_perturbation = Eigen::Vector3d::Zero();
                bool has_rot_dof = false;
                for (int k = 0; k < 3; ++k) {
                    int eq = node->eq_numbers[k];
                    if (eq >= 0) {
                        double new_disp_k = static_disps[i][k] + U_curr[eq];
                        // This iteration's increment (for the seabed friction spring).
                        if (k < 2) node->delta_disp_xy[k] = new_disp_k - node->disp[k];
                        node->disp[k] = new_disp_k;
                    }

                    int eq_rot = node->eq_numbers[k + 3];
                    if (eq_rot >= 0) { dyn_rot_perturbation[k] = U_curr[eq_rot]; has_rot_dof = true; }
                }
                // Proper composition of the dynamic perturbation on top of the static base
                // rotation (not a linear sum). See rotation_utils.hpp.
                if (has_rot_dof) node->rot = compose_rotations(static_rots[i], dyn_rot_perturbation);
            }

            // Update Element Effective Tensions based on deformed geometry
            for (const auto& elem : model->elements) {
                elem->update_effective_tension();
            }

            // 1. External Load Vector F_ext
            Eigen::VectorXd F_ext = Eigen::VectorXd::Zero(num_dofs);
            for (const auto& elem : model->elements) {
                double L = elem->initial_length;
                double g = 9.81;

                double w_dry = (elem->props.rho * elem->props.A + elem->props.rho_fluid * elem->inner_area()) * g;
                double w_buoyancy = water_density * elem->outer_area() * g;
                double elem_weight_total = (w_dry - w_buoyancy) * L;

                int eq1_z = elem->node1->eq_numbers[2];
                int eq2_z = elem->node2->eq_numbers[2];

                if (eq1_z >= 0) F_ext[eq1_z] -= elem_weight_total * 0.5;
                if (eq2_z >= 0) F_ext[eq2_z] -= elem_weight_total * 0.5;

                if (enable_current) {
                    double avg_z = 0.5 * (elem->node1->current_coords().z() + elem->node2->current_coords().z());
                    double f_drag_x = 0.0, f_drag_y = 0.0;
                    current.get_drag_force_per_meter(avg_z, elem->props.D_outer, water_density_for_mass, f_drag_x, f_drag_y);

                    int eq1_x = elem->node1->eq_numbers[0]; int eq2_x = elem->node2->eq_numbers[0];
                    int eq1_y = elem->node1->eq_numbers[1]; int eq2_y = elem->node2->eq_numbers[1];

                    if (eq1_x >= 0) F_ext[eq1_x] += f_drag_x * L * 0.5;
                    if (eq2_x >= 0) F_ext[eq2_x] += f_drag_x * L * 0.5;
                    if (eq1_y >= 0) F_ext[eq1_y] += f_drag_y * L * 0.5;
                    if (eq2_y >= 0) F_ext[eq2_y] += f_drag_y * L * 0.5;
                }
            }

            // 2. Assemble Global Mass (M) and Stiffness (K) Matrices
            Eigen::SparseMatrix<double> K_global(num_dofs, num_dofs);
            Eigen::VectorXd F_int = Eigen::VectorXd::Zero(num_dofs);
            assemble_system(K_global, F_ext, F_int);

            if (!model) return false;
            Eigen::SparseMatrix<double> M_global(num_dofs, num_dofs);
            std::vector<Eigen::Triplet<double>> m_triplets;
            // Goes through the Element interface, same rationale as Analysis::assemble_system()
            // (see analysis.cpp) -- works unchanged for any future element type.
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

            // Update Displacements, Velocities and Accelerations
            U_curr += delta_U;
            A_curr = c1 * (U_curr - U_prev) - (1.0 / (beta_newmark * dt_s)) * V_prev - (1.0 / (2.0 * beta_newmark) - 1.0) * A_prev;
            V_curr = V_prev + dt_s * ((1.0 - gamma_newmark) * A_prev + gamma_newmark * A_curr);
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

    if (!all_steps_converged) {
        std::cerr << "❌ 3D TIME-DOMAIN DYNAMIC SOLVER FINISHED WITH ONE OR MORE NON-CONVERGED STEPS!" << std::endl;
        return false;
    }

    std::cout << "🎉 3D TIME-DOMAIN DYNAMIC SOLVER CONVERGED SUCCESSFULLY!" << std::endl;
    return true;
}

} // namespace risersim
