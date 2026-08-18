/**
 * @file test_static_analysis.cpp
 * @brief Characterization (regression) test for StaticAnalysis::solve().
 *
 * Goal: pin down CURRENT behavior across the architecture refactor described in
 * `risersim/docs/mapa_classes_anflex_estatica.md`, so each roadmap step (Integrator, the
 * two-phase assembly/static solve, etc.) can be validated against "this didn't change by accident".
 *
 * Note: the synthetic geometry below locks the rotations of all intermediate nodes
 * (`eq_numbers = {0,1,2,-1,-1,-1}`), so this case doesn't exercise bending/rotation of the
 * corotational element -- it's a regression test of the pipeline (assembly/load stepping/
 * convergence), not a validation of the beam formulation itself.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

#include "risersim/model.hpp"
#include "risersim/node.hpp"
#include "risersim/element_beam.hpp"
#include "risersim/element_truss.hpp"
#include "risersim/static_analysis.hpp"
#include "risersim/static_integrator.hpp"
#include "risersim/hydrostatics.hpp"

namespace {

/**
 * @brief risersim's original synthetic parabolic catenary test geometry (no longer reachable
 * from the production CLI, which now requires a real input JSON -- see Simulation::load()), kept
 * here as a self-contained known-geometry fixture for this regression test.
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

    model->nodes().front()->eq_numbers = std::vector<int>(6, -1);
    model->nodes().back()->eq_numbers = std::vector<int>(6, -1);
    for (size_t i = 1; i < model->nodes().size() - 1; ++i) {
        model->nodes()[i]->eq_numbers = {0, 1, 2, -1, -1, -1};
    }

    risersim::BeamMaterialProps props;
    const double L_unstretched = total_length / static_cast<double>(num_elements);
    for (int i = 0; i < num_elements; ++i) {
        model->add_beam_element(i + 1, model->nodes()[i].get(), model->nodes()[i + 1].get(), props, L_unstretched);
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
    REQUIRE(dynamic_cast<risersim::CorotationalBeam3D*>(model->elements().front().get())->tension_effective()
            == Catch::Approx(expected_tension_effective_N).epsilon(0.005));

    delete model;
}

// Verifies the weight/buoyancy loop added for TrussElement/WinchElement (docs/roadmap.md,
// element-types entry) actually wires TrussProps::rho/A/D_outer into StaticIntegrator::
// assemble_load_vector() the same way CorotationalBeam3D's own weight/buoyancy already does --
// this was a documented gap (no weight/buoyancy at all) through the initial Truss/Winch round.
TEST_CASE("StaticIntegrator::assemble_load_vector applies weight/buoyancy to TrussElement", "[static_integrator][truss]") {
    using namespace risersim;

    RiserModel model;
    // Horizontal, both ends at the same depth -- both ends land in Hydrostatics' simple
    // "fully submerged" regime (5m >> D_outer), so the expected force is easy to recompute
    // independently below without re-deriving the partial-submersion formula.
    Node3D* n1 = model.add_node(1, 0.0, 0.0, -5.0);
    Node3D* n2 = model.add_node(2, 2.0, 0.0, -5.0);
    n1->eq_numbers = {0, 0, 0, 0, 0, 0}; // placeholder "free" sentinel -- assign_equation_numbers() below assigns the real indices
    n2->eq_numbers = {0, 0, 0, 0, 0, 0};

    TrussProps props;
    props.A = 0.001;     // small structural area -- buoyancy (from D_outer) should dominate dry weight
    props.rho = 7850.0;
    props.D_outer = 0.2;
    model.add_truss_element(1, n1, n2, props);

    model.environmental().water_surface_z = 0.0;
    model.environmental().water_density = 1000.0;

    StaticAnalysis analysis;
    analysis.model = &model;
    analysis.water_density = 1000.0;
    analysis.assign_equation_numbers();

    StaticIntegrator integrator(&analysis);
    Eigen::VectorXd F_ext = integrator.assemble_load_vector(1.0);

    // Recompute the expected force with the same Hydrostatics helper the implementation itself
    // uses -- this test checks the WIRING (TrussProps -> assemble_load_vector), not the
    // Hydrostatics formula itself (already exercised by the beam's own path).
    const double L = 2.0, g = 9.81;
    const double w_dry = props.rho * props.A * g;
    Hydrostatics hydro(props.D_outer, L, 1000.0);
    double zc[2] = {-5.0, -5.0};
    hydro.compute(zc, 0.0);
    const double expected_total_z = hydro.end_force(0) * g + hydro.end_force(1) * g - w_dry * L;

    double actual_total_z = F_ext[n1->eq_numbers[2]] + F_ext[n2->eq_numbers[2]];

    REQUIRE(actual_total_z == Catch::Approx(expected_total_z).margin(1.0e-6));
    REQUIRE(actual_total_z > 0.0); // net buoyant here: tiny structural area, sizeable D_outer
}

// Same gap, the matching tangent stiffness half (Analysis::assemble_buoyancy_stiffness()) --
// needs a node straddling the water surface (partial submersion) to get a nonzero result, since
// the fully-submerged/fully-dry regimes are zero-stiffness by construction (see hydrostatics.hpp).
TEST_CASE("Analysis::assemble_buoyancy_stiffness includes TrussElement's submersion-dependent stiffness", "[analysis][truss]") {
    using namespace risersim;

    RiserModel model;
    // D_outer=0.2 -> radius=0.1 -- both ends 0.05m below the surface sit inside (0, 2*radius),
    // i.e. genuinely straddling, the only regime with nonzero stiffness.
    Node3D* n1 = model.add_node(1, 0.0, 0.0, -0.05);
    Node3D* n2 = model.add_node(2, 2.0, 0.0, -0.05);
    n1->eq_numbers = {0, 0, 0, 0, 0, 0};
    n2->eq_numbers = {0, 0, 0, 0, 0, 0};

    TrussProps props;
    props.A = 0.001;
    props.D_outer = 0.2;
    model.add_truss_element(1, n1, n2, props);

    model.environmental().water_surface_z = 0.0;
    model.environmental().water_density = 1000.0;

    StaticAnalysis analysis;
    analysis.model = &model;
    analysis.water_density = 1000.0;
    analysis.assign_equation_numbers();

    Eigen::SparseMatrix<double> K = analysis.assemble_buoyancy_stiffness();

    int eq1_z = n1->eq_numbers[2];
    int eq2_z = n2->eq_numbers[2];
    REQUIRE(K.coeff(eq1_z, eq1_z) > 0.0);
    REQUIRE(K.coeff(eq2_z, eq2_z) > 0.0);
}
