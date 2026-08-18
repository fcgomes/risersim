/**
 * @file test_prescribed_motion.cpp
 * @brief Unit tests for PrescribedMotion (the penalty-spring formula in isolation) and an
 * end-to-end test that StaticAnalysis::solve_vessel_offset() -- migrated to use it in place of
 * the old direct-Dirichlet mechanism (roadmap step 7) -- still converges and actually moves the
 * top node to the requested offset.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

#include "risersim/model.hpp"
#include "risersim/node.hpp"
#include "risersim/element_beam.hpp"
#include "risersim/prescribed_motion.hpp"
#include "risersim/static_analysis.hpp"

using namespace risersim;

namespace {

RiserModel build_small_synthetic_catenary_model() {
    constexpr int num_elements = 10;
    constexpr double total_length = 45.0;
    constexpr double total_depth_z = -25.0;

    RiserModel model;

    const int num_nodes = num_elements + 1;
    const double h_water = std::abs(total_depth_z);
    const double L_total = total_length;

    const double S_susp = std::min(L_total * 0.70, 310.0);
    const double X_tdp = std::sqrt(std::max(1.0, S_susp * S_susp - h_water * h_water));

    for (int i = 0; i < num_nodes; ++i) {
        const double s = (static_cast<double>(i) / static_cast<double>(num_elements)) * L_total;
        double x = 0.0;
        double z = 0.0;
        if (s <= S_susp) {
            const double ratio = s / S_susp;
            x = ratio * X_tdp;
            z = -h_water * (2.0 * ratio - ratio * ratio);
        } else {
            const double s_seabed = s - S_susp;
            x = X_tdp + s_seabed;
            z = -h_water;
        }
        model.add_node(i + 1, x, 0.0, z);
    }

    model.nodes().front()->eq_numbers = std::vector<int>(6, -1);
    model.nodes().back()->eq_numbers = std::vector<int>(6, -1);
    for (size_t i = 1; i < model.nodes().size() - 1; ++i) {
        model.nodes()[i]->eq_numbers = {0, 1, 2, -1, -1, -1};
    }

    BeamMaterialProps props;
    const double L_unstretched = total_length / static_cast<double>(num_elements);
    for (int i = 0; i < num_elements; ++i) {
        model.add_beam_element(i + 1, model.nodes()[i].get(), model.nodes()[i + 1].get(), props, L_unstretched);
    }

    return model;
}

/// A single free node (all 6 DOFs numbered 0..5), enough to test apply()'s formula in isolation.
RiserModel build_single_free_node_model() {
    RiserModel model;
    Node3D* n = model.add_node(1, 0.0, 0.0, 0.0);
    n->eq_numbers = {0, 1, 2, 3, 4, 5};
    return model;
}

} // namespace

TEST_CASE("PrescribedMotion::apply adds big_number to K and overwrites F_int with the right sign", "[prescribed_motion]") {
    RiserModel model = build_single_free_node_model();
    Node3D* n = model.nodes().front().get();
    n->disp = Eigen::Vector3d(1.0, 0.0, 0.0); // currently at x=1.0

    PrescribedMotion pm(n);
    pm.dof_active = {true, false, false, false, false, false}; // only x
    pm.target_disp = Eigen::Vector3d(3.0, 0.0, 0.0); // target x=3.0 (2.0m away)

    const double big_number = 2.1e11; // matches ANFLEX's cBeam::get_big_number() choice (E)
    std::vector<Eigen::Triplet<double>> triplets;
    Eigen::VectorXd F_int = Eigen::VectorXd::Zero(6);
    // Pre-fill F_int at eq 0 as if an element had already contributed something there --
    // apply() must overwrite it, not add to it (mirrors ANFLEX's own direct assignment).
    F_int[0] = 12345.0;

    pm.apply(big_number, triplets, F_int);

    REQUIRE(triplets.size() == 1);
    REQUIRE(triplets[0].row() == 0);
    REQUIRE(triplets[0].col() == 0);
    REQUIRE(triplets[0].value() == big_number);

    // current(1.0) < target(3.0) -> F_int must be NEGATIVE (current - target) * big_number, so
    // that Residual = F_ext - F_int is POSITIVE at this DOF and Newton pushes disp UP toward the
    // target, not away from it.
    double expected_F_int = (1.0 - 3.0) * big_number;
    REQUIRE(F_int[0] == expected_F_int);
    REQUIRE(F_int[0] < 0.0);

    // Other DOFs (inactive, or active-but-not-set here) must be untouched.
    for (int i = 1; i < 6; ++i) REQUIRE(F_int[i] == 0.0);
}

TEST_CASE("PrescribedMotion::apply skips DOFs that are inactive or fixed on the node", "[prescribed_motion]") {
    RiserModel model = build_single_free_node_model();
    Node3D* n = model.nodes().front().get();
    n->eq_numbers[1] = -1; // fix the Y DOF, even though it has a valid slot conceptually

    PrescribedMotion pm(n);
    pm.dof_active = {true, true, false, false, false, false}; // x and y requested...
    pm.target_disp = Eigen::Vector3d(5.0, 5.0, 0.0);

    std::vector<Eigen::Triplet<double>> triplets;
    Eigen::VectorXd F_int = Eigen::VectorXd::Zero(6);
    pm.apply(1.0e11, triplets, F_int);

    // ...but only x actually has a free equation number, so only x gets a contribution.
    REQUIRE(triplets.size() == 1);
    REQUIRE(triplets[0].row() == 0);
    REQUIRE(F_int[1] == 0.0);
}

TEST_CASE("StaticAnalysis::solve_vessel_offset converges and moves the top node to the requested offset", "[prescribed_motion][static_analysis]") {
    RiserModel model = build_small_synthetic_catenary_model();
    Node3D* top_node = model.nodes().front().get();
    std::vector<int> original_top_eq_numbers = top_node->eq_numbers;

    StaticAnalysis sa;
    sa.model = &model;
    sa.water_density = 1025.0;
    sa.water_density_for_mass = 1025.0;
    sa.seabed = SeabedInteraction(-1.0e6, 0.0, 0.0); // pushed far away: no contact, isolates this test from soil behavior
    sa.load_steps = 10;
    sa.max_iter_per_step = 200;
    sa.tol = 0.01;

    REQUIRE(sa.solve_catenary_static(sa.load_steps, sa.max_iter_per_step, sa.tol));

    const double offset_magnitude = 2.0; // m, modest relative to this ~45m line
    VesselOffset vessel_offset(OffsetMode::Far, offset_magnitude);
    Eigen::Vector3d disp_before_offset = top_node->disp;

    REQUIRE(sa.solve_vessel_offset(vessel_offset, /*steps=*/5, /*max_iter=*/200, /*tolerance=*/1.0e-4));

    // The top node ends up close to its pre-offset position plus the requested +X offset --
    // "close", not exact, because the penalty spring converges to within residual/big_number of
    // the target (big_number ~ E ~ 1e11, so the gap is many orders of magnitude below any
    // physically meaningful tolerance here).
    double expected_x = disp_before_offset.x() + offset_magnitude;
    REQUIRE(top_node->disp.x() == Catch::Approx(expected_x).margin(1.0e-3));

    // The top node's DOF structure must be restored to fully fixed afterward -- solve_vessel_offset
    // uses a temporarily-free penalty-driven DOF internally, but must leave the model's contract
    // unchanged for any caller downstream (e.g. DynamicAnalysis reusing the same model).
    REQUIRE(top_node->eq_numbers == original_top_eq_numbers);
    REQUIRE(sa.prescribed_motions.empty());
}
