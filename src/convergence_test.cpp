/**
 * @file convergence_test.cpp
 * @brief ConvergenceTest implementation.
 */
#include "risersim/convergence_test.hpp"
#include <algorithm>
#include <cmath>

namespace risersim {

namespace {

/** @brief Per-DOF-type (translation/rotation) norm and max-absolute-value of a global DOF vector. */
struct DofNorms {
    double transl_norm = 0.0;
    double rot_norm = 0.0;
    double max_abs_transl = 0.0;
    double max_abs_rot = 0.0;
};

/** @brief Splits a global DOF vector into translation/rotation components via each node's `eq_numbers`. */
DofNorms split_by_dof_type(const RiserModel& model, const Eigen::VectorXd& v) {
    DofNorms r;
    double t2 = 0.0, r2 = 0.0;
    for (const auto& node : model.nodes()) {
        for (int i = 0; i < 3; ++i) {
            int eq = node->eq_numbers[i];
            if (eq >= 0) {
                t2 += v[eq] * v[eq];
                r.max_abs_transl = std::max(r.max_abs_transl, std::abs(v[eq]));
            }
            int eq_rot = node->eq_numbers[i + 3];
            if (eq_rot >= 0) {
                r2 += v[eq_rot] * v[eq_rot];
                r.max_abs_rot = std::max(r.max_abs_rot, std::abs(v[eq_rot]));
            }
        }
    }
    r.transl_norm = std::sqrt(t2);
    r.rot_norm = std::sqrt(r2);
    return r;
}

} // namespace

ConvergenceTest::ConvergenceTest(const ConvergenceConfig& config) : config_(config) {
    // Displacement criteria are always active, mirroring ANFLEX's unconditional
    // "Criterio de deslocamento sempre eh usado".
    at(ConvergenceCriterion::Translation).enabled = true;
    at(ConvergenceCriterion::Translation).tolerance = config_.transl_tol;
    at(ConvergenceCriterion::Rotation).enabled = true;
    at(ConvergenceCriterion::Rotation).tolerance = config_.rot_tol;

    at(ConvergenceCriterion::ForcesNorm).enabled = config_.use_force_criterion;
    at(ConvergenceCriterion::ForcesNorm).tolerance = config_.force_tol;
    at(ConvergenceCriterion::MomentsNorm).enabled = config_.use_force_criterion;
    at(ConvergenceCriterion::MomentsNorm).tolerance = config_.moment_tol;

    at(ConvergenceCriterion::UnbalancedForces).enabled = config_.check_unbalanced_force;
    at(ConvergenceCriterion::UnbalancedForces).tolerance = config_.max_unbalanced_force_tol;
    at(ConvergenceCriterion::UnbalancedMoments).enabled = config_.check_unbalanced_moment;
    at(ConvergenceCriterion::UnbalancedMoments).tolerance = config_.max_unbalanced_moment_tol;
}

void ConvergenceTest::start() {
    for (auto& c : criteria_) {
        c.converged = false;
        c.value = 0.0;
    }
}

bool ConvergenceTest::check(const RiserModel& model,
                             const Eigen::VectorXd& accepted_dU,
                             const Eigen::VectorXd& cumulative_dU,
                             const Eigen::VectorXd& residual,
                             int iter,
                             int max_iter) {
    bool conv = true;

    // Translation / rotation: ratio of this iteration's correction norm to the cumulative
    // displacement norm accumulated so far this load step. Mirrors real ANFLEX's
    // cConvergenceTest::check_translation/check_rotation (transl_norm / transl_dinc_norm).
    DofNorms this_inc = split_by_dof_type(model, accepted_dU);
    DofNorms cum_inc = split_by_dof_type(model, cumulative_dU);
    double ratio_transl = (cum_inc.transl_norm > 1.0e-12) ? (this_inc.transl_norm / cum_inc.transl_norm) : 0.0;
    double ratio_rot = (cum_inc.rot_norm > 1.0e-12) ? (this_inc.rot_norm / cum_inc.rot_norm) : 0.0;

    // At least 2 iterations required: at iter==0, cumulative_dU already includes this
    // iteration's own contribution, so the ratio would trivially read "converged" on the very
    // first correction. See mapa_classes_anflex_estatica.md.
    bool transl_conv = (iter >= 1) && at(ConvergenceCriterion::Translation).check(ratio_transl);
    bool rot_conv = (iter >= 1) && at(ConvergenceCriterion::Rotation).check(ratio_rot);
    conv = conv && transl_conv && rot_conv;

    DofNorms res = split_by_dof_type(model, residual);

    // Forces/moments norm, normalized by the residual at this step's 1st iteration -- mirrors
    // ANFLEX's check_force/check_moment (prop_transl = forces_norm / m_forces_norm_1st_iter).
    if (iter == 0) {
        forces_norm_1st_iter_ = (res.transl_norm > at(ConvergenceCriterion::ForcesNorm).tolerance) ? res.transl_norm : 1.0;
        moments_norm_1st_iter_ = (res.rot_norm > at(ConvergenceCriterion::MomentsNorm).tolerance) ? res.rot_norm : 1.0;
    }
    if (at(ConvergenceCriterion::ForcesNorm).enabled) {
        conv = conv && at(ConvergenceCriterion::ForcesNorm).check(res.transl_norm / forces_norm_1st_iter_);
    }
    if (at(ConvergenceCriterion::MomentsNorm).enabled) {
        conv = conv && at(ConvergenceCriterion::MomentsNorm).check(res.rot_norm / moments_norm_1st_iter_);
    }

    // Max unbalanced single-DOF force/moment, absolute (not normalized).
    if (at(ConvergenceCriterion::UnbalancedForces).enabled) {
        conv = conv && at(ConvergenceCriterion::UnbalancedForces).check(res.max_abs_transl);
    }
    if (at(ConvergenceCriterion::UnbalancedMoments).enabled) {
        conv = conv && at(ConvergenceCriterion::UnbalancedMoments).check(res.max_abs_rot);
    }

    // Real ANFLEX's near-iteration-limit escape hatch: within the last 3 iterations of the
    // budget, satisfying the max-unbalanced-force/moment criterion alone is enough, even if
    // other active criteria haven't converged yet -- avoids spuriously failing a step over a
    // slowly-decaying (but already small) increment ratio right at the iteration limit.
    if (iter > max_iter - 3) {
        if (at(ConvergenceCriterion::UnbalancedForces).enabled &&
            at(ConvergenceCriterion::UnbalancedForces).check(res.max_abs_transl)) {
            conv = true;
        }
        if (at(ConvergenceCriterion::UnbalancedMoments).enabled &&
            at(ConvergenceCriterion::UnbalancedMoments).check(res.max_abs_rot)) {
            conv = true;
        }
    }

    return conv;
}

const ConvergenceCriterionState& ConvergenceTest::criterion(ConvergenceCriterion which) const {
    return criteria_[static_cast<size_t>(which)];
}

ConvergenceCriterionState& ConvergenceTest::at(ConvergenceCriterion which) {
    return criteria_[static_cast<size_t>(which)];
}

} // namespace risersim
