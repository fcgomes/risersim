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
    }

    std::cout << "\n=========================================================================" << std::endl;
    std::cout << "  🎉 VESSEL OFFSET ANALYSIS CONVERGED SUCCESSFULLY!" << std::endl;
    std::cout << "=========================================================================\n" << std::endl;

    return true;
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
            file << "        {\"id\": " << (i + 1)
                 << ", \"tension_effective_kN\": " << snap.element_tensions_kN[i]
                 << ", \"bending_moment_kNm\": " << (i < snap.element_bending_moments_kNm.size() ? snap.element_bending_moments_kNm[i] : 0.0)
                 << ", \"curvature\": " << (i < snap.element_curvatures.size() ? snap.element_curvatures[i] : 0.0)
                 << ", \"von_mises_MPa\": " << (i < snap.element_von_mises_MPa.size() ? snap.element_von_mises_MPa[i] : 0.0)
                 << ", \"mbr_safety_factor\": " << (i < snap.element_mbr_safety_factors.size() ? snap.element_mbr_safety_factors[i] : 1.0)
                 << "}";
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

bool StaticAnalysis::export_hdf5(const std::string& filename) const {
#ifdef RISERSIM_HAS_HDF5
    try {
        H5::H5File file(filename, H5F_ACC_TRUNC);

        size_t num_steps = step_history.size();
        size_t num_nodes = nodes.size();
        size_t num_elems = elements.size();

        // Node Positions Matrix (num_steps x num_nodes x 3)
        hsize_t pos_dims[3] = { num_steps, num_nodes, 3 };
        H5::DataSpace pos_space(3, pos_dims);
        H5::DataSet pos_dataset = file.createDataSet("node_positions", H5::PredType::NATIVE_DOUBLE, pos_space);

        std::vector<double> pos_flat(num_steps * num_nodes * 3);
        for (size_t s = 0; s < num_steps; ++s) {
            for (size_t n = 0; n < num_nodes; ++n) {
                size_t idx = (s * num_nodes + n) * 3;
                pos_flat[idx + 0] = step_history[s].node_coords[n].x();
                pos_flat[idx + 1] = step_history[s].node_coords[n].y();
                pos_flat[idx + 2] = step_history[s].node_coords[n].z();
            }
        }
        pos_dataset.write(pos_flat.data(), H5::PredType::NATIVE_DOUBLE);

        // Effective Tension Matrix (num_steps x num_elems)
        hsize_t tens_dims[2] = { num_steps, num_elems };
        H5::DataSpace tens_space(2, tens_dims);
        H5::DataSet tens_dataset = file.createDataSet("element_tensions_kN", H5::PredType::NATIVE_DOUBLE, tens_space);

        std::vector<double> tens_flat(num_steps * num_elems);
        for (size_t s = 0; s < num_steps; ++s) {
            for (size_t e = 0; e < num_elems; ++e) {
                tens_flat[s * num_elems + e] = step_history[s].element_tensions_kN[e];
            }
        }
        tens_dataset.write(tens_flat.data(), H5::PredType::NATIVE_DOUBLE);

        // Bending Moment Matrix (num_steps x num_elems)
        H5::DataSet moment_dataset = file.createDataSet("element_bending_moments_kNm", H5::PredType::NATIVE_DOUBLE, tens_space);
        std::vector<double> moment_flat(num_steps * num_elems);
        for (size_t s = 0; s < num_steps; ++s) {
            for (size_t e = 0; e < num_elems; ++e) {
                moment_flat[s * num_elems + e] = (e < step_history[s].element_bending_moments_kNm.size()) ? step_history[s].element_bending_moments_kNm[e] : 0.0;
            }
        }
        moment_dataset.write(moment_flat.data(), H5::PredType::NATIVE_DOUBLE);

        // von Mises Stress Matrix (num_steps x num_elems)
        H5::DataSet vm_dataset = file.createDataSet("element_von_mises_MPa", H5::PredType::NATIVE_DOUBLE, tens_space);
        std::vector<double> vm_flat(num_steps * num_elems);
        for (size_t s = 0; s < num_steps; ++s) {
            for (size_t e = 0; e < num_elems; ++e) {
                vm_flat[s * num_elems + e] = (e < step_history[s].element_von_mises_MPa.size()) ? step_history[s].element_von_mises_MPa[e] : 0.0;
            }
        }
        vm_dataset.write(vm_flat.data(), H5::PredType::NATIVE_DOUBLE);

        file.close();
        std::cout << "✅ Binary HDF5 simulation history successfully exported to: " << filename << std::endl;
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
