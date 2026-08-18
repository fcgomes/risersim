/**
 * @file element_truss.hpp
 * @brief 2-node axial bar (cable/tendon/mooring line) element, mirroring ANFLEX's `cTruss`.
 */
#ifndef RISERSIM_ELEMENT_TRUSS_HPP
#define RISERSIM_ELEMENT_TRUSS_HPP

#include "risersim/element.hpp"
#include <Eigen/Dense>

namespace risersim {

/**
 * @brief Material/section properties of a TrussElement, mirroring real `cTruss`'s `cBarProperties`
 * subset actually used by `update_axial_strain_and_force`/`calc_stiff_mt` (`truss.cpp:142-169,
 * 229-298`).
 */
struct TrussProps {
    double E = 2.1e11;   ///< Young's modulus (Pa) -- same steel default as BeamMaterialProps::E.
    double A = 0.001;    ///< Cross-section area (m^2) -- a cable/tendon's, much smaller than a riser pipe wall's.
    double rho = 7850.0; ///< Structural mass density (kg/m^3), for the lumped mass matrix and dry weight.
    /**
     * @brief Axial pre-tension (N), added on top of the strain-derived force (real `cTruss`'s
     * `m_properties[0]->m_initial_tension`). Left at 0.0 for `WinchElement` (real `cWinch::
     * update_axial_strain_and_force` has no such term -- a winch's tension emerges from its
     * payout state, not a configured constant), so `TrussElement::update_effective_tension()` can
     * be reused unchanged by both.
     */
    double initial_tension = 0.0;
    /**
     * @brief Outer diameter (m), for the buoyancy envelope (`Hydrostatics`) and current drag --
     * see the weight/buoyancy/current loops in `static_integrator.cpp`/`static_analysis.cpp`/
     * `dynamic_analysis.cpp`. Default 0.0 = no hydrostatic/drag envelope at all (a `Hydrostatics`
     * built with a zero diameter always returns zero force/stiffness), so a JSON written before
     * this field existed keeps behaving exactly as before (dry weight only, no buoyancy/current).
     */
    double D_outer = 0.0;
};

/**
 * @brief 2-node, 3-DOF/node (translation only, no bending) axial bar element, mirroring ANFLEX's
 * `cTruss : public cBar` (`truss.h`/`truss.cpp`). Used for cables, tendons, and mooring lines
 * (`%MATERIAL...` + `FINITE_ELEMENT 'truss'`, e.g. `Boiao`/`Manifold`/`RHAS`/`Exemplo_01d`'s
 * `'TEND'`/`Exemplo_04`'s `'AMAR'`/`Reboque/*`).
 *
 * Corotational in the same sense as `CorotationalBeam3D` (direction cosines and axial strain are
 * always computed from the CURRENT node positions, not the initial chord), but without any
 * bending/rotation state -- there is no ghost frame, ghost triad, or birth-twist concept here, a
 * genuine simplification of the beam's formulation matching real `cTruss`'s own simpler
 * `cBar`-derived pipeline (`calc_stiff_mt`, `truss.cpp:229-298`; `calc_internal_forces`,
 * `truss.cpp:324-352`).
 *
 * **Self-weight/buoyancy/current** (real `cBar::calc_weight_load`, called from real `cTruss`'s own
 * `calc_load` -- EXTERNAL load, same classification as `CorotationalBeam3D`'s own weight/buoyancy)
 * -- filled in 2026-08-18 (was a documented gap through the initial Truss/Winch implementation):
 * `dynamic_cast<TrussElement*>` loops in `static_integrator.cpp::assemble_load_vector()`,
 * `static_analysis.cpp` (reference-norm + vessel-offset), and `dynamic_analysis.cpp` mirror
 * `CorotationalBeam3D`'s own treatment exactly -- dry weight `rho*A*g` (50/50 split, doesn't depend
 * on submersion) plus `Hydrostatics(D_outer, L, water_density)` for submersion-scaled buoyancy
 * force AND its matching tangent stiffness (added via `Analysis::assemble_buoyancy_stiffness()`,
 * also extended with a second `dynamic_cast<TrussElement*>` loop), plus current drag via the
 * model-wide `CurrentProfile` (same call as the beam's: `current.get_drag_force_per_length(...)`,
 * needs only `D_outer` -- current's own drag coefficient is a model-wide `CurrentProfile` field,
 * not per-element, so no separate `Cd` was needed on `TrussProps`). `WinchElement` gets this for
 * free (the loops match on `TrussElement*`, and `WinchElement` IS a `TrussElement`).
 * **Still NOT included**: wave/Morison drag+inertia (the beam's own dynamic-analysis Morison block
 * is a separate, more involved calculation than plain current drag) -- deferred, same documented
 * gap `BuoyElement` has for the same reason.
 * Seabed contact IS still active for a truss's nodes -- that loop is node-based, not element-type-
 * specific (`Analysis::assemble_system`'s seabed section iterates `model->nodes()` directly).
 *
 * Mass: a simple lumped mass (`rho*A*reference_length()/2` per node, translational DOFs only),
 * matching `cBar::calc_mass_vector`'s own leading term (`bar.cpp:391,411-415`) -- the internal-
 * fluid and hydrodynamic-added-mass refinements that formula also has (relevant to a riser-type
 * `cBar`, not really to a mooring cable) are left out, same simplification level as the weight
 * load above.
 */
class TrussElement : public Element {
public:
    TrussElement(int id, Node3D* node1, Node3D* node2, const TrussProps& props, double L_unstretched = 0.0)
        : id_(id), node1_(node1), node2_(node2), props_(props), axial_force_(0.0) {
        initial_length_ = (L_unstretched > 0.0) ? L_unstretched : (node2_->coords - node1_->coords).norm();
    }

