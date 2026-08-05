/**
 * @file test_static_analysis.cpp
 * @brief Characterization (regression) test for StaticAnalysis::solve().
 *
 * Goal: pin down CURRENT behavior across the architecture refactor described in
 * `risersim/docs/mapa_classes_anflex_estatica.md`, so each roadmap step (Integrator, the
 * two-phase assembly/static solve, etc.) can be validated against "this didn't change by accident".
 *
 * Note: the synthetic geometry below locks the rotations of all intermediate nodes
 * (`eq_numbers = {0,1,2,-1,-1,-1}`, matching `main_test.cpp`), so this case doesn't exercise
 * bending/rotation of the corotational element -- it's a regression test of the pipeline
 * (assembly/load stepping/convergence), not a validation of the beam formulation itself.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

#include "risersim/model.hpp"
#include "risersim/node.hpp"
#include "risersim/element_beam.hpp"
#include "risersim/static_analysis.hpp"

namespace {

/**
 * @brief Reproduces exactly the fallback parabolic geometry from `risersim/src/main_test.cpp`
 * (used when no input JSON is provided), to keep the two in sync.
 */
risersim::RiserModel* build_synthetic_catenary_model() {
    constexpr int num_elements = 40;
    constexpr double total_length = 180.0;
    constexpr double total_depth_z = -100.0;

    auto* model = new risersim::RiserModel();

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
        model->add_node(i + 1, x, 0.0, z);
    }

    model->nodes.front()->eq_numbers = std::vector<int>(6, -1);
    model->nodes.back()->eq_numbers = std::vector<int>(6, -1);
    for (size_t i = 1; i < model->nodes.size() - 1; ++i) {
        model->nodes[i]->eq_numbers = {0, 1, 2, -1, -1, -1};
    }

    risersim::BeamMaterialProps props;
    const double L_unstretched = total_length / static_cast<double>(num_elements);
    for (int i = 0; i < num_elements; ++i) {
        model->add_element(i + 1, model->nodes[i].get(), model->nodes[i + 1].get(), props, L_unstretched);
    }

    return model;
}

} // namespace

TEST_CASE("StaticAnalysis converges on the synthetic fallback catenary", "[static_analysis][characterization]") {
    auto* model = build_synthetic_catenary_model();

    risersim::StaticAnalysis static_analysis;
    static_analysis.model = model;
    static_analysis.water_density = 1025.0;
    static_analysis.water_density_for_mass = 1025.0;
    static_analysis.seabed = risersim::SeabedInteraction(-100.0, 1.0e5, 0.5);
    static_analysis.load_steps = 20;
    static_analysis.max_iter_per_step = 300;
    static_analysis.tol = 0.01;

    const bool converged = static_analysis.solve();

    REQUIRE(converged);

    // Reference value, UPDATED after fixing the divergence root cause
    // (mapa_classes_anflex_estatica.md, "Root cause found and fixed" section): internal
    // force now uses each node's LOCAL/deformational rotation (relative to the element's
    // ghost frame) instead of the total accumulated rotation. In this synthetic case
    // (rotation DOFs locked at 0 on all nodes), this intentionally and correctly changes
    // behavior: before, rotation=0 always => bending stiffness never contributed
    // (equivalent to a cable with no bending); now, the "ghost frame" tracks the current
    // chord while the node stays frozen at its initial orientation, so real bending
    // starts resisting deformation -- hence the much lower equilibrium tension. Value
    // UPDATED AGAIN after swapping the seabed friction spring for an incremental
    // elastic-plastic version (with per-node persistent state), mirroring real ANFLEX
    // (soil_uncoupled.cpp) -- the previous version used the TOTAL accumulated
    // displacement instead of the per-iteration increment, which saturates the friction
    // force in a physically incorrect way. This synthetic case has the seabed enabled
    // (k=1e5, mu=0.5), so the friction model change affects the result. Value UPDATED
    // AGAIN after decomposing friction into the line's LOCAL axial/lateral directions
    // (instead of global X/Y), mirroring real ANFLEX (soil.cpp:calc_transf_matrix). Value
    // UPDATED AGAIN after adding a backtracking line search to Newton-Raphson
    // (apply_newton_step_with_line_search in static_analysis.cpp) -- this case has the
    // seabed enabled, so the iteration trajectory changes whenever the full step
    // eventually diverges too far from the previous residual.
    //
    // Value UPDATED AGAIN (this time correcting a bug, not a legitimate behavior change)
    // after fixing StaticAnalysis::tol's unit mismatch (mapa_classes_anflex_estatica.md,
    // "tol=100.0 usado como tolerância de força vs. razão adimensional"): `tol` was
    // documented as a force residual in Newtons but wired directly into
    // ConvergenceTest's dimensionless translation/rotation increment-ratio criterion,
    // where a value of 100.0 made the check a near no-op (any ratio in ~[0,1] trivially
    // satisfies <= 100.0). Every load step was "converging" after 1-2 Newton iterations
    // regardless of the actual force imbalance -- visibly wrong in the 3D viewer as
    // self-folding geometry at the touchdown zone, where the residual was largest. All
    // previous reference values above were captured under this bug, i.e. NOT at genuine
    // equilibrium. With tol=0.01 (a sane dimensionless ratio), the first load step now
    // takes 12 real iterations (residual drops from ~2.4e9 N to ~4.85e5 N) instead of 1,
    // and the final residual is ~85 N instead of hundreds of millions. T_eff captured via
    // Docker after the fix: 1101.98202 kN at the top element -- much lower than before
    // because the line now actually settles onto the seabed instead of being held above
    // it by residual force imbalance.
    const double expected_tension_effective_N = 1101.9820161411786 * 1000.0;
    REQUIRE(model->elements.front()->tension_effective == Catch::Approx(expected_tension_effective_N).epsilon(0.005));

    delete model;
}
