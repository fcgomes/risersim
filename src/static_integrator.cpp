#include "risersim/static_integrator.hpp"
#include <cmath>
#include <vector>

namespace risersim {

Eigen::VectorXd StaticIntegrator::assemble_load_vector(double load_factor) const {
    Eigen::VectorXd F_ext = Eigen::VectorXd::Zero(analysis->num_dofs);
    auto* model = analysis->model;
    if (!model) return F_ext;

    for (auto* elem : model->elements) {
        double L = elem->initial_length;
        double g = 9.81;

        double w_dry = (elem->props.rho * elem->props.A + elem->props.rho_fluid * elem->inner_area()) * g;
        double w_buoyancy = analysis->water_density * elem->outer_area() * g;
        double elem_weight_total = (w_dry - w_buoyancy) * L * load_factor;

        int eq1_z = elem->node1->eq_numbers[2];
        int eq2_z = elem->node2->eq_numbers[2];

        if (eq1_z >= 0) F_ext[eq1_z] -= elem_weight_total * 0.5;
        if (eq2_z >= 0) F_ext[eq2_z] -= elem_weight_total * 0.5;

        if (analysis->enable_current) {
            double avg_z = 0.5 * (elem->node1->current_coords().z() + elem->node2->current_coords().z());
            double f_drag_x = 0.0, f_drag_y = 0.0;
            analysis->current.get_drag_force_per_meter(avg_z, elem->props.D_outer, analysis->water_density_for_mass, f_drag_x, f_drag_y);

            int eq1_x = elem->node1->eq_numbers[0]; int eq2_x = elem->node2->eq_numbers[0];
            int eq1_y = elem->node1->eq_numbers[1]; int eq2_y = elem->node2->eq_numbers[1];

            if (eq1_x >= 0) F_ext[eq1_x] += f_drag_x * L * 0.5 * load_factor;
            if (eq2_x >= 0) F_ext[eq2_x] += f_drag_x * L * 0.5 * load_factor;
            if (eq1_y >= 0) F_ext[eq1_y] += f_drag_y * L * 0.5 * load_factor;
            if (eq2_y >= 0) F_ext[eq2_y] += f_drag_y * L * 0.5 * load_factor;
        }
    }
    return F_ext;
}

void StaticIntegrator::assemble_stiffness_and_internal_forces(int iter, Eigen::SparseMatrix<double>& K_global, Eigen::VectorXd& F_int) {
    auto* model = analysis->model;

    // assemble_system ignora o parametro F_ext (peso/atrito ja entram direto
    // na formulacao de forca interna do elemento/solo) -- preservado aqui.
    Eigen::VectorXd F_ext_unused;
    analysis->assemble_system(K_global, F_ext_unused, F_int);

    if (!artificial_stiffness_enabled || !model || model->elements.empty()) return;

    // Rigidez artificial (regularizacao de Tikhonov), igual a tecnica do
    // ANFLEX real (beam.cpp:calc_artificial_stiffness / static_integrator.cpp).
    double avg_EA_L = 0.0;
    for (auto* elem : model->elements) {
        double L = elem->current_length();
        if (L > 1.0e-9) avg_EA_L += (elem->props.E * elem->props.A) / L;
    }
    avg_EA_L /= static_cast<double>(model->elements.size());

    // NOTA: testado com constante 5.0 (em vez de 1.25) como forma de manter a
    // rigidez artificial relevante por mais iteracoes -- isso de fato evita a
    // explosao do residuo em cadeias muito longas (~300+ elementos), mas tem
    // contrapartida: cadeias curtas convergem mais devagar (mais iteracoes
    // necessarias) dentro do mesmo orcamento, uma regressao real. Revertido
    // para 1.25 (comportamento original) ate um esquema adaptativo (ex.:
    // escalar com o numero de elementos/DOFs da cadeia) ser projetado.
    // Ver mapa_classes_anflex_estatica.md, secao sobre o limiar ~300 elementos.
    // NOTA: testado com constante 5.0 (em vez de 1.25) como forma de manter a
    // rigidez artificial relevante por mais iteracoes -- isso de fato evita a
    // explosao do residuo em cadeias muito longas (~300+ elementos) e foi
    // usado para isolar a causa real da divergencia do modelo completo (ver
    // secao "Causa real isolada" em mapa_classes_anflex_estatica.md: a
    // combinacao solo+corrente, nao o comprimento da cadeia). Tem contrapartida
    // real: cadeias curtas convergem mais devagar dentro do mesmo orcamento de
    // iteracoes. Revertido para 1.25 (comportamento original) ate um esquema
    // adaptativo (ex.: escalar com o numero de elementos/DOFs) ser projetado.
    double decay = std::exp(-static_cast<double>(iter) / 1.25);
    double k_transversal = avg_EA_L * decay;
    double k_rotational = k_transversal * 0.05;

    std::vector<Eigen::Triplet<double>> artif_triplets;
    for (auto* node : model->nodes) {
        for (int i = 0; i < 3; ++i) {
            int eq = node->eq_numbers[i];
            if (eq >= 0) artif_triplets.push_back(Eigen::Triplet<double>(eq, eq, k_transversal));

            int eq_rot = node->eq_numbers[i + 3];
            if (eq_rot >= 0) artif_triplets.push_back(Eigen::Triplet<double>(eq_rot, eq_rot, k_rotational));
        }
    }
    Eigen::SparseMatrix<double> K_artificial(analysis->num_dofs, analysis->num_dofs);
    K_artificial.setFromTriplets(artif_triplets.begin(), artif_triplets.end());
    K_global += K_artificial;
}

} // namespace risersim
