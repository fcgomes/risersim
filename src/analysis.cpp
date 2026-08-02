#include "risersim/analysis.hpp"

namespace risersim {

void Analysis::assemble_system(Eigen::SparseMatrix<double>& K_global, const Eigen::VectorXd& F_ext, Eigen::VectorXd& F_int) {
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

} // namespace risersim
