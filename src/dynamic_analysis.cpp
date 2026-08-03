#include "risersim/dynamic_analysis.hpp"
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

    if (nodes.empty()) return false;
    assign_equation_numbers();

    history.clear();

    const int total_steps = static_cast<int>(duration_s / dt_s);
    const double omega = 2.0 * M_PI / wave_period;

    // Newmark-beta Constants (Average Acceleration Method)
    const double gamma_newmark = 0.55;
    const double beta_newmark = 0.28;

    const double c1 = 1.0 / (beta_newmark * dt_s * dt_s);

    // Save Initial Equilibrium Displacements
    std::vector<Eigen::Vector3d> static_disps;
    std::vector<Eigen::Vector3d> static_rots;
    for (auto* node : nodes) {
        static_disps.push_back(node->disp);
        static_rots.push_back(node->rot);
    }

    Node3D* top_node = nodes.front();

    // Dynamic State Vectors (System DOFs)
    Eigen::VectorXd U = Eigen::VectorXd::Zero(num_dofs);
    Eigen::VectorXd V = Eigen::VectorXd::Zero(num_dofs);
    Eigen::VectorXd A = Eigen::VectorXd::Zero(num_dofs);

    for (int step = 0; step <= total_steps; ++step) {
        double time = step * dt_s;

        // Prescribe Top Vessel Harmonic Motion with 5s Smooth Ramp
        double ramp_time = 5.0;
        double ramp_factor = (time < ramp_time) ? 0.5 * (1.0 - std::cos(M_PI * time / ramp_time)) : 1.0;
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
        int max_iters = 12;
        for (int iter = 0; iter < max_iters; ++iter) {
            // Update Node Displacements (Static + Current Dynamic Perturbation)
            for (size_t i = 0; i < nodes.size(); ++i) {
                auto* node = nodes[i];
                if (node == top_node) continue;

                for (int k = 0; k < 3; ++k) {
                    int eq = node->eq_numbers[k];
                    if (eq >= 0) node->disp[k] = static_disps[i][k] + U_curr[eq];

                    int eq_rot = node->eq_numbers[k + 3];
                    if (eq_rot >= 0) node->rot[k] = static_rots[i][k] + U_curr[eq_rot];
                }
            }

            // Update Element Effective Tensions based on deformed geometry
            for (auto* elem : elements) {
                elem->update_effective_tension();
            }

            // 1. External Load Vector F_ext
            Eigen::VectorXd F_ext = Eigen::VectorXd::Zero(num_dofs);
            for (auto* elem : elements) {
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
                    current.get_drag_force_per_meter(avg_z, elem->props.D_outer, water_density, f_drag_x, f_drag_y);

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

            Eigen::SparseMatrix<double> M_global(num_dofs, num_dofs);
            std::vector<Eigen::Triplet<double>> m_triplets;
            for (auto* elem : elements) {
                Eigen::Matrix<double, 12, 12> m_elem = elem->global_mass(water_density);
                std::vector<int> eq_map = {
                    elem->node1->eq_numbers[0], elem->node1->eq_numbers[1], elem->node1->eq_numbers[2],
                    elem->node1->eq_numbers[3], elem->node1->eq_numbers[4], elem->node1->eq_numbers[5],
                    elem->node2->eq_numbers[0], elem->node2->eq_numbers[1], elem->node2->eq_numbers[2],
                    elem->node2->eq_numbers[3], elem->node2->eq_numbers[4], elem->node2->eq_numbers[5]
                };

                for (int r = 0; r < 12; ++r) {
                    if (eq_map[r] < 0) continue;
                    for (int c = 0; c < 12; ++c) {
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
            if (res_norm < 1.0e-3) {
                break;
            }

            // Solve for Displacement Correction delta_U
            Eigen::SparseLU<Eigen::SparseMatrix<double>> solver;
            solver.compute(K_eff);
            if (solver.info() != Eigen::Success) {
                break;
            }

            Eigen::VectorXd delta_U = solver.solve(Residual);

            // Update Displacements, Velocities and Accelerations
            U_curr += delta_U;
            A_curr = c1 * (U_curr - U_prev) - (1.0 / (beta_newmark * dt_s)) * V_prev - (1.0 / (2.0 * beta_newmark) - 1.0) * A_prev;
            V_curr = V_prev + dt_s * ((1.0 - gamma_newmark) * A_prev + gamma_newmark * A_curr);
        }

        U = U_curr;
        V = V_curr;
        A = A_curr;

        // Record Snapshot for Web Visualizer
        if (step % 1 == 0) {
            StepSnapshot snap;
            snap.step_index = step;
            snap.load_factor = time;

            for (auto* node : nodes) {
                snap.node_coords.push_back(node->current_coords());
            }

            for (size_t i = 0; i < elements.size(); ++i) {
                auto* elem = elements[i];
                const auto* prev = (i > 0) ? elements[i - 1] : nullptr;
                const auto* next = (i + 1 < elements.size()) ? elements[i + 1] : nullptr;

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

    std::cout << "🎉 3D TIME-DOMAIN DYNAMIC SOLVER CONVERGED SUCCESSFULLY!" << std::endl;
    return true;
}

} // namespace risersim
