/**
 * @file analysis.cpp
 * @brief Analysis::assemble_system(): assembles the global tangent stiffness, internal force, and seabed contact/friction contributions.
 */
#include "risersim/analysis.hpp"
#include "risersim/hydrostatics.hpp"
#include <algorithm>
#include <unordered_map>

namespace risersim {

void Analysis::assemble_system(Eigen::SparseMatrix<double>& K_global, const Eigen::VectorXd& F_ext, Eigen::VectorXd& F_int) {
    K_global.resize(num_dofs, num_dofs);
    K_global.setZero();
    F_int = Eigen::VectorXd::Zero(num_dofs);

    std::vector<Eigen::Triplet<double>> triplets;

    if (!model) return;

    // Average tangent per node (sum of adjacent elements' current chord directions),
    // used below to decompose seabed friction into the line's LOCAL axial/lateral
    // directions -- mirroring real ANFLEX (soil.cpp:calc_transf_matrix) -- instead
    // of global X/Y.
    // BUG REAL ACHADO VIA VALGRIND (memcheck, --track-origins=yes): `unordered_map::operator[]`
    // default-constrói o valor na primeira inserção, e o construtor padrão de `Eigen::Vector3d`
    // (tipo de tamanho fixo) NÃO zera os coeficientes -- ao contrário de `std::array`/`double`, é
    // lixo de memória mesmo (comportamento documentado do próprio Eigen, por performance). O
    // `+= ex` abaixo, na primeira vez que cada nó aparece, então soma `ex` a um vetor de lixo em
    // vez de partir de zero. `try_emplace` garante Zero() antes de qualquer `+=`. Esse lixo se
    // propagava pra `axial_dir`/`lateral_dir` (decomposição de atrito do solo, abaixo) sempre que
    // um nó tem `k_seabed>0`, dali pra dentro da matriz de rigidez e da fatoração de Cholesky --
    // explica boa parte da sensibilidade "beira de bifurcação" documentada nesta sessão (mudanças
    // sem relação nenhuma com a física, tipo o tamanho de alocações de heap anteriores, mudavam o
    // lixo lido aqui e por consequência a convergência).
    std::unordered_map<Node3D*, Eigen::Vector3d> node_tangent_sum;
    for (const auto& elem : model->elements()) {
        if (elem->num_nodes() < 2) continue; // e.g. a future single-node element (buoy) has no "along the line" direction
        Node3D* n1 = elem->node(0);
        Node3D* n2 = elem->node(1);
        Eigen::Vector3d ex = (n2->current_coords() - n1->current_coords()).normalized();
        node_tangent_sum.try_emplace(n1, Eigen::Vector3d::Zero());
        node_tangent_sum.try_emplace(n2, Eigen::Vector3d::Zero());
        node_tangent_sum[n1] += ex;
        node_tangent_sum[n2] += ex;
    }

    for (const auto& elem : model->elements()) {
        elem->set_time(current_time);
        elem->update_effective_tension();

        // Stiffness and internal force from the consistent corotational state (each
        // node's LOCAL/deformational rotation relative to the element's ghost frame --
        // not the total rotation accumulated since t=0). See
        // compute_corotational_forces() / mapa_classes_anflex_estatica.md.
        //
        // Goes through the Element interface (num_nodes()/node()/assemble()) rather than
        // hardcoding node1/node2 and a 12x12 size, so this loop works unchanged for any
        // future element type with a different node/DOF count -- see element.hpp.
        Eigen::MatrixXd K_elem;
        Eigen::VectorXd F_int_elem;
        elem->assemble(K_elem, F_int_elem);

        int n_dof = elem->num_nodes() * 6;
        std::vector<int> dofs(n_dof);
        for (int n = 0; n < elem->num_nodes(); ++n) {
            Node3D* nd = elem->node(n);
            for (int i = 0; i < 6; ++i) dofs[n * 6 + i] = nd->eq_numbers[i];
        }

        for (int i = 0; i < n_dof; ++i) {
            if (dofs[i] < 0) continue;
            for (int j = 0; j < n_dof; ++j) {
                if (dofs[j] < 0) continue;
                triplets.push_back(Eigen::Triplet<double>(dofs[i], dofs[j], K_elem(i, j)));
            }
        }

        for (int i = 0; i < n_dof; ++i) {
            if (dofs[i] >= 0) {
                F_int[dofs[i]] += F_int_elem[i];
            }
        }
    }

    // Apply Seabed Interaction (Bilinear Soil Springs at TDZ)
    for (const auto& node_ptr : model->nodes()) {
        Node3D* node = node_ptr.get();
        double f_seabed = 0.0, k_seabed = 0.0;
        seabed.calculate_seabed_reaction(node->current_coords().z(), f_seabed, k_seabed);

        int eq_z = node->eq_numbers[2];
        if (eq_z >= 0) {
            F_int[eq_z] -= f_seabed; // Upward normal reaction force from seabed
            triplets.push_back(Eigen::Triplet<double>(eq_z, eq_z, k_seabed));
        }

        // Friction springs along the LOCAL axial (along the line) and lateral
        // (perpendicular, horizontal plane) directions -- mirroring real ANFLEX
        // (soil.cpp:calc_transf_matrix: Y_lateral = normal x tangent,
        // X_axial = Y_lateral x normal). Without this (using global X/Y directly),
        // the real "lateral" direction -- the one that resists load perpendicular to
        // the line, e.g. from ocean current -- gets arbitrarily mixed with the axial
        // one whenever the line isn't aligned with the global axes, which is almost
        // always the case (see mapa_classes_anflex_estatica.md).
        // node->friction_force[0]/[1] hold the persistent state of the axial/lateral
        // components (incremental elastic-plastic, see seabed.hpp:calculate_friction_1d).
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
            if (seabed.soil_model() == SoilModel::Coupled) {
                // Combined axial+lateral yield surface (real ANFLEX's cCoupledSoil, e.g.
                // Exemplo_02a's %OPTION.SOIL.COUPLED) -- see seabed.hpp:calculate_friction_coupled.
                seabed.calculate_friction_coupled(f_seabed, du_axial, du_lateral,
                                                  node->friction_force[0], node->friction_force[1],
                                                  k_ax, k_lat);
            } else {
                // Independent per-axis caps (real ANFLEX's cUncoupledSoil, e.g. Exemplo_01a's
                // %SOIL.UNCOUPLED) -- see seabed.hpp:calculate_friction_1d.
                seabed.calculate_friction_1d(seabed.axial_friction(), seabed.axial_elastic_deflection_limit(),
                                             f_seabed, du_axial, node->friction_force[0], k_ax);
                seabed.calculate_friction_1d(seabed.lateral_friction(), seabed.lateral_elastic_deflection_limit(),
                                             f_seabed, du_lateral, node->friction_force[1], k_lat);
            }

            double f_global_x = node->friction_force[0] * axial_dir.x() + node->friction_force[1] * lateral_dir.x();
            double f_global_y = node->friction_force[0] * axial_dir.y() + node->friction_force[1] * lateral_dir.y();

            // Full tangent stiffness (2x2), transformed back to global X/Y --
            // includes the cross XY term, needed because the axial/lateral
            // direction rarely coincides with X/Y.
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
            // Contact lost: reset friction state, mirroring real ANFLEX
            // (soil_uncoupled.cpp:update, "else" branch when pen<=0).
            node->friction_force.setZero();
        }
    }

