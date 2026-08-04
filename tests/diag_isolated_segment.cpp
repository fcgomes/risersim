// Diagnostico (nao e um teste automatizado): isola um pedaco pequeno da
// malha REAL do Exemplo_01a (mesmo material/espacamento/orientacao) para
// descobrir se a divergencia observada no modelo completo de 500 elementos
// tambem aparece numa fatia pequena, ou se e um efeito que só emerge com
// muitos elementos acoplados.
//
// Uso:
//   risersim_diag_isolated_segment <input.json> <id_elemento_inicial> <num_elementos> [artif_mode] [fix_mode] [load_steps] [max_iter] [seabed_mode]
//   artif_mode:  0=OnlyFirstStep (default), 1=EveryStep, 2=Never
//   fix_mode:    0=engastado-engastado nas duas pontas (default, condicao original)
//                1=so a primeira ponta fixa, resto livre (corrente pendurada de 1 ponto,
//                  sem a condicao de contorno artificial "corda sem folga" na outra ponta)
//   load_steps:  numero de passos de carga (default 11)
//   max_iter:    iteracoes NR maximas por passo (default 40)
//   seabed_mode: 0=solo empurrado para longe/sem contato (default), 1=solo REAL
//                (mesmos parametros do modelo completo, z~0/k=800000/mu=0.95)
//   current_mode: 0=sem corrente (default), 1=corrente REAL (v=1.78 m/s, heading=270)
//
// Os dois nos nas pontas da fatia ficam fixos na posicao real (lida do
// JSON); os nos internos ficam livres (translacao + rotacao), igual ao
// modelo completo. O restante da fisica/solver e identico ao main_test.cpp.
#include <iostream>
#include <fstream>
#include <map>
#include <vector>
#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>

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
        std::cerr << "Nao consegui abrir " << json_path << std::endl;
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
    std::vector<int> node_order; // ordem de criacao == ordem ao longo da linha

    for (auto& e_j : elems_json) {
        int eid = e_j["id"].get<int>();
        if (eid < start_elem_id || eid > end_elem_id) continue;

        int n1_id = e_j["node1_id"].get<int>();
        int n2_id = e_j["node2_id"].get<int>();

        if (!node_by_id.count(n1_id)) {
            auto& c = coords_by_id[n1_id];
            auto* nn = new risersim::Node3D(n1_id, c[0], c[1], c[2]);
            node_by_id[n1_id] = nn;
            model->nodes.push_back(nn);
            node_order.push_back(n1_id);
        }
        if (!node_by_id.count(n2_id)) {
            auto& c = coords_by_id[n2_id];
            auto* nn = new risersim::Node3D(n2_id, c[0], c[1], c[2]);
            node_by_id[n2_id] = nn;
            model->nodes.push_back(nn);
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
        double I_geom = M_PI * (std::pow(props.D_outer, 4) - std::pow(props.D_inner, 4)) / 64.0;
        props.J = 2.0 * I_geom;

        Eigen::Vector3d c1(coords_by_id[n1_id][0], coords_by_id[n1_id][1], coords_by_id[n1_id][2]);
        Eigen::Vector3d c2(coords_by_id[n2_id][0], coords_by_id[n2_id][1], coords_by_id[n2_id][2]);
        double L_unstretched = (c2 - c1).norm();

        auto* elem = new risersim::CorotationalBeam3D(eid, node_by_id[n1_id], node_by_id[n2_id], props, L_unstretched);
        model->elements.push_back(elem);
    }

    if (model->nodes.size() < 2) {
        std::cerr << "Nenhum elemento encontrado no intervalo [" << start_elem_id << ", " << end_elem_id << "]" << std::endl;
        delete model;
        return 2;
    }

    // fix_mode=0: fixa os dois nos das pontas da fatia na posicao real (lida do JSON) --
    //   condicao engastado-engastado, artificial (a ponta "de longe" nao e um contorno
    //   real do modelo completo, e a corda fica sem folga alguma -- ver discussao).
    // fix_mode=1: fixa so a primeira ponta; o resto da cadeia, inclusive a ultima ponta,
    //   fica livre -- corrente pendurada de um unico ponto, sem BC artificial na outra ponta.
    risersim::Node3D* first_node = node_by_id[node_order.front()];
    risersim::Node3D* last_node = node_by_id[node_order.back()];
    first_node->eq_numbers = std::vector<int>(6, -1);
    if (fix_mode == 0) last_node->eq_numbers = std::vector<int>(6, -1);
    for (auto* node : model->nodes) {
        if (node != first_node && (fix_mode == 0 ? node != last_node : true)) {
            node->eq_numbers = {0, 1, 2, 3, 4, 5};
        }
    }

    std::cout << "=========================================================" << std::endl;
    std::cout << "  Diagnostico: segmento isolado da malha real (Exemplo_01a)" << std::endl;
    std::cout << "=========================================================" << std::endl;
    std::cout << "Elementos " << start_elem_id << ".." << end_elem_id
              << " (" << model->elements.size() << " elementos, " << model->nodes.size() << " nos)" << std::endl;
    std::cout << "No extremo 1 (id " << first_node->id << "): "
              << first_node->coords.x() << ", " << first_node->coords.y() << ", " << first_node->coords.z() << std::endl;
    std::cout << "No extremo 2 (id " << last_node->id << "): "
              << last_node->coords.x() << ", " << last_node->coords.y() << ", " << last_node->coords.z() << std::endl;
    double L_total = 0.0;
    for (auto* e : model->elements) L_total += e->initial_length;
    std::cout << "Comprimento total do segmento: " << L_total << " m" << std::endl;

    risersim::StaticAnalysis sa;
    sa.model = model;
    // Igual ao main_test.cpp quando o modelo vem de JSON: o peso submerso ja
    // esta embutido em props.rho, entao water_density (para empuxo) fica 0.
    sa.water_density = 0.0;
    sa.water_density_for_mass = 1025.0;
    int seabed_mode = argc > 8 ? std::stoi(argv[8]) : 0;
    if (seabed_mode == 1) {
        // Solo REAL, mesmos parametros do modelo completo (Exemplo_01a_A1.xml,
        // no <Solo>): vertical_stiffness=800 kN/m=800000 N/m,
        // axial_friction=0.92, axial_elastic_deflection_limit=0.03m,
        // lateral_friction=0.95, lateral_elastic_deflection_limit=0.279m --
        // axial e lateral bem diferentes entre si, e diferentes do default
        // isotropico (0.05m) usado antes.
        sa.seabed = risersim::SeabedInteraction(-1.52608e-05, 800000.0, 0.95, 0.279);
        sa.seabed.axial_friction = 0.92;
        sa.seabed.axial_elastic_deflection_limit = 0.03;
        sa.seabed.lateral_friction = 0.95;
        sa.seabed.lateral_elastic_deflection_limit = 0.279;
        std::cout << "Solo REAL habilitado (z=" << -1.52608e-05
                  << ", k=800000, axial mu/u=0.92/0.03, lateral mu/u=0.95/0.279)" << std::endl;
    } else {
        // Solo bem longe -- sem contato possivel, isola o elemento de qualquer
        // efeito de contato.
        sa.seabed = risersim::SeabedInteraction(-1.0e6, 0.0, 0.0);
    }
    sa.load_steps = argc > 6 ? std::stoi(argv[6]) : 11;
    sa.max_iter_per_step = argc > 7 ? std::stoi(argv[7]) : 40;
    int current_mode = argc > 9 ? std::stoi(argv[9]) : 0;
    if (current_mode == 1) {
        // Corrente REAL do Exemplo_01a (environmental.current no JSON completo):
        // v=1.78 m/s, heading=270 graus, perfil de lei de potencia (alpha=0.1428),
        // exatamente como main_test.cpp configura a partir do JSON.
        sa.enable_current = true;
        sa.current = risersim::CurrentProfile(1.78, -1.52608e-05, 270.0, 0.1428, 1.0);
        std::cout << "Corrente REAL habilitada (v=1.78 m/s, heading=270)" << std::endl;
    }
    sa.tol = 0.001;

    bool converged = sa.solve_catenary_static(sa.load_steps, sa.max_iter_per_step, sa.tol, artif_mode);

    std::cout << (converged ? "\n>>> CONVERGIU" : "\n>>> NAO CONVERGIU")
              << " (artif_mode=" << artif_mode_int << ")" << std::endl;

    delete model;
    return converged ? 0 : 1;
}
