#ifndef RISERSIM_ANALYSIS_HPP
#define RISERSIM_ANALYSIS_HPP

#include "risersim/model.hpp"
#include "risersim/seabed.hpp"
#include "risersim/current_profile.hpp"
#include "risersim/snapshot.hpp"
#include "risersim/linear_solver.hpp"
#include "risersim/rcm_reorder.hpp"
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
          linear_solver(std::make_unique<EigenSimplicialLDLTSolver>()) {}
    virtual ~Analysis() = default;

    // Numera os GDLs seguindo a ordem RCM (Reverse Cuthill-McKee), não a
    // ordem de model->nodes -- reduz a banda da matriz de rigidez global,
    // fiel ao ANFLEX real (model_builder_dat.cpp: reorderer default =
    // "reverse_cuthill_mckee", aplicado antes da montagem independente do
    // solver escolhido). Só afeta a numeração das equações -- model->nodes
    // continua na ordem original para tudo mais (saída, iteração, etc.).
    virtual void assign_equation_numbers() {
        num_dofs = 0;
        if (!model) return;
        for (int idx : compute_rcm_order(*model)) {
            Node3D* node = model->nodes[idx];
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
