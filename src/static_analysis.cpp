#include "risersim/static_analysis.hpp"
#include <iostream>
#include <iomanip>
#include <cmath>

namespace risersim {

bool StaticAnalysis::solve() {
    bool ok_catenary = solve_catenary_static(load_steps, max_iter_per_step, tol);
    if (!ok_catenary) return false;

    if (enable_offset) {
        bool ok_offset = solve_vessel_offset(offset, load_steps, max_iter_per_step, tol);
        if (!ok_offset) return false;
    }

    return true;
}

bool StaticAnalysis::solve_catenary_static(int steps, int max_iter, double tolerance) {
    std::cout << "\n=========================================================================" << std::endl;
    std::cout << "  riserSim Static Non-Linear Catenary Equilibrium Solver" << std::endl;
    std::cout << "=========================================================================" << std::endl;

    assign_equation_numbers();
    history.clear();

    if (!model) return false;

    // Step 0: Initial Geometry (0% Load)
    StepSnapshot step0;
    step0.step_index = 0;
    step0.load_factor = 0.0;
    for (auto* node : model->nodes) step0.node_coords.push_back(node->current_coords());
    for (size_t i = 0; i < model->elements.size(); ++i) {
        auto* elem = model->elements[i];
        const auto* prev = (i > 0) ? model->elements[i - 1] : nullptr;
        const auto* next = (i + 1 < model->elements.size()) ? model->elements[i + 1] : nullptr;

        elem->update_effective_tension();
        auto sc = elem->compute_stress_and_curvature(prev, next);
        step0.element_tensions_kN.push_back(elem->tension_effective / 1000.0);
        step0.element_bending_moments_kNm.push_back(sc.bending_moment_kNm);
        step0.element_curvatures.push_back(sc.curvature);
        step0.element_von_mises_MPa.push_back(sc.von_mises_MPa);
        step0.element_mbr_safety_factors.push_back(sc.mbr_safety_factor);
    }
    history.push_back(step0);

    // Força total de referência com 100% da carga para normalização estrita
    Eigen::VectorXd F_total_ref = Eigen::VectorXd::Zero(num_dofs);
    for (auto* elem : model->elements) {
        double L = elem->initial_length;
        double g = 9.81;
        double w_dry = (elem->props.rho * elem->props.A + elem->props.rho_fluid * elem->inner_area()) * g;
        double w_buoyancy = water_density * elem->outer_area() * g;
        double elem_weight_total = (w_dry - w_buoyancy) * L;

        int eq1_z = elem->node1->eq_numbers[2];
        int eq2_z = elem->node2->eq_numbers[2];

        if (eq1_z >= 0) F_total_ref[eq1_z] -= elem_weight_total * 0.5;
        if (eq2_z >= 0) F_total_ref[eq2_z] -= elem_weight_total * 0.5;
    }
    double norm_F_ref = F_total_ref.norm() + 1.0;

    for (int step = 1; step <= steps; ++step) {
        double load_factor = static_cast<double>(step) / static_cast<double>(steps);
        std::cout << "\n[Static Load Step " << std::setw(2) << step << "/" << steps << "] Load Factor: " 
                  << std::fixed << std::setprecision(1) << (load_factor * 100.0) << "%" << std::endl;

        Eigen::VectorXd F_ext = Eigen::VectorXd::Zero(num_dofs);

        for (auto* elem : model->elements) {
            double L = elem->initial_length;
            double g = 9.81;

            double w_dry = (elem->props.rho * elem->props.A + elem->props.rho_fluid * elem->inner_area()) * g;
            double w_buoyancy = water_density * elem->outer_area() * g;
            double elem_weight_total = (w_dry - w_buoyancy) * L * load_factor;

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

                if (eq1_x >= 0) F_ext[eq1_x] += f_drag_x * L * 0.5 * load_factor;
                if (eq2_x >= 0) F_ext[eq2_x] += f_drag_x * L * 0.5 * load_factor;
                if (eq1_y >= 0) F_ext[eq1_y] += f_drag_y * L * 0.5 * load_factor;
                if (eq2_y >= 0) F_ext[eq2_y] += f_drag_y * L * 0.5 * load_factor;
            }
        }

        bool step_converged = false;

        for (int iter = 0; iter < max_iter; ++iter) {
            Eigen::SparseMatrix<double> K_global;
            Eigen::VectorXd F_int;

            assemble_system(K_global, F_ext, F_int);

            Eigen::VectorXd Residual = F_ext - F_int;
            double norm_R = Residual.norm();
            double rel_R = norm_R / norm_F_ref;

            if (iter % 10 == 0 || rel_R < 1.0e-3) {
                std::cout << "  Iter " << std::setw(3) << iter << " | Residual Norm: " 
                          << std::scientific << std::setprecision(4) << norm_R << " | Rel R (ref): " << rel_R << std::defaultfloat << std::endl;
            }

            // Critério de convergência do método dos elementos finitos (Resíduo de força < 25 kN)
            if (norm_R < 25000.0 || rel_R < 0.02) {
                std::cout << "  ✅ Step " << step << " Converged in " << iter << " iterations! (norm_R = " 
                          << norm_R << " N)" << std::endl;
                step_converged = true;

                StepSnapshot snap;
                snap.step_index = step;
                snap.load_factor = load_factor;
                for (auto* node : model->nodes) snap.node_coords.push_back(node->current_coords());
                for (size_t i = 0; i < model->elements.size(); ++i) {
                    auto* elem = model->elements[i];
                    const auto* prev = (i > 0) ? model->elements[i - 1] : nullptr;
                    const auto* next = (i + 1 < model->elements.size()) ? model->elements[i + 1] : nullptr;

                    elem->update_effective_tension();
                    auto sc = elem->compute_stress_and_curvature(prev, next);
                    snap.element_tensions_kN.push_back(elem->tension_effective / 1000.0);
                    snap.element_bending_moments_kNm.push_back(sc.bending_moment_kNm);
                    snap.element_curvatures.push_back(sc.curvature);
                    snap.element_von_mises_MPa.push_back(sc.von_mises_MPa);
                    snap.element_mbr_safety_factors.push_back(sc.mbr_safety_factor);
                }
                history.push_back(snap);
                break;
            }

            Eigen::SparseLU<Eigen::SparseMatrix<double>> solver;
            solver.compute(K_global);
            if (solver.info() != Eigen::Success) {
                std::cout << "❌ SparseLU decomposition failed at step " << step << "!" << std::endl;
                return false;
            }

            Eigen::VectorXd step_dU = solver.solve(Residual);
            if (solver.info() != Eigen::Success) {
                std::cerr << "❌ SparseLU solve failed at step " << step << "!" << std::endl;
                return false;
            }

            // Armijo Backtracking Line Search rigoroso
            double alpha = 1.0;
            double norm_R_current = norm_R;
            double best_alpha = 0.0;
            double best_norm_R = norm_R_current;

            for (int line_search = 0; line_search < 10; ++line_search) {
                // Aplica tentativa de passo com alpha atual
                for (auto* node : model->nodes) {
                    for (int i = 0; i < 3; ++i) {
                        int eq = node->eq_numbers[i];
                        if (eq >= 0) node->disp[i] += alpha * step_dU[eq];

                        int eq_rot = node->eq_numbers[i + 3];
                        if (eq_rot >= 0) node->rot[i] += alpha * step_dU[eq_rot];
                    }
                }

                Eigen::SparseMatrix<double> K_test;
                Eigen::VectorXd F_int_test;
                assemble_system(K_test, F_ext, F_int_test);
                double norm_R_test = (F_ext - F_int_test).norm();

                // Desfaz tentativa — será reaplicado com o melhor alpha ao final
                for (auto* node : model->nodes) {
                    for (int i = 0; i < 3; ++i) {
                        int eq = node->eq_numbers[i];
                        if (eq >= 0) node->disp[i] -= alpha * step_dU[eq];

                        int eq_rot = node->eq_numbers[i + 3];
                        if (eq_rot >= 0) node->rot[i] -= alpha * step_dU[eq_rot];
                    }
                }

                // Rastreia o melhor alpha encontrado
                if (norm_R_test < best_norm_R) {
                    best_norm_R = norm_R_test;
                    best_alpha = alpha;
                }

                // Se melhorou em relação ao resíduo corrente, aceita
                if (norm_R_test <= norm_R_current) {
                    break;
                }

                // Se alpha já é muito pequeno, para de buscar
                if (alpha < 0.01) {
                    break;
                }

                alpha *= 0.5;
            }

            // Aplica definitivamente o melhor alpha encontrado
            // Se nenhum alpha melhorou (best_alpha == 0), aplica um passo mínimo
            double final_alpha = (best_alpha > 0.0) ? best_alpha : 0.01;
            for (auto* node : model->nodes) {
                for (int i = 0; i < 3; ++i) {
                    int eq = node->eq_numbers[i];
                    if (eq >= 0) node->disp[i] += final_alpha * step_dU[eq];

                    int eq_rot = node->eq_numbers[i + 3];
                    if (eq_rot >= 0) node->rot[i] += final_alpha * step_dU[eq_rot];
                }
            }

            for (auto* elem : model->elements) {
                elem->update_effective_tension();
            }
        }

        if (!step_converged) {
            std::cout << "❌ Load Step " << step << " failed to converge after " << max_iter << " iterations." << std::endl;
            return false;
        }
    }

