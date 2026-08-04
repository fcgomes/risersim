/**
 * @file linear_solver.cpp
 * @brief Implementations of the two LinearSolver backends: general LU and symmetric LDL^T.
 */
#include "risersim/linear_solver.hpp"

namespace risersim {

LinearSolverStatus EigenSparseLUSolver::solve(const Eigen::SparseMatrix<double>& K,
                                               const Eigen::VectorXd& b,
                                               Eigen::VectorXd& x) {
    Eigen::SparseLU<Eigen::SparseMatrix<double>> solver;
    solver.compute(K);
    if (solver.info() != Eigen::Success) {
        return LinearSolverStatus::DecompositionFailed;
    }

    x = solver.solve(b);
    if (solver.info() != Eigen::Success) {
        return LinearSolverStatus::SolveFailed;
    }

    return LinearSolverStatus::Success;
}

LinearSolverStatus EigenSimplicialLDLTSolver::solve(const Eigen::SparseMatrix<double>& K,
                                                     const Eigen::VectorXd& b,
                                                     Eigen::VectorXd& x) {
    Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> solver;
    solver.compute(K);
    if (solver.info() != Eigen::Success) {
        return LinearSolverStatus::DecompositionFailed;
    }

    x = solver.solve(b);
    if (solver.info() != Eigen::Success) {
        return LinearSolverStatus::SolveFailed;
    }

    return LinearSolverStatus::Success;
}

} // namespace risersim
