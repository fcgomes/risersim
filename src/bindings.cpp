/**
 * bindings.cpp
 * ============
 * pybind11 bindings para o riserSim.
 * Expõe toda a API necessária para rodar uma simulação completa a partir
 * de Python, sem gerar código C++ intermediário.
 *
 * Build:
 *   cmake -B build risersim/ && cmake --build build --config Release
 *   O módulo resultante é: build/risersim.pyd (Windows) ou risersim.so (Linux)
 *
 * Uso em Python:
 *   import risersim
 *   node = risersim.Node3D(1, 0.0, 0.0, -100.0)
 *   ...
 */

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/eigen.h>
#include <pybind11/functional.h>

#include "risersim/node.hpp"
#include "risersim/element_beam.hpp"
#include "risersim/analysis.hpp"
#include "risersim/static_analysis.hpp"
#include "risersim/dynamic_analysis.hpp"
#include "risersim/simulation_exporter.hpp"
#include "risersim/seabed.hpp"
#include "risersim/current_profile.hpp"
#include "risersim/buoyancy_and_restrictor.hpp"
#include "risersim/vessel_offset.hpp"
#include "risersim/snapshot.hpp"

namespace py = pybind11;
using namespace risersim;

PYBIND11_MODULE(risersim, m) {
    m.doc() = R"(
riserSim — Open-Source Offshore Riser & Flexible Pipe Structural Analysis Engine
==================================================================================
Módulo Python para análise de risers offshore usando elementos de viga corotacional 3D.

Exemplo mínimo de uso:
    import risersim, math

    props = risersim.BeamMaterialProps()
    props.E = 2.0e9
    props.D_outer = 0.25

    node1 = risersim.Node3D(1, 0.0, 0.0,   0.0)
    node2 = risersim.Node3D(2, 5.0, 0.0, -10.0)
    node1.fix_all()

    elem = risersim.CorotationalBeam3D(1, node1, node2, props)

    sa = risersim.StaticAnalysis()
    sa.nodes    = [node1, node2]
    sa.elements = [elem]
    sa.solve()
)";

    // =========================================================================
    // Node3D
    // =========================================================================
    py::class_<Node3D>(m, "Node3D", "Nó 3D com 6 graus de liberdade (ux, uy, uz, rx, ry, rz).")
        .def(py::init<int, double, double, double>(),
             py::arg("id"), py::arg("x"), py::arg("y"), py::arg("z"),
             "Cria nó com ID e posição inicial (X, Y, Z) em metros.")
        .def_readwrite("id",         &Node3D::id)
        .def_readwrite("coords",     &Node3D::coords,
                       "Posição inicial do nó (Eigen::Vector3d, metros).")
        .def_readwrite("disp",       &Node3D::disp,
                       "Vetor de deslocamento [ux, uy, uz] (metros).")
        .def_readwrite("rot",        &Node3D::rot,
                       "Vetor de rotação [rx, ry, rz] (radianos).")
        .def_readwrite("eq_numbers", &Node3D::eq_numbers,
                       "Números de equação para os 6 DOFs (-1 = restrito).")
        .def("current_coords", &Node3D::current_coords,
             "Retorna posição atual = coords + disp.")
        // Métodos auxiliares de condição de contorno
        .def("fix_all", [](Node3D& self) {
            self.eq_numbers = {-1, -1, -1, -1, -1, -1};
        }, "Fixa todos os 6 DOFs (apoio engastado).")
        .def("free_translations", [](Node3D& self) {
            self.eq_numbers = {0, 1, 2, -1, -1, -1};
        }, "Libera translações, fixa rotações (usado em nós interiores sem momento).")
        .def("fix_dofs", [](Node3D& self, bool ux, bool uy, bool uz,
                                           bool rx, bool ry, bool rz) {
            self.eq_numbers[0] = ux ? -1 : 0;
            self.eq_numbers[1] = uy ? -1 : 0;
            self.eq_numbers[2] = uz ? -1 : 0;
            self.eq_numbers[3] = rx ? -1 : 0;
            self.eq_numbers[4] = ry ? -1 : 0;
            self.eq_numbers[5] = rz ? -1 : 0;
        }, py::arg("ux")=false, py::arg("uy")=false, py::arg("uz")=false,
           py::arg("rx")=true,  py::arg("ry")=true,  py::arg("rz")=true,
           "Fixa DOFs individualmente. True = fixo, False = livre.");

    // =========================================================================
    // BeamMaterialProps
    // =========================================================================
    py::class_<BeamMaterialProps>(m, "BeamMaterialProps",
        "Propriedades do material e seção da viga (unidades SI: Pa, m, kg/m³).")
        .def(py::init<>())
        .def_readwrite("E",          &BeamMaterialProps::E,
                       "Módulo de elasticidade (Pa).")
        .def_readwrite("G",          &BeamMaterialProps::G,
                       "Módulo de cisalhamento (Pa).")
        .def_readwrite("A",          &BeamMaterialProps::A,
                       "Área da seção transversal estrutural (m²).")
        .def_readwrite("IY",         &BeamMaterialProps::IY,
                       "Momento de inércia em Y (m⁴).")
        .def_readwrite("IZ",         &BeamMaterialProps::IZ,
                       "Momento de inércia em Z (m⁴).")
        .def_readwrite("J",          &BeamMaterialProps::J,
                       "Constante de torção (m⁴).")
        .def_readwrite("rho",        &BeamMaterialProps::rho,
                       "Densidade de massa estrutural (kg/m³).")
        .def_readwrite("D_outer",    &BeamMaterialProps::D_outer,
                       "Diâmetro externo (m).")
        .def_readwrite("D_inner",    &BeamMaterialProps::D_inner,
                       "Diâmetro interno (m).")
        .def_readwrite("rho_fluid",  &BeamMaterialProps::rho_fluid,
                       "Densidade do fluido interno (kg/m³).")
        .def_readwrite("Ca",         &BeamMaterialProps::Ca,
                       "Coeficiente de massa adicional hidrodinâmica.");

    // =========================================================================
    // CorotationalBeam3D
    // =========================================================================
    py::class_<CorotationalBeam3D>(m, "CorotationalBeam3D",
        "Elemento de viga corotacional 3D com 12 DOFs (2 nós × 6 DOFs).")
        .def(py::init<int, Node3D*, Node3D*, const BeamMaterialProps&, double>(),
             py::arg("id"), py::arg("node1"), py::arg("node2"), py::arg("props"),
             py::arg("L_unstretched") = 0.0,
             "Cria elemento entre node1 e node2. L_unstretched=0 usa comprimento inicial.",
             py::keep_alive<1, 3>(),   // keep node1 alive
             py::keep_alive<1, 4>())   // keep node2 alive
        .def_readwrite("id",                 &CorotationalBeam3D::id)
        .def_readwrite("props",              &CorotationalBeam3D::props)
        .def_readwrite("p_i",               &CorotationalBeam3D::p_i,
                       "Pressão do fluido interno (Pa).")
        .def_readwrite("p_e",               &CorotationalBeam3D::p_e,
                       "Pressão hidrostática externa (Pa, calculada automaticamente se 0).")
        .def_readwrite("net_upward_buoyancy", &CorotationalBeam3D::net_upward_buoyancy,
                       "Força de flutuação extra de módulos (N/m, ex.: lazy wave).")
        .def_readwrite("tension_effective",   &CorotationalBeam3D::tension_effective,
                       "Tensão efetiva calculada T_eff = T_true + p_e*A_e - p_i*A_i (N).")
        .def_readwrite("initial_length",      &CorotationalBeam3D::initial_length,
                       "Comprimento não-deformado (m).")
        .def("current_length",         &CorotationalBeam3D::current_length,
             "Comprimento atual (m).")
        .def("outer_area",             &CorotationalBeam3D::outer_area)
        .def("inner_area",             &CorotationalBeam3D::inner_area)
        .def("update_effective_tension",&CorotationalBeam3D::update_effective_tension,
             "Recalcula e retorna a tensão efetiva (N).")
        .def("global_stiffness",       &CorotationalBeam3D::global_stiffness,
             "Retorna a matriz de rigidez global 12×12 (N/m).")
        .def("global_mass",            &CorotationalBeam3D::global_mass,
             py::arg("rho_water") = 1025.0,
             "Retorna a matriz de massa global 12×12 (kg).")
        .def("compute_stress_and_curvature",
             &CorotationalBeam3D::compute_stress_and_curvature,
             py::arg("prev_elem") = nullptr,
             py::arg("next_elem") = nullptr,
             py::arg("yield_stress_MPa") = 350.0,
             "Calcula curvatura, momento, von Mises e fator MBR.");

    // StressAndCurvatureResults (struct interna)
    py::class_<CorotationalBeam3D::StressAndCurvatureResults>(m, "StressResults",
        "Resultados de tensão e curvatura de um elemento.")
        .def_readonly("curvature",           &CorotationalBeam3D::StressAndCurvatureResults::curvature,
                      "Curvatura κ (1/m).")
        .def_readonly("bending_moment_kNm",  &CorotationalBeam3D::StressAndCurvatureResults::bending_moment_kNm,
                      "Momento fletor (kN·m).")
        .def_readonly("bend_radius",         &CorotationalBeam3D::StressAndCurvatureResults::bend_radius,
                      "Raio de curvatura real R = 1/κ (m).")
        .def_readonly("mbr_min",             &CorotationalBeam3D::StressAndCurvatureResults::mbr_min,
                      "Raio mínimo de curvatura MBR (m).")
        .def_readonly("mbr_safety_factor",   &CorotationalBeam3D::StressAndCurvatureResults::mbr_safety_factor,
                      "Fator de segurança MBR = R_atual / MBR_min.")
        .def_readonly("von_mises_MPa",       &CorotationalBeam3D::StressAndCurvatureResults::von_mises_MPa,
                      "Tensão combinada von Mises (MPa).");

    // =========================================================================
    // SeabedInteraction
    // =========================================================================
    py::class_<SeabedInteraction>(m, "SeabedInteraction",
        "Mola bilinear de interação com o fundo do mar.")
        .def(py::init<double, double, double>(),
             py::arg("depth")       = -100.0,
             py::arg("stiffness_z") = 1.0e5,
             py::arg("friction")    = 0.5,
             "depth: coordenada Z do fundo (m, negativo). stiffness_z: rigidez vertical (N/m). friction: coef. de atrito.")
        .def_readwrite("seabed_depth",  &SeabedInteraction::seabed_depth)
        .def_readwrite("stiffness_z",   &SeabedInteraction::stiffness_z)
        .def_readwrite("friction_coeff",&SeabedInteraction::friction_coeff);

    // =========================================================================
    // CurrentProfile
    // =========================================================================
    py::class_<CurrentProfile>(m, "CurrentProfile",
        "Perfil de corrente por lei de potência (perfil de Hellmann).")
        .def(py::init<double, double, double, double, double>(),
             py::arg("v_surface")      = 1.5,
             py::arg("seabed_depth")   = -100.0,
             py::arg("heading_deg")    = 90.0,
             py::arg("power_exponent") = 0.1428,
             py::arg("Cd")             = 1.0,
             "v_surface: vel. de superfície (m/s). seabed_depth: Z do fundo (m, negativo). heading_deg: azimute (°).")
        .def_readwrite("v_surface",      &CurrentProfile::v_surface)
        .def_readwrite("seabed_depth",   &CurrentProfile::seabed_depth)
        .def_readwrite("heading_deg",    &CurrentProfile::heading_deg)
        .def_readwrite("power_exponent", &CurrentProfile::power_exponent)
        .def_readwrite("Cd",             &CurrentProfile::Cd)
        .def("get_velocity", &CurrentProfile::get_velocity, py::arg("z"),
             "Retorna velocidade da corrente na profundidade z (m/s).");

    // =========================================================================
    // VesselOffset & OffsetMode
    // =========================================================================
    py::enum_<OffsetMode>(m, "OffsetMode", "Modo de offset da plataforma.")
        .value("Near",   OffsetMode::Near,   "Afastamento em direção à âncora (-X).")
        .value("Far",    OffsetMode::Far,     "Afastamento em direção contrária à âncora (+X).")
        .value("Cross",  OffsetMode::Cross,   "Afastamento lateral (+Y).")
        .value("Custom", OffsetMode::Custom,  "Deslocamento customizado (dx, dy, dz).")
        .export_values();

    py::class_<VesselOffset>(m, "VesselOffset",
        "Offset imposto ao nó de topo do riser (simula posição da plataforma).")
        .def(py::init<OffsetMode, double>(),
             py::arg("mode") = OffsetMode::Far,
             py::arg("magnitude") = 10.0,
             "Cria offset por modo e magnitude (m).")
        .def(py::init<double, double, double>(),
             py::arg("dx"), py::arg("dy"), py::arg("dz"),
             "Cria offset customizado com vetor (dx, dy, dz) em metros.")
        .def_readwrite("mode",        &VesselOffset::mode)
        .def_readwrite("offset_disp", &VesselOffset::offset_disp,
                       "Vetor de deslocamento imposto (Eigen::Vector3d, metros).");

    // =========================================================================
    // BuoyancyModule & BendRestrictor
    // =========================================================================
    py::class_<BuoyancyModule>(m, "BuoyancyModule",
        "Módulo de flutuação para criar configuração Lazy Wave.")
        .def(py::init<double, double>(),
             py::arg("d_buoyancy") = 0.80,
             py::arg("net_force")  = 3600.0,
             "d_buoyancy: diâmetro do módulo (m). net_force: força líquida ascendente (N/m).")
        .def_readwrite("D_buoyancy",       &BuoyancyModule::D_buoyancy)
        .def_readwrite("net_upward_force", &BuoyancyModule::net_upward_force)
        .def("apply_to_element", &BuoyancyModule::apply_to_element,
             py::arg("element"),
             "Aplica o módulo ao elemento (altera D_outer e net_upward_buoyancy).");

    py::class_<BendRestrictor>(m, "BendRestrictor",
        "Limitador de curvatura (aumenta EI do elemento próximo ao topo).")
        .def(py::init<double>(),
             py::arg("factor") = 5.0,
             "factor: multiplicador da rigidez EI.")
        .def_readwrite("stiffness_multiplier", &BendRestrictor::stiffness_multiplier)
        .def("apply_to_element", &BendRestrictor::apply_to_element,
             py::arg("element"),
             "Multiplica IY e IZ do elemento pelo fator.");

    // =========================================================================
    // StepSnapshot
    // =========================================================================
    py::class_<StepSnapshot>(m, "StepSnapshot",
        "Snapshot de um passo da simulação (coordenadas + resultados por elemento).")
        .def(py::init<>())
        .def_readwrite("step_index",                  &StepSnapshot::step_index)
        .def_readwrite("load_factor",                 &StepSnapshot::load_factor,
                       "Fator de carga (0–1) para análise estática, ou tempo (s) para dinâmica.")
        .def_readwrite("node_coords",                 &StepSnapshot::node_coords,
                       "Lista de Eigen::Vector3d com posições atuais dos nós.")
        .def_readwrite("element_tensions_kN",         &StepSnapshot::element_tensions_kN)
        .def_readwrite("element_bending_moments_kNm", &StepSnapshot::element_bending_moments_kNm)
        .def_readwrite("element_curvatures",          &StepSnapshot::element_curvatures)
        .def_readwrite("element_von_mises_MPa",       &StepSnapshot::element_von_mises_MPa)
        .def_readwrite("element_mbr_safety_factors",  &StepSnapshot::element_mbr_safety_factors);

    // =========================================================================
    // RiserModel
    // =========================================================================
    py::class_<RiserModel>(m, "RiserModel", "Modelo estrutural contendo nós e elementos de viga.")
        .def(py::init<>())
        .def_readwrite("nodes",          &RiserModel::nodes)
        .def_readwrite("elements",       &RiserModel::elements)
        .def("clear",                  &RiserModel::clear);

    // =========================================================================
    // Analysis (base — não instanciável diretamente)
    // =========================================================================
    py::class_<Analysis>(m, "Analysis")
        .def_readwrite("model",          &Analysis::model)
        .def_readwrite("seabed",         &Analysis::seabed)
        .def_readwrite("current",        &Analysis::current)
        .def_readwrite("enable_current", &Analysis::enable_current)
        .def_readwrite("water_density",  &Analysis::water_density,
                       "Densidade da água do mar (kg/m³, default 1025.0).")
        .def_readwrite("num_dofs",       &Analysis::num_dofs)
        .def_readwrite("history",        &Analysis::history,
                       "Lista de StepSnapshot com o histórico da simulação.")
        .def("assign_equation_numbers", &Analysis::assign_equation_numbers,
             "Numera automaticamente os graus de liberdade livres.");

    py::enum_<ArtificialStiffnessMode>(m, "ArtificialStiffnessMode",
        "Controla quando a rigidez artificial fica ativa em solve_catenary_static.")
        .value("OnlyFirstStep", ArtificialStiffnessMode::OnlyFirstStep, "Só no passo 1 (comportamento histórico).")
        .value("EveryStep",     ArtificialStiffnessMode::EveryStep,     "Em todos os passos (fase 'assembly').")
        .value("Never",         ArtificialStiffnessMode::Never,         "Nunca (fase 'static' limpa).")
        .export_values();

    // =========================================================================
    // StaticAnalysis
    // =========================================================================
    py::class_<StaticAnalysis, Analysis>(m, "StaticAnalysis",
        R"(Análise estática não-linear (Newton-Raphson com incremento de carga).

Sequência de uso:
    model = risersim.RiserModel()
    model.nodes    = [n1, n2, ...]
    model.elements = [e1, e2, ...]
    
    sa = risersim.StaticAnalysis()
    sa.model    = model
    sa.seabed   = risersim.SeabedInteraction(-100.0, 1e5, 0.5)
    sa.load_steps = 20
    sa.tol = 100.0
    sa.enable_offset = True
    sa.offset = risersim.VesselOffset(risersim.OffsetMode.Far, 10.0)
    ok = sa.solve()
    # sa.history contém os snapshots de cada passo
)")
        .def(py::init<>())
        .def_readwrite("load_steps",        &StaticAnalysis::load_steps,
                       "Número de incrementos de carga (default 20).")
        .def_readwrite("max_iter_per_step", &StaticAnalysis::max_iter_per_step,
                       "Máximo de iterações N-R por passo (default 300).")
        .def_readwrite("tol",               &StaticAnalysis::tol,
                       "Tolerância de convergência da norma do resíduo (N, default 100.0).")
        .def_readwrite("offset",            &StaticAnalysis::offset,
                       "Offset de plataforma a aplicar após convergência estática.")
        .def_readwrite("enable_offset",     &StaticAnalysis::enable_offset,
                       "Se True, aplica o offset após a análise catenary.")
        .def("solve", &StaticAnalysis::solve,
             "Executa a análise estática completa (catenary + offset se enable_offset=True).\n"
             "Retorna True se convergiu.")
        .def("solve_catenary_static", &StaticAnalysis::solve_catenary_static,
             py::arg("steps") = 20, py::arg("max_iter") = 300, py::arg("tolerance") = 100.0,
             py::arg("artif_mode") = ArtificialStiffnessMode::OnlyFirstStep,
             "Executa apenas a fase catenary (sem offset).")
        .def("solve_vessel_offset", &StaticAnalysis::solve_vessel_offset,
             py::arg("offset"), py::arg("steps") = 20, py::arg("max_iter") = 300,
             py::arg("tolerance") = 100.0,
             "Aplica offset da plataforma sobre o equilíbrio catenary.");

    // =========================================================================
    // DynamicAnalysis
    // =========================================================================
    py::class_<DynamicAnalysis, Analysis>(m, "DynamicAnalysis",
        R"(Análise dinâmica no domínio do tempo (Newmark-β + Newton-Raphson).

Pode ser inicializado a partir de uma StaticAnalysis já resolvida:
    da = risersim.DynamicAnalysis.from_static(static_analysis)
    da.duration_s     = 20.0
    da.dt_s           = 0.05
    da.wave_amplitude = 2.5
    da.wave_period    = 10.0
    ok = da.solve()
    # da.history contém os snapshots de cada timestep
)")
        .def(py::init<>())
        .def(py::init<const Analysis&>(), py::arg("static_analysis"),
             "Inicializa copiando nós, elementos e parâmetros de uma StaticAnalysis resolvida.")
        .def_readwrite("duration_s",     &DynamicAnalysis::duration_s,
                       "Duração total da análise dinâmica (s).")
        .def_readwrite("dt_s",           &DynamicAnalysis::dt_s,
                       "Passo de tempo (s).")
        .def_readwrite("wave_amplitude", &DynamicAnalysis::wave_amplitude,
                       "Amplitude da onda prescrita no topo (m).")
        .def_readwrite("wave_period",    &DynamicAnalysis::wave_period,
                       "Período da onda (s).")
        .def_readwrite("alpha_rayleigh", &DynamicAnalysis::alpha_rayleigh,
                       "Coeficiente α de Rayleigh (amortecimento de massa).")
        .def_readwrite("beta_rayleigh",  &DynamicAnalysis::beta_rayleigh,
                       "Coeficiente β de Rayleigh (amortecimento de rigidez).")
        .def("solve", &DynamicAnalysis::solve,
             "Executa a análise dinâmica. Retorna True se completou sem erros.")
        .def("solve_time_domain_dynamic",
             &DynamicAnalysis::solve_time_domain_dynamic,
             py::arg("duration") = 20.0, py::arg("dt") = 0.05,
             py::arg("amp") = 2.5, py::arg("period") = 10.0,
             py::arg("alpha") = 0.05, py::arg("beta") = 0.01,
             "Executa solver Newmark-β com os parâmetros fornecidos.")
        // Método de fábrica estático conveniente
        .def_static("from_static", [](const Analysis& sa) {
            return new DynamicAnalysis(sa);
        }, py::arg("static_analysis"),
        py::return_value_policy::take_ownership,
        "Cria DynamicAnalysis copiando o estado de uma StaticAnalysis resolvida.");

    // =========================================================================
    // SimulationExporter
    // =========================================================================
    py::class_<SimulationExporter>(m, "SimulationExporter",
        "Exporta resultados da simulação para JSON e HDF5.")
        .def_static("export_json",
            [](const Analysis& sa, const Analysis& da, const std::string& fn) {
                return SimulationExporter::export_json(sa, da, fn);
            },
            py::arg("static_analysis"), py::arg("dynamic_analysis"),
            py::arg("filename") = "catenary_results.json",
            "Exporta histórico completo (estático + dinâmico) para JSON.")
        .def_static("export_hdf5",
            [](const Analysis& sa, const Analysis& da, const std::string& fn) {
                return SimulationExporter::export_hdf5(sa, da, fn);
            },
            py::arg("static_analysis"), py::arg("dynamic_analysis"),
            py::arg("filename") = "catenary_results.h5",
            "Exporta histórico completo (estático + dinâmico) para HDF5 binário.");

    // =========================================================================
    // Funções utilitárias de módulo
    // =========================================================================
    m.def("build_catenary_nodes",
        [](int num_elements, double total_length_m, double depth_m,
           double span_x_m) -> std::vector<Node3D*> {
            int num_nodes = num_elements + 1;
            std::vector<Node3D*> nodes;
            nodes.reserve(num_nodes);
            for (int i = 0; i < num_nodes; ++i) {
                double ratio = static_cast<double>(i) / static_cast<double>(num_elements);
                double x = ratio * span_x_m;
                double z = ratio * (-std::abs(depth_m))
                         - std::abs(depth_m) * 0.25 * std::sin(ratio * 3.14159265358979);
                nodes.push_back(new Node3D(i + 1, x, 0.0, z));
            }
            // Nós de topo e fundo fixos
            nodes.front()->eq_numbers = {-1, -1, -1, -1, -1, -1};
            nodes.back()->eq_numbers  = {-1, -1, -1, -1, -1, -1};
            // Nós intermediários: translações livres, rotações fixas
            for (size_t i = 1; i < nodes.size() - 1; ++i) {
                nodes[i]->eq_numbers = {0, 1, 2, -1, -1, -1};
            }
            return nodes;
        },
        py::arg("num_elements"),
        py::arg("total_length_m"),
        py::arg("depth_m"),
        py::arg("span_x_m"),
        py::return_value_policy::take_ownership,
        R"(Cria malha de nós na configuração catenary inicial.

Retorna lista de Node3D* alocados no heap (ownership transferida para Python).
Nós de topo (front) e fundo (back) são fixados automaticamente.
Nós intermediários têm translações livres e rotações fixas.

Args:
    num_elements: número de elementos finitos
    total_length_m: comprimento não-deformado da linha (m)
    depth_m: lâmina d'água (m, positivo — a função aplica o sinal)
    span_x_m: extensão horizontal da catenária (m)
)");

    m.def("build_catenary_elements",
        [](std::vector<Node3D*>& nodes, const BeamMaterialProps& props) -> std::vector<CorotationalBeam3D*> {
            if (nodes.size() < 2) return {};
            int num_elements = static_cast<int>(nodes.size()) - 1;
            double L_elem = (nodes.back()->coords - nodes.front()->coords).norm()
                            / static_cast<double>(num_elements);
            // Usa comprimento ao longo da catenária (linear)
            double total_length = 0.0;
            for (int i = 0; i < num_elements; ++i) {
                total_length += (nodes[i+1]->coords - nodes[i]->coords).norm();
            }
            double L_unstretched = total_length / static_cast<double>(num_elements);

            std::vector<CorotationalBeam3D*> elements;
            elements.reserve(num_elements);
            for (int i = 0; i < num_elements; ++i) {
                elements.push_back(new CorotationalBeam3D(
                    i + 1, nodes[i], nodes[i + 1], props, L_unstretched));
            }
            return elements;
        },
        py::arg("nodes"), py::arg("props"),
        py::return_value_policy::take_ownership,
        R"(Cria elementos de viga entre os nós fornecidos.

Args:
    nodes: lista de Node3D* (deve ter pelo menos 2)
    props: BeamMaterialProps com propriedades do material

Retorna lista de CorotationalBeam3D* (ownership transferida para Python).
O comprimento não-deformado de cada elemento é calculado automaticamente.
)");

    // Versão do módulo
    m.attr("__version__") = "1.0.0";
    m.attr("__author__")  = "riserSim Team";
}
