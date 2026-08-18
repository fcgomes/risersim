/**
 * @file test_multiline.cpp
 * @brief Regression test for multi-line support (docs/roadmap.md backlog: "múltiplas linhas com
 * corpo flutuante compartilhado", driven by exemplos/Multilinhas/Multilinhas.aml).
 *
 * Builds a model with TWO fully disconnected catenary chains directly via RiserModel::add_node/
 * add_element (no JSON/AML involved -- that's Phase 2/3), wires them together only through
 * RiserModel::references/connections/lines (see model.hpp), and checks that:
 *  1. Solving both together gives each line the SAME result as solving it alone (proves no
 *     cross-talk in equation numbering/assembly/prescribed motion between disconnected lines).
 *  2. A model with no `lines` populated at all (every existing single-line model) keeps behaving
 *     exactly as before -- resolve_line_attachments() falls back to nodes.front(), unchanged.
 *  3. DynamicAnalysis applies its per-step prescribed target independently to each line's own top
 *     node (not just line 1).
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>

#include "risersim/model.hpp"
#include "risersim/node.hpp"
#include "risersim/element_beam.hpp"
#include "risersim/static_analysis.hpp"
#include "risersim/dynamic_analysis.hpp"
#include "risersim/model_builder.hpp"

namespace {

constexpr int kNumElements = 40;
constexpr double kTotalLength = 180.0;
constexpr double kTotalDepthZ = -100.0;

/**
 * @brief Same synthetic parabolic catenary geometry as test_static_analysis.cpp's
 * build_synthetic_catenary_model(), parametrized by an id/Y offset so two (or more) fully
 * disconnected instances can be added into the same RiserModel. Both ends start fixed
 * (eq_numbers all -1) -- solve_vessel_offset() is what temporarily frees each chain's own top
 * node (s=0 end) via its line's top_connection_id.
 * @return The id of this chain's top node (s=0 end) -- what a ConnectionInfo::node_id should point at.
 */
int add_catenary_chain(risersim::RiserModel& model, int id_offset, double y_origin) {
    const int num_nodes = kNumElements + 1;
    const double h_water = std::abs(kTotalDepthZ);
    const double S_susp = std::min(kTotalLength * 0.70, 310.0);
    const double X_tdp = std::sqrt(std::max(1.0, S_susp * S_susp - h_water * h_water));

    for (int i = 0; i < num_nodes; ++i) {
        const double s = (static_cast<double>(i) / static_cast<double>(kNumElements)) * kTotalLength;
        double x = 0.0, z = 0.0;
        if (s <= S_susp) {
            const double ratio = s / S_susp;
            x = ratio * X_tdp;
            z = -h_water * (2.0 * ratio - ratio * ratio);
        } else {
            const double s_seabed = s - S_susp;
            x = X_tdp + s_seabed;
            z = -h_water;
        }
        model.add_node(id_offset + i + 1, x, y_origin, z);
    }

    const size_t base = model.nodes().size() - num_nodes;
    model.nodes()[base]->eq_numbers = std::vector<int>(6, -1);
    model.nodes()[base + num_nodes - 1]->eq_numbers = std::vector<int>(6, -1);
    for (size_t i = 1; i < static_cast<size_t>(num_nodes) - 1; ++i) {
        model.nodes()[base + i]->eq_numbers = {0, 1, 2, -1, -1, -1};
    }

    risersim::BeamMaterialProps props;
    const double L_unstretched = kTotalLength / static_cast<double>(kNumElements);
    for (int i = 0; i < kNumElements; ++i) {
        model.add_element(id_offset + i + 1, model.nodes()[base + i].get(), model.nodes()[base + i + 1].get(), props, L_unstretched);
    }

    return model.nodes()[base]->id;
}

/**
 * @brief Same catenary math as add_catenary_chain(), but emits `model.nodes()`/`elements` JSON
 * objects (appended to `nodes_out`/`elements_out`) instead of building a RiserModel directly --
 * used by the ModelBuilder::load_from_json() fixture tests below (Fase 2: proving the JSON
 * parsing path, not just direct C++ construction). Small (10 elements, not kNumElements=40) since
 * these tests only need to prove parsing + basic convergence, not re-litigate Fase 1's
 * solved-alone-vs-combined physics comparison.
 * @return {"top_id": ..., "anchor_id": ...} for wiring boundary_conditions/connections.
 */
