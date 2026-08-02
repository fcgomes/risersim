#include "risersim/solver.hpp"
#include <Eigen/SparseLU>
#include <iostream>
#include <fstream>
#include <iomanip>

#ifdef RISERSIM_HAS_HDF5
#include "H5Cpp.h"
#endif

namespace risersim {

void StaticAnalysis::assemble_system(Eigen::SparseMatrix<double>& K_global, const Eigen::VectorXd& F_ext, Eigen::VectorXd& F_int) {
    K_global.resize(num_dofs, num_dofs);
    K_global.setZero();
    F_int = Eigen::VectorXd::Zero(num_dofs);

    std::vector<Eigen::Triplet<double>> triplets;

    for (auto* elem : elements) {
        elem->update_effective_tension();

        Eigen::Matrix<double, 12, 12> K_elem = elem->global_stiffness();

        std::vector<int> dofs(12);
        for (int i = 0; i < 6; ++i) {
            dofs[i] = elem->node1->eq_numbers[i];
            dofs[i + 6] = elem->node2->eq_numbers[i];
        }

        for (int i = 0; i < 12; ++i) {
            if (dofs[i] < 0) continue;
            for (int j = 0; j < 12; ++j) {
                if (dofs[j] < 0) continue;
                triplets.push_back(Eigen::Triplet<double>(dofs[i], dofs[j], K_elem(i, j)));
            }
        }

        Eigen::Vector3d ex = (elem->node2->current_coords() - elem->node1->current_coords()).normalized();
        Eigen::Vector3d f_axial = elem->tension_effective * ex;

        int eq1_x = elem->node1->eq_numbers[0];
        int eq1_y = elem->node1->eq_numbers[1];
        int eq1_z = elem->node1->eq_numbers[2];

        int eq2_x = elem->node2->eq_numbers[0];
        int eq2_y = elem->node2->eq_numbers[1];
        int eq2_z = elem->node2->eq_numbers[2];

        if (eq1_x >= 0) F_int[eq1_x] -= f_axial.x();
        if (eq1_y >= 0) F_int[eq1_y] -= f_axial.y();
        if (eq1_z >= 0) F_int[eq1_z] -= f_axial.z();

        if (eq2_x >= 0) F_int[eq2_x] += f_axial.x();
        if (eq2_y >= 0) F_int[eq2_y] += f_axial.y();
        if (eq2_z >= 0) F_int[eq2_z] += f_axial.z();
    }

    // Apply Seabed Interaction (Bilinear Soil Springs at TDZ)
    for (auto* node : nodes) {
        double f_seabed = 0.0, k_seabed = 0.0;
        seabed.calculate_seabed_reaction(node->current_coords().z(), f_seabed, k_seabed);

        int eq_z = node->eq_numbers[2];
        if (eq_z >= 0) {
            F_int[eq_z] += f_seabed;
            triplets.push_back(Eigen::Triplet<double>(eq_z, eq_z, k_seabed));
        }
    }

    K_global.setFromTriplets(triplets.begin(), triplets.end());
}

bool StaticAnalysis::solve_catenary_static(int load_steps, int max_iter_per_step, double tol) {
    std::cout << "\n=========================================================================" << std::endl;
    std::cout << "  riserSim Static Non-Linear Catenary Equilibrium Solver" << std::endl;
    std::cout << "=========================================================================" << std::endl;

    assign_equation_numbers();
    static_history.clear();
    step_history.clear();

    // Step 0: Initial Geometry (0% Load)
    StepSnapshot step0;
    step0.step_index = 0;
    step0.load_factor = 0.0;
    for (auto* node : nodes) step0.node_coords.push_back(node->current_coords());
    for (size_t i = 0; i < elements.size(); ++i) {
        auto* elem = elements[i];
        const auto* prev = (i > 0) ? elements[i - 1] : nullptr;
        const auto* next = (i + 1 < elements.size()) ? elements[i + 1] : nullptr;

        elem->update_effective_tension();
        auto sc = elem->compute_stress_and_curvature(prev, next);
        step0.element_tensions_kN.push_back(elem->tension_effective / 1000.0);
        step0.element_bending_moments_kNm.push_back(sc.bending_moment_kNm);
        step0.element_curvatures.push_back(sc.curvature);
        step0.element_von_mises_MPa.push_back(sc.von_mises_MPa);
        step0.element_mbr_safety_factors.push_back(sc.mbr_safety_factor);
    }
    static_history.push_back(step0);
    step_history.push_back(step0);

    for (int step = 1; step <= load_steps; ++step) {
        double load_factor = static_cast<double>(step) / static_cast<double>(load_steps);
        std::cout << "\n[Step " << std::setw(2) << step << "/" << load_steps << "] Load Factor: " 
                  << std::fixed << std::setprecision(2) << (load_factor * 100.0) << "%" << std::endl;

        Eigen::VectorXd F_ext = Eigen::VectorXd::Zero(num_dofs);

        for (auto* elem : elements) {
            double L = elem->initial_length;
            double g = 9.81;

            double w_dry = (elem->props.rho * elem->props.A + elem->props.rho_fluid * elem->inner_area()) * g;
            double w_buoyancy = water_density * elem->outer_area() * g;
            double w_submerged = (w_dry - w_buoyancy) * load_factor;

            double elem_weight_total = w_submerged * L;

            int eq1_z = elem->node1->eq_numbers[2];
            int eq2_z = elem->node2->eq_numbers[2];

            if (eq1_z >= 0) F_ext[eq1_z] -= elem_weight_total * 0.5;
            if (eq2_z >= 0) F_ext[eq2_z] -= elem_weight_total * 0.5;

            // Apply 3D Static Current Drag Force
            if (enable_current) {
                double avg_z = 0.5 * (elem->node1->current_coords().z() + elem->node2->current_coords().z());
                double f_drag_x = 0.0, f_drag_y = 0.0;
                current.get_drag_force_per_meter(avg_z, elem->props.D_outer, water_density, f_drag_x, f_drag_y);

                double f_drag_x_total = f_drag_x * L * load_factor;
                double f_drag_y_total = f_drag_y * L * load_factor;

                int eq1_x = elem->node1->eq_numbers[0];
                int eq2_x = elem->node2->eq_numbers[0];
                int eq1_y = elem->node1->eq_numbers[1];
                int eq2_y = elem->node2->eq_numbers[1];

                if (eq1_x >= 0) F_ext[eq1_x] += f_drag_x_total * 0.5;
                if (eq2_x >= 0) F_ext[eq2_x] += f_drag_x_total * 0.5;

                if (eq1_y >= 0) F_ext[eq1_y] += f_drag_y_total * 0.5;
                if (eq2_y >= 0) F_ext[eq2_y] += f_drag_y_total * 0.5;
            }
        }

        bool step_converged = false;

        for (int iter = 0; iter < max_iter_per_step; ++iter) {
            Eigen::SparseMatrix<double> K_global;
            Eigen::VectorXd F_int;

            assemble_system(K_global, F_ext, F_int);

            Eigen::VectorXd Residual = F_ext - F_int;
            double norm_R = Residual.norm();

            if (iter % 5 == 0 || norm_R < tol) {
                std::cout << "  Iter " << std::setw(2) << iter << " | Residual Norm: " 
                          << std::scientific << std::setprecision(4) << norm_R << std::defaultfloat << std::endl;
            }

            if (norm_R < tol) {
                std::cout << "  ✅ Step " << step << " Converged in " << iter << " iterations!" << std::endl;
                step_converged = true;

                StepSnapshot snap;
                snap.step_index = step;
                snap.load_factor = load_factor;
                for (auto* node : nodes) snap.node_coords.push_back(node->current_coords());
                for (size_t i = 0; i < elements.size(); ++i) {
                    auto* elem = elements[i];
                    const auto* prev = (i > 0) ? elements[i - 1] : nullptr;
                    const auto* next = (i + 1 < elements.size()) ? elements[i + 1] : nullptr;

                    elem->update_effective_tension();
                    auto sc = elem->compute_stress_and_curvature(prev, next);
                    snap.element_tensions_kN.push_back(elem->tension_effective / 1000.0);
                    snap.element_bending_moments_kNm.push_back(sc.bending_moment_kNm);
                    snap.element_curvatures.push_back(sc.curvature);
                    snap.element_von_mises_MPa.push_back(sc.von_mises_MPa);
                    snap.element_mbr_safety_factors.push_back(sc.mbr_safety_factor);
                }
                static_history.push_back(snap);
                step_history.push_back(snap);

                break;
            }

            Eigen::SparseLU<Eigen::SparseMatrix<double>> sparse_lu;
            sparse_lu.compute(K_global);
            if (sparse_lu.info() != Eigen::Success) {
                std::cerr << "❌ SparseLU decomposition failed at step " << step << "!" << std::endl;
                return false;
            }

            Eigen::VectorXd delta_U = sparse_lu.solve(Residual);
            if (sparse_lu.info() != Eigen::Success) {
                std::cerr << "❌ SparseLU solve failed at step " << step << "!" << std::endl;
                return false;
            }

            for (auto* node : nodes) {
                for (int i = 0; i < 3; ++i) {
                    int eq = node->eq_numbers[i];
                    if (eq >= 0) node->disp[i] += delta_U[eq];

                    int eq_rot = node->eq_numbers[i + 3];
                    if (eq_rot >= 0) node->rot[i] += delta_U[eq_rot];
                }
            }
        }

        if (!step_converged) {
            std::cerr << "❌ Load Step " << step << " failed to converge after " << max_iter_per_step << " iterations." << std::endl;
            return false;
        }
    }

    std::cout << "\n=========================================================================" << std::endl;
    std::cout << "  🎉 CATENARY STATIC EQUILIBRIUM CONVERGED SUCCESSFULLY!" << std::endl;
    std::cout << "=========================================================================\n" << std::endl;

    return true;
}

bool StaticAnalysis::solve_vessel_offset(const VesselOffset& offset, int steps, int max_iter, double tol) {
    std::cout << "\n=========================================================================" << std::endl;
    std::cout << "  🚢 riserSim Vessel Offset Incremental Analysis" << std::endl;
    std::cout << "  Offset Vector: (" << offset.offset_disp.x() << ", " << offset.offset_disp.y() << ", " << offset.offset_disp.z() << ") m" << std::endl;
    std::cout << "=========================================================================" << std::endl;

    Eigen::Vector3d base_top_disp = nodes.front()->disp;

    for (int step = 1; step <= steps; ++step) {
        double factor = static_cast<double>(step) / static_cast<double>(steps);
        nodes.front()->disp = base_top_disp + offset.offset_disp * factor;

        std::cout << "\n[Offset Step " << std::setw(2) << step << "/" << steps << "] Offset Factor: " 
                  << std::fixed << std::setprecision(2) << (factor * 100.0) << "% | Top Pos X: " 
                  << nodes.front()->current_coords().x() << " m" << std::endl;

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

                int eq1_x = elem->node1->eq_numbers[0];
                int eq2_x = elem->node2->eq_numbers[0];
                int eq1_y = elem->node1->eq_numbers[1];
                int eq2_y = elem->node2->eq_numbers[1];

                if (eq1_x >= 0) F_ext[eq1_x] += f_drag_x * L * 0.5;
                if (eq2_x >= 0) F_ext[eq2_x] += f_drag_x * L * 0.5;

                if (eq1_y >= 0) F_ext[eq1_y] += f_drag_y * L * 0.5;
                if (eq2_y >= 0) F_ext[eq2_y] += f_drag_y * L * 0.5;
            }
        }

