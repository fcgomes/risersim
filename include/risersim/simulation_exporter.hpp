#ifndef RISERSIM_SIMULATION_EXPORTER_HPP
#define RISERSIM_SIMULATION_EXPORTER_HPP

#include "risersim/analysis.hpp"
#include <string>

namespace risersim {

class SimulationExporter {
public:
    static bool export_json(const Analysis& static_analysis, const Analysis& dynamic_analysis, const std::string& filename = "catenary_results.json");
    static bool export_hdf5(const Analysis& static_analysis, const Analysis& dynamic_analysis, const std::string& filename = "catenary_results.h5");
};

} // namespace risersim

#endif
