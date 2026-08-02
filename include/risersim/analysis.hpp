#ifndef RISERSIM_ANALYSIS_HPP
#define RISERSIM_ANALYSIS_HPP

#include "risersim/node.hpp"
#include "risersim/element_beam.hpp"
#include "risersim/seabed.hpp"
#include "risersim/current_profile.hpp"
#include "risersim/snapshot.hpp"
#include <vector>
#include <string>
#include <Eigen/Sparse>

namespace risersim {

class Analysis {
public:
    std::vector<Node3D*> nodes;
    std::vector<CorotationalBeam3D*> elements;
    SeabedInteraction seabed;
    CurrentProfile current;
    bool enable_current;
    double water_density;
    int num_dofs;

    std::vector<StepSnapshot> history;

    Analysis() : seabed(-80.0), enable_current(false), water_density(1025.0), num_dofs(0) {}
    virtual ~Analysis() = default;

    virtual void assign_equation_numbers() {
        num_dofs = 0;
        for (auto* node : nodes) {
            for (int i = 0; i < 6; ++i) {
                if (node->eq_numbers[i] >= 0) {
                    node->eq_numbers[i] = num_dofs++;
                }
            }
        }
    }

    virtual void assemble_system(Eigen::SparseMatrix<double>& K_global, const Eigen::VectorXd& F_ext, Eigen::VectorXd& F_int);

    // Uniform polymorphic solve method
    virtual bool solve() = 0;
};

} // namespace risersim

#endif
