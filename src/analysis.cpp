#include "risersim/analysis.hpp"
#include <unordered_map>

namespace risersim {

void Analysis::assemble_system(Eigen::SparseMatrix<double>& K_global, const Eigen::VectorXd& F_ext, Eigen::VectorXd& F_int) {
    K_global.resize(num_dofs, num_dofs);
    K_global.setZero();
    F_int = Eigen::VectorXd::Zero(num_dofs);

    std::vector<Eigen::Triplet<double>> triplets;

    if (!model) return;

    // Tangente media por no (soma das direcoes de corda atual dos elementos
    // adjacentes), usada abaixo para decompor o atrito do solo nas direcoes
    // axial/lateral LOCAIS da linha -- fiel ao ANFLEX real
    // (soil.cpp:calc_transf_matrix) -- em vez de X/Y globais.
    std::unordered_map<Node3D*, Eigen::Vector3d> node_tangent_sum;
    for (auto* elem : model->elements) {
        Eigen::Vector3d ex = (elem->node2->current_coords() - elem->node1->current_coords()).normalized();
        node_tangent_sum[elem->node1] += ex;
        node_tangent_sum[elem->node2] += ex;
    }

    for (auto* elem : model->elements) {
        elem->update_effective_tension();

        // Rigidez e forca interna a partir do estado corrotacional consistente
        // (rotacao LOCAL/deformacional de cada no relativa ao ghost frame do
        // elemento -- nao a rotacao total acumulada desde t=0). Ver
        // compute_corotational_forces() / mapa_classes_anflex_estatica.md.
        Eigen::Matrix<double, 12, 12> K_elem;
        Eigen::Matrix<double, 12, 1> F_int_elem;
        elem->compute_corotational_forces(K_elem, F_int_elem);

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

        // Molas de atrito nas direções axial (ao longo da linha) e lateral
        // (perpendicular, no plano horizontal) LOCAIS -- fiel ao ANFLEX real
        // (soil.cpp:calc_transf_matrix: Y_lateral = normal×tangente,
        // X_axial = Y_lateral×normal). Sem isso (usar X/Y globais direto),
        // a direção "lateral" real -- a que resiste à carga perpendicular à
        // linha, ex. de corrente marítima -- fica arbitrariamente misturada
        // com a axial sempre que a linha não está alinhada aos eixos globais,
        // quase sempre o caso (ver mapa_classes_anflex_estatica.md).
        // node->friction_force[0]/[1] guardam o estado persistente das
        // componentes axial/lateral (elástico-plástico incremental, ver
        // seabed.hpp:calculate_friction_1d).
        if (k_seabed > 0.0) {
            Eigen::Vector3d axial_dir(1.0, 0.0, 0.0), lateral_dir(0.0, 1.0, 0.0);
            auto it = node_tangent_sum.find(node);
            if (it != node_tangent_sum.end()) {
                Eigen::Vector2d tangent_h(it->second.x(), it->second.y());
                if (tangent_h.norm() > 1.0e-9) {
                    tangent_h.normalize();
                    axial_dir = Eigen::Vector3d(tangent_h.x(), tangent_h.y(), 0.0);
                    lateral_dir = Eigen::Vector3d(-tangent_h.y(), tangent_h.x(), 0.0);
                }
            }

            double du_axial = node->delta_disp_xy.x() * axial_dir.x() + node->delta_disp_xy.y() * axial_dir.y();
            double du_lateral = node->delta_disp_xy.x() * lateral_dir.x() + node->delta_disp_xy.y() * lateral_dir.y();

            double k_ax = 0.0, k_lat = 0.0;
            seabed.calculate_friction_1d(seabed.axial_friction, seabed.axial_elastic_deflection_limit,
                                         f_seabed, du_axial, node->friction_force[0], k_ax);
            seabed.calculate_friction_1d(seabed.lateral_friction, seabed.lateral_elastic_deflection_limit,
                                         f_seabed, du_lateral, node->friction_force[1], k_lat);

            double f_global_x = node->friction_force[0] * axial_dir.x() + node->friction_force[1] * lateral_dir.x();
            double f_global_y = node->friction_force[0] * axial_dir.y() + node->friction_force[1] * lateral_dir.y();

            // Rigidez tangente completa (2x2), transformada de volta para X/Y
            // globais -- inclui o termo cruzado XY, necessario porque a
            // direção axial/lateral raramente coincide com X/Y.
            double k_xx = k_ax * axial_dir.x() * axial_dir.x() + k_lat * lateral_dir.x() * lateral_dir.x();
            double k_yy = k_ax * axial_dir.y() * axial_dir.y() + k_lat * lateral_dir.y() * lateral_dir.y();
            double k_xy = k_ax * axial_dir.x() * axial_dir.y() + k_lat * lateral_dir.x() * lateral_dir.y();

            int eq_x = node->eq_numbers[0];
            int eq_y = node->eq_numbers[1];
            if (eq_x >= 0) {
                F_int[eq_x] += f_global_x;
                triplets.push_back(Eigen::Triplet<double>(eq_x, eq_x, k_xx));
            }
            if (eq_y >= 0) {
                F_int[eq_y] += f_global_y;
                triplets.push_back(Eigen::Triplet<double>(eq_y, eq_y, k_yy));
            }
            if (eq_x >= 0 && eq_y >= 0) {
                triplets.push_back(Eigen::Triplet<double>(eq_x, eq_y, k_xy));
                triplets.push_back(Eigen::Triplet<double>(eq_y, eq_x, k_xy));
            }
        } else {
            // Contato perdido: zera o estado de atrito, igual ao ANFLEX real
            // (soil_uncoupled.cpp:update, ramo "else" quando pen<=0).
            node->friction_force.setZero();
        }
    }

    K_global.setFromTriplets(triplets.begin(), triplets.end());
}

} // namespace risersim
