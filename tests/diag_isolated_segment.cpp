/**
 * @file diag_isolated_segment.cpp
 * @brief Diagnostic tool (not an automated test): isolates a small piece of the REAL Exemplo_01a mesh to check whether the divergence seen in the full 500-element model also shows up in a small slice, or only emerges once many elements are coupled together.
 *
 * Usage:
 * @code
 *   risersim_diag_isolated_segment <input.json> <start_elem_id> <num_elements> [artif_mode] [fix_mode] [load_steps] [max_iter] [seabed_mode] [current_mode] [enable_unbalanced] [enable_step_limiting] [max_translation_step_m] [max_rotation_step_rad]
 * @endcode
 *     artif_mode:   0=OnlyFirstStep (default), 1=EveryStep, 2=Never
 *     fix_mode:     0=clamped-clamped at both ends (default, original condition)
 *                   1=only the first end fixed, rest free (a chain hanging from one point,
 *                     without the artificial "taut chord" boundary condition at the other end)
 *     load_steps:   number of load steps (default 11)
 *     max_iter:     max NR iterations per step (default 40)
 *     seabed_mode:  0=seabed pushed far away/no contact possible (default), 1=REAL seabed
 *                   (same parameters as the full model, z~0/k=800000/mu=0.95)
 *     current_mode: 0=no current (default), 1=REAL current (v=1.78 m/s, heading=270)
 *     enable_unbalanced: 0=off (default), 1=enable StaticAnalysis::enable_unbalanced_criteria
 *                   (UnbalancedForces/UnbalancedMoments escape-hatch criterion, see
 *                   static_analysis.hpp) -- targets the near-touchdown limit-cycle chattering
 *                   pattern (see mapa_classes_anflex_estatica.md).
 *     enable_step_limiting: 0=off (default), 1=enable StaticAnalysis::enable_step_limiting
 *                   (caps each node's Newton correction to a physical displacement/rotation
 *                   size before the existing residual-based line search runs, see
 *                   static_analysis.hpp) -- targets the soil+current chattering/blow-up pattern.
 *     max_translation_step_m: cap in meters when enable_step_limiting=1 (default 0.5).
 *     max_rotation_step_rad: cap in radians when enable_step_limiting=1 (default 0.3).
 *
 * The two nodes at the ends of the slice are fixed at their real position (read from the
 * JSON); internal nodes are free (translation + rotation), matching the full model. The rest
 * of the physics/solver is identical to `risersim::Simulation::run()` (simulation.cpp).
 */
#include <iostream>
#include <fstream>
#include <map>
#include <vector>
#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>

#include "risersim/config.hpp"
#include "risersim/node.hpp"
#include "risersim/element_beam.hpp"
#include "risersim/model.hpp"
#include "risersim/static_analysis.hpp"
#include "risersim/current_profile.hpp"

using json = nlohmann::json;