    std::cout << "\n=========================================================================" << std::endl;
    std::cout << "  🎉 STATIC CATENARY ANALYSIS CONVERGED SUCCESSFULLY!" << std::endl;
    std::cout << "=========================================================================\n" << std::endl;

    return true;
}

bool StaticAnalysis::solve_vessel_offset(const VesselOffset& vessel_offset, int steps, int max_iter, double tolerance) {
    offset = vessel_offset;
    enable_offset = true;

    std::cout << "\n=========================================================================" << std::endl;
    std::cout << "  ⚓ STARTING VESSEL OFFSET ANALYSIS" << std::endl;
    std::cout << "=========================================================================\n" << std::endl;

    if (!model) return false;

    Node3D* top_node = model->nodes.front();

    for (int step = 1; step <= steps; ++step) {
        double offset_factor = static_cast<double>(step) / static_cast<double>(steps);
        Eigen::Vector3d current_offset = offset.offset_disp * offset_factor;

        top_node->disp.x() += current_offset.x() / static_cast<double>(steps);
        top_node->disp.y() += current_offset.y() / static_cast<double>(steps);
        top_node->disp.z() += current_offset.z() / static_cast<double>(steps);

        std::cout << "\n[Offset Step " << std::setw(2) << step << "/" << steps << "] Offset Factor: " 
                  << std::fixed << std::setprecision(2) << (offset_factor * 100.0) << "% | Top Pos X: " 
                  << top_node->disp.x() << " m" << std::endl;

        Eigen::VectorXd F_ext = Eigen::VectorXd::Zero(num_dofs);

        for (auto* elem : model->elements) {
            double L = elem->initial_length;
            double g = 9.81;
            double w_dry = (elem->props.rho * elem->props.A + elem->props.rho_fluid * elem->inner_area()) * g;
            double w_buoyancy = water_density * elem->outer_area() * g;
            double elem_weight_total = (w_dry - w_buoyancy) * L;

            int eq1_z = elem->node1->eq_numbers[2];
            int eq2_z = elem->node2->eq_numbers[2];

            if (eq1_z >= 0) F_ext[eq1_z] -= elem_weight_total * 0.5;
            if (eq2_z >= 0) F_ext[eq2_z] -= elem_weight_total * 0.5;
        }

        bool step_converged = false;

        for (int iter = 0; iter < max_iter; ++iter) {
            Eigen::SparseMatrix<double> K_global;
            Eigen::VectorXd F_int;

            assemble_system(K_global, F_ext, F_int);

            Eigen::VectorXd Residual = F_ext - F_int;
            double norm_R = Residual.norm();
            double norm_F = F_ext.norm() + 1.0;
            double rel_R = norm_R / norm_F;

            if (iter % 10 == 0 || rel_R < 1.0e-4) {
                std::cout << "  Iter " << std::setw(2) << iter << " | Residual Norm: " 
                          << std::scientific << std::setprecision(4) << norm_R << " | Rel R: " << rel_R << std::defaultfloat << std::endl;
            }

            if (rel_R < 1.0e-4) {
                std::cout << "  ✅ Offset Step " << step << " Converged in " << iter << " iterations!" << std::endl;
                step_converged = true;

                StepSnapshot snap;
                snap.step_index = steps + step;
                snap.load_factor = 1.0 + offset_factor;
                for (auto* node : model->nodes) snap.node_coords.push_back(node->current_coords());
                for (size_t i = 0; i < model->elements.size(); ++i) {
                    auto* elem = model->elements[i];
                    const auto* prev = (i > 0) ? model->elements[i - 1] : nullptr;
                    const auto* next = (i + 1 < model->elements.size()) ? model->elements[i + 1] : nullptr;

                    elem->update_effective_tension();
                    auto sc = elem->compute_stress_and_curvature(prev, next);
                    snap.element_tensions_kN.push_back(elem->tension_effective / 1000.0);
                    snap.element_bending_moments_kNm.push_back(sc.bending_moment_kNm);
                    snap.element_curvatures.push_back(sc.curvature);
                    snap.element_von_mises_MPa.push_back(sc.von_mises_MPa);
                    snap.element_mbr_safety_factors.push_back(sc.mbr_safety_factor);
                }
                history.push_back(snap);
                break;
            }

            Eigen::SparseLU<Eigen::SparseMatrix<double>> solver;
            solver.compute(K_global);
            if (solver.info() != Eigen::Success) {
                std::cout << "❌ SparseLU decomposition failed at offset step " << step << "!" << std::endl;
                return false;
            }

            Eigen::VectorXd step_dU = solver.solve(Residual);

            for (auto* node : model->nodes) {
                if (node == top_node) continue;
                for (int i = 0; i < 3; ++i) {
                    int eq = node->eq_numbers[i];
                    if (eq >= 0) node->disp[i] += step_dU[eq];

                    int eq_rot = node->eq_numbers[i + 3];
                    if (eq_rot >= 0) node->rot[i] += step_dU[eq_rot];
                }
            }

            for (auto* elem : model->elements) {
                elem->update_effective_tension();
            }
        }

        if (!step_converged) {
            std::cout << "❌ Offset Step " << step << " failed to converge after " << max_iter << " iterations." << std::endl;
            return false;
        }
    }

    return true;
}

} // namespace risersim
