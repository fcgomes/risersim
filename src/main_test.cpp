#include <iostream>
#include <iomanip>
#include "risersim/node.hpp"
#include "risersim/element_beam.hpp"
#include "risersim/solver.hpp"
#include "risersim/hydrodynamics.hpp"
#include "risersim/seabed.hpp"
#include "risersim/buoyancy_and_restrictor.hpp"
#include "risersim/current_profile.hpp"
#include "risersim/vessel_offset.hpp"

int main() {
    std::cout << "=========================================================" << std::endl;
    std::cout << "  riserSim Item 2: Vessel Offset Analysis (Far/Near/Cross)" << std::endl;
    std::cout << "=========================================================" << std::endl;

    // 1. Define Riser Properties (Flexible Riser Pipe)
    risersim::BeamMaterialProps props;
    props.E = 2.0e9;           // 2.0 GPa (Flexible Riser Composite Core)
    props.G = 8.0e8;           // 0.8 GPa
    props.A = 0.015;           // 0.015 m^2
    props.IY = 5.0e-5;         // 5e-5 m^4
    props.IZ = 5.0e-5;         // 5e-5 m^4
    props.J = 1.0e-4;          // 1e-4 m^4
    props.rho = 7850.0;        // Steel mass density (kg/m^3)
    props.D_outer = 0.25;      // 250 mm outer diameter
    props.D_inner = 0.20;      // 200 mm inner diameter
    props.rho_fluid = 850.0;   // Oil fluid density (kg/m^3)
    props.Ca = 1.0;            // Hydrodynamic added mass

    // 2. Discretize Classic Deepwater Catenary Riser into 40 Elements (Total Line Scope = 180 m)
    const int num_elements = 40;
    const int num_nodes = num_elements + 1;
    const double total_span_x = 120.0;   // 120 meters horizontal span
    const double total_depth_z = -100.0; // 100 meters water depth

    std::vector<risersim::Node3D*> nodes;
    std::vector<risersim::CorotationalBeam3D*> elements;

    for (int i = 0; i < num_nodes; ++i) {
        double ratio = static_cast<double>(i) / static_cast<double>(num_elements);
        double x = ratio * total_span_x;
        // Initial smooth catenary curve with natural sagging
        double z = ratio * total_depth_z - 25.0 * std::sin(ratio * M_PI);
        
        auto* node = new risersim::Node3D(i + 1, x, 0.0, z);
        nodes.push_back(node);
    }

    nodes.front()->eq_numbers = std::vector<int>(6, -1);
    nodes.back()->eq_numbers = std::vector<int>(6, -1);

    for (size_t i = 1; i < nodes.size() - 1; ++i) {
        nodes[i]->eq_numbers = {0, 1, 2, -1, -1, -1};
    }

    const double L_unstretched = 4.35; // Un-stretched manufactured element length (174m total un-stretched pipe)
    for (int i = 0; i < num_elements; ++i) {
        auto* elem = new risersim::CorotationalBeam3D(i + 1, nodes[i], nodes[i + 1], props, L_unstretched);
        elem->p_i = 3.0e7; // 300 bar
        elements.push_back(elem);
    }

    // 3. Apply Bend Restrictor to Top Element 1
    risersim::BendRestrictor restrictor(5.0);
    restrictor.apply_to_element(elements[0]);

    // 4. Apply Buoyancy Modules (D_buoy = 0.60 m) to Elements 14-26 (Classic Lazy Wave S-Arch)
    risersim::BuoyancyModule buoy(0.60, 0.0);
    for (int i = 14; i <= 26; ++i) {
        buoy.apply_to_element(elements[i]);
    }

    // 5. Setup Static Analysis
    risersim::StaticAnalysis analysis;
    analysis.nodes = nodes;
    analysis.elements = elements;
    analysis.water_density = 1025.0;
    analysis.seabed = risersim::SeabedInteraction(-100.0, 1.0e5, 0.5);

    // Phase 1: Initial Catenary Static Equilibrium
    std::cout << "\n--- Phase 1: Initial Catenary Static Equilibrium ---" << std::endl;
    bool success_catenary = analysis.solve_catenary_static(20, 300, 100.0);

    // Phase 2: Impose Vessel Offset Far (+10.0m displacement in +X)
    std::cout << "\n--- Phase 2: Vessel Offset Far (+10.0 m) ---" << std::endl;
    risersim::VesselOffset offset_far(risersim::OffsetMode::Far, 10.0); // +10m Far Offset
    bool success_offset = analysis.solve_vessel_offset(offset_far, 20, 300, 100.0);

    // Phase 3: 3D Time-Domain Dynamic Wave Response (20s duration, dt = 0.05s)
    std::cout << "\n--- Phase 3: 3D Time-Domain Dynamic Wave Response ---" << std::endl;
    bool success_dynamic = analysis.solve_time_domain_dynamic(20.0, 0.05, 2.5, 10.0);

    analysis.export_json("risersim/catenary_results.json");
    analysis.export_json("catenary_results.json");
    analysis.export_hdf5("risersim/catenary_results.h5");
    analysis.export_hdf5("catenary_results.h5");

    if (success_catenary && success_offset) {
        std::cout << "\n=========================================================" << std::endl;
        std::cout << "  🚢 RISER PROFILE UNDER VESSEL OFFSET FAR (+10.0 m)" << std::endl;
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

        std::cout << "\n[TOP POSITION] Platform Top Position X: " << nodes.front()->current_coords().x() << " m" << std::endl;
        std::cout << "[EFFECTIVE TENSION] Top Effective Tension under Far Offset: " 
                  << (elements.front()->tension_effective / 1000.0) << " kN" << std::endl;

        std::cout << "\n=========================================================================" << std::endl;
        std::cout << "  📊 ITEM 3: BENDING MOMENTS, CURVATURE & VON MISES STRESS" << std::endl;
        std::cout << "=========================================================================" << std::endl;
        std::cout << std::setw(8) << "Elem ID" 
                  << std::setw(14) << "Tension (kN)" 
                  << std::setw(16) << "Moment (kN.m)" 
                  << std::setw(16) << "Curvature(1/m)" 
                  << std::setw(16) << "von Mises(MPa)" 
                  << std::setw(14) << "MBR SF" << std::endl;
        std::cout << "-------------------------------------------------------------------------" << std::endl;

        for (size_t i = 0; i < elements.size(); ++i) {
            auto* elem = elements[i];
            const auto* prev = (i > 0) ? elements[i - 1] : nullptr;
            const auto* next = (i + 1 < elements.size()) ? elements[i + 1] : nullptr;

            auto sc = elem->compute_stress_and_curvature(prev, next, 350.0);
            std::cout << std::setw(8) << elem->id
                      << std::setw(14) << std::fixed << std::setprecision(1) << (elem->tension_effective / 1000.0)
                      << std::setw(16) << std::setprecision(2) << sc.bending_moment_kNm
                      << std::setw(16) << std::scientific << std::setprecision(3) << sc.curvature << std::defaultfloat
                      << std::setw(16) << std::fixed << std::setprecision(1) << sc.von_mises_MPa
                      << std::setw(14) << std::setprecision(2) << sc.mbr_safety_factor << std::endl;
        }
    }

    for (auto* elem : elements) delete elem;
    for (auto* node : nodes) delete node;

    return (success_catenary && success_offset) ? 0 : 1;
}