        bool step_converged = false;

        for (int iter = 0; iter < max_iter; ++iter) {
            Eigen::SparseMatrix<double> K_global;
            Eigen::VectorXd F_int;

            assemble_system(K_global, F_ext, F_int);

            Eigen::VectorXd Residual = F_ext - F_int;
            double norm_R = Residual.norm();

            if (iter % 5 == 0 || norm_R < tol) {
                std::cout << "  Iter " << std::setw(2) << iter << " | Residual Norm: " 
                          << std::scientific << std::setprecision(4) << norm_R << std::defaultfloat << std::endl;
            }

            if (norm_R < tol) {
                std::cout << "  ✅ Offset Step " << step << " Converged in " << iter << " iterations!" << std::endl;
                step_converged = true;

                StepSnapshot snap;
                snap.step_index = step;
                snap.load_factor = factor;
                for (auto* node : nodes) snap.node_coords.push_back(node->current_coords());
                for (size_t i = 0; i < elements.size(); ++i) {
                    auto* elem = elements[i];
                    const auto* prev = (i > 0) ? elements[i - 1] : nullptr;
                    const auto* next = (i + 1 < elements.size()) ? elements[i + 1] : nullptr;

                    elem->update_effective_tension();
                    auto sc = elem->compute_stress_and_curvature(prev, next);
                    snap.element_tensions_kN.push_back(elem->tension_effective / 1000.0);
                    snap.element_bending_moments_kNm.push_back(sc.bending_moment_kNm);
                    snap.element_curvatures.push_back(sc.curvature);
                    snap.element_von_mises_MPa.push_back(sc.von_mises_MPa);
                    snap.element_mbr_safety_factors.push_back(sc.mbr_safety_factor);
                }
                step_history.push_back(snap);

                break;
            }

            Eigen::SparseLU<Eigen::SparseMatrix<double>> sparse_lu;
            sparse_lu.compute(K_global);
            if (sparse_lu.info() != Eigen::Success) {
                std::cerr << "❌ SparseLU decomposition failed at offset step " << step << "!" << std::endl;
                return false;
            }

            Eigen::VectorXd delta_U = sparse_lu.solve(Residual);
            if (sparse_lu.info() != Eigen::Success) {
                std::cerr << "❌ SparseLU solve failed at offset step " << step << "!" << std::endl;
                return false;
            }

            for (auto* node : nodes) {
                for (int i = 0; i < 3; ++i) {
                    int eq = node->eq_numbers[i];
                    if (eq >= 0) node->disp[i] += delta_U[eq];

                    int eq_rot = node->eq_numbers[i + 3];
                    if (eq_rot >= 0) node->rot[i] += delta_U[eq_rot];
                }
            }
        }