    // Prescribed motions (see prescribed_motion.hpp): a stiff penalty spring pulling specific
    // node DOFs toward a target value, mirroring ANFLEX's big-number technique. Empty by
    // default, so this is a no-op unless a caller (e.g. solve_vessel_offset()) populates it.
    if (!prescribed_motions.empty()) {
        double big_number = 0.0;
        for (const auto& elem : model->elements()) big_number = std::max(big_number, elem->characteristic_stiffness());
        for (const auto& pm : prescribed_motions) pm.apply(big_number, triplets, F_int);
    }

    K_global.setFromTriplets(triplets.begin(), triplets.end());
}

Eigen::SparseMatrix<double> Analysis::assemble_buoyancy_stiffness() const {
    Eigen::SparseMatrix<double> K_buoyancy(num_dofs, num_dofs);
    if (!model) return K_buoyancy;

    double water_surface_z = model->environmental().water_surface_z;
    double g = 9.81;
    std::vector<Eigen::Triplet<double>> triplets;

    for (const auto& elem_base : model->elements()) {
        // Hydrostatic buoyancy is beam-specific (outer diameter, submerged length) -- an element
        // type with no such physical envelope (e.g. a connector/spring) simply contributes none.
        auto* elem = dynamic_cast<CorotationalBeam3D*>(elem_base.get());
        if (!elem) continue;

        double zc[2] = {elem->node1()->current_coords().z(), elem->node2()->current_coords().z()};
        Hydrostatics hydro(elem->props().D_outer, elem->initial_length(), water_density);
        hydro.compute(zc, water_surface_z);

        int eq1_z = elem->node1()->eq_numbers[2];
        int eq2_z = elem->node2()->eq_numbers[2];
        // d(F_int)/dz == -d(F_ext_buoyancy)/dz -- F_ext's buoyancy term REDUCES the downward
        // weight as an end rises toward the surface, so its own Z-derivative is already the
        // right-signed restoring stiffness for the residual (F_ext - F_int); since the caller
        // adds this directly to K_global (which multiplies displacement corrections against the
        // residual the same way assemble_system()'s other stiffness terms do), the sign here
        // must match those -- verified against real ANFLEX's own (always non-negative) stiffness
        // convention in nl_hidrostatic.cpp, ported as-is (end_stiffness() >= 0 always).
        if (eq1_z >= 0) triplets.push_back(Eigen::Triplet<double>(eq1_z, eq1_z, hydro.end_stiffness(0) * g));
        if (eq2_z >= 0) triplets.push_back(Eigen::Triplet<double>(eq2_z, eq2_z, hydro.end_stiffness(1) * g));
    }

    // Same treatment for TrussElement (and WinchElement, which IS a TrussElement) -- see
    // TrussElement's own doc comment for why this was filled in (was a documented gap).
    for (const auto& elem_base : model->elements()) {
        auto* truss = dynamic_cast<TrussElement*>(elem_base.get());
        if (!truss) continue;

        double zc[2] = {truss->node1()->current_coords().z(), truss->node2()->current_coords().z()};
        Hydrostatics hydro(truss->props().D_outer, truss->initial_length(), water_density);
        hydro.compute(zc, water_surface_z);

        int eq1_z = truss->node1()->eq_numbers[2];
        int eq2_z = truss->node2()->eq_numbers[2];
        if (eq1_z >= 0) triplets.push_back(Eigen::Triplet<double>(eq1_z, eq1_z, hydro.end_stiffness(0) * g));
        if (eq2_z >= 0) triplets.push_back(Eigen::Triplet<double>(eq2_z, eq2_z, hydro.end_stiffness(1) * g));
    }

    K_buoyancy.setFromTriplets(triplets.begin(), triplets.end());
    return K_buoyancy;
}