nlohmann::json build_line_json(int id_offset, double y_origin, std::vector<nlohmann::json>& nodes_out,
                                std::vector<nlohmann::json>& elements_out) {
    constexpr int num_elements = 10;
    constexpr double total_length = 80.0;
    constexpr double total_depth_z = -100.0;
    const int num_nodes = num_elements + 1;
    const double h_water = std::abs(total_depth_z);
    const double S_susp = std::min(total_length * 0.70, 310.0);
    const double X_tdp = std::sqrt(std::max(1.0, S_susp * S_susp - h_water * h_water));

    std::vector<int> node_ids;
    for (int i = 0; i < num_nodes; ++i) {
        const double s = (static_cast<double>(i) / static_cast<double>(num_elements)) * total_length;
        double x = 0.0, z = 0.0;
        if (s <= S_susp) {
            const double ratio = s / S_susp;
            x = ratio * X_tdp;
            z = -h_water * (2.0 * ratio - ratio * ratio);
        } else {
            const double s_seabed = s - S_susp;
            x = X_tdp + s_seabed;
            z = -h_water;
        }
        const int id = id_offset + i + 1;
        node_ids.push_back(id);
        nodes_out.push_back({{"id", id}, {"coords", {x, y_origin, z}}});
    }
    for (int i = 0; i < num_elements; ++i) {
        elements_out.push_back({{"id", id_offset + i + 1}, {"node1_id", node_ids[i]}, {"node2_id", node_ids[i + 1]}});
    }
    return {{"top_id", node_ids.front()}, {"anchor_id", node_ids.back()}};
}

/** @brief Writes `doc` to a temp file, calls ModelBuilder::load_from_json() on it, deletes the
 * file, and returns the loaded model (nullptr on failure -- caller REQUIREs). */
risersim::RiserModel* load_json_fixture(risersim::ModelBuilder& builder, const nlohmann::json& doc, const char* path) {
    { std::ofstream f(path); f << doc.dump(2); }
    bool ok = builder.load_from_json(path);
    std::remove(path);
    return ok ? builder.get_model() : nullptr;
}

struct SolveResult {
    bool converged;
    Eigen::Vector3d top_disp;
    double top_tension_N;
};

SolveResult solve_static_offset(risersim::RiserModel& model) {
    risersim::StaticAnalysis sa;
    sa.model = &model;
    sa.water_density = 1025.0;
    sa.water_density_for_mass = 1025.0;
    sa.seabed = risersim::SeabedInteraction(-100.0, 1.0e5, 0.5);
    sa.load_steps = 20;
    sa.max_iter_per_step = 300;
    sa.tol = 0.01;
    sa.enable_offset = true;
    sa.offset = risersim::VesselOffset(risersim::OffsetMode::Far, 5.0);

    SolveResult r;
    r.converged = sa.solve();
    r.top_disp = model.nodes().front()->disp; // válido pra qualquer chain isolada (id_offset=0) OU multi-linha (ver abaixo)
    r.top_tension_N = model.elements().front()->tension_effective();
    return r;
}

} // namespace