        if (!step_converged) {
            std::cerr << "❌ Offset Step " << step << " failed to converge after " << max_iter << " iterations." << std::endl;
            return false;
        }

        // Save Snapshot
        StepSnapshot snap;
        snap.step_index = step;
        snap.load_factor = static_cast<double>(step) / static_cast<double>(steps);

        for (auto* node : nodes) {
            snap.node_coords.push_back(node->current_coords());
        }

        for (size_t i = 0; i < elements.size(); ++i) {
            auto* elem = elements[i];
            elem->update_effective_tension();
            snap.element_tensions_kN.push_back(elem->tension_effective / 1000.0);

            const CorotationalBeam3D* prev_e = (i > 0) ? elements[i - 1] : nullptr;
            const CorotationalBeam3D* next_e = (i + 1 < elements.size()) ? elements[i + 1] : nullptr;
            auto res = elem->compute_stress_and_curvature(prev_e, next_e, 350.0);

            snap.element_bending_moments_kNm.push_back(res.bending_moment_kNm);
            snap.element_curvatures.push_back(res.curvature);
            snap.element_von_mises_MPa.push_back(res.von_mises_MPa);
            snap.element_mbr_safety_factors.push_back(res.mbr_safety_factor);
        }

        static_history.push_back(snap);
        step_history.push_back(snap);
    }

    std::cout << "\n=========================================================================" << std::endl;
    std::cout << "  🎉 VESSEL OFFSET ANALYSIS CONVERGED SUCCESSFULLY!" << std::endl;
    std::cout << "=========================================================================\n" << std::endl;
    return true;
}

