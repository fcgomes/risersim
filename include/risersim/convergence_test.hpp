/**
 * @file convergence_test.hpp
 * @brief Multi-criterion Newton-Raphson convergence test, mirroring ANFLEX's `cConvergenceTest`.
 */
#ifndef RISERSIM_CONVERGENCE_TEST_HPP
#define RISERSIM_CONVERGENCE_TEST_HPP

#include "risersim/model.hpp"
#include <Eigen/Core>
#include <array>

namespace risersim {

/**
 * @brief The six convergence criteria ANFLEX supports, mirroring `eConvergenceCriterion`.
 *
 * Translation and Rotation are always active (matching ANFLEX, where the displacement
 * criterion is unconditionally enabled -- `// Criterio de deslocamento sempre eh usado`). The
 * other four are opt-in (`ConvergenceConfig`), matching ANFLEX's own AML-driven defaults.
 */
enum class ConvergenceCriterion {
    Translation,
    Rotation,
    ForcesNorm,
    MomentsNorm,
    UnbalancedForces,
    UnbalancedMoments,
    Count // sentinel, not a real criterion
};

/** @brief Per-criterion state: whether it's active, its tolerance, and the last evaluated value. */
struct ConvergenceCriterionState {
    bool enabled = false;
    double tolerance = 0.0;
    bool converged = false;
    double value = 0.0;

    /**
     * @brief Evaluates this criterion against a value, updating converged/value as a side effect.
     * @return true if disabled (a disabled criterion never blocks convergence) or if `value <= tolerance`.
     */
    bool check(double v) {
        if (!enabled) return true;
        value = v;
        converged = (value <= tolerance);
        return converged;
    }
};

/**
 * @brief Configuration for ConvergenceTest, mirroring the relevant fields of ANFLEX's `sAnalysisData`.
 *
 * `transl_tol`/`rot_tol` default to risersim's historical single shared tolerance (the `tolerance`
 * parameter already threaded through `StaticAnalysis::solve_catenary_static()`). The other four
 * criteria default to disabled, matching ANFLEX's own AML-driven opt-in behavior and preserving
 * risersim's current convergence behavior exactly until a caller explicitly enables them.
 */
struct ConvergenceConfig {
    double transl_tol = 100.0;
    double rot_tol = 100.0;

    bool use_force_criterion = false;
    double force_tol = 1.0e-3;
    double moment_tol = 1.0e-3;

    bool check_unbalanced_force = false;
    double max_unbalanced_force_tol = 100.0;   ///< N
    bool check_unbalanced_moment = false;
    double max_unbalanced_moment_tol = 100.0;  ///< N.m
};

/**
 * @brief Multi-criterion Newton-Raphson convergence test, mirroring ANFLEX's `cConvergenceTest`.
 *
 * Extracted from the inline logic that used to live directly in
 * `StaticAnalysis::solve_catenary_static()` (translation/rotation increment ratios only). Adds
 * the four residual-based criteria ANFLEX also supports (forces/moments norm, max single-DOF
 * unbalanced force/moment) -- implemented but disabled by default, so existing behavior is
 * unchanged until a caller opts in via ConvergenceConfig.
 */
class ConvergenceTest {
public:
    explicit ConvergenceTest(const ConvergenceConfig& config);

    /** @brief Resets per-load-step bookkeeping (call once at the start of each load step). */
    void start();

    /**
     * @brief Evaluates all active criteria for this iteration.
     *
     * @param model Used to classify each global DOF as translation or rotation, via each node's
     *              `eq_numbers` (indices 0-2 translation, 3-5 rotation).
     * @param accepted_dU This iteration's accepted displacement increment (all DOFs).
     * @param cumulative_dU Total displacement increment accumulated so far this load step,
     *                       including `accepted_dU` (all DOFs).
     * @param residual Force/moment residual `F_ext - F_int` for this iteration (all DOFs).
     * @param iter 0-based Newton iteration index within this load step.
     * @param max_iter This load step's iteration budget.
     * @return true if every active criterion is satisfied.
     */
    bool check(const RiserModel& model,
               const Eigen::VectorXd& accepted_dU,
               const Eigen::VectorXd& cumulative_dU,
               const Eigen::VectorXd& residual,
               int iter,
               int max_iter);

    /** @brief Read-only access to a criterion's last-evaluated state, for introspection/reporting. */
    const ConvergenceCriterionState& criterion(ConvergenceCriterion which) const;

private:
    ConvergenceCriterionState& at(ConvergenceCriterion which);

    ConvergenceConfig config_;
    std::array<ConvergenceCriterionState, static_cast<size_t>(ConvergenceCriterion::Count)> criteria_;
    double forces_norm_1st_iter_ = 1.0;
    double moments_norm_1st_iter_ = 1.0;
};

} // namespace risersim

#endif
