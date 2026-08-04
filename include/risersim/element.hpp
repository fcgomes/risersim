/**
 * @file element.hpp
 * @brief Abstract finite element interface, mirroring ANFLEX's `cElement`.
 */
#ifndef RISERSIM_ELEMENT_HPP
#define RISERSIM_ELEMENT_HPP

#include "risersim/node.hpp"
#include <Eigen/Dense>

namespace risersim {

/**
 * @brief Abstract interface for a finite element, mirroring ANFLEX's `cElement`.
 *
 * Deliberately lean (see roadmap step 5 in `docs/mapa_classes_anflex_estatica.md`): it
 * abstracts only the generic surface actually needed by the assembly loop (node access,
 * tangent stiffness + internal force, mass) -- not the huge surface of real ANFLEX's
 * `cElement` (fluid loading, wake effects, damping, results-variable maps, etc.), most of
 * which doesn't apply here. risersim currently has a single concrete element type
 * (CorotationalBeam3D); element-type-specific data (material properties, tension,
 * stress/curvature results) intentionally stays on the concrete class rather than being
 * forced into this interface, mirroring how ANFLEX's own `cBar`/`cBeam` add substantial
 * state beyond their shared `cElement` base.
 */
class Element {
public:
    virtual ~Element() = default;

    /** @brief Number of nodes this element connects (2 for every element type risersim has today). */
    virtual int num_nodes() const = 0;

    /** @brief Returns the element's node at local index in `[0, num_nodes())`. */
    virtual Node3D* node(int local_index) const = 0;

    /**
     * @brief Assembles this element's tangent stiffness and internal force, in local node order.
     *
     * Row/column block `[6*i, 6*i+6)` corresponds to `node(i)`'s 6 DOFs (translation then
     * rotation), for `i` in `[0, num_nodes())`. K and F_int are computed together (one call,
     * not two) so implementations with a shared intermediate state -- e.g. the corotational
     * ghost frame in CorotationalBeam3D::compute_corotational_forces() -- don't recompute it twice.
     *
     * @param[out] K_local Tangent stiffness, sized `(6*num_nodes()) x (6*num_nodes())`.
     * @param[out] F_int_local Internal (resisting) force, sized `6*num_nodes()`.
     */
    virtual void assemble(Eigen::MatrixXd& K_local, Eigen::VectorXd& F_int_local) const = 0;

    /**
     * @brief Consistent mass matrix, same local node ordering as assemble().
     * @param rho_water Seawater density, for elements with hydrodynamic added mass.
     */
    virtual Eigen::MatrixXd mass_matrix(double rho_water) const = 0;
};

} // namespace risersim

#endif
