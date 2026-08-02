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
    double alpha_rayleigh;
    double beta_rayleigh;

    DynamicAnalysis()
        : Analysis(),
          duration_s(20.0),
          dt_s(0.05),
          wave_amplitude(2.5),
          wave_period(10.0),
          alpha_rayleigh(0.05),
          beta_rayleigh(0.01) {}

    explicit DynamicAnalysis(const Analysis& static_analysis)
        : Analysis(),
          duration_s(20.0),
          dt_s(0.05),
          wave_amplitude(2.5),
          wave_period(10.0),
          alpha_rayleigh(0.05),
          beta_rayleigh(0.01) {
        nodes = static_analysis.nodes;
        elements = static_analysis.elements;
        seabed = static_analysis.seabed;
        current = static_analysis.current;
        enable_current = static_analysis.enable_current;
        water_density = static_analysis.water_density;
        num_dofs = static_analysis.num_dofs;
    }

    bool solve_time_domain_dynamic(double duration = 20.0, double dt = 0.05, double amp = 2.5, double period = 10.0, double alpha = 0.05, double beta = 0.01);

    // Uniform polymorphic solve implementation
    bool solve() override;
};

} // namespace risersim

#endif