TEST_CASE("Two disconnected lines converge independently, matching each solved alone", "[static_analysis][multiline]") {
    // --- Modelo combinado: duas linhas, mesma geometria, espacialmente separadas (Y=0 e Y=50). ---
    risersim::RiserModel combined;
    int topA_id = add_catenary_chain(combined, 0, 0.0);
    int topB_id = add_catenary_chain(combined, 1000, 50.0);

    combined.references().push_back({1, risersim::ReferenceType::Global, "anchor", {}});
    combined.connections().push_back({1, 1, topA_id});
    combined.connections().push_back({2, 1, topB_id});
    combined.lines().push_back({1, "A", 1, -1});
    combined.lines().push_back({2, "B", 2, -1});

    risersim::StaticAnalysis sa;
    sa.model = &combined;
    sa.water_density = 1025.0;
    sa.water_density_for_mass = 1025.0;
    sa.seabed = risersim::SeabedInteraction(-100.0, 1.0e5, 0.5);
    sa.load_steps = 20;
    sa.max_iter_per_step = 300;
    sa.tol = 0.01;
    sa.enable_offset = true;
    sa.offset = risersim::VesselOffset(risersim::OffsetMode::Far, 5.0);

    REQUIRE(sa.solve());

    // Localiza os nós/elementos de topo de cada linha pelo id (não por posição -- o array tem
    // ambas as chains concatenadas).
    auto find_node = [&](int id) {
        auto it = std::find_if(combined.nodes().begin(), combined.nodes().end(), [&](const auto& n) { return n->id == id; });
        REQUIRE(it != combined.nodes().end());
        return it->get();
    };
    auto find_top_element = [&](risersim::Node3D* top_node) {
        auto it = std::find_if(combined.elements().begin(), combined.elements().end(),
            [&](const auto& e) { return e->node1() == top_node || e->node2() == top_node; });
        REQUIRE(it != combined.elements().end());
        return it->get();
    };

    risersim::Node3D* topA = find_node(topA_id);
    risersim::Node3D* topB = find_node(topB_id);
    const Eigen::Vector3d combined_dispA = topA->disp;
    const Eigen::Vector3d combined_dispB = topB->disp;
    const double combined_tensionA = find_top_element(topA)->tension_effective();
    const double combined_tensionB = find_top_element(topB)->tension_effective();

    // Ambas as linhas de fato se moveram em direção ao offset (mecanismo funcionou, não ficou parado).
    REQUIRE(combined_dispA.x() == Catch::Approx(5.0).epsilon(0.02));
    REQUIRE(combined_dispB.x() == Catch::Approx(5.0).epsilon(0.02));

    // --- Cada chain resolvida SOZINHA (modelo single-line, sem `lines` -- caminho de fallback
    // legado, resolve_line_attachments() cai em nodes.front()). Compara contra a mesma chain
    // dentro do modelo combinado -- devem bater, provando que a presença da outra linha não afeta
    // em nada a numeração de equação/montagem/prescribed motion desta. ---
    risersim::RiserModel solo;
    add_catenary_chain(solo, 0, 0.0); // mesma geometria da chain A, id_offset=0 -- nodes.front() é o topo
    SolveResult solo_result = solve_static_offset(solo);
    REQUIRE(solo_result.converged);

    REQUIRE(combined_dispA.x() == Catch::Approx(solo_result.top_disp.x()).epsilon(1e-6));
    REQUIRE(combined_dispA.y() == Catch::Approx(solo_result.top_disp.y()).margin(1e-9));
    REQUIRE(combined_dispA.z() == Catch::Approx(solo_result.top_disp.z()).epsilon(1e-6));
    REQUIRE(combined_tensionA == Catch::Approx(solo_result.top_tension_N).epsilon(1e-6));

    // Chain B tem a MESMA geometria da A (só deslocada em Y, o que não afeta a física 2D
    // efetiva de peso/empuxo/solo aqui) -- então também deve bater contra o mesmo solo_result.
    REQUIRE(combined_dispB.x() == Catch::Approx(solo_result.top_disp.x()).epsilon(1e-6));
    REQUIRE(combined_tensionB == Catch::Approx(solo_result.top_tension_N).epsilon(1e-6));
}

TEST_CASE("A model with no lines[] keeps the legacy single top-node behavior", "[static_analysis][multiline]") {
    risersim::RiserModel model;
    add_catenary_chain(model, 0, 0.0);
    REQUIRE(model.lines().empty());

    auto attachments = model.resolve_line_attachments();
    REQUIRE(attachments.size() == 1);
    CHECK(attachments.front().top_node == model.nodes().front().get());
    CHECK(attachments.front().vessel_motion == &model.environmental().vessel_motion);

    SolveResult r = solve_static_offset(model);
    REQUIRE(r.converged);
    REQUIRE(r.top_disp.x() == Catch::Approx(5.0).epsilon(0.02));
}