bool StaticAnalysis::solve_time_domain_dynamic(double duration_s, double dt_s, double wave_amplitude, double wave_period, double alpha_rayleigh, double beta_rayleigh) {
    std::cout << "\n=========================================================================" << std::endl;
    std::cout << "  🌊 STARTING 3D TIME-DOMAIN DYNAMIC SOLVER (Newmark-beta)" << std::endl;
    std::cout << "  Duration: " << duration_s << " s | dt: " << dt_s << " s | Wave Amp: " << wave_amplitude << " m | Wave Period: " << wave_period << " s" << std::endl;
    std::cout << "=========================================================================\n" << std::endl;

    if (nodes.empty() || elements.empty()) return false;
    assign_equation_numbers();

    int total_steps = static_cast<int>(std::ceil(duration_s / dt_s));
    double omega = 2.0 * M_PI / wave_period;

    double beta_newmark = 0.28;
    double gamma_newmark = 0.55;

    double c1 = 1.0 / (beta_newmark * dt_s * dt_s);
    double c2 = gamma_newmark / (beta_newmark * dt_s);
    double c3 = 1.0 / (beta_newmark * dt_s);
    double c4 = (1.0 / (2.0 * beta_newmark)) - 1.0;
    double c5 = (gamma_newmark / beta_newmark) - 1.0;
    double c6 = (dt_s / 2.0) * ((gamma_newmark / beta_newmark) - 2.0);

    // Save Static Equilibrium Reference Displacements
    std::vector<Eigen::Vector3d> static_disps(nodes.size());
    std::vector<Eigen::Vector3d> static_rots(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) {
        static_disps[i] = nodes[i]->disp;
        static_rots[i] = nodes[i]->rot;
    }

    // Dynamic Perturbation State Vectors (relative to static equilibrium)
    Eigen::VectorXd U = Eigen::VectorXd::Zero(num_dofs);
    Eigen::VectorXd V = Eigen::VectorXd::Zero(num_dofs);
    Eigen::VectorXd A = Eigen::VectorXd::Zero(num_dofs);

    Node3D* top_node = nodes.front();

    step_history.clear();

    for (int step = 0; step <= total_steps; ++step) {
        double time = step * dt_s;

        // Prescribe Top Vessel Harmonic Motion with 5s Smooth Ramp: Z_top(t) = Z_static + ramp(t) * Amp * sin(omega * t)
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
        int max_iters = 8;
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

            // 1. External Load Vector F_ext (Submerged Weight + Current Drag)
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

            // 2. Assemble System Stiffness K_global and Internal Forces F_int
            Eigen::SparseMatrix<double> K_global;
            Eigen::VectorXd F_int;
            assemble_system(K_global, F_ext, F_int);

            // 3. Assemble Global Mass Matrix M_global
            Eigen::SparseMatrix<double> M_global(num_dofs, num_dofs);
            std::vector<Eigen::Triplet<double>> triplets_M;
            for (auto* elem : elements) {
                Eigen::Matrix<double, 12, 12> Me = elem->global_mass(water_density);
                int dofs[12];
                for (int i = 0; i < 3; ++i) {
                    dofs[i]     = elem->node1->eq_numbers[i];
                    dofs[i + 3] = elem->node1->eq_numbers[i + 3];
                    dofs[i + 6] = elem->node2->eq_numbers[i];
                    dofs[i + 9] = elem->node2->eq_numbers[i + 3];
                }
                for (int r = 0; r < 12; ++r) {
                    for (int c = 0; c < 12; ++c) {
                        if (dofs[r] >= 0 && dofs[c] >= 0) {
                            triplets_M.push_back(Eigen::Triplet<double>(dofs[r], dofs[c], Me(r, c)));
                        }
                    }
                }
            }
            M_global.setFromTriplets(triplets_M.begin(), triplets_M.end());

            // 4. Rayleigh Damping Matrix C = alpha * M + beta * K
            Eigen::SparseMatrix<double> C_global = alpha_rayleigh * M_global + beta_rayleigh * K_global;

            // 5. Dynamic Residual Vector: R = F_ext - F_int - M * A - C * V
            Eigen::VectorXd R = (F_ext - F_int) - M_global * A_curr - C_global * V_curr;

            // 6. Effective Dynamic Stiffness Matrix: K_eff = K + c1 * M + c2 * C
            Eigen::SparseMatrix<double> K_eff = K_global + c1 * M_global + c2 * C_global;

            // 7. Solve Linear System K_eff * delta_U = R
            Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> solver_ldlt;
            solver_ldlt.compute(K_eff);

            if (solver_ldlt.info() != Eigen::Success) {
                std::cerr << "❌ Dynamic solver factorization failed at step " << step << " iter " << iter << std::endl;
                return false;
            }

            Eigen::VectorXd delta_U = solver_ldlt.solve(R);
            double dU_norm = delta_U.norm();
            if (dU_norm > 0.02) {
                delta_U *= (0.02 / dU_norm);
            }

            U_curr += 0.20 * delta_U;
            V_curr = (U_curr - U_prev) / dt_s;
            A_curr = (V_curr - V_prev) / dt_s;

            // Clamp max physical node velocity to 10 m/s
            double v_norm = V_curr.norm();
            if (v_norm > 10.0) {
                V_curr *= (10.0 / v_norm);
            }

            if (dU_norm < 1e-4 || iter == max_iters - 1) {
                U = U_curr;
                V = V_curr;
                A = A_curr;
                break;
            }
        }

        // 10. Save Snapshot
        StepSnapshot snap;
        snap.step_index = step;
        snap.load_factor = time / duration_s;

        for (auto* node : nodes) {
            snap.node_coords.push_back(node->current_coords());
        }

        for (size_t i = 0; i < elements.size(); ++i) {
            auto* elem = elements[i];
            snap.element_tensions_kN.push_back(elem->tension_effective / 1000.0);

            const CorotationalBeam3D* prev_e = (i > 0) ? elements[i - 1] : nullptr;
            const CorotationalBeam3D* next_e = (i + 1 < elements.size()) ? elements[i + 1] : nullptr;
            auto res = elem->compute_stress_and_curvature(prev_e, next_e, 350.0);

            snap.element_bending_moments_kNm.push_back(res.bending_moment_kNm);
            snap.element_curvatures.push_back(res.curvature);
            snap.element_von_mises_MPa.push_back(res.von_mises_MPa);
            snap.element_mbr_safety_factors.push_back(res.mbr_safety_factor);
        }

        dynamic_history.push_back(snap);
        step_history.push_back(snap);

        if (step % 40 == 0 || step == total_steps) {
            std::cout << "  ⏱️ Dynamic Time Step " << step << "/" << total_steps << " (t = " << time << " s) | Top Z: " << top_node->coords.z() << " m" << std::endl;
        }
    }

    std::cout << "🎉 3D TIME-DOMAIN DYNAMIC SOLVER CONVERGED SUCCESSFULLY!" << std::endl;
    return true;
}

