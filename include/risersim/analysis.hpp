/**
 * @file analysis.hpp
 * @brief Abstract base for a riser/mooring analysis run (system assembly, equation numbering, solve loop).
 */
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

/**
 * @brief Owns a model plus its environment (seabed, current) and drives one analysis run, mirroring ANFLEX's `cAnflexAnalysis`.
 *
 * StaticAnalysis and DynamicAnalysis are the concrete implementations of solve().
 */
class Analysis {
public:
    RiserModel* model;
    SeabedInteraction seabed;
    CurrentProfile current;
    bool enable_current;
    double water_density;           ///< Buoyancy contribution to F_ext (0 when rho already embeds submerged weight).
    double water_density_for_mass;  ///< Added mass in M and drag loading (always 1025.0).
    int num_dofs;

    std::vector<StepSnapshot> history;

    /**
     * @brief Linear solver backend (see linear_solver.hpp), swappable without touching code that consumes this interface.
     */
    std::unique_ptr<LinearSolver> linear_solver;

    Analysis()
        : model(nullptr), seabed(-80.0), enable_current(false), water_density(1025.0),
          water_density_for_mass(1025.0), num_dofs(0),
          linear_solver(std::make_unique<EigenSimplicialLDLTSolver>()) {}
    virtual ~Analysis() = default;

    /**
     * @brief Numbers global DOF equations following Reverse Cuthill-McKee (RCM) order, not `model->nodes` order.
     *
     * Reduces the bandwidth of the assembled global stiffness matrix, mirroring ANFLEX's real
     * default (`model_builder_dat.cpp`: reorderer default = `"reverse_cuthill_mckee"`, applied
     * before assembly regardless of the chosen solver). Only affects equation numbering --
     * `model->nodes` stays in its original order for everything else (output, iteration, etc.).
     */
    virtual void assign_equation_numbers() {
        num_dofs = 0;
        if (!model) return;
        for (int idx : compute_rcm_order(*model)) {
            Node3D* node = model->nodes[idx].get();
            for (int i = 0; i < 6; ++i) {
                if (node->eq_numbers[i] >= 0) {
                    node->eq_numbers[i] = num_dofs++;
                }
            }
        }
    }

    /**
     * @brief Assembles the global tangent stiffness, external load, and internal force vectors for the model's current state.
     * @param[out] K_global Global tangent stiffness matrix.
     * @param F_ext External load vector (weight, buoyancy, current drag, prescribed motions).
     * @param[out] F_int Global internal (resisting) force vector.
     */
    virtual void assemble_system(Eigen::SparseMatrix<double>& K_global, const Eigen::VectorXd& F_ext, Eigen::VectorXd& F_int);

    /** @brief Runs this analysis to completion. @return true on convergence/success. */
    virtual bool solve() = 0;
};

} // namespace risersim

#endif