TEST_CASE("DynamicAnalysis prescribes motion independently per line", "[dynamic_analysis][multiline]") {
    risersim::RiserModel combined;
    int topA_id = add_catenary_chain(combined, 0, 0.0);
    int topB_id = add_catenary_chain(combined, 1000, 50.0);

    combined.references().push_back({1, risersim::ReferenceType::Global, "anchor", {}});
    combined.connections().push_back({1, 1, topA_id});
    combined.connections().push_back({2, 1, topB_id});
    combined.lines().push_back({1, "A", 1, -1});
    combined.lines().push_back({2, "B", 2, -1});

    risersim::StaticAnalysis sa;
    sa.model = &combined;
    sa.water_density = 1025.0;
    sa.water_density_for_mass = 1025.0;
    sa.seabed = risersim::SeabedInteraction(-100.0, 1.0e5, 0.5);
    sa.load_steps = 20;
    sa.max_iter_per_step = 300;
    sa.tol = 0.01;
    REQUIRE(sa.solve()); // só o assentamento em catenária -- sem offset, sem vessel motion real

    auto find_node = [&](int id) {
        auto it = std::find_if(combined.nodes().begin(), combined.nodes().end(), [&](const auto& n) { return n->id == id; });
        REQUIRE(it != combined.nodes().end());
        return it->get();
    };
    risersim::Node3D* topA = find_node(topA_id);
    risersim::Node3D* topB = find_node(topB_id);
    const double static_zA = topA->disp.z();
    const double static_zB = topB->disp.z();

    risersim::DynamicAnalysis da(sa);
    da.stop_on_first_non_convergence = true;
    // Sem environmental.vessel_motion.enabled (default false) -- exercita o fallback senoidal em
    // Z pra AMBAS as linhas independentemente (nenhuma tabela RAO real precisa existir pra este
    // teste; isso é coberto na Fase 3, contra o Multilinhas.aml real).
    REQUIRE(da.solve_time_domain_dynamic(/*duration=*/1.0, /*dt=*/0.1, /*amp=*/2.5, /*period=*/10.0, /*alpha=*/0.05, /*beta=*/0.01));

    // Ambos os nós de topo devem ter se afastado da posição estática (o fallback senoidal os move
    // em Z) -- prova que a mola de penalidade foi aplicada às DUAS linhas, não só à primeira.
    CHECK(std::abs(topA->disp.z() - static_zA) > 1e-3);
    CHECK(std::abs(topB->disp.z() - static_zB) > 1e-3);

    // O nó de topo de cada linha voltou ao contrato de GDL original (engastado) ao final --
    // mesma limpeza que o caminho single-line já fazia.
    CHECK(topA->eq_numbers == std::vector<int>(6, -1));
    CHECK(topB->eq_numbers == std::vector<int>(6, -1));
}

// --- Fase 2: ModelBuilder::load_from_json() -- o mesmo mecanismo acima, mas vindo de JSON real
// (references/connections/lines), não construído à mão em C++. ---

