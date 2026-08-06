/**
 * @file simulation.hpp
 * @brief Orchestrates one full simulation run (load model, run static + dynamic analysis, export
 * results), mirroring real ANFLEX's cAnflexAnalysis (owns model builder(s), calls load_model()
 * then solve(), which drives assembly/static/dynamic and export).
 */
#ifndef RISERSIM_SIMULATION_HPP
#define RISERSIM_SIMULATION_HPP

#include "risersim/model_builder.hpp"
#include "risersim/static_analysis.hpp"
#include "risersim/dynamic_analysis.hpp"
#include <memory>
#include <string>

namespace risersim {

/**
 * @brief Drives one riser/mooring simulation end to end. This is what `main()` talks to --
 * `main()` itself only parses argv and reports the exit code, it does not touch RiserModel/
 * StaticAnalysis/DynamicAnalysis directly.
 */
class Simulation {
public:
    ModelBuilder model_builder;
    StaticAnalysis static_analysis;

    /// Only constructed inside run(), after static_analysis.solve() -- DynamicAnalysis(const
    /// Analysis&) copies model/seabed/current/num_dofs from static_analysis, and num_dofs is
    /// only correct once assign_equation_numbers() has run inside the static solve.
    std::unique_ptr<DynamicAnalysis> dynamic_analysis;

    bool parsed_from_json = false;
    bool success_static = false;
    bool success_dynamic = true;

    /**
     * @brief Loads the model from `json_path`, falling back to the synthetic parabolic catenary
     * test geometry if the path is empty or the file can't be opened/parsed. Prints every
     * warning ModelBuilder accumulated while loading.
     */
    void load(const std::string& json_path);

    /**
     * @brief Configures and runs the static analysis, then -- if dynamic analysis is enabled and
     * the static analysis converged -- configures and runs the dynamic analysis. Same phase
     * ordering/skip conditions as the original main_test.cpp.
     */
    void run();

    /** @brief Exports static + dynamic results to `<output_dir>/catenary_results.{json,h5}`. */
    void export_results(const std::string& output_dir) const;

    /** @brief True if every phase that ran converged. */
    bool ok() const { return success_static && success_dynamic; }
};

} // namespace risersim

#endif // RISERSIM_SIMULATION_HPP
