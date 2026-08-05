/**
 * @file test_convergence_test.cpp
 * @brief Unit tests for ConvergenceTest: the translation/rotation gate, the optional
 * residual-based criteria (disabled by default), and the near-iteration-limit escape hatch --
 * each a distinct, easy-to-get-wrong piece of the real ANFLEX cConvergenceTest semantics this
 * class mirrors (see docs/mapa_classes_anflex_estatica.md, "Passo 4").
 */
#include <catch2/catch_test_macros.hpp>

#include "risersim/model.hpp"
#include "risersim/convergence_test.hpp"

using namespace risersim;

namespace {

/// A single free node (all 6 DOFs numbered 0..5), enough to exercise DOF-type splitting
/// without needing a full beam mesh.
RiserModel build_single_free_node_model() {
    RiserModel model;
    Node3D* n = model.add_node(1, 0.0, 0.0, 0.0);
    n->eq_numbers = {0, 1, 2, 3, 4, 5};
    return model;
}

} // namespace

TEST_CASE("ConvergenceTest: translation/rotation are always active and gated by iter >= 1", "[convergence_test]") {
    RiserModel model = build_single_free_node_model();
    ConvergenceConfig config;
    config.transl_tol = 0.01;
    config.rot_tol = 0.01;
    ConvergenceTest ct(config);
    ct.start();

    REQUIRE(ct.criterion(ConvergenceCriterion::Translation).enabled);
    REQUIRE(ct.criterion(ConvergenceCriterion::Rotation).enabled);
    // Matches ANFLEX's default: the other four criteria are opt-in, off unless configured.
    REQUIRE_FALSE(ct.criterion(ConvergenceCriterion::ForcesNorm).enabled);
    REQUIRE_FALSE(ct.criterion(ConvergenceCriterion::MomentsNorm).enabled);
    REQUIRE_FALSE(ct.criterion(ConvergenceCriterion::UnbalancedForces).enabled);
    REQUIRE_FALSE(ct.criterion(ConvergenceCriterion::UnbalancedMoments).enabled);

    Eigen::VectorXd accepted_dU(6);
    accepted_dU << 0.0001, 0, 0, 0.0001, 0, 0;
    Eigen::VectorXd cumulative_dU(6);
    cumulative_dU << 1.0, 0, 0, 1.0, 0, 0; // this iteration's share is tiny relative to the total
    Eigen::VectorXd residual = Eigen::VectorXd::Zero(6);

    // Ratios here are ~0.0001, well under the 0.01 tolerance -- but iter==0 must still fail:
    // cumulative_dU already includes this iteration's own contribution, so a ratio check at the
    // very first iteration would be a degenerate/misleading pass.
    REQUIRE_FALSE(ct.check(model, accepted_dU, cumulative_dU, residual, /*iter=*/0, /*max_iter=*/100));
    REQUIRE(ct.check(model, accepted_dU, cumulative_dU, residual, /*iter=*/1, /*max_iter=*/100));
}

TEST_CASE("ConvergenceTest: forces/moments norm criterion, when enabled, normalizes by the 1st-iteration residual", "[convergence_test]") {
    RiserModel model = build_single_free_node_model();
    ConvergenceConfig config;
    config.transl_tol = 0.01;
    config.rot_tol = 0.01;
    config.use_force_criterion = true;
    config.force_tol = 1.0e-3;
    config.moment_tol = 1.0e-3;
    ConvergenceTest ct(config);
    ct.start();

    // Well-converged translation/rotation increments at every iteration from here on, so only
    // the forces/moments criterion decides the outcome.
    Eigen::VectorXd accepted_dU(6);
    accepted_dU << 0.0001, 0, 0, 0.0001, 0, 0;
    Eigen::VectorXd cumulative_dU(6);
    cumulative_dU << 1.0, 0, 0, 1.0, 0, 0;

    // iter 0: establishes the 1st-iteration reference residual (Fx=100N).
    Eigen::VectorXd residual_iter0(6);
    residual_iter0 << 100.0, 0, 0, 0, 0, 0;
    ct.check(model, accepted_dU, cumulative_dU, residual_iter0, 0, 100);

    // iter 1: residual dropped to 0.05N -> ratio 0.05/100 = 5e-4 < 1e-3 tolerance -> converges.
    Eigen::VectorXd residual_converged(6);
    residual_converged << 0.05, 0, 0, 0, 0, 0;
    REQUIRE(ct.check(model, accepted_dU, cumulative_dU, residual_converged, 1, 100));

    // iter 2: residual back up to 50N -> ratio 0.5, well above tolerance -> blocks convergence
    // even though translation/rotation still pass.
    Eigen::VectorXd residual_not_converged(6);
    residual_not_converged << 50.0, 0, 0, 0, 0, 0;
    REQUIRE_FALSE(ct.check(model, accepted_dU, cumulative_dU, residual_not_converged, 2, 100));
}

TEST_CASE("ConvergenceTest: unbalanced-force escape hatch only applies near the iteration limit", "[convergence_test]") {
    RiserModel model = build_single_free_node_model();
    ConvergenceConfig config;
    config.transl_tol = 0.01;
    config.rot_tol = 0.01;
    config.check_unbalanced_force = true;
    config.max_unbalanced_force_tol = 10.0;
    ConvergenceTest ct(config);
    ct.start();

    // Translation/rotation never converge in this test (ratio stays 1.0 throughout): this
    // iteration's increment equals the full cumulative increment every time.
    Eigen::VectorXd accepted_dU(6);
    accepted_dU << 1.0, 0, 0, 1.0, 0, 0;
    Eigen::VectorXd cumulative_dU = accepted_dU;

    // Max unbalanced force well within tolerance (5N <= 10N).
    Eigen::VectorXd residual(6);
    residual << 5.0, 0, 0, 0, 0, 0;

    const int max_iter = 5;
    // Away from the iteration limit: the satisfied unbalanced-force criterion does NOT override
    // the still-failing translation/rotation gate.
    REQUIRE_FALSE(ct.check(model, accepted_dU, cumulative_dU, residual, /*iter=*/1, max_iter));
    // Within the last 3 iterations of the budget: real ANFLEX's escape hatch kicks in, and the
    // satisfied unbalanced-force criterion alone is enough.
    REQUIRE(ct.check(model, accepted_dU, cumulative_dU, residual, /*iter=*/3, max_iter));
}
