/**
 * @file dynamic_analysis.hpp
 * @brief Time-domain dynamic analysis: irregular wave loading on top of a converged static equilibrium.
 */
#ifndef RISERSIM_DYNAMIC_ANALYSIS_HPP
#define RISERSIM_DYNAMIC_ANALYSIS_HPP

#include "risersim/analysis.hpp"

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
     * Default false, matching the original behavior: run every time step, accepting whatever
     * state each step's Newton-Raphson reached even if it didn't converge, and only report
     * overall failure at the end (deliberate for time-domain dynamics, where continuing past a
     * bad step and hoping the system recovers is common practice). But when a model is known to
     * diverge early and stay diverged (e.g. while isolating a convergence problem), running the
     * full duration anyway just burns the iteration budget on hundreds of hopeless steps for no
     * new information -- this lets a caller opt into stopping immediately instead.
     */
    bool stop_on_first_non_convergence = false;

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
