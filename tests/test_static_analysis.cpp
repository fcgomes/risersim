// Testes de caracterizacao (regressao) para StaticAnalysis::solve().
//
// Objetivo: fixar o comportamento ATUAL antes do refactor de arquitetura
// descrito em risersim/docs/mapa_classes_anflex_estatica.md, para que cada
// passo do roadmap (Integrator, duas fases assembly/static, etc.) possa
// ser validado contra "isso nao mudou por acidente".
//
// Nota: a geometria sintetica abaixo trava as rotacoes de todos os nos
// intermediarios (eq_numbers = {0,1,2,-1,-1,-1} em main_test.cpp), entao
// este caso nao exercita flexao/rotacao do elemento corrotacional -- e um
// teste de regressao do pipeline (montagem/passos de carga/convergencia),
// nao uma validacao da formulacao de viga em si.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

#include "risersim/model.hpp"
#include "risersim/node.hpp"
#include "risersim/element_beam.hpp"
#include "risersim/static_analysis.hpp"

namespace {

// Reproduz exatamente a geometria parabolica de fallback de
// risersim/src/main_test.cpp (usada quando nenhum JSON de entrada e
// fornecido), para manter os dois em sincronia.
risersim::RiserModel* build_synthetic_catenary_model() {
    constexpr int num_elements = 40;
    constexpr double total_length = 180.0;
    constexpr double total_depth_z = -100.0;

    auto* model = new risersim::RiserModel();

    const int num_nodes = num_elements + 1;
    const double h_water = std::abs(total_depth_z);
    const double L_total = total_length;

    const double S_susp = std::min(L_total * 0.70, 310.0);
    const double X_tdp = std::sqrt(std::max(1.0, S_susp * S_susp - h_water * h_water));

    for (int i = 0; i < num_nodes; ++i) {
        const double s = (static_cast<double>(i) / static_cast<double>(num_elements)) * L_total;
        double x = 0.0;
        double z = 0.0;
        if (s <= S_susp) {
            const double ratio = s / S_susp;
            x = ratio * X_tdp;
            z = -h_water * (2.0 * ratio - ratio * ratio);
        } else {
            const double s_seabed = s - S_susp;
            x = X_tdp + s_seabed;
            z = -h_water;
        }
        model->nodes.push_back(new risersim::Node3D(i + 1, x, 0.0, z));
    }

    model->nodes.front()->eq_numbers = std::vector<int>(6, -1);
    model->nodes.back()->eq_numbers = std::vector<int>(6, -1);
    for (size_t i = 1; i < model->nodes.size() - 1; ++i) {
        model->nodes[i]->eq_numbers = {0, 1, 2, -1, -1, -1};
    }

    risersim::BeamMaterialProps props;
    const double L_unstretched = total_length / static_cast<double>(num_elements);
    for (int i = 0; i < num_elements; ++i) {
        auto* elem = new risersim::CorotationalBeam3D(i + 1, model->nodes[i], model->nodes[i + 1], props, L_unstretched);
        model->elements.push_back(elem);
    }

    return model;
}

} // namespace

TEST_CASE("StaticAnalysis converges on the synthetic fallback catenary", "[static_analysis][characterization]") {
    auto* model = build_synthetic_catenary_model();

    risersim::StaticAnalysis static_analysis;
    static_analysis.model = model;
    static_analysis.water_density = 1025.0;
    static_analysis.water_density_for_mass = 1025.0;
    static_analysis.seabed = risersim::SeabedInteraction(-100.0, 1.0e5, 0.5);
    static_analysis.load_steps = 20;
    static_analysis.max_iter_per_step = 300;
    static_analysis.tol = 100.0;

    const bool converged = static_analysis.solve();

    REQUIRE(converged);

    // Valor de referencia ATUALIZADO apos a correcao da causa raiz da
    // divergencia (mapa_classes_anflex_estatica.md, secao "Causa raiz
    // encontrada e corrigida"): a forca interna passou a usar a rotacao
    // LOCAL/deformacional de cada no (relativa ao ghost frame do elemento)
    // em vez da rotacao total acumulada. Neste caso sintetico (DOFs de
    // rotacao travados em 0 em todos os nos), isso muda o comportamento de
    // forma intencional e correta: antes, rotacao=0 sempre => rigidez de
    // flexao nunca contribuia (equivalente a um cabo sem flexao); agora, o
    // "ghost frame" acompanha a corda atual enquanto o no fica congelado na
    // orientacao inicial, entao a flexao real passa a resistir a deformacao
    // -- daí a tensao de equilibrio bem menor. Valor ATUALIZADO DE NOVO apos
    // trocar a mola de atrito do solo por uma versao elastico-plastica
    // incremental (com estado persistente por no), fiel ao ANFLEX real
    // (soil_uncoupled.cpp) -- a versao anterior usava o deslocamento TOTAL
    // acumulado em vez do incremento por iteracao, o que satura a forca de
    // atrito de forma fisicamente incorreta. Este caso sintetico tem solo
    // habilitado (seabed com k=1e5, mu=0.5), entao a mudanca de modelo de
    // atrito afeta o resultado. Valor ATUALIZADO DE NOVO apos decompor o
    // atrito nas direcoes axial/lateral LOCAIS da linha (em vez de X/Y
    // globais), fiel ao ANFLEX real (soil.cpp:calc_transf_matrix). Valor
    // ATUALIZADO DE NOVO apos adicionar um line search de backtracking ao
    // Newton-Raphson (apply_newton_step_with_line_search em
    // static_analysis.cpp) -- este caso tem solo habilitado, entao a
    // trajetoria de iteracoes muda quando o passo cheio eventualmente diverge
    // muito do residuo anterior. T_eff capturado via Docker apos a correcao:
    // 3539.18588 kN no elemento do topo.
    const double expected_tension_effective_N = 3539.1858815034814 * 1000.0;
    REQUIRE(model->elements.front()->tension_effective == Catch::Approx(expected_tension_effective_N).epsilon(0.005));

    delete model;
}
