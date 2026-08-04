#ifndef RISERSIM_DYNAMIC_ANALYSIS_HPP
#define RISERSIM_DYNAMIC_ANALYSIS_HPP

#include "risersim/analysis.hpp"

namespace risersim {

class DynamicAnalysis : public Analysis {
public:
    double duration_s;
    double dt_s;
    double wave_amplitude;
    double wave_period;
    double wave_angle_deg;
    double wave_gamma;
    double alpha_rayleigh;
    double beta_rayleigh;
    int max_nr_iters;
    double nr_tolerance;

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

    bool solve_time_domain_dynamic(double duration = 20.0, double dt = 0.05, double amp = 2.5, double period = 10.0, double alpha = 0.05, double beta = 0.01);

    // Uniform polymorphic solve implementation
    bool solve() override;
};

} // namespace risersim

#endif
