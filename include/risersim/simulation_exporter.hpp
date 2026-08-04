/**
 * @file simulation_exporter.hpp
 * @brief Result export to JSON and HDF5 for post-processing/visualization.
 */
#ifndef RISERSIM_SIMULATION_EXPORTER_HPP
#define RISERSIM_SIMULATION_EXPORTER_HPP

#include "risersim/analysis.hpp"
#include <string>

namespace risersim {

/** @brief Static-only utility for writing static + dynamic analysis results to disk. */
class SimulationExporter {
public:
    /**
     * @brief Exports static and dynamic analysis history to a JSON file.
     * @param static_analysis Converged static analysis (initial/reference state).
     * @param dynamic_analysis Dynamic analysis whose step history is exported (may be an empty/unsolved instance).
     * @param filename Output path.
     * @return true on success.
     */
    static bool export_json(const Analysis& static_analysis, const Analysis& dynamic_analysis, const std::string& filename = "catenary_results.json");

    /**
     * @brief Exports static and dynamic analysis history to an HDF5 file (requires `RISERSIM_HAS_HDF5`).
     * @param static_analysis Converged static analysis (initial/reference state).
     * @param dynamic_analysis Dynamic analysis whose step history is exported (may be an empty/unsolved instance).
     * @param filename Output path.
     * @return true on success.
     */
    static bool export_hdf5(const Analysis& static_analysis, const Analysis& dynamic_analysis, const std::string& filename = "catenary_results.h5");
};

} // namespace risersim

#endif
