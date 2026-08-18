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
#include "risersim/prescribed_motion.hpp"
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

    /**
     * @brief Pseudo-time forwarded to every element's `Element::set_time()` at the start of each
     * `assemble_system()` call -- currently only consumed by `WinchElement`'s payout curve. The
     * caller (StaticAnalysis/DynamicAnalysis) is responsible for keeping this current before
     * assembling: StaticAnalysis sets it to the load-step fraction `t` in `[0,1]` (same convention
     * already used for the current-load ramp -- real ANFLEX's own static "current_time" is a
     * step-progression value, not wall-clock time either), DynamicAnalysis sets it to the real
     * elapsed simulation time. Defaults to 0.0 so any caller that never sets it (every analysis
     * with no WinchElement in the model) behaves exactly as before this field existed.
     */
    double current_time = 0.0;

    std::vector<StepSnapshot> history;

    /**
     * @brief Motions actively enforced this assembly, via a stiff penalty spring (see prescribed_motion.hpp).
     *
     * Empty by default -- zero cost/behavior change unless a caller populates it (e.g.
     * StaticAnalysis::solve_vessel_offset()).
     */
    std::vector<PrescribedMotion> prescribed_motions;

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
     * @brief Numbers global DOF equations following Reverse Cuthill-McKee (RCM) order, not `model->nodes()` order.
     *
     * Reduces the bandwidth of the assembled global stiffness matrix, mirroring ANFLEX's real
     * default (`model_builder_dat.cpp`: reorderer default = `"reverse_cuthill_mckee"`, applied
     * before assembly regardless of the chosen solver). Only affects equation numbering --
     * `model->nodes()` stays in its original order for everything else (output, iteration, etc.).
     */
    virtual void assign_equation_numbers() {
        num_dofs = 0;
        if (!model) return;
        for (int idx : compute_rcm_order(*model)) {
            Node3D* node = model->nodes()[idx].get();
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

    /**
     * @brief Tangent stiffness contribution from submersion-dependent buoyancy (see
     * hydrostatics.hpp, docs/roadmap.md item 1b), for the model's CURRENT node positions.
     *
     * Buoyancy's FORCE side lives in each caller's own F_ext assembly (weight+buoyancy have
     * always been computed there, not in assemble_system() -- see static_integrator.cpp,
     * static_analysis.cpp, dynamic_analysis.cpp), so this only returns the matching stiffness,
     * meant to be added on top of assemble_system()'s K_global (`K_global += K_buoyancy`) by any
     * caller that's actually running a Newton iteration (as opposed to e.g. a one-off reference-
     * norm computation, which only needs the force). Diagonal-only (one triplet per node's
     * vertical DOF, no cross term between an element's two ends) -- matches real ANFLEX's own
     * cNL_Hidrostatics usage in beam.cpp, which does the same simplification.
     */
    Eigen::SparseMatrix<double> assemble_buoyancy_stiffness() const;

    /** @brief Runs this analysis to completion. @return true on convergence/success. */
    virtual bool solve() = 0;
};

} // namespace risersim

#endif