TEST_CASE("ModelBuilder parses a multi-line JSON fixture (references/connections/lines)", "[model_builder][multiline]") {
    std::vector<nlohmann::json> nodes, elements;
    auto lineA = build_line_json(0, 0.0, nodes, elements);
    auto lineB = build_line_json(100, 50.0, nodes, elements);

    nlohmann::json doc;
    doc["schema_version"] = 1;
    doc["model"]["nodes"] = nodes;
    doc["model"]["elements"] = elements;
    doc["boundary_conditions"]["prescribed_dofs"] = {
        {{"node_id", lineA["top_id"]}, {"dofs", {-1, -1, -1, -1, -1, -1}}},
        {{"node_id", lineB["top_id"]}, {"dofs", {-1, -1, -1, -1, -1, -1}}},
    };
    doc["boundary_conditions"]["restrained_dofs"] = {
        {{"node_id", lineA["anchor_id"]}, {"dofs", {-1, -1, -1, -1, -1, -1}}},
        {{"node_id", lineB["anchor_id"]}, {"dofs", {-1, -1, -1, -1, -1, -1}}},
    };
    doc["references"] = {{{"id", 1}, {"type", "GLOBAL"}}};
    doc["connections"] = {
        {{"id", 1}, {"reference_id", 1}, {"node_id", lineA["top_id"]}},
        {{"id", 2}, {"reference_id", 1}, {"node_id", lineB["top_id"]}},
    };
    doc["lines"] = {
        {{"id", 1}, {"name", "A"}, {"top_connection_id", 1}},
        {{"id", 2}, {"name", "B"}, {"top_connection_id", 2}},
    };

    risersim::ModelBuilder builder;
    risersim::RiserModel* model = load_json_fixture(builder, doc, "test_multiline_fixture_2line.json");
    REQUIRE(model != nullptr);

    REQUIRE(model->lines().size() == 2);
    REQUIRE(model->connections().size() == 2);
    REQUIRE(model->references().size() == 1);

    auto attachments = model->resolve_line_attachments();
    REQUIRE(attachments.size() == 2);
    CHECK(attachments[0].top_node->id == lineA["top_id"].get<int>());
    CHECK(attachments[1].top_node->id == lineB["top_id"].get<int>());

    // Solve estático (sem offset -- só prova que o modelo parseado converge, física detalhada já
    // coberta pelo teste C++-direto acima) -- ambas as linhas independentemente.
    // max_iter_per_step 2000 (não 300 como os outros testes deste arquivo): build_line_json()'s
    // TDP fica quase vertical (X_tdp cai no piso de 1m via max(1, S_susp²-h_water²)), um trecho
    // suspenso muito mais "dobrado" que o real -- e curvature1/curvature2 (birth twist,
    // model_builder.cpp) agora refletem essa curvatura real em vez de ficarem artificialmente
    // amortecidos por um bug de eixo já corrigido (build_frame_from_chord() projetava a curvatura
    // no eixo Y errado -- ver histórico do fix, 2026-08-18). Malhas reais do tec_line (ex.
    // Exemplo_01c) refinam o elemento exatamente nessas regiões (0.25-1m, não os 8m deste
    // fixture) então não pagam esse custo -- aqui é só um teste de parsing/convergência, não de
    // física fina, então dar mais orçamento de iteração é a correção certa, não afrouxar a física.
    risersim::StaticAnalysis sa;
    sa.model = model;
    sa.water_density = 1025.0;
    sa.water_density_for_mass = 1025.0;
    sa.seabed = risersim::SeabedInteraction(-100.0, 1.0e5, 0.5);
    sa.load_steps = 20;
    sa.max_iter_per_step = 2000;
    sa.tol = 0.01;
    REQUIRE(sa.solve());
}

TEST_CASE("ModelBuilder keeps single-line JSON (no lines[]) working exactly as before", "[model_builder][multiline]") {
    std::vector<nlohmann::json> nodes, elements;
    auto lineA = build_line_json(0, 0.0, nodes, elements);

    nlohmann::json doc;
    doc["schema_version"] = 1;
    doc["model"]["nodes"] = nodes;
    doc["model"]["elements"] = elements;
    doc["boundary_conditions"]["prescribed_dofs"] = {
        {{"node_id", lineA["top_id"]}, {"dofs", {-1, -1, -1, -1, -1, -1}}},
    };
    doc["boundary_conditions"]["restrained_dofs"] = {
        {{"node_id", lineA["anchor_id"]}, {"dofs", {-1, -1, -1, -1, -1, -1}}},
    };
    // Deliberadamente SEM "references"/"connections"/"lines" -- todo JSON escrito antes desta
    // feature existir tem exatamente esta forma.

    risersim::ModelBuilder builder;
    risersim::RiserModel* model = load_json_fixture(builder, doc, "test_multiline_fixture_singleline.json");
    REQUIRE(model != nullptr);

    REQUIRE(model->lines().empty());
    REQUIRE(model->connections().empty());
    REQUIRE(model->references().empty());

    auto attachments = model->resolve_line_attachments();
    REQUIRE(attachments.size() == 1);
    CHECK(attachments.front().top_node == model->nodes().front().get());
    CHECK(attachments.front().vessel_motion == &model->environmental().vessel_motion);

    // max_iter_per_step 2000: mesma justificativa do teste acima (build_line_json()'s TDP quase
    // vertical + curvature1/curvature2 agora corretos, não mais amortecidos pelo bug de eixo).
    risersim::StaticAnalysis sa;
    sa.model = model;
    sa.water_density = 1025.0;
    sa.water_density_for_mass = 1025.0;
    sa.seabed = risersim::SeabedInteraction(-100.0, 1.0e5, 0.5);
    sa.load_steps = 20;
    sa.max_iter_per_step = 2000;
    sa.tol = 0.01;
    REQUIRE(sa.solve());
}
