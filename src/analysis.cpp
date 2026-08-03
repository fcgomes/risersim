#include "risersim/analysis.hpp"

namespace risersim {

void Analysis::assemble_system(Eigen::SparseMatrix<double>& K_global, const Eigen::VectorXd& F_ext, Eigen::VectorXd& F_int) {
    K_global.resize(num_dofs, num_dofs);
    K_global.setZero();
    F_int = Eigen::VectorXd::Zero(num_dofs);

    std::vector<Eigen::Triplet<double>> triplets;

    if (!model) return;

    for (auto* elem : model->elements) {
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

        // Calcular F_int_elem = K_elem * u_elem (inclui axial + flexão + torção)
        Eigen::Matrix<double, 12, 1> u_elem;
        for (int i = 0; i < 3; ++i) {
            u_elem[i]     = elem->node1->disp[i];
            u_elem[i + 3] = elem->node1->rot[i];
            u_elem[i + 6] = elem->node2->disp[i];
            u_elem[i + 9] = elem->node2->rot[i];
        }

        Eigen::Matrix<double, 12, 1> F_int_elem = K_elem * u_elem;

        for (int i = 0; i < 12; ++i) {
            if (dofs[i] >= 0) {
                F_int[dofs[i]] += F_int_elem[i];
            }
        }
    }

    // Apply Seabed Interaction (Bilinear Soil Springs at TDZ)
    for (auto* node : model->nodes) {
        double f_seabed = 0.0, k_seabed = 0.0;
        seabed.calculate_seabed_reaction(node->current_coords().z(), f_seabed, k_seabed);

        int eq_z = node->eq_numbers[2];
        if (eq_z >= 0) {
            F_int[eq_z] -= f_seabed; // Upward normal reaction force from seabed
            triplets.push_back(Eigen::Triplet<double>(eq_z, eq_z, k_seabed));
        }
    }

    K_global.setFromTriplets(triplets.begin(), triplets.end());
}

} // namespace risersim
