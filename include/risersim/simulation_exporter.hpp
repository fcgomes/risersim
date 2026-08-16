/**
 * @file simulation_exporter.hpp
 * @brief Result export to HDF5 for post-processing/visualization.
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
     * @brief Exports static and dynamic analysis history to an HDF5 file (requires `RISERSIM_HAS_HDF5`).
     * @param static_analysis Converged static analysis (initial/reference state).
     * @param dynamic_analysis Dynamic analysis whose step history is exported (may be an empty/unsolved instance).
     * @param seabed_depth Z of the seabed plane, for the viewer's seabed plane/bounding box.
     * @param water_surface_z Z of the sea surface plane, for the viewer's water-surface plane/bounding box.
     * @param filename Output path.
     * @return true on success.
     */
    static bool export_hdf5(const Analysis& static_analysis, const Analysis& dynamic_analysis,
                             double seabed_depth, double water_surface_z,
                             const std::string& filename = "catenary_results.h5");
};

} // namespace risersim

#endif
