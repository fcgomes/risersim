/**
 * @file static_analysis.cpp
 * @brief StaticAnalysis: incremental load stepping, full-step Newton-Raphson, and an optional backtracking line search.
 */
#include "risersim/static_analysis.hpp"
#include "risersim/static_integrator.hpp"
#include "risersim/rotation_utils.hpp"
#include "risersim/convergence_test.hpp"
#include <iostream>
#include <iomanip>
#include <cmath>
#include <cstdlib>
#include <vector>
#include <functional>

namespace risersim {

namespace {

/** @brief Per-node state saved/restored around a line-search trial step. */
struct NodeStateSnapshot {
    Eigen::Vector3d disp, rot;
    Eigen::Vector2d friction_force;
};

/// Function signature shared by StaticIntegrator::assemble_stiffness_and_internal_forces()
/// and Analysis::assemble_system(), used to try trial steps without depending on a
/// concrete integrator type.
using AssembleFn = std::function<void(int, Eigen::SparseMatrix<double>&, Eigen::VectorXd&)>;

/**
 * @brief Applies this iteration's Newton-Raphson increment with a simple backtracking line search.
 *
 * Tries the full step (alpha=1); if the residual does *not* decrease (within a generous
 * tolerance), halves alpha and retries, up to a maximum number of attempts -- after that it
 * accepts the smallest alpha tried anyway, to guarantee progress and never stall. Real ANFLEX
 * has *no* such technique (`newton_raphson.cpp` always applies the full step, unchecked); this
 * is a deliberate risersim robustness addition, motivated by the "chattering" pattern (residual
 * oscillating/stuck near a contact or friction discontinuity, never converging nor exploding in a
 * sustained way) found while investigating combined seabed+current loading -- see
 * `risersim/docs/mapa_classes_anflex_estatica.md`.
 *
 * The acceptance tolerance is deliberately loose (100x norm_R_before): the goal is not to chase
 * monotonic residual decrease (full-step Newton legitimately worsens the residual for several
 * intermediate iterations far from the solution, and that self-corrects normally; too tight a
 * threshold steers the solver into a trajectory of artificially small steps that, in
 * well-behaved cases, makes the final result worse instead of helping -- see the "regression"
 * documented in `mapa_classes_anflex_estatica.md` after testing a strict threshold). This is a
 * safety net against genuine catastrophic (order-of-magnitude) blow-up, not a replacement for the
 * normal full step.
 *
 * @param assemble_fn Callback to (re-)assemble K/F_int for a trial displacement state.
 * @param model The model being solved (node states are mutated in place during trials).
 * @param F_ext Current external load vector.
 * @param step_dU Full Newton-Raphson increment for this iteration (all DOFs).
 * @param norm_R_before Residual norm before this step, used as the backtracking baseline.
 * @param iter Current iteration index, forwarded to `assemble_fn` (e.g. for artificial-stiffness decay).
 * @param excluded_node Node to leave untouched (e.g. a node under prescribed motion), or nullptr.
 * @return The accepted (possibly scaled-down) displacement increment.
 */
Eigen::VectorXd apply_newton_step_with_line_search(const AssembleFn& assemble_fn,
                                                    RiserModel* model,
                                                    const Eigen::VectorXd& F_ext,
                                                    const Eigen::VectorXd& step_dU,
                                                    double norm_R_before,
                                                    int iter,
                                                    Node3D* excluded_node) {
    std::vector<NodeStateSnapshot> snapshot(model->nodes.size());
    for (size_t k = 0; k < model->nodes.size(); ++k) {
        snapshot[k] = {model->nodes[k]->disp, model->nodes[k]->rot, model->nodes[k]->friction_force};
    }

    auto apply_scaled = [&](double alpha) {
        for (const auto& node_ptr : model->nodes) {
            Node3D* node = node_ptr.get();
            if (node == excluded_node) continue;
            Eigen::Vector3d delta_rot = Eigen::Vector3d::Zero();
            bool has_rot_dof = false;
            for (int i = 0; i < 3; ++i) {
                int eq = node->eq_numbers[i];
                double du = (eq >= 0) ? alpha * step_dU[eq] : 0.0;
                if (eq >= 0) node->disp[i] += du;
                if (i < 2) node->delta_disp_xy[i] = du; // consumed by the seabed friction spring
                int eq_rot = node->eq_numbers[i + 3];
                if (eq_rot >= 0) { delta_rot[i] = alpha * step_dU[eq_rot]; has_rot_dof = true; }
            }
            if (has_rot_dof) node->rot = compose_rotations(node->rot, delta_rot);
        }
    };
    auto restore = [&]() {
        for (size_t k = 0; k < model->nodes.size(); ++k) {
            model->nodes[k]->disp = snapshot[k].disp;
            model->nodes[k]->rot = snapshot[k].rot;
            model->nodes[k]->friction_force = snapshot[k].friction_force;
        }
    };

    const int max_backtracks = 5;
    double alpha = 1.0;
    Eigen::VectorXd accepted_dU;
    for (int bt = 0; bt <= max_backtracks; ++bt) {
        restore();
        apply_scaled(alpha);
        for (const auto& elem : model->elements) elem->update_effective_tension();

        Eigen::SparseMatrix<double> K_trial;
        Eigen::VectorXd F_int_trial;
        assemble_fn(iter, K_trial, F_int_trial);
        double norm_trial = (F_ext - F_int_trial).norm();

        // Deliberately loose tolerance (100x) -- see the rationale in this function's
        // docstring above: this is a safety net against catastrophic blow-up, not a
        // monotonic-decrease line search.
        if (norm_trial <= norm_R_before * 100.0 || bt == max_backtracks) {
            accepted_dU = alpha * step_dU;
            break;
        }
        alpha *= 0.5;
    }

    // This function's assemble_fn calls already consumed delta_disp_xy to update
    // node->friction_force (incremental elastic-plastic, see seabed.hpp). Reset it so
    // the next assemble (at the top of the following iteration, outside this function)
    // doesn't reapply the SAME increment a second time -- without this, the friction
    // force gets double-counted every iteration.
    for (const auto& node : model->nodes) node->delta_disp_xy.setZero();

    return accepted_dU;
}

/**
 * @brief Snapshots the model's current state (node positions + per-element results) into a
 * StepSnapshot, for a given step index/load factor.
 *
 * Extracted so `StaticAnalysis::solve()` can capture the model's TRUE pristine geometry (0%
 * load, before any Newton iteration in either phase) once, up front -- see the comment in
 * `solve()` for why this matters when phase 1 (assembly) fails to converge.
 */
StepSnapshot capture_snapshot(RiserModel* model, int step_index, double load_factor) {
    StepSnapshot snap;
    snap.step_index = step_index;
    snap.load_factor = load_factor;
    for (const auto& node : model->nodes) snap.node_coords.push_back(node->current_coords());
    for (size_t i = 0; i < model->elements.size(); ++i) {
        auto* elem = model->elements[i].get();
        const auto* prev = (i > 0) ? model->elements[i - 1].get() : nullptr;
        const auto* next = (i + 1 < model->elements.size()) ? model->elements[i + 1].get() : nullptr;

        elem->update_effective_tension();
        auto sc = elem->compute_stress_and_curvature(prev, next);
        snap.element_tensions_kN.push_back(elem->tension_effective / 1000.0);
        snap.element_bending_moments_kNm.push_back(sc.bending_moment_kNm);
        snap.element_curvatures.push_back(sc.curvature);
        snap.element_von_mises_MPa.push_back(sc.von_mises_MPa);
        snap.element_mbr_safety_factors.push_back(sc.mbr_safety_factor);
    }
    return snap;
}

} // namespace

bool StaticAnalysis::solve() {
    // Captures the model's TRUE pristine geometry (0% load, before any Newton iteration in
    // either phase) once, here -- before phase 1 gets a chance to touch node->disp.
    //
    // Why this matters: node->disp is deliberately NOT reset between phase 1 and phase 2 (see
    // the comment below -- phase 2 is meant to continue from wherever phase 1 left off, even if
    // phase 1 didn't converge). But solve_catenary_static() *always* clears `history` and
    // (re)captures its own "step 0" at its own start, from whatever the model's state happens to
    // be at that moment. So when phase 1 fails to converge, phase 2's own "step 0" capture isn't
    // the true 0%-load geometry -- it's phase 1's last (possibly badly diverged) failed Newton
    // iterate, mislabeled `load_factor: 0.0` in the exported/viewed results. This produced a
    // visibly wrong, self-intersecting "line" in the 3D viewer for the Exemplo_02a case
    // (mapa_classes_anflex_estatica.md) that had nothing to do with the actual input geometry.
    StepSnapshot true_step0 = capture_snapshot(model, 0, 0.0);

    // Two-phase solve, mirroring real ANFLEX (cAnflexAnalysis::solve_assembly() ->
    // solve_static()). Phase 1 ("assembly") gives the mesh artificial stiffness
    // available on EVERY load step (not just the first) to find an approximate
    // equilibrium configuration; phase 2 ("static") starts from that state and
    // solves cleanly, without artificial stiffness, with the full load in a single
    // step (the geometry is already consistent with 100% of the load at the end of
    // phase 1 -- repeating the load ramp in phase 2 would reintroduce the same
    // mismatch that broke the earlier external warm-start attempt).
    std::cout << "\n--- Phase 1/2: Assembly (artificial stiffness on every step) ---" << std::endl;
    bool ok_assembly = solve_catenary_static(load_steps, max_iter_per_step, tol, ArtificialStiffnessMode::EveryStep);
    if (!ok_assembly) {
        std::cout << "WARNING: assembly phase did not fully converge -- proceeding to the static phase "
                     "from the state reached (the assembly phase is a pre-solve, it doesn't need to be perfect)."
                  << std::endl;
    }

    std::cout << "\n--- Phase 2/2: Static (no artificial stiffness, full load in 1 step) ---" << std::endl;
    bool ok_static = solve_catenary_static(1, max_iter_per_step, tol, ArtificialStiffnessMode::Never);

    // Restores the true pristine geometry as "step 0" in the exported/viewed history -- see the
    // comment above `true_step0` for why history[0] (phase 2's own step0 capture) can't be
    // trusted to actually be the 0%-load state whenever phase 1 didn't fully converge.
    if (!history.empty()) {
        history[0] = true_step0;
    } else {
        history.push_back(true_step0);
    }

    if (!ok_static) return false;

    if (enable_offset) {
        bool ok_offset = solve_vessel_offset(offset, load_steps, max_iter_per_step, tol);
        if (!ok_offset) return false;
    }

    return true;
}

bool StaticAnalysis::solve_catenary_static(int steps, int max_iter, double tolerance, ArtificialStiffnessMode artif_mode) {
    std::cout << "\n=========================================================================" << std::endl;
    std::cout << "  riserSim Static Non-Linear Catenary Equilibrium Solver" << std::endl;
    std::cout << "=========================================================================" << std::endl;

    assign_equation_numbers();
    history.clear();

    if (!model) return false;

    // Step 0: Initial Geometry (0% Load). Note: this is "0% load relative to whatever state the
    // model is in right now" -- if this is phase 2 of StaticAnalysis::solve() and phase 1 didn't
    // converge, this is NOT the true pristine input geometry (see the comment in solve() around
    // `true_step0`, which overwrites history[0] with the real thing once both phases finish).
    history.push_back(capture_snapshot(model, 0, 0.0));

    // Total reference force at 100% load, used for a strict normalization
    Eigen::VectorXd F_total_ref = Eigen::VectorXd::Zero(num_dofs);
    for (const auto& elem : model->elements) {
        double L = elem->initial_length;
        double g = 9.81;
        double w_dry = (elem->props.rho * elem->props.A + elem->props.rho_fluid * elem->inner_area()) * g;
        double w_buoyancy = water_density * elem->outer_area() * g;
        double elem_weight_total = (w_dry - w_buoyancy) * L;

        int eq1_z = elem->node1->eq_numbers[2];
        int eq2_z = elem->node2->eq_numbers[2];

        if (eq1_z >= 0) F_total_ref[eq1_z] -= elem_weight_total * 0.5;
        if (eq2_z >= 0) F_total_ref[eq2_z] -= elem_weight_total * 0.5;
    }
    double norm_F_ref = F_total_ref.norm() + 1.0;

    StaticIntegrator integrator(this);

    // Passo 4 do roadmap de modernização (mapa_classes_anflex_estatica.md): translation/rotation
    // increment ratios go through the same ConvergenceTest class real ANFLEX uses
    // (convergence_test.cpp), instead of ad-hoc inline logic. The four residual-based criteria
    // it also supports (forces/moments norm, max unbalanced force/moment) are implemented but
    // left disabled (ConvergenceConfig defaults), so behavior here is unchanged from before.
    ConvergenceConfig convergence_config;
    convergence_config.transl_tol = tolerance;
    convergence_config.rot_tol = tolerance;
    convergence_config.check_unbalanced_force = enable_unbalanced_criteria;
    convergence_config.max_unbalanced_force_tol = unbalanced_force_tol;
    convergence_config.check_unbalanced_moment = enable_unbalanced_criteria;
    convergence_config.max_unbalanced_moment_tol = unbalanced_moment_tol;
    ConvergenceTest convergence_test(convergence_config);

    for (int step = 1; step <= steps; ++step) {
        double load_factor = static_cast<double>(step) / static_cast<double>(steps);
        std::cout << "\n[Static Load Step " << std::setw(2) << step << "/" << steps << "] Load Factor: "
                  << std::fixed << std::setprecision(1) << (load_factor * 100.0) << "%" << std::endl;

        Eigen::VectorXd F_ext = integrator.assemble_load_vector(load_factor);

        // StaticIntegrator decides artificial stiffness via this flag, rather than
        // an inline condition inside the iteration loop -- which lets the same
        // function be reused for both phases of StaticAnalysis::solve().
        switch (artif_mode) {
            case ArtificialStiffnessMode::OnlyFirstStep:
                integrator.artificial_stiffness_enabled = (step == 1);
                break;
            case ArtificialStiffnessMode::EveryStep:
                integrator.artificial_stiffness_enabled = true;
                break;
            case ArtificialStiffnessMode::Never:
                integrator.artificial_stiffness_enabled = false;
                break;
        }

        bool step_converged = false;

        // Accumulates this load step's total displacement (reset every new load step),
        // used by the relative-increment convergence criterion (see below).
        Eigen::VectorXd cumulative_dU = Eigen::VectorXd::Zero(num_dofs);
        convergence_test.start();

        for (int iter = 0; iter < max_iter; ++iter) {
            Eigen::SparseMatrix<double> K_global;
            Eigen::VectorXd F_int;

            integrator.assemble_stiffness_and_internal_forces(iter, K_global, F_int);

            Eigen::VectorXd Residual = F_ext - F_int;
            double norm_R = Residual.norm();
            double rel_R = norm_R / norm_F_ref;

            if (iter % 10 == 0 || rel_R < 1.0e-3) {
                std::cout << "  Iter " << std::setw(3) << iter << " | Residual Norm: "
                          << std::scientific << std::setprecision(4) << norm_R << " | Rel R (ref): " << rel_R << std::defaultfloat << std::endl;
            }

            // Temporary diagnostic (mapa_classes_anflex_estatica.md investigation): identifies
            // which node/DOF carries the largest residual component, to pin down which
            // nonlinearity (contact on/off, friction saturation, ...) is behind a stalled/
            // oscillating residual. Opt-in via env var, zero cost/behavior change otherwise.
            if (std::getenv("RISERSIM_DEBUG_RESIDUAL") && iter >= 15) {
                int worst_eq = -1;
                double worst_val = 0.0;
                for (int k = 0; k < Residual.size(); ++k) {
                    if (std::abs(Residual[k]) > std::abs(worst_val)) { worst_val = Residual[k]; worst_eq = k; }
                }
                for (const auto& node_ptr : model->nodes) {
                    Node3D* nd = node_ptr.get();
                    for (int i = 0; i < 6; ++i) {
                        if (nd->eq_numbers[i] == worst_eq) {
                            static const char* names[6] = {"tx","ty","tz","rx","ry","rz"};
                            double pen = seabed.seabed_depth - nd->current_coords().z();
                            std::cout << "    [DEBUG] worst DOF: node " << nd->id << " " << names[i]
                                      << " = " << worst_val << " | z=" << nd->current_coords().z()
                                      << " pen=" << pen
                                      << " friction=(" << nd->friction_force[0] << "," << nd->friction_force[1] << ")"
                                      << std::endl;
                        }
                    }
                }
            }

            Eigen::VectorXd step_dU;
            LinearSolverStatus lin_status = linear_solver->solve(K_global, Residual, step_dU);
            if (lin_status == LinearSolverStatus::DecompositionFailed) {
                std::cout << "❌ SparseLU decomposition failed at step " << step << "!" << std::endl;
                return false;
            }
            if (lin_status == LinearSolverStatus::SolveFailed) {
                std::cerr << "❌ SparseLU solve failed at step " << step << "!" << std::endl;
                return false;
            }

            // Backtracking line search (see apply_newton_step_with_line_search()):
            // tries the full step, halves it if the residual doesn't decrease.
            // Targets the contact/friction "chattering" found with combined
            // seabed+current loading (mapa_classes_anflex_estatica.md), without
            // requiring a change to the contact/friction formulation itself.
            AssembleFn assemble_fn = [&integrator](int it, Eigen::SparseMatrix<double>& K, Eigen::VectorXd& F) {
                integrator.assemble_stiffness_and_internal_forces(it, K, F);
            };
            Eigen::VectorXd accepted_dU = apply_newton_step_with_line_search(
                assemble_fn, model, F_ext, step_dU, norm_R, iter, nullptr);

            // Real ANFLEX's multi-criterion convergence test (see convergence_test.hpp/.cpp): by
            // default only translation/rotation are active, so this iteration's correction must
            // be small relative to the total displacement already accumulated this step -- not
            // relative to an absolute force residual. This converges even when the force
            // residual hasn't dropped to zero yet (the remaining imbalance is resolved in
            // subsequent load steps, as the real stiffness settles in).
            cumulative_dU += accepted_dU;
            bool iteration_converged = convergence_test.check(*model, accepted_dU, cumulative_dU, Residual, iter, max_iter);

            if (iteration_converged) {
                double ratio_transl = convergence_test.criterion(ConvergenceCriterion::Translation).value;
                double ratio_rot = convergence_test.criterion(ConvergenceCriterion::Rotation).value;
                std::cout << "  ✅ Step " << step << " Converged in " << iter << " iterations! (incremento transl/rot = "
                          << ratio_transl << " / " << ratio_rot << ", norm_R = " << norm_R << " N)" << std::endl;
                step_converged = true;
                history.push_back(capture_snapshot(model, step, load_factor));
                break;
            }
        }

        if (!step_converged) {
            std::cout << "❌ Load Step " << step << " failed to converge after " << max_iter << " iterations." << std::endl;
            return false;
        }
    }

    std::cout << "\n=========================================================================" << std::endl;
    std::cout << "  🎉 STATIC CATENARY ANALYSIS CONVERGED SUCCESSFULLY!" << std::endl;
    std::cout << "=========================================================================\n" << std::endl;

    return true;
}

bool StaticAnalysis::solve_vessel_offset(const VesselOffset& vessel_offset, int steps, int max_iter, double tolerance) {
    offset = vessel_offset;
    enable_offset = true;

    std::cout << "\n=========================================================================" << std::endl;
    std::cout << "  ⚓ STARTING VESSEL OFFSET ANALYSIS" << std::endl;
    std::cout << "=========================================================================\n" << std::endl;

    if (!model) return false;

    Node3D* top_node = model->nodes.front().get();
    Eigen::Vector3d start_disp = top_node->disp;
    std::vector<int> saved_eq_numbers = top_node->eq_numbers;

    // Passo 7 do roadmap de modernização (mapa_classes_anflex_estatica.md): em vez de escrever
    // top_node->disp diretamente fora do sistema linear (a técnica antiga -- um Dirichlet BC
    // rígido, com o topo permanentemente engastado), o topo vira um GDL livre de verdade, guiado
    // por uma mola de penalidade (PrescribedMotion, ver prescribed_motion.hpp) -- o mesmo
    // mecanismo que o ANFLEX real usa para movimento imposto (cLoad + "big number"), genérico o
    // bastante para futuramente vir de uma série temporal medida, não só um offset sintético.
    // Restaurado para engastado ao final (ver abaixo), preservando exatamente o contrato de GDL
    // que o resto do código (ex.: DynamicAnalysis) espera do nó de topo.
    top_node->eq_numbers = {0, 1, 2, 3, 4, 5};
    assign_equation_numbers();

    prescribed_motions.clear();
    prescribed_motions.emplace_back(top_node);
    PrescribedMotion& top_motion = prescribed_motions.back();
    top_motion.dof_active = {true, true, true, false, false, false}; // translation only, matches VesselOffset's scope

    bool all_steps_converged = true;

    for (int step = 1; step <= steps && all_steps_converged; ++step) {
        double offset_factor = static_cast<double>(step) / static_cast<double>(steps);
        top_motion.target_disp = start_disp + offset.offset_disp * offset_factor;

        std::cout << "\n[Offset Step " << std::setw(2) << step << "/" << steps << "] Offset Factor: "
                  << std::fixed << std::setprecision(2) << (offset_factor * 100.0) << "% | Target Top Pos X: "
                  << top_motion.target_disp.x() << " m" << std::endl;

        Eigen::VectorXd F_ext = Eigen::VectorXd::Zero(num_dofs);

        for (const auto& elem : model->elements) {
            double L = elem->initial_length;
            double g = 9.81;
            double w_dry = (elem->props.rho * elem->props.A + elem->props.rho_fluid * elem->inner_area()) * g;
            double w_buoyancy = water_density * elem->outer_area() * g;
            double elem_weight_total = (w_dry - w_buoyancy) * L;

            int eq1_z = elem->node1->eq_numbers[2];
            int eq2_z = elem->node2->eq_numbers[2];

            if (eq1_z >= 0) F_ext[eq1_z] -= elem_weight_total * 0.5;
            if (eq2_z >= 0) F_ext[eq2_z] -= elem_weight_total * 0.5;
        }

        bool step_converged = false;
        bool solver_failed = false;

        for (int iter = 0; iter < max_iter; ++iter) {
            Eigen::SparseMatrix<double> K_global;
            Eigen::VectorXd F_int;

            assemble_system(K_global, F_ext, F_int);

            Eigen::VectorXd Residual = F_ext - F_int;
            double norm_R = Residual.norm();
            double norm_F = F_ext.norm() + 1.0;
            double rel_R = norm_R / norm_F;

            if (iter % 10 == 0 || rel_R < 1.0e-4) {
                std::cout << "  Iter " << std::setw(2) << iter << " | Residual Norm: "
                          << std::scientific << std::setprecision(4) << norm_R << " | Rel R: " << rel_R << std::defaultfloat << std::endl;
            }

            if (rel_R < 1.0e-4) {
                std::cout << "  ✅ Offset Step " << step << " Converged in " << iter << " iterations! (Top Pos X: "
                          << top_node->disp.x() << " m)" << std::endl;
                step_converged = true;

                StepSnapshot snap;
                snap.step_index = steps + step;
                snap.load_factor = 1.0 + offset_factor;
                for (const auto& node : model->nodes) snap.node_coords.push_back(node->current_coords());
                for (size_t i = 0; i < model->elements.size(); ++i) {
                    auto* elem = model->elements[i].get();
                    const auto* prev = (i > 0) ? model->elements[i - 1].get() : nullptr;
                    const auto* next = (i + 1 < model->elements.size()) ? model->elements[i + 1].get() : nullptr;

                    elem->update_effective_tension();
                    auto sc = elem->compute_stress_and_curvature(prev, next);
                    snap.element_tensions_kN.push_back(elem->tension_effective / 1000.0);
                    snap.element_bending_moments_kNm.push_back(sc.bending_moment_kNm);
                    snap.element_curvatures.push_back(sc.curvature);
                    snap.element_von_mises_MPa.push_back(sc.von_mises_MPa);
                    snap.element_mbr_safety_factors.push_back(sc.mbr_safety_factor);
                }
                history.push_back(snap);
                break;
            }

            Eigen::VectorXd step_dU;
            LinearSolverStatus lin_status = linear_solver->solve(K_global, Residual, step_dU);
            if (lin_status == LinearSolverStatus::DecompositionFailed) {
                std::cout << "❌ SparseLU decomposition failed at offset step " << step << "!" << std::endl;
                solver_failed = true;
                break;
            }

            // No longer excludes top_node: it's a genuine free DOF now (held by the penalty
            // spring, not by direct assignment), so it must receive Newton corrections like any
            // other free node.
            AssembleFn assemble_fn = [this](int, Eigen::SparseMatrix<double>& K, Eigen::VectorXd& F) {
                Eigen::VectorXd F_ext_unused;
                this->assemble_system(K, F_ext_unused, F);
            };
            apply_newton_step_with_line_search(assemble_fn, model, F_ext, step_dU, norm_R, iter, nullptr);
        }

        if (solver_failed) {
            all_steps_converged = false;
        } else if (!step_converged) {
            std::cout << "❌ Offset Step " << step << " failed to converge after " << max_iter << " iterations." << std::endl;
            all_steps_converged = false;
        }
    }

    // Restaura o nó de topo ao seu contrato original de GDL (engastado), congelando a posição de
    // offset convergida como um valor de Dirichlet de novo -- consistente com o que o resto do
    // código (ex.: DynamicAnalysis, que reusa o mesmo model) espera do nó de topo.
    top_node->eq_numbers = saved_eq_numbers;
    prescribed_motions.clear();
    assign_equation_numbers();

    return all_steps_converged;
}

} // namespace risersim
