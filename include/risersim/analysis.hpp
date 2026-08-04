#ifndef RISERSIM_ANALYSIS_HPP
#define RISERSIM_ANALYSIS_HPP

#include "risersim/model.hpp"
#include "risersim/seabed.hpp"
#include "risersim/current_profile.hpp"
#include "risersim/snapshot.hpp"
#include "risersim/linear_solver.hpp"
#include <vector>
#include <string>
#include <memory>
#include <Eigen/Sparse>

namespace risersim {

class Analysis {
public:
    RiserModel* model;
    SeabedInteraction seabed;
    CurrentProfile current;
    bool enable_current;
    double water_density;           // Para empuxo no F_ext (0 quando rho já embute peso submerso)
    double water_density_for_mass;  // Para massa adicionada na M e drag (sempre 1025.0)
    int num_dofs;

    std::vector<StepSnapshot> history;

    // Backend do solver linear (Passo 1 do roadmap de modernização) — default
    // Eigen::SparseLU via EigenSparseLUSolver, trocável por outro backend
    // (ex.: MKL/Pardiso) sem alterar o código que consome esta interface.
    std::unique_ptr<LinearSolver> linear_solver;

    Analysis()
        : model(nullptr), seabed(-80.0), enable_current(false), water_density(1025.0),
          water_density_for_mass(1025.0), num_dofs(0),
          linear_solver(std::make_unique<EigenSparseLUSolver>()) {}
    virtual ~Analysis() = default;

    virtual void assign_equation_numbers() {
        num_dofs = 0;
        if (!model) return;
        for (auto* node : model->nodes) {
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
