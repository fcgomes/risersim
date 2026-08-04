/**
 * @file linear_solver.hpp
 * @brief Pluggable direct linear solver backend, mirroring ANFLEX's `cLinearSolver`/`cLinearSOE`.
 *
 * Extracts the direct use of `Eigen::SparseLU` behind a pluggable interface. Eigen remains the
 * default backend (open, no dependency on Intel hardware); an optional MKL/Pardiso backend could
 * be added later behind this same interface without touching callers.
 */
#ifndef RISERSIM_LINEAR_SOLVER_HPP
#define RISERSIM_LINEAR_SOLVER_HPP

#include <Eigen/Sparse>

namespace risersim {

/** @brief Outcome of a LinearSolver::solve() call. */
enum class LinearSolverStatus {
    Success,
    DecompositionFailed,
    SolveFailed
};

/** @brief Abstract interface for solving `K * x = b` for the global tangent system. */
class LinearSolver {
public:
    virtual ~LinearSolver() = default;

    /**
     * @brief Solves `K * x = b`.
     * @param K Sparse system matrix.
     * @param b Right-hand side.
     * @param[out] x Solution vector.
     * @return Status; callers decide how to log/react (e.g. "at step N") -- this function prints nothing.
     */
    virtual LinearSolverStatus solve(const Eigen::SparseMatrix<double>& K,
                                      const Eigen::VectorXd& b,
                                      Eigen::VectorXd& x) = 0;
};

/**
 * @brief General (non-symmetric) sparse LU with partial pivoting.
 *
 * Works for any matrix, including indefinite ones, with no extra care required. No longer the
 * default (see EigenSimplicialLDLTSolver), but kept available as a robust fallback in case the
 * system matrix becomes genuinely indefinite in some scenario where LDLT fails.
 */
class EigenSparseLUSolver : public LinearSolver {
public:
    LinearSolverStatus solve(const Eigen::SparseMatrix<double>& K,
                              const Eigen::VectorXd& b,
                              Eigen::VectorXd& x) override;
};

/**
 * @brief Default solver: exploits the symmetry of K_global via LDL^T decomposition (`Eigen::SimplicialLDLT`).
 *
 * Matches what all three ANFLEX backends do (Skyline/MKL/Pardiso are all symmetric solvers) --
 * roughly half the memory/time cost of general LU. `K_global` is always assembled symmetrically
 * (see `analysis.cpp`), but can become **indefinite** during nonlinear iteration (geometric
 * stiffness in compression, contact regions); Eigen's `SimplicialLDLT` does *not* perform
 * numerical pivoting for that (unlike real Pardiso, which uses `mtype=-2` "symmetric indefinite"
 * with MPS scaling/permutation specifically because of this).
 *
 * Validated against the full regression battery (`docs/mapa_classes_anflex_estatica.md`):
 * converges on the same cases as EigenSparseLUSolver, with the same numerical result, and does
 * not fail differently on the already-problematic cases (seabed + current). If that changes in
 * the future (e.g. larger/more indefinite models), switching back to EigenSparseLUSolver is the
 * safe alternative.
 */
class EigenSimplicialLDLTSolver : public LinearSolver {
public:
    LinearSolverStatus solve(const Eigen::SparseMatrix<double>& K,
                              const Eigen::VectorXd& b,
                              Eigen::VectorXd& x) override;
};

} // namespace risersim

#endif // RISERSIM_LINEAR_SOLVER_HPP
