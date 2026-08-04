#ifndef RISERSIM_INTEGRATOR_HPP
#define RISERSIM_INTEGRATOR_HPP

#include <Eigen/Sparse>

namespace risersim {

// Interface abstrata para "montar K e F para esta iteracao", extraida de
// dentro do loop de Newton-Raphson (Passo 2 do roadmap de modernizacao,
// risersim/docs/mapa_classes_anflex_estatica.md). Espelha cIntegrator do
// ANFLEX real -- StaticIntegrator e (futuramente) DynamicIntegrator
// implementam essa montagem cada um a sua maneira.
class Integrator {
public:
    virtual ~Integrator() = default;

    // Monta a rigidez global tangente e a forca interna resistente para o
    // estado atual do modelo (posicoes/rotacoes correntes dos nos).
    virtual void assemble_stiffness_and_internal_forces(int iter,
                                                          Eigen::SparseMatrix<double>& K_global,
                                                          Eigen::VectorXd& F_int) = 0;
};

} // namespace risersim

#endif // RISERSIM_INTEGRATOR_HPP
