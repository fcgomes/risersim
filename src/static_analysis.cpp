#include "risersim/static_analysis.hpp"
#include "risersim/static_integrator.hpp"
#include <iostream>
#include <iomanip>
#include <cmath>

namespace risersim {

bool StaticAnalysis::solve() {
    // Passo 3 do roadmap de modernização (risersim/docs/mapa_classes_anflex_estatica.md):
    // duas fases, igual ao ANFLEX real (cAnflexAnalysis::solve_assembly() ->
    // solve_static()). Fase 1 ("assembly") dá à malha rigidez artificial
    // disponível em TODOS os passos de carga (não só o primeiro) para achar
    // uma configuração de equilíbrio aproximada; fase 2 ("static") parte
    // desse estado e resolve limpo, sem rigidez artificial, com a carga
    // total num único passo (a geometria já está consistente com 100% da
    // carga ao final da fase 1 — repetir o ramp de carga na fase 2
    // reintroduziria o mesmo descompasso que quebrou a tentativa de warm
    // start externo).
    std::cout << "\n--- Fase 1/2: Assembly (rigidez artificial em todos os passos) ---" << std::endl;
    bool ok_assembly = solve_catenary_static(load_steps, max_iter_per_step, tol, ArtificialStiffnessMode::EveryStep);
    if (!ok_assembly) {
        std::cout << "⚠️ Fase de assembly não convergiu totalmente — prosseguindo para a fase estática "
                     "a partir do estado alcançado (a fase de assembly é um pré-solve, não precisa ser perfeita)."
                  << std::endl;
    }

    std::cout << "\n--- Fase 2/2: Static (sem rigidez artificial, carga total em 1 passo) ---" << std::endl;
    bool ok_static = solve_catenary_static(1, max_iter_per_step, tol, ArtificialStiffnessMode::Never);
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

    // Step 0: Initial Geometry (0% Load)
    StepSnapshot step0;
    step0.step_index = 0;
    step0.load_factor = 0.0;
    for (auto* node : model->nodes) step0.node_coords.push_back(node->current_coords());
    for (size_t i = 0; i < model->elements.size(); ++i) {
        auto* elem = model->elements[i];
        const auto* prev = (i > 0) ? model->elements[i - 1] : nullptr;
        const auto* next = (i + 1 < model->elements.size()) ? model->elements[i + 1] : nullptr;

        elem->update_effective_tension();
        auto sc = elem->compute_stress_and_curvature(prev, next);
        step0.element_tensions_kN.push_back(elem->tension_effective / 1000.0);
        step0.element_bending_moments_kNm.push_back(sc.bending_moment_kNm);
        step0.element_curvatures.push_back(sc.curvature);
        step0.element_von_mises_MPa.push_back(sc.von_mises_MPa);
        step0.element_mbr_safety_factors.push_back(sc.mbr_safety_factor);
    }
    history.push_back(step0);

    // Força total de referência com 100% da carga para normalização estrita
    Eigen::VectorXd F_total_ref = Eigen::VectorXd::Zero(num_dofs);
    for (auto* elem : model->elements) {
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

    for (int step = 1; step <= steps; ++step) {
        double load_factor = static_cast<double>(step) / static_cast<double>(steps);
        std::cout << "\n[Static Load Step " << std::setw(2) << step << "/" << steps << "] Load Factor: "
                  << std::fixed << std::setprecision(1) << (load_factor * 100.0) << "%" << std::endl;

        Eigen::VectorXd F_ext = integrator.assemble_load_vector(load_factor);

        // StaticIntegrator decide a rigidez artificial por este flag, não
        // mais por uma condição inline dentro do loop de iteração — o que
        // permite reutilizar a mesma função para as duas fases do Passo 3.
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

        // Acumula o deslocamento total do passo (zerado a cada novo load step), usado
        // pelo critério de convergência por incremento relativo (ver abaixo).
        Eigen::VectorXd cumulative_dU = Eigen::VectorXd::Zero(num_dofs);

        // Soma as normas (translação, rotação) de um vetor de incremento restrito aos
        // DOFs livres de cada tipo, igual a cIntegrator::get_translation_inc_norm /
        // get_rotation_inc_norm do ANFLEX real.
        auto split_norms = [&](const Eigen::VectorXd& v, double& transl_norm, double& rot_norm) {
            double t = 0.0, r = 0.0;
            for (auto* node : model->nodes) {
                for (int i = 0; i < 3; ++i) {
                    int eq = node->eq_numbers[i];
                    if (eq >= 0) t += v[eq] * v[eq];
                    int eq_rot = node->eq_numbers[i + 3];
                    if (eq_rot >= 0) r += v[eq_rot] * v[eq_rot];
                }
            }
            transl_norm = std::sqrt(t);
            rot_norm = std::sqrt(r);
        };

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

            // Newton-Raphson puro (passo completo, sem line search), igual ao ANFLEX real
            // (newton_raphson.cpp: aplica m_linear_soe->get_x() diretamente). A rigidez
            // artificial do primeiro passo é o que mantém isso estável.
            for (auto* node : model->nodes) {
                for (int i = 0; i < 3; ++i) {
                    int eq = node->eq_numbers[i];
                    if (eq >= 0) node->disp[i] += step_dU[eq];

                    int eq_rot = node->eq_numbers[i + 3];
                    if (eq_rot >= 0) node->rot[i] += step_dU[eq_rot];
                }
            }

            for (auto* elem : model->elements) {
                elem->update_effective_tension();
            }

            // Critério de convergência multi-critério do ANFLEX real (convergence_test.cpp):
            // a correção desta iteração precisa ser pequena em relação ao deslocamento total
            // já acumulado neste passo — não em relação a um resíduo de força absoluto. Isso
            // converge mesmo quando o resíduo de força ainda não caiu a zero (o desequilíbrio
            // remanescente é resolvido nos passos de carga seguintes, conforme a rigidez real
            // vai se estabelecendo). Exige pelo menos 2 iterações (iter >= 1).
            cumulative_dU += step_dU;
            double this_transl_norm, this_rot_norm, cum_transl_norm, cum_rot_norm;
            split_norms(step_dU, this_transl_norm, this_rot_norm);
            split_norms(cumulative_dU, cum_transl_norm, cum_rot_norm);

            double ratio_transl = (cum_transl_norm > 1.0e-12) ? (this_transl_norm / cum_transl_norm) : 0.0;
            double ratio_rot = (cum_rot_norm > 1.0e-12) ? (this_rot_norm / cum_rot_norm) : 0.0;
            bool transl_converged = ratio_transl < tolerance;
            bool rot_converged = ratio_rot < tolerance;

            if (iter >= 1 && transl_converged && rot_converged) {
                std::cout << "  ✅ Step " << step << " Converged in " << iter << " iterations! (incremento transl/rot = "
                          << ratio_transl << " / " << ratio_rot << ", norm_R = " << norm_R << " N)" << std::endl;
                step_converged = true;

                StepSnapshot snap;
                snap.step_index = step;
                snap.load_factor = load_factor;
                for (auto* node : model->nodes) snap.node_coords.push_back(node->current_coords());
                for (size_t i = 0; i < model->elements.size(); ++i) {
                    auto* elem = model->elements[i];
                    const auto* prev = (i > 0) ? model->elements[i - 1] : nullptr;
                    const auto* next = (i + 1 < model->elements.size()) ? model->elements[i + 1] : nullptr;

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

    Node3D* top_node = model->nodes.front();

    for (int step = 1; step <= steps; ++step) {
        double offset_factor = static_cast<double>(step) / static_cast<double>(steps);
        Eigen::Vector3d current_offset = offset.offset_disp * offset_factor;

        top_node->disp.x() += current_offset.x() / static_cast<double>(steps);
        top_node->disp.y() += current_offset.y() / static_cast<double>(steps);
        top_node->disp.z() += current_offset.z() / static_cast<double>(steps);

        std::cout << "\n[Offset Step " << std::setw(2) << step << "/" << steps << "] Offset Factor: " 
                  << std::fixed << std::setprecision(2) << (offset_factor * 100.0) << "% | Top Pos X: " 
                  << top_node->disp.x() << " m" << std::endl;

        Eigen::VectorXd F_ext = Eigen::VectorXd::Zero(num_dofs);

        for (auto* elem : model->elements) {
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
                std::cout << "  ✅ Offset Step " << step << " Converged in " << iter << " iterations!" << std::endl;
                step_converged = true;

                StepSnapshot snap;
                snap.step_index = steps + step;
                snap.load_factor = 1.0 + offset_factor;
                for (auto* node : model->nodes) snap.node_coords.push_back(node->current_coords());
                for (size_t i = 0; i < model->elements.size(); ++i) {
                    auto* elem = model->elements[i];
                    const auto* prev = (i > 0) ? model->elements[i - 1] : nullptr;
                    const auto* next = (i + 1 < model->elements.size()) ? model->elements[i + 1] : nullptr;

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
                return false;
            }

            for (auto* node : model->nodes) {
                if (node == top_node) continue;
                for (int i = 0; i < 3; ++i) {
                    int eq = node->eq_numbers[i];
                    if (eq >= 0) node->disp[i] += step_dU[eq];

                    int eq_rot = node->eq_numbers[i + 3];
                    if (eq_rot >= 0) node->rot[i] += step_dU[eq_rot];
                }
            }

            for (auto* elem : model->elements) {
                elem->update_effective_tension();
            }
        }

        if (!step_converged) {
            std::cout << "❌ Offset Step " << step << " failed to converge after " << max_iter << " iterations." << std::endl;
            return false;
        }
    }

    return true;
}

} // namespace risersim
