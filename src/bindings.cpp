#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/eigen.h>

#include "risersim/node.hpp"
#include "risersim/element_beam.hpp"
#include "risersim/solver.hpp"

namespace py = pybind11;

PYBIND11_MODULE(risersim, m) {
    m.doc() = "riserSim: Open-Source Offshore Riser & Pipe Structural Analysis Engine";

    py::class_<risersim::Node3D>(m, "Node3D")
        .def(py::init<int, double, double, double>(), py::arg("id"), py::arg("x"), py::arg("y"), py::arg("z"))
        .def_readwrite("id", &risersim::Node3D::id)
        .def_readwrite("coords", &risersim::Node3D::coords)
        .def_readwrite("disp", &risersim::Node3D::disp)
        .def_readwrite("rot", &risersim::Node3D::rot)
        .def("current_coords", &risersim::Node3D::current_coords)
        .def("fix_dofs", [](risersim::Node3D& self, bool ux, bool uy, bool uz, bool rx, bool ry, bool rz) {
            if (ux) self.eq_numbers[0] = -1; else self.eq_numbers[0] = 0;
            if (uy) self.eq_numbers[1] = -1; else self.eq_numbers[1] = 0;
            if (uz) self.eq_numbers[2] = -1; else self.eq_numbers[2] = 0;
            if (rx) self.eq_numbers[3] = -1; else self.eq_numbers[3] = 0;
            if (ry) self.eq_numbers[4] = -1; else self.eq_numbers[4] = 0;
            if (rz) self.eq_numbers[5] = -1; else self.eq_numbers[5] = 0;
        });

    py::class_<risersim::BeamMaterialProps>(m, "BeamMaterialProps")
        .def(py::init<>())
        .def_readwrite("E", &risersim::BeamMaterialProps::E)
        .def_readwrite("G", &risersim::BeamMaterialProps::G)
        .def_readwrite("A", &risersim::BeamMaterialProps::A)
        .def_readwrite("IY", &risersim::BeamMaterialProps::IY)
        .def_readwrite("IZ", &risersim::BeamMaterialProps::IZ)
        .def_readwrite("J", &risersim::BeamMaterialProps::J)
        .def_readwrite("rho", &risersim::BeamMaterialProps::rho);

    py::class_<risersim::CorotationalBeam3D>(m, "CorotationalBeam3D")
        .def(py::init<int, risersim::Node3D*, risersim::Node3D*, const risersim::BeamMaterialProps&>())
        .def_readwrite("id", &risersim::CorotationalBeam3D::id)
        .def_readwrite("tension_effective", &risersim::CorotationalBeam3D::tension_effective)
        .def("current_length", &risersim::CorotationalBeam3D::current_length)
        .def("global_stiffness", &risersim::CorotationalBeam3D::global_stiffness);

    py::class_<risersim::StaticAnalysis>(m, "StaticAnalysis")
        .def(py::init<>())
        .def_readwrite("nodes", &risersim::StaticAnalysis::nodes)
        .def_readwrite("elements", &risersim::StaticAnalysis::elements)
        .def("solve_step", &risersim::StaticAnalysis::solve_step, py::arg("F_ext"), py::arg("max_iter") = 20, py::arg("tol") = 1e-6);
}
