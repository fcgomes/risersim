/**
 * @file main.cpp
 * @brief Standalone CLI entry point: parses argv and reports the exit code. All actual
 * model-loading/analysis/export work is delegated to risersim::Simulation (simulation.hpp),
 * mirroring how real ANFLEX's own driver just calls cAnflexAnalysis::load_model()/solve().
 */
#include <filesystem>
#include <iostream>
#include <string>

#include "risersim/simulation.hpp"

int main(int argc, char* argv[]) {
    std::cout << "=========================================================" << std::endl;
    std::cout << "              RiserSim - Simulation Engine               " << std::endl;
    std::cout << "=========================================================" << std::endl;

    if (argc < 2) {
        std::cerr << "Uso: " << argv[0] << " <input.json> [nome_arquivo_resultado]" << std::endl;
        return 1;
    }
    std::string input_json_path = argv[1];
    // Optional -- the run manager (run_worker.py) already knows the project+case name at run
    // creation time and passes the real results filename here, so the file is born with the
    // right name instead of a fixed placeholder that gets renamed afterward. Callers that don't
    // care (e.g. the manual run_from_aml.py CLI workflow) just omit it.
    std::string results_filename = argc > 2 ? argv[2] : "catenary_results.h5";

    // The results file is always written next to the input file -- no separate output-dir
    // argument. Every caller (run_worker.py, run_from_aml.py) already places input_simulation.json
    // inside the run's own directory, so this was already true in practice; deriving it here just
    // drops the redundant CLI argument.
    std::filesystem::path input_path(input_json_path);
    std::string output_dir = input_path.has_parent_path() ? input_path.parent_path().string() : ".";

    risersim::Simulation sim;
    sim.load(input_json_path);
    if (!sim.parsed_from_json) {
        return 1;
    }
    sim.run();
    sim.export_results(output_dir, results_filename);

    return sim.ok() ? 0 : 1;
}
