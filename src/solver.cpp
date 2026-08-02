#include "risersim/solver.hpp"
#include <Eigen/SparseLU>
#include <iostream>
#include <fstream>
#include <iomanip>

namespace risersim {

void StaticAnalysis::assemble_system(Eigen::SparseMatrix<double>& K_global, const Eigen::VectorXd& F_ext, Eigen::VectorXd& F_int) {
    K_global.resize(num_dofs, num_dofs);
    K_global.setZero();
    F_int = Eigen::VectorXd::Zero(num_dofs);

    std::vector<Eigen::Triplet<double>> triplets;

    for (auto* elem : elements) {
        elem->update_effective_tension();
        
        if (elem->tension_effective < 10.0e3) {
            elem->tension_effective = 10.0e3;
        }

        Eigen::Matrix<double, 12, 12> K_elem = elem->global_stiffness();

        Eigen::Vector<double, 12> u_elem = Eigen::Vector<double, 12>::Zero();
        for (int i = 0; i < 3; ++i) {
            u_elem[i]     = elem->node1->disp[i];
            u_elem[i + 6] = elem->node2->disp[i];
        }

        Eigen::Vector<double, 12> f_int_elem = K_elem * u_elem;

        int eq[12];
        for (int i = 0; i < 3; ++i) {
            eq[i]     = elem->node1->eq_numbers[i];
            eq[i + 3] = elem->node1->eq_numbers[i + 3];
            eq[i + 6] = elem->node2->eq_numbers[i];
            eq[i + 9] = elem->node2->eq_numbers[i + 3];
        }

        for (int i = 0; i < 12; ++i) {
            if (eq[i] < 0) continue;
            F_int[eq[i]] += f_int_elem[i];

            for (int j = 0; j < 12; ++j) {
                if (eq[j] < 0) continue;
                triplets.push_back(Eigen::Triplet<double>(eq[i], eq[j], K_elem(i, j)));
            }
        }
    }

    // Apply Soil Reaction Stiffness K_z to nodes below seabed depth
    for (auto* node : nodes) {
        int eq_z = node->eq_numbers[2];
        if (eq_z >= 0) {
            double current_z = node->current_coords().z();
            double f_soil = 0.0, k_soil = 0.0;
            seabed.calculate_seabed_reaction(current_z, f_soil, k_soil);

            F_int[eq_z] -= f_soil;
            triplets.push_back(Eigen::Triplet<double>(eq_z, eq_z, k_soil));
        }
    }

    K_global.setFromTriplets(triplets.begin(), triplets.end());
}

bool StaticAnalysis::solve_catenary_static(int load_steps, int max_iter_per_step, double tol) {
    std::cout << "\n=========================================================================" << std::endl;
    std::cout << "  riserSim Static Non-Linear Catenary Equilibrium Solver" << std::endl;
    std::cout << "=========================================================================" << std::endl;

    assign_equation_numbers();
    step_history.clear();

    // Step 0: Initial Geometry (0% Load)
    StepSnapshot step0;
    step0.step_index = 0;
    step0.load_factor = 0.0;
    for (auto* node : nodes) step0.node_coords.push_back(node->current_coords());
    for (auto* elem : elements) step0.element_tensions_kN.push_back(elem->tension_effective / 1000.0);
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

                for (auto* elem : elements) elem->update_effective_tension();

                StepSnapshot snap;
                snap.step_index = step;
                snap.load_factor = load_factor;
                for (auto* node : nodes) snap.node_coords.push_back(node->current_coords());
                for (auto* elem : elements) snap.element_tensions_kN.push_back(elem->tension_effective / 1000.0);
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
                for (auto* elem : elements) snap.element_tensions_kN.push_back(elem->tension_effective / 1000.0);
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
    }

    std::cout << "\n=========================================================================" << std::endl;
    std::cout << "  🎉 VESSEL OFFSET ANALYSIS CONVERGED SUCCESSFULLY!" << std::endl;
    std::cout << "=========================================================================\n" << std::endl;

    return true;
}

bool StaticAnalysis::export_json(const std::string& filename) const {
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "❌ Failed to open output JSON file: " << filename << std::endl;
        return false;
    }

    file << "{\n";
    file << "  \"simulation_type\": \"riserSim Incremental 3D Catenary Equilibrium\",\n";
    file << "  \"seabed_depth\": " << seabed.seabed_depth << ",\n";
    file << "  \"num_steps\": " << step_history.size() << ",\n";
    file << "  \"steps\": [\n";

    for (size_t s = 0; s < step_history.size(); ++s) {
        const auto& snap = step_history[s];
        file << "    {\n";
        file << "      \"step\": " << snap.step_index << ",\n";
        file << "      \"load_factor\": " << snap.load_factor << ",\n";
        file << "      \"nodes\": [\n";
        for (size_t i = 0; i < snap.node_coords.size(); ++i) {
            const auto& c = snap.node_coords[i];
            file << "        {\"id\": " << (i + 1) << ", \"x\": " << c.x() << ", \"y\": " << c.y() << ", \"z\": " << c.z() << "}";
            if (i + 1 < snap.node_coords.size()) file << ",";
            file << "\n";
        }
        file << "      ],\n";
        file << "      \"elements\": [\n";
        for (size_t i = 0; i < snap.element_tensions_kN.size(); ++i) {
            file << "        {\"id\": " << (i + 1) << ", \"tension_effective_kN\": " << snap.element_tensions_kN[i] << "}";
            if (i + 1 < snap.element_tensions_kN.size()) file << ",";
            file << "\n";
        }
        file << "      ]\n";
        file << "    }";
        if (s + 1 < step_history.size()) file << ",";
        file << "\n";
    }

    file << "  ]\n";
    file << "}\n";

    file.close();
    std::cout << "✅ Full incremental simulation history exported to JSON: " << filename << std::endl;
    return true;
}

} // namespace risersim
