#ifndef RISERSIM_STATIC_ANALYSIS_HPP
#define RISERSIM_STATIC_ANALYSIS_HPP

#include "risersim/analysis.hpp"
#include "risersim/vessel_offset.hpp"

namespace risersim {

class StaticAnalysis : public Analysis {
public:
    int load_steps;
    int max_iter_per_step;
    double tol;
    VesselOffset offset;
    bool enable_offset;

    StaticAnalysis()
        : Analysis(),
          load_steps(20),
          max_iter_per_step(300),
          tol(100.0),
          offset(OffsetMode::Far, 10.0),
          enable_offset(true) {}

    bool solve_catenary_static(int steps = 20, int max_iter = 300, double tolerance = 100.0);
    bool solve_vessel_offset(const VesselOffset& vessel_offset, int steps = 20, int max_iter = 300, double tolerance = 100.0);

    // Uniform polymorphic solve implementation
    bool solve() override;
};

} // namespace risersim

#endif
