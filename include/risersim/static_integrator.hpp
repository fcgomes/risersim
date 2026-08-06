/**
 * @file static_integrator.hpp
 * @brief Integrator implementation for static analysis, mirroring ANFLEX's `cStaticIntegrator`.
 */
#ifndef RISERSIM_STATIC_INTEGRATOR_HPP
#define RISERSIM_STATIC_INTEGRATOR_HPP

#include "risersim/integrator.hpp"
#include "risersim/analysis.hpp"

namespace risersim {

/**
 * @brief Assembles K/F for static analysis, mirroring ANFLEX's `cStaticIntegrator`.
 *
 * `artificial_stiffness_enabled` is what would drive a two-phase pattern (an "assembly" phase
 * with artificial stiffness, followed by a "static" phase without it, matching ANFLEX's own
 * two-phase static solve) without duplicating code -- it's the same class, just configured
 * differently per phase.
 */
class StaticIntegrator : public Integrator {
public:
    Analysis* analysis;  ///< Non-owning pointer to the owning Analysis.
    bool artificial_stiffness_enabled = true; ///< Whether Tikhonov-style artificial stiffness regularization is applied.

    explicit StaticIntegrator(Analysis* a) : analysis(a) {}

    /**
     * @brief Assembles the external load vector (submerged weight + current drag).
     *
     * Mirrors the gravity/current portion of ANFLEX's `cStaticIntegrator::form_global_load_vector`.
     * Submerged weight (self-weight + buoyancy) is always applied at full magnitude -- real
     * ANFLEX never ramps it either (`cBar::calc_weight_load`'s `m_has_gravitational_load` gate is
     * never set anywhere in its source, so the ramp branch is dead code there). Only current
     * drag is scaled, by its own independent ramp -- see `EnvironmentalConfig::current_ramp_x/y`
     * and `docs/mapa_classes_anflex_estatica.md`.
     * @param current_factor Current-drag load scaling factor in [0, 1], for incremental load stepping.
     */
    Eigen::VectorXd assemble_load_vector(double current_factor) const;

    void assemble_stiffness_and_internal_forces(int iter,
                                                 Eigen::SparseMatrix<double>& K_global,
                                                 Eigen::VectorXd& F_int) override;
};

} // namespace risersim

#endif // RISERSIM_STATIC_INTEGRATOR_HPP
