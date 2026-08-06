/**
 * @file static_analysis.hpp
 * @brief Static (catenary/vessel-offset) analysis: incremental load stepping with full-step Newton-Raphson.
 */
#ifndef RISERSIM_STATIC_ANALYSIS_HPP
#define RISERSIM_STATIC_ANALYSIS_HPP

#include "risersim/analysis.hpp"
#include "risersim/vessel_offset.hpp"

namespace risersim {

/**
 * @brief Controls when artificial stiffness (Tikhonov regularization, see StaticIntegrator) is active within solve_catenary_static().
 */
enum class ArtificialStiffnessMode {
    OnlyFirstStep, ///< risersim's historical behavior: artificial stiffness only during load step 1.
    EveryStep,     ///< Used in the "assembly" phase (see docs/mapa_classes_anflex_estatica.md): decays every iteration within *every* load step, giving the mesh a safety net across the whole load path, not just the first increment.
    Never          ///< Used in the "static" (real, clean) phase, matching ANFLEX's real behavior (`m_have_artificial_stiffness=false`).
};

/**
 * @brief Static analysis: builds up load in discrete steps, each solved by full-step Newton-Raphson, equivalent to ANFLEX's static solve.
 */
class StaticAnalysis : public Analysis {
public:
    int load_steps;
    int max_iter_per_step;
    double tol;
    VesselOffset offset;
    bool enable_offset;

    /**
     * @brief Opts into ConvergenceTest's UnbalancedForces/UnbalancedMoments criteria (see
     * convergence_test.hpp), disabled by default -- matches ANFLEX's own AML-driven opt-in
     * behavior, and preserves risersim's existing behavior/reference values exactly unless a
     * caller explicitly turns this on.
     *
     * Targets a specific failure mode found investigating the Exemplo_01a soil+current
     * divergence (mapa_classes_anflex_estatica.md): a node sitting almost exactly at the seabed
     * boundary can settle into a stable, tiny-amplitude limit cycle (residual flipping sign each
     * iteration, ~1e-5 relative, already physically negligible) that never satisfies the
     * translation/rotation increment-ratio criterion because the ratio itself doesn't shrink --
     * it's oscillating, not converging or diverging. The escape hatch (see ConvergenceTest::check,
     * "within the last 3 iterations of the budget") accepts the step once the worst single-DOF
     * unbalanced force/moment is already below `unbalanced_force_tol`/`unbalanced_moment_tol`,
     * even though the ratio criterion never settles -- exactly ANFLEX's own mechanism for this
     * situation, just never wired up in risersim until now.
     */
    bool enable_unbalanced_criteria = false;
    double unbalanced_force_tol = 1.0;   ///< N
    double unbalanced_moment_tol = 1.0;  ///< N.m

    /**
     * @brief Whether solve() runs the "assembly" pre-phase (artificial stiffness every step,
     * followed by a single full-load step with no artificial stiffness) or a single ramped solve
     * (ArtificialStiffnessMode::OnlyFirstStep, matching real ANFLEX's `solve_static()` alone).
     *
     * Default `true` preserves the historical two-phase pipeline exactly, for any model that
     * doesn't carry the real `%ASSEMBLY.USING` flag (synthetic fallback model, older JSONs).
     * Real ANFLEX only runs an assembly phase when `%ASSEMBLY.USING.TRUE` is set in the AML
     * (`cAnflexAnalysis::solve()`, `anflex_analysis.cpp`); Exemplo_01a/01c both have
     * `%ASSEMBLY.USING.FALSE` and solve with a single smooth multi-step ramp instead -- forcing
     * the two-phase pipeline on them was the dominant cause of a >10x Newton-iteration-count gap
     * against real ANFLEX for the same problem. See docs/mapa_classes_anflex_estatica.md.
     */
    bool use_assembly_phase = true;

    StaticAnalysis()
        : Analysis(),
          load_steps(20),
          max_iter_per_step(300),
          tol(0.01),
          offset(OffsetMode::Far, 0.0),
          enable_offset(false) {}

    /**
     * @brief Solves the catenary/free-hanging static equilibrium by incremental load stepping.
     * @param steps Number of load increments (0..1 ramp).
     * @param max_iter Maximum Newton-Raphson iterations per load step.
     * @param tolerance Dimensionless translation/rotation increment-ratio tolerance, fed into
     *                  ConvergenceTest's Translation/Rotation criteria (see convergence_test.hpp):
     *                  this iteration's correction norm / cumulative correction norm for the load
     *                  step. Default 0.01 (1%). NOT a force residual in Newtons -- an earlier
     *                  version of this parameter (default 100.0) was documented as one but wired
     *                  directly into this ratio-based check, making the criterion a near no-op
     *                  (see mapa_classes_anflex_estatica.md, "tol=100.0 usado como tolerância de
     *                  força vs. razão adimensional").
     * @param artif_mode When artificial stiffness regularization is applied (see ArtificialStiffnessMode).
     * @return true if every load step converged.
     */
    bool solve_catenary_static(int steps = 20, int max_iter = 300, double tolerance = 0.01,
                                ArtificialStiffnessMode artif_mode = ArtificialStiffnessMode::OnlyFirstStep);

    /**
     * @brief Applies a prescribed top-node offset on top of a converged static equilibrium, incrementally.
     * @param vessel_offset Target top-node displacement to ramp up to.
     * @param steps Number of increments.
     * @param max_iter Maximum Newton-Raphson iterations per increment.
     * @param tolerance Unused -- convergence is checked via a hardcoded relative force-residual
     *                  threshold (see solve_vessel_offset()'s implementation). Kept for API
     *                  signature stability; does not affect behavior.
     * @return true if every increment converged.
     */
    bool solve_vessel_offset(const VesselOffset& vessel_offset, int steps = 20, int max_iter = 300, double tolerance = 0.01);

    /** @brief Runs solve_catenary_static() with this instance's configured parameters. */
    bool solve() override;
};

} // namespace risersim

#endif
