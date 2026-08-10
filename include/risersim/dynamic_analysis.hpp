/**
 * @file dynamic_analysis.hpp
 * @brief Time-domain dynamic analysis: irregular wave loading on top of a converged static equilibrium.
 */
#ifndef RISERSIM_DYNAMIC_ANALYSIS_HPP
#define RISERSIM_DYNAMIC_ANALYSIS_HPP

#include "risersim/analysis.hpp"
#include "risersim/vessel_motion.hpp"
#include <optional>

namespace risersim {

/**
 * @brief Time-domain dynamic analysis (Newmark-type integration, Rayleigh damping, JONSWAP wave loading).
 */
class DynamicAnalysis : public Analysis {
public:
    double duration_s;
    double dt_s;
    double wave_amplitude;
    double wave_period;
    double wave_angle_deg;
    double wave_gamma;
    double alpha_rayleigh; ///< Rayleigh mass-proportional damping coefficient.
    double beta_rayleigh;  ///< Rayleigh stiffness-proportional damping coefficient.
    int max_nr_iters;
    double nr_tolerance;

    /**
     * @brief If true, stops the time-domain loop at the first time step whose Newton-Raphson
     * doesn't converge, instead of running the full duration regardless.
     *
     * Default true: a non-converged step's accepted state is physically meaningless (whatever
     * the Newton loop happened to land on when it bailed out, not an equilibrium), so every
     * later step built on top of it is meaningless too -- letting the loop run to completion
     * anyway just produces a full timeline of numbers that look like results but aren't (see
     * docs/roadmap.md item 1b: this was previously false, and the resulting "steps 16-20 also
     * fail" noise from a single bad step 15 obscured, rather than clarified, where the real
     * problem was). Set false only when deliberately characterizing how badly/far a known-bad
     * case diverges (a diagnostic use, not a normal run).
     */
    bool stop_on_first_non_convergence = true;

    /// Real RAO+JONSWAP top motion (see vessel_motion.hpp), set by Simulation::run() when the
    /// input JSON has real data for it. Empty (`std::nullopt`) falls back to the old single-Z
    /// regular-wave sinusoid already in solve_time_domain_dynamic() -- zero behavior change for
    /// any model without this data (synthetic, older JSONs, `aml_reader.py`).
    std::optional<VesselMotion> vessel_motion;

    DynamicAnalysis()
        : Analysis(),
          duration_s(20.0),
          dt_s(0.05),
          wave_amplitude(2.5),
          wave_period(10.0),
          wave_angle_deg(0.0),
          wave_gamma(3.3),
          alpha_rayleigh(0.05),
          beta_rayleigh(0.01),
          max_nr_iters(20),
          nr_tolerance(1.0e-4) {}

    /**
     * @brief Builds a dynamic analysis that continues from a converged static analysis's model and environment.
     * @param static_analysis The prior static analysis to carry the model/seabed/current state over from.
     */
    explicit DynamicAnalysis(const Analysis& static_analysis)
        : Analysis(),
          duration_s(20.0),
          dt_s(0.05),
          wave_amplitude(2.5),
          wave_period(10.0),
          wave_angle_deg(0.0),
          wave_gamma(3.3),
          alpha_rayleigh(0.05),
          beta_rayleigh(0.01),
          max_nr_iters(20),
          nr_tolerance(1.0e-4) {
        model = static_analysis.model;
        seabed = static_analysis.seabed;
        current = static_analysis.current;
        enable_current = static_analysis.enable_current;
        water_density = static_analysis.water_density;
        water_density_for_mass = static_analysis.water_density_for_mass;
        num_dofs = static_analysis.num_dofs;
    }

    /**
     * @brief Runs the time-domain dynamic simulation.
     * @param duration Total simulated time (s).
     * @param dt Time step (s).
     * @param amp Significant wave height (m).
     * @param period Peak wave period (s).
     * @param alpha Rayleigh mass-proportional damping coefficient.
     * @param beta Rayleigh stiffness-proportional damping coefficient.
     * @return true if every time step converged.
     */
    bool solve_time_domain_dynamic(double duration = 20.0, double dt = 0.05, double amp = 2.5, double period = 10.0, double alpha = 0.05, double beta = 0.01);

    /** @brief Runs solve_time_domain_dynamic() with this instance's configured parameters. */
    bool solve() override;
};

} // namespace risersim

#endif
