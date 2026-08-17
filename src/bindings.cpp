/**
 * @file bindings.cpp
 * @brief pybind11 bindings for risersim.
 *
 * Exposes the full API needed to run a complete simulation from Python, without generating
 * intermediate C++ code.
 *
 * Build:
 *   cmake -B build risersim/ && cmake --build build --config Release
 *   The resulting module is: build/risersim.pyd (Windows) or risersim.so (Linux)
 *
 * Usage from Python:
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
riserSim -- Open-Source Offshore Riser & Flexible Pipe Structural Analysis Engine
==================================================================================
Python module for offshore riser analysis using 3D corotational beam elements.

Minimal usage example:
    import risersim, math

    model = risersim.RiserModel()
    node1 = model.add_node(1, 0.0, 0.0,   0.0)
    node2 = model.add_node(2, 5.0, 0.0, -10.0)
    node1.fix_all()

    props = risersim.BeamMaterialProps()
    props.E = 2.0e9
    props.D_outer = 0.25
    elem = model.add_element(1, node1, node2, props)

    sa = risersim.StaticAnalysis()
    sa.model = model
    sa.solve()
)";

    // =========================================================================
    // Node3D
    // =========================================================================
    py::class_<Node3D>(m, "Node3D", "3D node with 6 degrees of freedom (ux, uy, uz, rx, ry, rz).")
        .def(py::init<int, double, double, double>(),
             py::arg("id"), py::arg("x"), py::arg("y"), py::arg("z"),
             "Creates a node with an ID and initial position (X, Y, Z) in meters.")
        .def_readwrite("id",         &Node3D::id)
        .def_readwrite("coords",     &Node3D::coords,
                       "Initial node position (Eigen::Vector3d, meters).")
        .def_readwrite("disp",       &Node3D::disp,
                       "Displacement vector [ux, uy, uz] (meters).")
        .def_readwrite("rot",        &Node3D::rot,
                       "Rotation vector [rx, ry, rz] (radians).")
        .def_readwrite("eq_numbers", &Node3D::eq_numbers,
                       "Equation numbers for the 6 DOFs (-1 = restrained).")
        .def("current_coords", &Node3D::current_coords,
             "Returns the current position = coords + disp.")
        // Boundary condition helper methods
        .def("fix_all", [](Node3D& self) {
            self.eq_numbers = {-1, -1, -1, -1, -1, -1};
        }, "Fixes all 6 DOFs (fully clamped support).")
        .def("free_translations", [](Node3D& self) {
            self.eq_numbers = {0, 1, 2, -1, -1, -1};
        }, "Frees translations, fixes rotations (used for interior nodes with no moment).")
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
           "Fixes DOFs individually. True = fixed, False = free.");

    // =========================================================================
    // BeamMaterialProps
    // =========================================================================
    py::class_<BeamMaterialProps>(m, "BeamMaterialProps",
        "Beam material and cross-section properties (SI units: Pa, m, kg/m3).")
        .def(py::init<>())
        .def_readwrite("E",          &BeamMaterialProps::E,
                       "Young's modulus (Pa).")
        .def_readwrite("G",          &BeamMaterialProps::G,
                       "Shear modulus (Pa).")
        .def_readwrite("A",          &BeamMaterialProps::A,
                       "Structural cross-section area (m2).")
        .def_readwrite("IY",         &BeamMaterialProps::IY,
                       "Moment of inertia about Y (m4).")
        .def_readwrite("IZ",         &BeamMaterialProps::IZ,
                       "Moment of inertia about Z (m4).")
        .def_readwrite("J",          &BeamMaterialProps::J,
                       "Torsional constant (m4).")
        .def_readwrite("rho",        &BeamMaterialProps::rho,
                       "Weight-equivalent density for the static self-weight formula (kg/m3) -- "
                       "NOT physical structural mass, see rho_structural.")
        .def_readwrite("rho_structural", &BeamMaterialProps::rho_structural,
                       "True structural mass density, for inertia/mass-matrix purposes (kg/m3).")
        .def_readwrite("D_outer",    &BeamMaterialProps::D_outer,
                       "Outer diameter (m).")
        .def_readwrite("D_inner",    &BeamMaterialProps::D_inner,
                       "Inner diameter (m).")
        .def_readwrite("rho_fluid",  &BeamMaterialProps::rho_fluid,
                       "Internal fluid density (kg/m3).")
        .def_readwrite("Ca",         &BeamMaterialProps::Ca,
                       "Hydrodynamic added mass coefficient.")
        .def_readwrite("Cd",         &BeamMaterialProps::Cd,
                       "Hydrodynamic drag coefficient.");

    // =========================================================================
    // CorotationalBeam3D
    // =========================================================================
    py::class_<CorotationalBeam3D>(m, "CorotationalBeam3D",
        "3D corotational beam element with 12 DOFs (2 nodes x 6 DOFs).")
        .def(py::init<int, Node3D*, Node3D*, const BeamMaterialProps&, double>(),
             py::arg("id"), py::arg("node1"), py::arg("node2"), py::arg("props"),
             py::arg("L_unstretched") = 0.0,
             "Creates an element between node1 and node2. L_unstretched=0 uses the initial length.",
             py::keep_alive<1, 3>(),   // keep node1 alive
             py::keep_alive<1, 4>())   // keep node2 alive
        .def_property_readonly("id",         &CorotationalBeam3D::id)
        .def_property_readonly("props",      static_cast<BeamMaterialProps& (CorotationalBeam3D::*)()>(&CorotationalBeam3D::props),
                       py::return_value_policy::reference_internal,
                       "Cross-section/material properties (mutable in place -- see BuoyancyModule/BendRestrictor).")
        .def_property("p_i",                 &CorotationalBeam3D::p_i, &CorotationalBeam3D::set_p_i,
                       "Internal fluid pressure (Pa).")
        .def_property("p_e",                 &CorotationalBeam3D::p_e, &CorotationalBeam3D::set_p_e,
                       "External hydrostatic pressure (Pa, computed automatically if 0).")
        .def_property("net_upward_buoyancy", &CorotationalBeam3D::net_upward_buoyancy, &CorotationalBeam3D::set_net_upward_buoyancy,
                       "Extra net upward buoyancy force from modules (N/m, e.g. lazy wave).")
        .def_property_readonly("tension_effective", &CorotationalBeam3D::tension_effective,
                       "Computed effective tension T_eff = T_true + p_e*A_e - p_i*A_i (N).")
        .def_property_readonly("initial_length",    &CorotationalBeam3D::initial_length,
                       "Unstretched length (m).")
        .def("current_length",         &CorotationalBeam3D::current_length,
             "Current length (m).")
        .def("outer_area",             &CorotationalBeam3D::outer_area)
        .def("inner_area",             &CorotationalBeam3D::inner_area)
        .def("update_effective_tension",&CorotationalBeam3D::update_effective_tension,
             "Recomputes and returns the effective tension (N).")
        .def("global_stiffness",       &CorotationalBeam3D::global_stiffness,
             "Returns the 12x12 global stiffness matrix (N/m).")
        .def("global_mass",            &CorotationalBeam3D::global_mass,
             py::arg("rho_water") = 1025.0,
             "Returns the 12x12 global mass matrix (kg).")
        .def("compute_stress_and_curvature",
             &CorotationalBeam3D::compute_stress_and_curvature,
             py::arg("prev_elem") = nullptr,
             py::arg("next_elem") = nullptr,
             py::arg("yield_stress_MPa") = 350.0,
             "Computes curvature, moment, von Mises stress, and MBR safety factor.");

    // StressAndCurvatureResults (nested struct)
    py::class_<CorotationalBeam3D::StressAndCurvatureResults>(m, "StressResults",
        "Stress and curvature results for an element.")
        .def_readonly("curvature",           &CorotationalBeam3D::StressAndCurvatureResults::curvature,
                      "Curvature kappa (1/m).")
        .def_readonly("bending_moment_kNm",  &CorotationalBeam3D::StressAndCurvatureResults::bending_moment_kNm,
                      "Bending moment (kN.m).")
        .def_readonly("bend_radius",         &CorotationalBeam3D::StressAndCurvatureResults::bend_radius,
                      "Actual bend radius R = 1/kappa (m).")
        .def_readonly("mbr_min",             &CorotationalBeam3D::StressAndCurvatureResults::mbr_min,
                      "Minimum bend radius MBR (m).")
        .def_readonly("mbr_safety_factor",   &CorotationalBeam3D::StressAndCurvatureResults::mbr_safety_factor,
                      "MBR safety factor = R_actual / MBR_min.")
        .def_readonly("von_mises_MPa",       &CorotationalBeam3D::StressAndCurvatureResults::von_mises_MPa,
                      "Combined von Mises stress (MPa).");

    // =========================================================================
    // SeabedInteraction
    // =========================================================================
    py::class_<SeabedInteraction>(m, "SeabedInteraction",
        "Bilinear seabed interaction spring.")
        .def(py::init<double, double, double>(),
             py::arg("depth")       = -100.0,
             py::arg("stiffness_z") = 1.0e5,
             py::arg("friction")    = 0.5,
             "depth: seabed Z coordinate (m, negative). stiffness_z: vertical stiffness (N/m). friction: friction coefficient.")
        .def_property("seabed_depth",  &SeabedInteraction::seabed_depth,  &SeabedInteraction::set_seabed_depth)
        .def_property("stiffness_z",   &SeabedInteraction::stiffness_z,   &SeabedInteraction::set_stiffness_z)
        .def_property("friction_coeff",&SeabedInteraction::friction_coeff,&SeabedInteraction::set_friction_coeff);

    // =========================================================================
    // CurrentProfile
    // =========================================================================
    py::class_<CurrentProfile>(m, "CurrentProfile",
        "Power-law (Hellmann) current velocity profile.")
        .def(py::init<double, double, double, double, double>(),
             py::arg("v_surface")      = 1.5,
             py::arg("seabed_depth")   = -100.0,
             py::arg("heading_deg")    = 90.0,
             py::arg("power_exponent") = 0.1428,
             py::arg("Cd")             = 1.0,
             "v_surface: surface velocity (m/s). seabed_depth: seabed Z (m, negative). heading_deg: azimuth (deg).")
        .def_property("v_surface",      &CurrentProfile::v_surface,      &CurrentProfile::set_v_surface)
        .def_property("seabed_depth",   &CurrentProfile::seabed_depth,   &CurrentProfile::set_seabed_depth)
        .def_property("heading_deg",    &CurrentProfile::heading_deg,    &CurrentProfile::set_heading_deg)
        .def_property("power_exponent", &CurrentProfile::power_exponent, &CurrentProfile::set_power_exponent)
        .def_property("Cd",             &CurrentProfile::Cd,             &CurrentProfile::set_Cd)
        .def("get_velocity", &CurrentProfile::get_velocity, py::arg("z"),
             "Returns the current velocity at depth z (m/s).");

    // =========================================================================
    // VesselOffset & OffsetMode
    // =========================================================================
    py::enum_<OffsetMode>(m, "OffsetMode", "Vessel offset mode.")
        .value("Near",   OffsetMode::Near,   "Moves towards the anchor (-X).")
        .value("Far",    OffsetMode::Far,     "Moves away from the anchor (+X).")
        .value("Cross",  OffsetMode::Cross,   "Moves laterally (+Y).")
        .value("Custom", OffsetMode::Custom,  "Custom displacement (dx, dy, dz).")
        .export_values();

    py::class_<VesselOffset>(m, "VesselOffset",
        "Offset imposed on the riser's top node (simulates the vessel's position).")
        .def(py::init<OffsetMode, double>(),
             py::arg("mode") = OffsetMode::Far,
             py::arg("magnitude") = 10.0,
             "Creates an offset from a mode and magnitude (m).")
        .def(py::init<double, double, double>(),
             py::arg("dx"), py::arg("dy"), py::arg("dz"),
             "Creates a custom offset from a (dx, dy, dz) vector in meters.")
        .def_readwrite("mode",        &VesselOffset::mode)
        .def_readwrite("offset_disp", &VesselOffset::offset_disp,
                       "Imposed displacement vector (Eigen::Vector3d, meters).");

    // =========================================================================
    // BuoyancyModule & BendRestrictor
    // =========================================================================
    py::class_<BuoyancyModule>(m, "BuoyancyModule",
        "Buoyancy module used to create a Lazy Wave configuration.")
        .def(py::init<double, double>(),
             py::arg("d_buoyancy") = 0.80,
             py::arg("net_force")  = 3600.0,
             "d_buoyancy: module diameter (m). net_force: net upward force (N/m).")
        .def_readwrite("D_buoyancy",       &BuoyancyModule::D_buoyancy)
        .def_readwrite("net_upward_force", &BuoyancyModule::net_upward_force)
        .def("apply_to_element", &BuoyancyModule::apply_to_element,
             py::arg("element"),
             "Applies the module to the element (changes D_outer and net_upward_buoyancy).");

    py::class_<BendRestrictor>(m, "BendRestrictor",
        "Bend restrictor (increases an element's EI near the top).")
        .def(py::init<double>(),
             py::arg("factor") = 5.0,
             "factor: EI stiffness multiplier.")
        .def_readwrite("stiffness_multiplier", &BendRestrictor::stiffness_multiplier)
        .def("apply_to_element", &BendRestrictor::apply_to_element,
             py::arg("element"),
             "Multiplies the element's IY and IZ by the factor.");

    // =========================================================================
    // StepSnapshot
    // =========================================================================
    py::class_<StepSnapshot>(m, "StepSnapshot",
        "Snapshot of one simulation step (coordinates + per-element results).")
        .def(py::init<>())
        .def_readwrite("step_index",                  &StepSnapshot::step_index)
        .def_readwrite("load_factor",                 &StepSnapshot::load_factor,
                       "Load factor (0-1) for static analysis, or time (s) for dynamic analysis.")
        .def_readwrite("node_coords",                 &StepSnapshot::node_coords,
                       "List of Eigen::Vector3d with the nodes' current positions.")
        .def_readwrite("element_tensions_kN",         &StepSnapshot::element_tensions_kN)
        .def_readwrite("element_bending_moments_kNm", &StepSnapshot::element_bending_moments_kNm)
        .def_readwrite("element_curvatures",          &StepSnapshot::element_curvatures)
        .def_readwrite("element_von_mises_MPa",       &StepSnapshot::element_von_mises_MPa)
        .def_readwrite("element_mbr_safety_factors",  &StepSnapshot::element_mbr_safety_factors);

    // =========================================================================
    // RiserModel
    // =========================================================================
    // nodes/elements are owned by the model via std::unique_ptr (see model.hpp) --
    // pybind11 can't safely expose a vector<unique_ptr<T>> as a writable property
    // (there's no automatic way to steal ownership away from an existing Python-side
    // object into a fresh unique_ptr), so they're exposed read-only here, as plain
    // non-owning pointer lists. Use add_node()/add_element() to build a model from
    // Python -- they construct the instance directly inside the model's own storage.
    py::class_<RiserModel>(m, "RiserModel", "Structural model containing beam nodes and elements.")
        .def(py::init<>())
        .def_property_readonly("nodes", [](const RiserModel& model) {
            std::vector<Node3D*> v;
            v.reserve(model.nodes().size());
            for (const auto& n : model.nodes()) v.push_back(n.get());
            return v;
        }, "Read-only list of the model's nodes (owned by the model -- see add_node()).")
        .def_property_readonly("elements", [](const RiserModel& model) {
            std::vector<CorotationalBeam3D*> v;
            v.reserve(model.elements().size());
            for (const auto& e : model.elements()) v.push_back(e.get());
            return v;
        }, "Read-only list of the model's elements (owned by the model -- see add_element()).")
        .def("add_node", [](RiserModel& model, int id, double x, double y, double z) {
                return model.add_node(id, x, y, z);
            },
            py::arg("id"), py::arg("x"), py::arg("y"), py::arg("z"),
            py::return_value_policy::reference_internal,
            "Constructs a node owned by this model and returns it.")
        .def("add_element", [](RiserModel& model, int id, Node3D* node1, Node3D* node2,
                                const BeamMaterialProps& props, double L_unstretched) {
                return model.add_element(id, node1, node2, props, L_unstretched);
            },
            py::arg("id"), py::arg("node1"), py::arg("node2"), py::arg("props"),
            py::arg("L_unstretched") = 0.0,
            py::return_value_policy::reference_internal,
            "Constructs a beam element owned by this model and returns it.")
        .def("clear",                  &RiserModel::clear);

    // =========================================================================
    // Analysis (base -- not directly instantiable)
    // =========================================================================
    py::class_<Analysis>(m, "Analysis")
        .def_readwrite("model",          &Analysis::model)
        .def_readwrite("seabed",         &Analysis::seabed)
        .def_readwrite("current",        &Analysis::current)
        .def_readwrite("enable_current", &Analysis::enable_current)
        .def_readwrite("water_density",  &Analysis::water_density,
                       "Seawater density (kg/m3, default 1025.0).")
        .def_readwrite("num_dofs",       &Analysis::num_dofs)
        .def_readwrite("history",        &Analysis::history,
                       "List of StepSnapshot with the simulation history.")
        .def("assign_equation_numbers", &Analysis::assign_equation_numbers,
             "Automatically numbers the free degrees of freedom.");

    py::enum_<ArtificialStiffnessMode>(m, "ArtificialStiffnessMode",
        "Controls when artificial stiffness is active in solve_catenary_static.")
        .value("OnlyFirstStep", ArtificialStiffnessMode::OnlyFirstStep, "Only on step 1 (historical behavior).")
        .value("EveryStep",     ArtificialStiffnessMode::EveryStep,     "On every step (the 'assembly' phase).")
        .value("Never",         ArtificialStiffnessMode::Never,         "Never (the clean 'static' phase).")
        .export_values();

    // =========================================================================
    // StaticAnalysis
    // =========================================================================
    py::class_<StaticAnalysis, Analysis>(m, "StaticAnalysis",
        R"(Nonlinear static analysis (Newton-Raphson with load stepping).

Usage sequence:
    model = risersim.RiserModel()
    n1 = model.add_node(1, 0.0, 0.0, 0.0)
    n2 = model.add_node(2, 5.0, 0.0, -10.0)
    e1 = model.add_element(1, n1, n2, risersim.BeamMaterialProps())

    sa = risersim.StaticAnalysis()
    sa.model    = model
    sa.seabed   = risersim.SeabedInteraction(-100.0, 1e5, 0.5)
    sa.load_steps = 20
    sa.tol = 0.01
    sa.enable_offset = True
    sa.offset = risersim.VesselOffset(risersim.OffsetMode.Far, 10.0)
    ok = sa.solve()
    # sa.history contains the snapshots of each step
)")
        .def(py::init<>())
        .def_readwrite("load_steps",        &StaticAnalysis::load_steps,
                       "Number of load increments (default 20).")
        .def_readwrite("max_iter_per_step", &StaticAnalysis::max_iter_per_step,
                       "Maximum Newton-Raphson iterations per step (default 300).")
        .def_readwrite("tol",               &StaticAnalysis::tol,
                       "Translation/rotation increment-ratio convergence tolerance "
                       "(dimensionless, default 0.01 = 1%).")
        .def_readwrite("offset",            &StaticAnalysis::offset,
                       "Vessel offset to apply after static convergence.")
        .def_readwrite("enable_offset",     &StaticAnalysis::enable_offset,
                       "If True, applies the offset after the catenary analysis.")
        .def("solve", &StaticAnalysis::solve,
             "Runs the full static analysis (catenary + offset if enable_offset=True).\n"
             "Returns True if converged.")
        .def("solve_catenary_static", &StaticAnalysis::solve_catenary_static,
             py::arg("steps") = 20, py::arg("max_iter") = 300, py::arg("tolerance") = 0.01,
             py::arg("artif_mode") = ArtificialStiffnessMode::OnlyFirstStep,
             "Runs only the catenary phase (no offset).")
        .def("solve_vessel_offset", &StaticAnalysis::solve_vessel_offset,
             py::arg("offset"), py::arg("steps") = 20, py::arg("max_iter") = 300,
             py::arg("tolerance") = 0.01,
             "Applies the vessel offset on top of the catenary equilibrium.");

    // =========================================================================
    // DynamicAnalysis
    // =========================================================================
    py::class_<DynamicAnalysis, Analysis>(m, "DynamicAnalysis",
        R"(Time-domain dynamic analysis (Newmark-beta + Newton-Raphson).

Can be initialized from an already-solved StaticAnalysis:
    da = risersim.DynamicAnalysis.from_static(static_analysis)
    da.duration_s     = 20.0
    da.dt_s           = 0.05
    da.wave_amplitude = 2.5
    da.wave_period    = 10.0
    ok = da.solve()
    # da.history contains the snapshots of each timestep
)")
        .def(py::init<>())
        .def(py::init<const Analysis&>(), py::arg("static_analysis"),
             "Initializes by copying nodes, elements, and parameters from a solved StaticAnalysis.")
        .def_readwrite("duration_s",     &DynamicAnalysis::duration_s,
                       "Total duration of the dynamic analysis (s).")
        .def_readwrite("dt_s",           &DynamicAnalysis::dt_s,
                       "Time step (s).")
        .def_readwrite("wave_amplitude", &DynamicAnalysis::wave_amplitude,
                       "Amplitude of the prescribed wave at the top (m).")
        .def_readwrite("wave_period",    &DynamicAnalysis::wave_period,
                       "Wave period (s).")
        .def_readwrite("alpha_rayleigh", &DynamicAnalysis::alpha_rayleigh,
                       "Rayleigh alpha coefficient (mass-proportional damping).")
        .def_readwrite("beta_rayleigh",  &DynamicAnalysis::beta_rayleigh,
                       "Rayleigh beta coefficient (stiffness-proportional damping).")
        .def("solve", &DynamicAnalysis::solve,
             "Runs the dynamic analysis. Returns True if it completed without errors.")
        .def("solve_time_domain_dynamic",
             &DynamicAnalysis::solve_time_domain_dynamic,
             py::arg("duration") = 20.0, py::arg("dt") = 0.05,
             py::arg("amp") = 2.5, py::arg("period") = 10.0,
             py::arg("alpha") = 0.05, py::arg("beta") = 0.01,
             "Runs the Newmark-beta solver with the given parameters.")
        // Convenient static factory method
        .def_static("from_static", [](const Analysis& sa) {
            return new DynamicAnalysis(sa);
        }, py::arg("static_analysis"),
        py::return_value_policy::take_ownership,
        "Creates a DynamicAnalysis copying the state of a solved StaticAnalysis.");

    // =========================================================================
    // SimulationExporter
    // =========================================================================
    py::class_<SimulationExporter>(m, "SimulationExporter",
        "Exports simulation results to HDF5.")
        .def_static("export_hdf5",
            [](const Analysis& sa, const Analysis& da, double seabed_depth, double water_surface_z, const std::string& fn) {
                return SimulationExporter::export_hdf5(sa, da, seabed_depth, water_surface_z, fn);
            },
            py::arg("static_analysis"), py::arg("dynamic_analysis"),
            py::arg("seabed_depth") = -100.0, py::arg("water_surface_z") = 0.0,
            py::arg("filename") = "catenary_results.h5",
            "Exports the full history (static + dynamic) to binary HDF5, including the seabed/water-surface "
            "plane Z levels used by the 3D viewer.");

    // =========================================================================
    // Module-level utility functions
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
            // Fix the top and bottom nodes
            nodes.front()->eq_numbers = {-1, -1, -1, -1, -1, -1};
            nodes.back()->eq_numbers  = {-1, -1, -1, -1, -1, -1};
            // Intermediate nodes: free translations, fixed rotations
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
        R"(Creates a node mesh in the initial catenary configuration.

Returns a list of heap-allocated Node3D* (ownership transferred to Python).
Top (front) and bottom (back) nodes are fixed automatically.
Intermediate nodes have free translations and fixed rotations.

Standalone utility, independent of RiserModel: these nodes are owned directly by
Python, not by a model, so they can't be assigned into RiserModel.nodes (read-only --
see RiserModel.add_node()). Useful for quick introspection/scripting without a full model.

Args:
    num_elements: number of finite elements
    total_length_m: unstretched line length (m)
    depth_m: water depth (m, positive -- the function applies the sign)
    span_x_m: horizontal span of the catenary (m)
)");

    m.def("build_catenary_elements",
        [](std::vector<Node3D*>& nodes, const BeamMaterialProps& props) -> std::vector<CorotationalBeam3D*> {
            if (nodes.size() < 2) return {};
            int num_elements = static_cast<int>(nodes.size()) - 1;
            double L_elem = (nodes.back()->coords - nodes.front()->coords).norm()
                            / static_cast<double>(num_elements);
            // Uses the length along the (piecewise-linear) catenary
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
        R"(Creates beam elements between the given nodes.

Args:
    nodes: list of Node3D* (must have at least 2)
    props: BeamMaterialProps with the material properties

Returns a list of CorotationalBeam3D* (ownership transferred to Python).
Each element's unstretched length is computed automatically.

Standalone utility, independent of RiserModel -- see the note on build_catenary_nodes().
)");

    // Module version
    m.attr("__version__") = "1.0.0";
    m.attr("__author__")  = "riserSim Team";
}