int main(int argc, char** argv) {
    std::string json_path = argc > 1 ? argv[1] : "risersim_results/input_simulation.json";
    int start_elem_id = argc > 2 ? std::stoi(argv[2]) : 1;
    int num_elements = argc > 3 ? std::stoi(argv[3]) : 40;
    int end_elem_id = start_elem_id + num_elements - 1;
    int artif_mode_int = argc > 4 ? std::stoi(argv[4]) : 0;
    auto artif_mode = artif_mode_int == 1 ? risersim::ArtificialStiffnessMode::EveryStep
                     : artif_mode_int == 2 ? risersim::ArtificialStiffnessMode::Never
                                           : risersim::ArtificialStiffnessMode::OnlyFirstStep;
    int fix_mode = argc > 5 ? std::stoi(argv[5]) : 0;

    std::ifstream ifs(json_path);
    if (!ifs.is_open()) {
        std::cerr << "Could not open " << json_path << std::endl;
        return 2;
    }
    json j;
    ifs >> j;

    auto nodes_json = j["model"]["nodes"];
    auto elems_json = j["model"]["elements"];

    std::map<int, std::vector<double>> coords_by_id;
    for (auto& n : nodes_json) coords_by_id[n["id"].get<int>()] = n["coords"].get<std::vector<double>>();

    auto* model = new risersim::RiserModel();
    std::map<int, risersim::Node3D*> node_by_id;
    std::vector<int> node_order; // creation order == order along the line

    for (auto& e_j : elems_json) {
        int eid = e_j["id"].get<int>();
        if (eid < start_elem_id || eid > end_elem_id) continue;

        int n1_id = e_j["node1_id"].get<int>();
        int n2_id = e_j["node2_id"].get<int>();

        if (!node_by_id.count(n1_id)) {
            auto& c = coords_by_id[n1_id];
            auto* nn = model->add_node(n1_id, c[0], c[1], c[2]);
            node_by_id[n1_id] = nn;
            node_order.push_back(n1_id);
        }
        if (!node_by_id.count(n2_id)) {
            auto& c = coords_by_id[n2_id];
            auto* nn = model->add_node(n2_id, c[0], c[1], c[2]);
            node_by_id[n2_id] = nn;
            node_order.push_back(n2_id);
        }

        auto sp = e_j["section_properties"];
        risersim::BeamMaterialProps props;
        props.E = sp.value("E", props.E);
        props.G = sp.value("G", props.G);
        props.A = sp.value("A", props.A);
        props.D_outer = sp.value("D_outer", props.D_outer);
        props.D_inner = sp.value("D_inner", props.D_inner);
        props.Ca = sp.value("Ca", props.Ca);

        double weight_wet_N = sp.value("weight_wet_kNm", 0.4395) * 1000.0;
        props.rho = (weight_wet_N / 9.81) / (props.A > 0.0 ? props.A : 0.0282);
        props.rho_fluid = 0.0;

        double EI = sp.value("EI", 21700.0);
        double I_eff = EI / props.E;
        props.IY = I_eff;
        props.IZ = I_eff;
        double I_geom = std::numbers::pi * (std::pow(props.D_outer, 4) - std::pow(props.D_inner, 4)) / 64.0;
        props.J = 2.0 * I_geom;

        Eigen::Vector3d c1(coords_by_id[n1_id][0], coords_by_id[n1_id][1], coords_by_id[n1_id][2]);
        Eigen::Vector3d c2(coords_by_id[n2_id][0], coords_by_id[n2_id][1], coords_by_id[n2_id][2]);
        double L_unstretched = (c2 - c1).norm();

        model->add_beam_element(eid, node_by_id[n1_id], node_by_id[n2_id], props, L_unstretched);
    }

    if (model->nodes().size() < 2) {
        std::cerr << "No elements found in range [" << start_elem_id << ", " << end_elem_id << "]" << std::endl;
        delete model;
        return 2;
    }

    // fix_mode=0: fixes both end nodes of the slice at their real position (read from JSON) --
    //   a clamped-clamped condition, artificial (the "far" end isn't a real boundary of the
    //   full model, and the chord ends up with zero slack at all -- see the discussion above).
    // fix_mode=1: fixes only the first end; the rest of the chain, including the last end,
    //   stays free -- a chain hanging from a single point, with no artificial BC at the other end.
    risersim::Node3D* first_node = node_by_id[node_order.front()];
    risersim::Node3D* last_node = node_by_id[node_order.back()];
    first_node->eq_numbers = std::vector<int>(6, -1);
    if (fix_mode == 0) last_node->eq_numbers = std::vector<int>(6, -1);
    for (const auto& node_ptr : model->nodes()) {
        risersim::Node3D* node = node_ptr.get();
        if (node != first_node && (fix_mode == 0 ? node != last_node : true)) {
            node->eq_numbers = {0, 1, 2, 3, 4, 5};
        }
    }

    std::cout << "=========================================================" << std::endl;
    std::cout << "  Diagnostic: isolated segment from the real mesh (Exemplo_01a)" << std::endl;
    std::cout << "=========================================================" << std::endl;
    std::cout << "Elements " << start_elem_id << ".." << end_elem_id
              << " (" << model->elements().size() << " elements, " << model->nodes().size() << " nodes)" << std::endl;
    std::cout << "End node 1 (id " << first_node->id << "): "
              << first_node->coords.x() << ", " << first_node->coords.y() << ", " << first_node->coords.z() << std::endl;
    std::cout << "End node 2 (id " << last_node->id << "): "
              << last_node->coords.x() << ", " << last_node->coords.y() << ", " << last_node->coords.z() << std::endl;
    double L_total = 0.0;
    for (const auto& e : model->elements()) {
        if (auto* beam = dynamic_cast<risersim::CorotationalBeam3D*>(e.get())) L_total += beam->initial_length();
    }
    std::cout << "Total segment length: " << L_total << " m" << std::endl;

    risersim::StaticAnalysis sa;
    sa.model = model;
    // Same as Simulation::run() when the model comes from JSON: submerged weight is
    // already embedded in props.rho, so water_density (for buoyancy) stays 0.
    sa.water_density = 0.0;
    sa.water_density_for_mass = 1025.0;
    int seabed_mode = argc > 8 ? std::stoi(argv[8]) : 0;
    if (seabed_mode == 1) {
        // REAL seabed, same parameters as the full model (Exemplo_01a_A1.xml,
        // <Solo> section): vertical_stiffness=800 kN/m=800000 N/m,
        // axial_friction=0.92, axial_elastic_deflection_limit=0.03m,
        // lateral_friction=0.95, lateral_elastic_deflection_limit=0.279m --
        // axial and lateral are quite different from each other, and from the
        // isotropic default (0.05m) used previously.
        sa.seabed = risersim::SeabedInteraction(-1.52608e-05, 800000.0, 0.95, 0.279);
        sa.seabed.set_axial_friction(0.92);
        sa.seabed.set_axial_elastic_deflection_limit(0.03);
        sa.seabed.set_lateral_friction(0.95);
        sa.seabed.set_lateral_elastic_deflection_limit(0.279);
        std::cout << "REAL seabed enabled (z=" << -1.52608e-05
                  << ", k=800000, axial mu/u=0.92/0.03, lateral mu/u=0.95/0.279)" << std::endl;
    } else {
        // Seabed pushed far away -- no contact possible, isolates the element from
        // any contact effect.
        sa.seabed = risersim::SeabedInteraction(-1.0e6, 0.0, 0.0);
    }
    sa.load_steps = argc > 6 ? std::stoi(argv[6]) : 11;
    sa.max_iter_per_step = argc > 7 ? std::stoi(argv[7]) : 40;
    int current_mode = argc > 9 ? std::stoi(argv[9]) : 0;
    if (current_mode == 1) {
        // REAL current from Exemplo_01a (environmental.current in the full JSON):
        // v=1.78 m/s, heading=270 degrees, power-law profile (alpha=0.1428),
        // exactly as Simulation::run() configures it from JSON.
        sa.enable_current = true;
        sa.current = risersim::CurrentProfile(1.78, -1.52608e-05, 270.0, 0.1428, 1.0);
        std::cout << "REAL current enabled (v=1.78 m/s, heading=270)" << std::endl;
    }
    sa.tol = 0.001;

    if (argc > 10 && std::stoi(argv[10]) == 1) {
        sa.enable_unbalanced_criteria = true;
        std::cout << "Unbalanced force/moment escape-hatch criterion enabled (tol="
                  << sa.unbalanced_force_tol << " N / " << sa.unbalanced_moment_tol << " N.m)" << std::endl;
    }

    if (argc > 11 && std::stoi(argv[11]) == 1) {
        sa.enable_step_limiting = true;
        sa.max_translation_step_m = argc > 12 ? std::stod(argv[12]) : sa.max_translation_step_m;
        sa.max_rotation_step_rad = argc > 13 ? std::stod(argv[13]) : sa.max_rotation_step_rad;
        std::cout << "Physical step limiting enabled (max_translation_step_m="
                  << sa.max_translation_step_m << ", max_rotation_step_rad="
                  << sa.max_rotation_step_rad << ")" << std::endl;
    }

    bool converged = sa.solve_catenary_static(sa.load_steps, sa.max_iter_per_step, sa.tol, artif_mode);

    std::cout << (converged ? "\n>>> CONVERGED" : "\n>>> DID NOT CONVERGE")
              << " (artif_mode=" << artif_mode_int << ")" << std::endl;

    delete model;
    return converged ? 0 : 1;
}
