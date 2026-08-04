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

// LU geral (nao-simetrico) com pivoteamento parcial -- funciona para
// qualquer matriz, inclusive indefinida, sem exigir cuidado extra. Nao e mais
// o default (ver EigenSimplicialLDLTSolver), mas fica disponivel como opcao
// robusta caso a matriz fique genuinamente indefinida em algum cenario onde
// o LDLT falhe.
class EigenSparseLUSolver : public LinearSolver {
public:
    LinearSolverStatus solve(const Eigen::SparseMatrix<double>& K,
                              const Eigen::VectorXd& b,
                              Eigen::VectorXd& x) override;
};

// Default: explora a simetria de K_global via decomposicao LDL^T
// (Eigen::SimplicialLDLT), igual ao que os tres backends do ANFLEX real fazem
// (Skyline/MKL/Pardiso sao todos simetricos) -- cerca de metade do custo de
// memoria/tempo do LU geral. K_global e sempre montada simetricamente
// (analysis.cpp), mas pode ser INDEFINIDA durante a iteracao nao-linear
// (rigidez geometrica em compressao, regiao de contato); SimplicialLDLT do
// Eigen NAO faz pivoteamento numerico para isso (diferente do Pardiso real,
// que usa mtype=-2 "simetrico indefinido" com escalonamento/permutacao MPS
// especificamente por causa disso). Testado contra toda a bateria de
// regressao (risersim/docs/mapa_classes_anflex_estatica.md): converge nos
// mesmos casos que EigenSparseLUSolver, com o mesmo resultado numerico, e nao
// falha de forma diferente nos casos ja problematicos (solo+corrente) -- se
// isso mudar no futuro (ex.: modelos maiores/mais indefinidos), trocar para
// EigenSparseLUSolver e a alternativa segura.
class EigenSimplicialLDLTSolver : public LinearSolver {
public:
    LinearSolverStatus solve(const Eigen::SparseMatrix<double>& K,
                              const Eigen::VectorXd& b,
                              Eigen::VectorXd& x) override;
};

} // namespace risersim

#endif // RISERSIM_LINEAR_SOLVER_HPP
