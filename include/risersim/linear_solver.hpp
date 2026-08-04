#ifndef RISERSIM_LINEAR_SOLVER_HPP
#define RISERSIM_LINEAR_SOLVER_HPP

#include <Eigen/Sparse>

namespace risersim {

// Passo 1 do roadmap de modernizacao (risersim/docs/mapa_classes_anflex_estatica.md):
// extrai o uso direto de Eigen::SparseLU para uma interface plugavel, espelhando
// cLinearSolver/cLinearSOE do ANFLEX real. Eigen continua sendo o backend padrao
// (aberto, sem dependencia de hardware Intel); um backend MKL/Pardiso opcional
// pode ser adicionado depois atras desta mesma interface, sem tocar quem a usa.
enum class LinearSolverStatus {
    Success,
    DecompositionFailed,
    SolveFailed
};

class LinearSolver {
public:
    virtual ~LinearSolver() = default;

    // Resolve K * x = b. Preenche x e retorna o status; nao imprime nada --
    // quem chama decide a mensagem/contexto (ex.: "no passo N").
    virtual LinearSolverStatus solve(const Eigen::SparseMatrix<double>& K,
                                      const Eigen::VectorXd& b,
                                      Eigen::VectorXd& x) = 0;
};

class EigenSparseLUSolver : public LinearSolver {
public:
    LinearSolverStatus solve(const Eigen::SparseMatrix<double>& K,
                              const Eigen::VectorXd& b,
                              Eigen::VectorXd& x) override;
};

} // namespace risersim

#endif // RISERSIM_LINEAR_SOLVER_HPP
