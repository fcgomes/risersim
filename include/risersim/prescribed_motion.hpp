/**
 * @file prescribed_motion.hpp
 * @brief Prescribed node motion via a stiff penalty spring, mirroring ANFLEX's `cLoad` + big-number technique.
 */
#ifndef RISERSIM_PRESCRIBED_MOTION_HPP
#define RISERSIM_PRESCRIBED_MOTION_HPP

#include "risersim/node.hpp"
#include <Eigen/Sparse>
#include <array>

namespace risersim {

/**
 * @brief Drives one or more of a node's 6 DOFs toward a target value via a very stiff penalty spring.
 *
 * Mirrors ANFLEX's real mechanism for imposed motion (`dof.h`'s `sPrescribedDOF`, applied in
 * `cIntegrator::set_load_dofs`/`apply_load_stiffness`): rather than eliminating the DOF (a hard
 * Dirichlet boundary condition -- what `StaticAnalysis::solve_vessel_offset` used to do, moving
 * the top node's `disp` directly outside the linear system), a fictitious spring of stiffness
 * `big_number` pulls the DOF toward `target_disp`/`target_rot` every iteration. ANFLEX picks
 * `big_number` as the model's largest element Young's modulus E (`cBeam::get_big_number()`) --
 * already many orders of magnitude stiffer than any real translational/rotational stiffness term
 * for typical element geometry, without needing a dimensional-analysis-derived constant.
 *
 * This is what lets an arbitrary node/DOF combination carry a prescribed motion (not just one
 * node with all 6 DOFs eliminated), which is the piece needed to later drive a node from a
 * measured/time-series trajectory instead of only a ramped synthetic offset (see roadmap step 7,
 * `docs/mapa_classes_anflex_estatica.md`). `target_disp`/`target_rot` are plain public fields the
 * caller updates directly (e.g. once per load step) -- no time-series/function abstraction is
 * implemented yet, since nothing in risersim needs one today.
 */
class PrescribedMotion {
public:
    Node3D* node; ///< Node this motion applies to (non-owning; must outlive this object).

    /// Which of the 6 DOFs are prescribed: indices 0-2 = translation x/y/z, 3-5 = rotation x/y/z.
    /// A DOF must also have a valid (non-negative) equation number on the node for this to have
    /// any effect -- apply() silently skips DOFs that are `false` here or fixed (-1) on the node.
    std::array<bool, 6> dof_active{};

    Eigen::Vector3d target_disp = Eigen::Vector3d::Zero(); ///< Target translation (m).
    Eigen::Vector3d target_rot = Eigen::Vector3d::Zero();  ///< Target rotation vector (rad).

    explicit PrescribedMotion(Node3D* n) : node(n) {}

    /**
     * @brief Adds this motion's penalty contribution to the global tangent stiffness and internal force.
     *
     * For each active, free DOF: adds `big_number` to the stiffness diagonal (summed with
     * whatever else targets that entry, like any other triplet contribution) and *overwrites*
     * (not adds to) that DOF's internal force with the spring force `(current - target) *
     * big_number` -- mirroring ANFLEX's own direct assignment in `set_load_dofs`, which discards
     * whatever a connected element's own elastic formulation would have contributed to that one
     * row, in favor of the penalty term representing the reaction needed to hold it in place.
     * Must be called after the normal element/soil assembly has already written to `F_int`, so
     * the overwrite happens last.
     *
     * Sign check: with `Residual = F_ext - F_int` and `u_new = u_old + K^-1 * Residual` (risersim's
     * convention throughout), `F_int = (current - target) * big_number` makes the decoupled
     * Newton correction at this DOF exactly `du = target - current` -- i.e. it converges to the
     * target in a single iteration when uncoupled from the rest of the system.
     *
     * @param big_number Penalty stiffness (see class docs).
     * @param[out] triplets Global stiffness triplet list to append to.
     * @param[in,out] F_int Global internal force vector to overwrite at this motion's active DOFs.
     */
    void apply(double big_number, std::vector<Eigen::Triplet<double>>& triplets, Eigen::VectorXd& F_int) const {
        if (!node) return;
        for (int i = 0; i < 6; ++i) {
            if (!dof_active[i]) continue;
            int eq = node->eq_numbers[i];
            if (eq < 0) continue;
            double current = (i < 3) ? node->disp[i] : node->rot[i - 3];
            double target = (i < 3) ? target_disp[i] : target_rot[i - 3];
            triplets.emplace_back(eq, eq, big_number);
            F_int[eq] = (current - target) * big_number;
        }
    }
};

} // namespace risersim

#endif
