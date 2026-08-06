/**
 * @file main.cpp
 * @brief Standalone CLI entry point: parses argv and reports the exit code. All actual
 * model-loading/analysis/export work is delegated to risersim::Simulation (simulation.hpp),
 * mirroring how real ANFLEX's own driver just calls cAnflexAnalysis::load_model()/solve().
 */
#include <iostream>
#include <string>

#include "risersim/simulation.hpp"

int main(int argc, char* argv[]) {
    std::cout << "=========================================================" << std::endl;
    std::cout << "              RiserSim - Simulation Engine               " << std::endl;
    std::cout << "=========================================================" << std::endl;

    std::string input_json_path = argc > 1 ? argv[1] : "";
    std::string output_dir = argc > 2 ? argv[2] : ".";

    risersim::Simulation sim;
    sim.load(input_json_path);
    sim.run();
    sim.export_results(output_dir);

    return sim.ok() ? 0 : 1;
}
