#ifndef RISERSIM_SOLVER_HPP
#define RISERSIM_SOLVER_HPP

#include "risersim/node.hpp"
#include "risersim/element_beam.hpp"
#include "risersim/seabed.hpp"
#include "risersim/current_profile.hpp"
#include "risersim/vessel_offset.hpp"
#include <vector>
#include <string>
#include <Eigen/Sparse>

namespace risersim {

struct StepSnapshot {
    int step_index;
    double load_factor;
    std::vector<Eigen::Vector3d> node_coords;
    std::vector<double> element_tensions_kN;
};

class StaticAnalysis {
public:
    std::vector<Node3D*> nodes;
    std::vector<CorotationalBeam3D*> elements;
    SeabedInteraction seabed;
    CurrentProfile current;
    bool enable_current;
    double water_density;
    int num_dofs;

    std::vector<StepSnapshot> step_history;

    StaticAnalysis() : seabed(-80.0), enable_current(false), water_density(1025.0), num_dofs(0) {}

    void assign_equation_numbers() {
        num_dofs = 0;
        for (auto* node : nodes) {
            for (int i = 0; i < 6; ++i) {
                if (node->eq_numbers[i] >= 0) {
                    node->eq_numbers[i] = num_dofs++;
                }
            }
        }
    }

    void assemble_system(Eigen::SparseMatrix<double>& K_global, const Eigen::VectorXd& F_ext, Eigen::VectorXd& F_int);
    bool solve_catenary_static(int load_steps = 10, int max_iter_per_step = 150, double tol = 1.0e-3);
    bool solve_vessel_offset(const VesselOffset& offset, int steps = 10, int max_iter = 100, double tol = 1.0e-3);
    bool export_json(const std::string& filename = "catenary_results.json") const;
};

} // namespace risersim

#endif