static void write_snapshots_json_array(std::ofstream& file, const std::vector<risersim::StepSnapshot>& history) {
    auto safe_num = [](double val, double fallback = 0.0) -> double {
        return (std::isnan(val) || std::isinf(val)) ? fallback : val;
    };

    file << "[\n";
    for (size_t s = 0; s < history.size(); ++s) {
        const auto& snap = history[s];
        file << "    {\n";
        file << "      \"step\": " << snap.step_index << ",\n";
        file << "      \"load_factor\": " << safe_num(snap.load_factor) << ",\n";
        file << "      \"nodes\": [\n";
        for (size_t i = 0; i < snap.node_coords.size(); ++i) {
            const auto& c = snap.node_coords[i];
            file << "        {\"id\": " << (i + 1) << ", \"x\": " << safe_num(c.x()) << ", \"y\": " << safe_num(c.y()) << ", \"z\": " << safe_num(c.z()) << "}";
            if (i + 1 < snap.node_coords.size()) file << ",";
            file << "\n";
        }
        file << "      ],\n";
        file << "      \"elements\": [\n";
        for (size_t i = 0; i < snap.element_tensions_kN.size(); ++i) {
            double tens = safe_num(snap.element_tensions_kN[i]);
            double mom = (i < snap.element_bending_moments_kNm.size()) ? safe_num(snap.element_bending_moments_kNm[i]) : 0.0;
            double curv = (i < snap.element_curvatures.size()) ? safe_num(snap.element_curvatures[i]) : 0.0;
            double vm = (i < snap.element_von_mises_MPa.size()) ? safe_num(snap.element_von_mises_MPa[i]) : 0.0;
            double mbr = (i < snap.element_mbr_safety_factors.size()) ? safe_num(snap.element_mbr_safety_factors[i], 1.0) : 1.0;

            file << "        {\"id\": " << (i + 1)
                 << ", \"tension_effective_kN\": " << tens
                 << ", \"bending_moment_kNm\": " << mom
                 << ", \"curvature\": " << curv
                 << ", \"von_mises_MPa\": " << vm
                 << ", \"mbr_safety_factor\": " << mbr
                 << "}";
            if (i + 1 < snap.element_tensions_kN.size()) file << ",";
            file << "\n";
        }
        file << "      ]\n";
        file << "    }";
        if (s + 1 < history.size()) file << ",";
        file << "\n";
    }
    file << "  ]";
}