    int id() const { return id_; }
    Node3D* node1() const { return node1_; }
    Node3D* node2() const { return node2_; }
    const TrussProps& props() const { return props_; }
    TrussProps& props() { return props_; }

    /** @brief Unstretched length at construction (m) -- real `cTruss`'s constant `m_original_length`. */
    double initial_length() const { return initial_length_; }

    /** @brief Current (deformed) element length: distance between the current node positions. */
    double current_length() const {
        return (node2_->current_coords() - node1_->current_coords()).norm();
    }

    /**
     * @brief The unstretched reference length strain is measured against -- real `cTruss`'s
     * `m_current_length`, constant at `initial_length()`. Virtual so `WinchElement` can override it
     * with a time-varying payout length (real `cWinch`'s own override of this same concept,
     * `winch.cpp:117-120`) while reusing every other formula unchanged.
     */
    virtual double reference_length() const { return initial_length_; }

    /** @brief Axial force from the last update_effective_tension() call (N, tension positive). */
    double axial_force() const { return axial_force_; }

    int num_nodes() const override { return 2; }
    Node3D* node(int local_index) const override { return local_index == 0 ? node1_ : node2_; }

    /**
     * @brief Recomputes the axial force from the current node positions, mirroring real `cTruss::
     * update_axial_strain_and_force` (`truss.cpp:142-169`, static-elasticity path only -- the
     * dynamic-elasticity/`m_FOT4`/`m_DEF4` branch is a refinement no real `.aml` in this repo's
     * survey exercises, see this class's doc comment).
     */
    double update_effective_tension() override {
        double L_ref = reference_length();
        double strain = (current_length() - L_ref) / L_ref;
        axial_force_ = strain * props_.E * props_.A + props_.initial_tension;
        return axial_force_;
    }

    /** @brief `Element::characteristic_stiffness()` override: the truss's elastic modulus (same convention as the beam). */
    double characteristic_stiffness() const override { return props_.E; }

    /**
     * @brief Assembles this truss's 3x3-per-node axial stiffness/force into the shared 12x12/12x1
     * layout (6 DOF/node, matching every other Element -- see element.hpp's doc comment).
     * Rotational rows/columns (local indices 3-5, 9-11) stay exactly zero: real `cTruss` has no
     * rotational DOF at all (`m_num_dof=3`, `truss.cpp:50`), so this element simply contributes no
     * stiffness/moment there, same as a real ANFLEX truss-only node has no rotational restraint
     * from this element (mirrors `calc_stiff_mt`/`calc_internal_forces`, `truss.cpp:229-352`).
     */
    void assemble(Eigen::MatrixXd& K_local, Eigen::VectorXd& F_int_local) const override {
        K_local = Eigen::MatrixXd::Zero(12, 12);
        F_int_local = Eigen::VectorXd::Zero(12);

        double L_ref = reference_length();
        Eigen::Vector3d delta = node2_->current_coords() - node1_->current_coords();
        double L_def = delta.norm();
        if (L_def < 1.0e-12 || L_ref < 1.0e-12) return; // degenerate (coincident nodes / zero reference length)
        Eigen::Vector3d cosdir = delta / L_def;

        double B = axial_force_ / L_ref;
        double EAXL = props_.E * props_.A / L_ref;

        Eigen::Matrix3d K3 = EAXL * (cosdir * cosdir.transpose()) + B * Eigen::Matrix3d::Identity();
        K_local.block<3, 3>(0, 0) = K3;
        K_local.block<3, 3>(0, 6) = -K3;
        K_local.block<3, 3>(6, 0) = -K3;
        K_local.block<3, 3>(6, 6) = K3;

        F_int_local.segment<3>(0) = cosdir * axial_force_;
        F_int_local.segment<3>(6) = -cosdir * axial_force_;
    }

    /**
     * @brief Lumped translational mass (`rho*A*reference_length()/2` per node) -- see this class's
     * doc comment for what's deliberately left out (internal fluid, hydrodynamic added mass).
     */
    Eigen::MatrixXd mass_matrix(double /*rho_water*/) const override {
        Eigen::MatrixXd M = Eigen::MatrixXd::Zero(12, 12);
        double lumped = 0.5 * props_.rho * props_.A * reference_length();
        for (int i = 0; i < 3; ++i) {
            M(i, i) = lumped;
            M(i + 6, i + 6) = lumped;
        }
        return M;
    }

protected:
    int id_;
    Node3D *node1_, *node2_;
    TrussProps props_;
    double initial_length_;
    double axial_force_;
};

} // namespace risersim

#endif
