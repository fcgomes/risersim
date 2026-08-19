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

    /**
     * @brief Refreshes whatever internal state this element's own assemble()/results depend on
     * that isn't already implied by the nodes' current positions -- for `CorotationalBeam3D`,
     * the axial tension (`tension_true`/`tension_effective`, mirrors ANFLEX's `update_axial_
     * strain_and_force`). Default no-op: a pure connector element (e.g. a spring) with no such
     * derived state needs to override nothing. Called once per element per Newton iteration,
     * before `assemble()`/results extraction -- see `Analysis::assemble_system`.
     */
    virtual double update_effective_tension() { return 0.0; }

    /**
     * @brief A representative stiffness scale for this element (e.g. a beam's `E`), used only by
     * `Analysis::assemble_system`'s prescribed-motion penalty ("big number" technique) to pick a
     * spring stiffness large relative to the model's own. Default 0.0 -- an element that never
     * anchors a prescribed motion doesn't need to override this; the penalty scale only needs the
     * model-wide MAXIMUM across whichever elements do have a physically meaningful value.
     */
    virtual double characteristic_stiffness() const { return 0.0; }

    /**
     * @brief Rayleigh mass-proportional (`rayleigh_alpha`) and stiffness-proportional
     * (`rayleigh_beta`) damping coefficients for THIS element's own material, mirroring real
     * ANFLEX's genuinely per-material `cBar::calc_damping_matrix()` (`m_damping_matrix =
     * beta_i*K_local + alpha_i*diag(M_local)`) -- see `Analysis::assemble_damping()`. Default
     * 0.0/0.0: an element with no notion of Rayleigh damping (e.g. ScalarElement/BuoyElement,
     * matching real ANFLEX's unconditionally-empty `cScalar`/`cBuoyElement::calc_damping_matrix()`)
     * simply never overrides these -- combined with `rayleigh_configured()`'s own default below,
     * that reads as "configured to zero", not "fall back to the model default".
     */
    virtual double rayleigh_alpha() const { return 0.0; }
    virtual double rayleigh_beta() const { return 0.0; }

    /**
     * @brief Whether `rayleigh_alpha()`/`rayleigh_beta()` should be used AS GIVEN (including a
     * genuine zero, e.g. a real material with `consider_damping="no"`) or replaced by
     * `Analysis::assemble_damping()`'s caller-supplied fallback (an element whose JSON never
     * mentioned Rayleigh damping at all -- the old, model-wide-only behavior, kept for backward
     * compatibility with JSONs that predate per-element damping).
     *
     * Default TRUE: a type with no notion of Rayleigh damping at all (ScalarElement/BuoyElement)
     * is always "configured to zero" -- it must never fall back to the model default, matching
     * real ANFLEX exactly (`cScalar`/`cBuoyElement::calc_damping_matrix()` are unconditionally
     * empty, not gated by any material flag). Only `CorotationalBeam3D`/`TrussElement` override
     * this, reflecting whether their own per-element JSON data (`section_properties.rayleigh_alpha`/
     * `truss_properties.rayleigh_alpha`, etc.) was actually present -- see
     * `ModelBuilder::load_from_json()`.
     */
    virtual bool rayleigh_configured() const { return true; }

    /**
     * @brief Notifies this element of the analysis's current pseudo-time, for an element whose
     * own state is an explicit function of time -- currently only `WinchElement`'s payout-length
     * curve (mirrors real ANFLEX's `cWinch::update_axial_strain_and_force(step, current_time,
     * is_static)`). Default no-op: every other element type ignores it. Called by
     * `Analysis::assemble_system()` once per element per Newton iteration, right before
     * `update_effective_tension()` -- see `Analysis::current_time`'s doc comment for what value
     * callers set it to (load-step fraction during statics, elapsed simulation time during
     * dynamics).
     */
    virtual void set_time(double /*current_time*/) {}
};

} // namespace risersim

#endif