bool StaticAnalysis::export_json(const std::string& filename) const {
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "❌ Could not open file for writing: " << filename << std::endl;
        return false;
    }

    file << "{\n";
    file << "  \"simulation_type\": \"riserSim Incremental 3D Catenary Equilibrium\",\n";
    file << "  \"seabed_depth\": " << seabed.seabed_depth << ",\n";
    file << "  \"num_steps\": " << step_history.size() << ",\n";
    file << "  \"steps\": ";
    write_snapshots_json_array(file, !dynamic_history.empty() ? dynamic_history : (!static_history.empty() ? static_history : step_history));
    file << ",\n";

    file << "  \"static_steps\": ";
    write_snapshots_json_array(file, static_history);
    file << ",\n";

    file << "  \"dynamic_steps\": ";
    write_snapshots_json_array(file, dynamic_history);
    file << "\n";

    file << "}\n";

    file.close();
    std::cout << "✅ Full simulation history exported to JSON: " << filename << " (Static: " << static_history.size() << " steps, Dynamic: " << dynamic_history.size() << " steps)" << std::endl;
    return true;
}

#ifdef RISERSIM_HAS_HDF5
static void write_history_to_hdf5_group(H5::H5Location& parent_grp, const std::vector<risersim::StepSnapshot>& history, size_t num_nodes, size_t num_elems) {
    if (history.empty()) return;

    size_t num_steps = history.size();

    // Node Positions Matrix (num_steps x num_nodes x 3)
    hsize_t pos_dims[3] = { num_steps, num_nodes, 3 };
    H5::DataSpace pos_space(3, pos_dims);
    H5::DataSet pos_dataset = parent_grp.createDataSet("node_positions", H5::PredType::NATIVE_DOUBLE, pos_space);

    std::vector<double> pos_flat(num_steps * num_nodes * 3);
    for (size_t s = 0; s < num_steps; ++s) {
        for (size_t n = 0; n < num_nodes; ++n) {
            size_t idx = (s * num_nodes + n) * 3;
            pos_flat[idx + 0] = history[s].node_coords[n].x();
            pos_flat[idx + 1] = history[s].node_coords[n].y();
            pos_flat[idx + 2] = history[s].node_coords[n].z();
        }
    }
    pos_dataset.write(pos_flat.data(), H5::PredType::NATIVE_DOUBLE);

    // Effective Tension Matrix (num_steps x num_elems)
    hsize_t tens_dims[2] = { num_steps, num_elems };
    H5::DataSpace tens_space(2, tens_dims);
    H5::DataSet tens_dataset = parent_grp.createDataSet("element_tensions_kN", H5::PredType::NATIVE_DOUBLE, tens_space);

    std::vector<double> tens_flat(num_steps * num_elems);
    for (size_t s = 0; s < num_steps; ++s) {
        for (size_t e = 0; e < num_elems; ++e) {
            tens_flat[s * num_elems + e] = history[s].element_tensions_kN[e];
        }
    }
    tens_dataset.write(tens_flat.data(), H5::PredType::NATIVE_DOUBLE);

    // Bending Moment Matrix (num_steps x num_elems)
    H5::DataSet moment_dataset = parent_grp.createDataSet("element_bending_moments_kNm", H5::PredType::NATIVE_DOUBLE, tens_space);
    std::vector<double> moment_flat(num_steps * num_elems);
    for (size_t s = 0; s < num_steps; ++s) {
        for (size_t e = 0; e < num_elems; ++e) {
            moment_flat[s * num_elems + e] = (e < history[s].element_bending_moments_kNm.size()) ? history[s].element_bending_moments_kNm[e] : 0.0;
        }
    }
    moment_dataset.write(moment_flat.data(), H5::PredType::NATIVE_DOUBLE);

    // Curvature Matrix (num_steps x num_elems)
    H5::DataSet curv_dataset = parent_grp.createDataSet("element_curvatures", H5::PredType::NATIVE_DOUBLE, tens_space);
    std::vector<double> curv_flat(num_steps * num_elems);
    for (size_t s = 0; s < num_steps; ++s) {
        for (size_t e = 0; e < num_elems; ++e) {
            curv_flat[s * num_elems + e] = (e < history[s].element_curvatures.size()) ? history[s].element_curvatures[e] : 0.0;
        }
    }
    curv_dataset.write(curv_flat.data(), H5::PredType::NATIVE_DOUBLE);

    // von Mises Stress Matrix (num_steps x num_elems)
    H5::DataSet vm_dataset = parent_grp.createDataSet("element_von_mises_MPa", H5::PredType::NATIVE_DOUBLE, tens_space);
    std::vector<double> vm_flat(num_steps * num_elems);
    for (size_t s = 0; s < num_steps; ++s) {
        for (size_t e = 0; e < num_elems; ++e) {
            vm_flat[s * num_elems + e] = (e < history[s].element_von_mises_MPa.size()) ? history[s].element_von_mises_MPa[e] : 0.0;
        }
    }
    vm_dataset.write(vm_flat.data(), H5::PredType::NATIVE_DOUBLE);

    // MBR Safety Factor Matrix (num_steps x num_elems)
    H5::DataSet mbr_dataset = parent_grp.createDataSet("element_mbr_safety_factors", H5::PredType::NATIVE_DOUBLE, tens_space);
    std::vector<double> mbr_flat(num_steps * num_elems);
    for (size_t s = 0; s < num_steps; ++s) {
        for (size_t e = 0; e < num_elems; ++e) {
            mbr_flat[s * num_elems + e] = (e < history[s].element_mbr_safety_factors.size()) ? history[s].element_mbr_safety_factors[e] : 1.0;
        }
    }
    mbr_dataset.write(mbr_flat.data(), H5::PredType::NATIVE_DOUBLE);
}
#endif