Eigen::SparseMatrix<double> Analysis::assemble_damping(double fallback_alpha, double fallback_beta) const {
    Eigen::SparseMatrix<double> C_global(num_dofs, num_dofs);
    if (!model) return C_global;

    std::vector<Eigen::Triplet<double>> triplets;

    for (const auto& elem : model->elements()) {
        double a = elem->rayleigh_configured() ? elem->rayleigh_alpha() : fallback_alpha;
        double b = elem->rayleigh_configured() ? elem->rayleigh_beta() : fallback_beta;
        if (a == 0.0 && b == 0.0) continue; // also skips ScalarElement/BuoyElement for free

        Eigen::MatrixXd K_local;
        Eigen::VectorXd F_int_unused;
        elem->assemble(K_local, F_int_unused);
        Eigen::MatrixXd M_local = elem->mass_matrix(water_density_for_mass);
        Eigen::MatrixXd C_local = a * M_local + b * K_local;

        int n_dof = elem->num_nodes() * 6;
        std::vector<int> dofs(n_dof);
        for (int n = 0; n < elem->num_nodes(); ++n) {
            Node3D* nd = elem->node(n);
            for (int i = 0; i < 6; ++i) dofs[n * 6 + i] = nd->eq_numbers[i];
        }

        for (int i = 0; i < n_dof; ++i) {
            if (dofs[i] < 0) continue;
            for (int j = 0; j < n_dof; ++j) {
                if (dofs[j] < 0) continue;
                triplets.push_back(Eigen::Triplet<double>(dofs[i], dofs[j], C_local(i, j)));
            }
        }
    }

    C_global.setFromTriplets(triplets.begin(), triplets.end());
    return C_global;
}

} // namespace risersim
