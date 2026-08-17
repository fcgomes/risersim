/**
 * @file static_integrator.cpp
 * @brief StaticIntegrator: static load vector assembly and artificial-stiffness (Tikhonov) regularization.
 */
#include "risersim/static_integrator.hpp"
#include "risersim/hydrostatics.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace risersim {

Eigen::VectorXd StaticIntegrator::assemble_load_vector(double current_factor) const {
    Eigen::VectorXd F_ext = Eigen::VectorXd::Zero(analysis->num_dofs);
    auto* model = analysis->model;
    if (!model) return F_ext;

    double water_surface_z = model->environmental.water_surface_z;

    for (const auto& elem : model->elements) {
        double L = elem->initial_length;
        double g = 9.81;

        double z1 = elem->node1->current_coords().z();
        double z2 = elem->node2->current_coords().z();

        // Dry weight (net of internal-fluid buoyancy, which is a fixed structural property, not
        // Z-dependent) stays exactly as before. External buoyancy now scales with how much of
        // the element is actually below the water surface, instead of always applying the
        // fully-submerged value regardless of Z -- see hydrostatics.hpp for why (docs/roadmap.md
        // item 1b).
        double w_dry = (elem->props.rho * elem->props.A + elem->props.rho_fluid * elem->inner_area()) * g;
        double zc[2] = {z1, z2};
        Hydrostatics hydro(elem->props.D_outer, L, analysis->water_density);
        hydro.compute(zc, water_surface_z);

        int eq1_z = elem->node1->eq_numbers[2];
        int eq2_z = elem->node2->eq_numbers[2];

        // Dry weight still splits 50/50 (doesn't depend on submersion). Buoyancy uses
        // Hydrostatics' own per-end apportionment (generally asymmetric while straddling --
        // e.g. one end fully wet, the other still dry -- NOT a blanket 50/50 split of the
        // element's total buoyant force).
        if (eq1_z >= 0) F_ext[eq1_z] += hydro.end_force(0) * g - 0.5 * w_dry * L;
        if (eq2_z >= 0) F_ext[eq2_z] += hydro.end_force(1) * g - 0.5 * w_dry * L;

        if (analysis->enable_current) {

            // Fraction of this element's length not buried more than 1m below the seabed --
            // mirrors real ANFLEX's element-state classification (node.cpp:set_surrounding_state,
            // "m_surround=3" once a node is more than 1m below the seabed line) and
            // cMorison::calc_external_flow (morison.cpp:141-145), which returns zero
            // current/wave force for a BURIED_ELEMENT and prorates it by the exposed length for
            // a WET_BURIED_ELEMENT, instead of always applying full-length drag regardless of how
            // deep a node has been pushed into the ground. risersim had no equivalent before this
            // fix -- during a bad Newton trial step that shoves a touchdown-zone node well
            // underground, full lateral current force kept fighting the seabed's contact
            // reaction instead of backing off like real ANFLEX, a plausible amplifier of the
            // seabed+current divergence (see mapa_classes_anflex_estatica.md).
            double buried_threshold_z = analysis->seabed.seabed_depth - 1.0;
            bool node1_buried = z1 < buried_threshold_z;
            bool node2_buried = z2 < buried_threshold_z;
            double exposed_fraction = 1.0;
            if (node1_buried && node2_buried) {
                exposed_fraction = 0.0;
            } else if (node1_buried != node2_buried) {
                double dz = z2 - z1;
                double t = (std::fabs(dz) > 1.0e-9) ? (buried_threshold_z - z1) / dz : 0.5;
                t = std::clamp(t, 0.0, 1.0);
                exposed_fraction = node1_buried ? (1.0 - t) : t;
            }

            if (exposed_fraction > 0.0) {
                double avg_z = 0.5 * (z1 + z2);
                Eigen::Vector3d elem_axis = (elem->node2->current_coords() - elem->node1->current_coords()).normalized();
                Eigen::Vector3d f_drag = analysis->current.get_drag_force_per_length(
                    avg_z, elem->props.D_outer, analysis->water_density_for_mass, elem_axis);

                int eq1_x = elem->node1->eq_numbers[0]; int eq2_x = elem->node2->eq_numbers[0];
                int eq1_y = elem->node1->eq_numbers[1]; int eq2_y = elem->node2->eq_numbers[1];

                double L_exposed = L * exposed_fraction;
                if (eq1_x >= 0) F_ext[eq1_x] += f_drag.x() * L_exposed * 0.5 * current_factor;
                if (eq2_x >= 0) F_ext[eq2_x] += f_drag.x() * L_exposed * 0.5 * current_factor;
                if (eq1_y >= 0) F_ext[eq1_y] += f_drag.y() * L_exposed * 0.5 * current_factor;
                if (eq2_y >= 0) F_ext[eq2_y] += f_drag.y() * L_exposed * 0.5 * current_factor;
                if (eq1_z >= 0) F_ext[eq1_z] += f_drag.z() * L_exposed * 0.5 * current_factor;
                if (eq2_z >= 0) F_ext[eq2_z] += f_drag.z() * L_exposed * 0.5 * current_factor;
            }
        }
    }
    return F_ext;
}

