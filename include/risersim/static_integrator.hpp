#ifndef RISERSIM_STATIC_INTEGRATOR_HPP
#define RISERSIM_STATIC_INTEGRATOR_HPP

#include "risersim/integrator.hpp"
#include "risersim/analysis.hpp"

namespace risersim {

// Monta K/F para a analise estatica, espelhando cStaticIntegrator do ANFLEX
// real. O flag artificial_stiffness_enabled e o que vai habilitar o padrao
// de duas fases (Passo 3 do roadmap: assembly com rigidez artificial ->
// static sem ela) sem duplicar codigo -- e a mesma classe, so configurada
// diferente em cada fase.
class StaticIntegrator : public Integrator {
public:
    Analysis* analysis;  // non-owning
    bool artificial_stiffness_enabled = true;

    explicit StaticIntegrator(Analysis* a) : analysis(a) {}

    // Monta o vetor de carga externa (peso submerso + arrasto de corrente)
    // para um dado fator de carga (0..1). Espelha
    // cStaticIntegrator::form_global_load_vector (parte gravidade/corrente).
    Eigen::VectorXd assemble_load_vector(double load_factor) const;

    void assemble_stiffness_and_internal_forces(int iter,
                                                 Eigen::SparseMatrix<double>& K_global,
                                                 Eigen::VectorXd& F_int) override;
};

} // namespace risersim

#endif // RISERSIM_STATIC_INTEGRATOR_HPP