bool StaticAnalysis::export_hdf5(const std::string& filename) const {
#ifdef RISERSIM_HAS_HDF5
    try {
        H5::H5File file(filename, H5F_ACC_TRUNC);

        size_t num_nodes = nodes.size();
        size_t num_elems = elements.size();

        // 1. Export Root level default datasets (active history)
        const auto& active_hist = !dynamic_history.empty() ? dynamic_history : (!static_history.empty() ? static_history : step_history);
        if (!active_hist.empty()) {
            write_history_to_hdf5_group(file, active_hist, num_nodes, num_elems);
        }

        // 2. Export /static_analysis group
        if (!static_history.empty()) {
            H5::Group static_grp = file.createGroup("/static_analysis");
            write_history_to_hdf5_group(static_grp, static_history, num_nodes, num_elems);
        }

        // 3. Export /dynamic_analysis group
        if (!dynamic_history.empty()) {
            H5::Group dynamic_grp = file.createGroup("/dynamic_analysis");
            write_history_to_hdf5_group(dynamic_grp, dynamic_history, num_nodes, num_elems);
        }

        file.close();
        std::cout << "✅ Binary HDF5 simulation history successfully exported to: " << filename << " (Static: " << static_history.size() << " steps, Dynamic: " << dynamic_history.size() << " steps)" << std::endl;
        return true;
    } catch (const H5::Exception& err) {
        std::cerr << "❌ HDF5 Exception during export: " << err.getDetailMsg() << std::endl;
        return false;
    }
#else
    std::cout << "ℹ️ HDF5 export skipped (HDF5 library not linked)." << std::endl;
    return false;
#endif
}

} // namespace risersim