void StaticIntegrator::assemble_stiffness_and_internal_forces(int iter, Eigen::SparseMatrix<double>& K_global, Eigen::VectorXd& F_int) {
    auto* model = analysis->model;

    // assemble_system ignores the F_ext parameter (weight/friction already enter
    // directly through the element/soil internal-force formulation) -- kept here as-is.
    Eigen::VectorXd F_ext_unused;
    analysis->assemble_system(K_global, F_ext_unused, F_int);

    // Submersion-dependent buoyancy stiffness (see hydrostatics.hpp, docs/roadmap.md item 1b) --
    // the matching force already lives in assemble_load_vector() above, this is its tangent
    // contribution, same "K_global += ..." pattern as K_artificial below.
    K_global += analysis->assemble_buoyancy_stiffness();

    if (!artificial_stiffness_enabled || !model || model->elements.empty()) return;

    add_artificial_stiffness(model, analysis->num_dofs, iter, K_global);
}

void add_artificial_stiffness(const RiserModel* model, int num_dofs, int iter, Eigen::SparseMatrix<double>& K_global) {
    if (!model || model->elements.empty()) return;

    // Artificial stiffness (Tikhonov regularization), the same technique used by
    // real ANFLEX (beam.cpp:calc_artificial_stiffness / static_integrator.cpp).
    double avg_EA_L = 0.0;
    for (const auto& elem : model->elements) {
        double L = elem->current_length();
        if (L > 1.0e-9) avg_EA_L += (elem->props.E * elem->props.A) / L;
    }
    avg_EA_L /= static_cast<double>(model->elements.size());

    // NOTE: tested with constant 5.0 (instead of 1.25) as a way to keep artificial
    // stiffness relevant for more iterations -- this does prevent residual blow-up
    // in very long chains (~300+ elements), and was used to isolate the real cause
    // of the full model's divergence (see the "Isolated real cause" section in
    // mapa_classes_anflex_estatica.md: the seabed+current combination, not chain
    // length). It has a real trade-off, though: short chains converge more slowly
    // within the same iteration budget -- a real regression. Reverted to 1.25
    // (original behavior) until an adaptive scheme (e.g. scaling with the chain's
    // element/DOF count) is designed. See mapa_classes_anflex_estatica.md, section
    // on the ~300-element threshold.
    double decay = std::exp(-static_cast<double>(iter) / 1.25);
    double k_transversal = avg_EA_L * decay;
    double k_rotational = k_transversal * 0.05;

    std::vector<Eigen::Triplet<double>> artif_triplets;
    for (const auto& node : model->nodes) {
        for (int i = 0; i < 3; ++i) {
            int eq = node->eq_numbers[i];
            if (eq >= 0) artif_triplets.push_back(Eigen::Triplet<double>(eq, eq, k_transversal));

            int eq_rot = node->eq_numbers[i + 3];
            if (eq_rot >= 0) artif_triplets.push_back(Eigen::Triplet<double>(eq_rot, eq_rot, k_rotational));
        }
    }
    Eigen::SparseMatrix<double> K_artificial(num_dofs, num_dofs);
    K_artificial.setFromTriplets(artif_triplets.begin(), artif_triplets.end());
    K_global += K_artificial;
}

} // namespace risersim
