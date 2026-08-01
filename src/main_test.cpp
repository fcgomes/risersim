#include <iostream>
#include <iomanip>
#include "risersim/node.hpp"
#include "risersim/element_beam.hpp"
#include "risersim/solver.hpp"
#include "risersim/hydrodynamics.hpp"
#include "risersim/seabed.hpp"
#include "risersim/buoyancy_and_restrictor.hpp"
#include "risersim/current_profile.hpp"

int main() {
    std::cout << "=========================================================" << std::endl;
    std::cout << "  riserSim Item 1: 3D Cross-Current & Drag Force Test    " << std::endl;
    std::cout << "=========================================================" << std::endl;

    // 1. Define Riser Properties
    risersim::BeamMaterialProps props;
    props.E = 2.1e11;          // 210 GPa (Steel)
    props.G = 8.0e10;          // 80 GPa
    props.A = 0.015;           // 0.015 m^2
    props.IY = 5.0e-5;         // 5e-5 m^4
    props.IZ = 5.0e-5;         // 5e-5 m^4
    props.J = 1.0e-4;          // 1e-4 m^4
    props.rho = 7850.0;        // Steel mass density (kg/m^3)
    props.D_outer = 0.25;      // 250 mm outer diameter
    props.D_inner = 0.20;      // 200 mm inner diameter
    props.rho_fluid = 850.0;   // Oil fluid density (kg/m^3)
    props.Ca = 1.0;            // Hydrodynamic added mass

    // 2. Discretize Catenary Riser into 20 Elements
    const int num_elements = 20;
    const int num_nodes = num_elements + 1;
    const double total_span_x = 100.0;  // 100 meters
    const double total_depth_z = -80.0; // 80 meters depth

    std::vector<risersim::Node3D*> nodes;
    std::vector<risersim::CorotationalBeam3D*> elements;

    for (int i = 0; i < num_nodes; ++i) {
        double ratio = static_cast<double>(i) / static_cast<double>(num_elements);
        double x = ratio * total_span_x;
        double z = ratio * total_depth_z;
        
        auto* node = new risersim::Node3D(i + 1, x, 0.0, z);
        nodes.push_back(node);
    }

    nodes.front()->eq_numbers = std::vector<int>(6, -1);
    nodes.back()->eq_numbers = std::vector<int>(6, -1);

    for (size_t i = 1; i < nodes.size() - 1; ++i) {
        nodes[i]->eq_numbers = {0, 1, 2, -1, -1, -1};
    }

    for (int i = 0; i < num_elements; ++i) {
        auto* elem = new risersim::CorotationalBeam3D(i + 1, nodes[i], nodes[i + 1], props);
        elem->p_i = 3.0e7; // 300 bar
        elements.push_back(elem);
    }

    // 3. Apply Bend Restrictor to Top Element 1
    risersim::BendRestrictor restrictor(5.0);
    restrictor.apply_to_element(elements[0]);

    // 4. Apply Buoyancy Modules to Elements 8-11 (Lazy Wave)
    risersim::BuoyancyModule buoy(0.80, 1200.0);
    for (int i = 7; i <= 10; ++i) {
        buoy.apply_to_element(elements[i]);
    }

    // 5. Configure 3D Subsurface Cross-Current Profile (V_surf = 1.5 m/s, Heading = 90 deg -> +Y)
    risersim::StaticAnalysis analysis;
    analysis.nodes = nodes;
    analysis.elements = elements;
    analysis.water_density = 1025.0;
    analysis.seabed = risersim::SeabedInteraction(-80.0, 1.0e5, 0.5);

    analysis.enable_current = true;
    analysis.current = risersim::CurrentProfile(1.5, -80.0, 90.0, 0.1428, 1.0); // 1.5 m/s, 90° (+Y)

    std::cout << "[INFO] Enabled 3D Subsurface Cross-Current (V_surf: 1.5 m/s, Heading: 90 deg -> +Y)." << std::endl;

    for (auto* elem : elements) {
        double avg_z = 0.5 * (elem->node1->coords.z() + elem->node2->coords.z());
        elem->p_e = 1025.0 * 9.81 * std::abs(avg_z);
        elem->update_effective_tension();
    }

    bool success = analysis.solve_catenary_static(10, 200, 2.5);
    analysis.export_json("catenary_results.json");

    if (success) {
        std::cout << "\n=========================================================" << std::endl;
        std::cout << "  🌊 3D LAZY WAVE WITH LATERAL CROSS-CURRENT PROFILE" << std::endl;
        std::cout << "=========================================================" << std::endl;
        std::cout << std::setw(8) << "Node ID" << std::setw(12) << "X (m)" << std::setw(12) << "Y (m)" << std::setw(12) << "Z (m)" << std::setw(22) << "Status" << std::endl;
        std::cout << "---------------------------------------------------------" << std::endl;
        for (auto* node : nodes) {
            Eigen::Vector3d curr = node->current_coords();
            std::string status = "🌊 SUSPENSO";
            if (node->id >= 8 && node->id <= 12) status = "🎈 FLUTUADOR (LAZY WAVE)";
            else if (curr.z() <= -79.9) status = "🏖️ NO SOLO (TDZ)";

            std::cout << std::setw(8) << node->id 
                      << std::setw(12) << std::fixed << std::setprecision(3) << curr.x()
                      << std::setw(12) << curr.y()
                      << std::setw(12) << curr.z()
                      << std::setw(22) << status << std::endl;
        }

        std::cout << "\n[3D DEFLECTION] Mid-Water Lateral Y Deflection at Node 10: " 
                  << nodes[9]->current_coords().y() << " m" << std::endl;
        std::cout << "[EFFECTIVE TENSION] Top Effective Tension: " 
                  << (elements.front()->tension_effective / 1000.0) << " kN" << std::endl;
    }

    for (auto* elem : elements) delete elem;
    for (auto* node : nodes) delete node;

    return success ? 0 : 1;
}
