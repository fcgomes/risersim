/**
 * @file integrator.hpp
 * @brief Abstract "assemble K and F for this iteration" interface, mirroring ANFLEX's `cIntegrator`.
 */
#ifndef RISERSIM_INTEGRATOR_HPP
#define RISERSIM_INTEGRATOR_HPP

#include <Eigen/Sparse>

namespace risersim {

/**
 * @brief Abstract interface for assembling the tangent stiffness and internal force for one Newton-Raphson iteration.
 *
 * Extracted from inside the Newton-Raphson loop so different analysis types (static, and in the
 * future dynamic) can each assemble their own way while sharing the same nonlinear solve loop.
 * Mirrors ANFLEX's `cIntegrator`; StaticIntegrator is the current implementation.
 */
class Integrator {
public:
    virtual ~Integrator() = default;

    /**
     * @brief Assembles the global tangent stiffness and resisting internal force for the model's current state.
     * @param iter Current Newton-Raphson iteration index (some integrators use this to decay artificial stiffness).
     * @param[out] K_global Global tangent stiffness matrix.
     * @param[out] F_int Global internal (resisting) force vector.
     */
    virtual void assemble_stiffness_and_internal_forces(int iter,
                                                          Eigen::SparseMatrix<double>& K_global,
                                                          Eigen::VectorXd& F_int) = 0;
};

} // namespace risersim

#endif // RISERSIM_INTEGRATOR_HPP
