/**
 * @file element_buoy.hpp
 * @brief Single-node buoy/hydrostatic-restoring element, mirroring ANFLEX's `cBuoyElement`.
 */
#ifndef RISERSIM_ELEMENT_BUOY_HPP
#define RISERSIM_ELEMENT_BUOY_HPP

#include "risersim/element.hpp"
#include <Eigen/Dense>
#include <numbers>

namespace risersim {

/**
 * @brief Properties of a BuoyElement, a small subset of real `cBuoyElement`'s `sBuoyElementProperties`
 * (`buoy_element.h:24-41`) -- only what this simplified port actually uses (see BuoyElement's own
 * doc comment for what's deliberately left out: Morison drag/wave forcing, orientation tracking).
 */
struct BuoyProps {
    /**
     * @brief Dry weight in air (N). NOT applied by `BuoyElement::assemble()` (an `Element`'s
     * `assemble()` has no access to the global system, only its own node -- see class doc comment)
     * -- callers add it directly to `F_ext` via a `dynamic_cast<BuoyElement*>` loop, same pattern
     * already used for `CorotationalBeam3D`'s weight/buoyancy in `static_integrator.cpp`/
     * `static_analysis.cpp`/`dynamic_analysis.cpp`. Mirrors real `cBuoyElement::calc_load`'s own
     * `load[2] -= m_properties->weight` exactly.
     */
    double weight = 0.0;
    double volume = 0.0; ///< Displaced volume (m^3) when fully submerged.
    /**
     * @brief Horizontal (waterplane) projected area (m^2) -- real `cBuoyElement`'s `z_area`, also
     * used (with `volume`) to derive the buoy's own vertical extent via `height()` below, matching
     * real ANFLEX's own simplification of treating the buoy as a constant-cross-section body
     * (`m_properties->height = m_properties->volume / m_properties->z_area`, `buoy_element.cpp:113`).
     */
    double z_area = 0.0;
    /**
     * @brief Added-mass coefficient per local axis (excess over the displaced-fluid mass) -- same
     * convention as `BeamMaterialProps::Ca` (a coefficient added directly, not real ANFLEX's raw
     * `inertia_coef`/`Cm`, which only counts the excess over 1.0 -- `buoy_element.cpp:90-95`).
     */
    Eigen::Vector3d Ca = Eigen::Vector3d::Zero();
    /** @brief Structural rotational inertia (rx,ry,rz), kg*m^2 -- real `moment_inertia`. */
    Eigen::Vector3d moment_inertia = Eigen::Vector3d::Zero();
};

/**
 * @brief Single-node element with hydrostatic (waterplane) restoring stiffness/force, mirroring
 * ANFLEX's `cBuoyElement` (`buoy_element.h`/`buoy_element.cpp`) -- used for a discrete buoyancy
 * module/float (`Manifold`, `Reboque/Off_Bottom`, `Reboque/Buoyant_Catenary`).
 *
 * Real `cBuoyElement`'s hydrostatic restoring behavior is classified as INTERNAL force/stiffness
 * (`calc_stiff_mt`/`calc_internal_forces`, the same pure-virtual bucket as `cTruss`'s own elastic
 * force) -- unlike beam buoyancy, which risersim implemented as a separate, beam-specific F_ext/
 * K_buoyancy mechanism (`Analysis::assemble_buoyancy_stiffness()`) because `CorotationalBeam3D::
 * assemble()`'s formulation predates that need. Following real ANFLEX's own classification here
 * fits directly into the existing `Element::assemble()`/`mass_matrix()` interface with NO new
 * hooks: `water_surface_z` is captured once at construction (risersim's water surface doesn't
 * change over a run -- wave elevation isn't fed into element-level hydrostatics anywhere yet, same
 * documented gap as the beam's own hydrostatics), so `assemble()` never needs access to the model/
 * environmental config beyond what it already has.
 *
 * Formulas are a direct, sign-for-sign port of real `calc_stiff_mt`/`calc_internal_forces`
 * (`buoy_element.cpp:143-245`) -- NOT re-derived from first-principles sign reasoning (a from-
 * scratch physical equilibrium check briefly suggested a different sign during this port; direct
 * literal translation was used instead once the same style of check on the already-verified
 * TrussElement showed that reasoning is unreliable in isolation -- trust the ported formula, the
 * same principle followed for every element type this session). Only DIFFERENCE from real: `Kzz`/
 * `Krot` are computed as `rho_water * g * area` (explicit `g`, risersim's own mass-density
 * convention, matching `Hydrostatics`/`assemble_buoyancy_stiffness()`'s pattern) instead of real's
 * raw `area * m_water_density` -- real ANFLEX's `sGlobalData::m_water_density`, used un-divided by
 * gravity here, is evidently already a WEIGHT density in that one raw formula (consistent with the
 * standard naval-architecture heave-restoring-stiffness formula `rho*g*A_waterplane`), whereas
 * risersim's `water_density` is consistently a MASS density (kg/m^3) with `g` applied explicitly by
 * every caller -- same physical formula, translated to this codebase's own unit convention.
 *
 * **Deliberate simplifications vs. real `cBuoyElement`**:
 * - **Fixed (identity) local frame**: real `cBuoyElement` tracks the buoy's own orientation
 *   (`m_transf_matrix`, updated from the node's accumulated rotation when `num_dofs > 3`) and
 *   rotates the waterplane/added-mass tensors into it every iteration. This port always uses the
 *   global frame directly (rotational restoring force uses `node->rot` as-is, no transform) --
 *   appropriate for the small-tilt regime a real buoy/float actually operates in, same simplification
 *   level as `ScalarElement`'s own fixed local frame.
 * - **No Morison drag/inertia and no wave/current forcing** (real `calc_load`'s dominant remaining
 *   term besides weight) -- genuinely complex (needs wave kinematics at a point, which risersim's
 *   own regular-wave-in-Z-only model doesn't fully support yet either) and deferred, same
 *   `dynamic_cast`-skip-if-absent pattern as `TrussElement`/`WinchElement`'s own weight/buoyancy/
 *   current gap. Tracked in `docs/roadmap.md`.
 * - **3 or 6 DOF is real ANFLEX's own choice per instance** (`num_dofs` constructor param, so a
 *   model can explicitly omit rotation); this port always uses the full 6-DOF layout every
 *   `Element` does (see element.hpp) -- an unconfigured `moment_inertia`/zero waterplane-derived
 *   `Krot` simply contributes nothing there, equivalent in practice.
 */
class BuoyElement : public Element {
public:
    /**
     * @param water_surface_z Real Z of the water surface (`model->environmental().water_surface_z`
     * at construction time) -- see class doc comment for why this is captured once, not re-read.
     * @param water_density Seawater mass density (kg/m^3), used for both the hydrostatic stiffness
     * (with an explicit `g`) and the mass matrix's added-mass term.
     */
    BuoyElement(int id, Node3D* node, const BuoyProps& props, double water_surface_z, double water_density = 1025.0)
        : id_(id), node_(node), props_(props), water_surface_z_(water_surface_z), water_density_(water_density) {}

    int id() const { return id_; }
    Node3D* node1() const { return node_; }
    const BuoyProps& props() const { return props_; }
    BuoyProps& props() { return props_; }

    /** @brief The buoy's own vertical extent, `volume/z_area` (0.0 if `z_area<=0`, no waterplane). */
    double height() const { return (props_.z_area > 1.0e-12) ? (props_.volume / props_.z_area) : 0.0; }

    /**
     * @brief Submersion depth relative to the buoy's own half-height: `height()/2 - (z_current -
     * water_surface_z)`. Negative = fully emerged; in `(0, height())` = straddling the waterline;
     * `>= height()` = fully submerged. Mirrors real `cBuoyElement`'s own `d`.
     */
    double submersion_depth() const {
        return height() / 2.0 - (node_->current_coords().z() - water_surface_z_);
    }

    /** @brief Always 1 -- a single-node element (real `cBuoyElement`'s own `num_nodes=1`). */
    int num_nodes() const override { return 1; }
    Node3D* node(int /*local_index*/) const override { return node_; }

    void assemble(Eigen::MatrixXd& K_local, Eigen::VectorXd& F_int_local) const override {
        K_local = Eigen::MatrixXd::Zero(6, 6);
        F_int_local = Eigen::VectorXd::Zero(6);
        if (props_.z_area <= 1.0e-12) return; // no waterplane -- no hydrostatic restoring at all

        const double g = 9.81;
        double h = height();
        double d = submersion_depth();

        if (d > 0.0 && d < h) {
            double a_wl = props_.z_area;
            double ix_wl = a_wl * a_wl / (4.0 * std::numbers::pi);
            double v = d * a_wl;
            double Kzz = a_wl * water_density_ * g;
            double Krot = water_density_ * g * (ix_wl + v * (d - h) / 2.0);

            K_local(2, 2) = Kzz;
            K_local(3, 3) = Krot;
            K_local(4, 4) = Krot;

            F_int_local(2) = Kzz * d;
            // Real cBuoyElement's m_rotations = -transform^T * global_rot (buoy_element.cpp:201);
            // with this port's fixed identity local frame, that reduces to -node->rot directly.
            F_int_local(3) = -Krot * node_->rot.x();
            F_int_local(4) = -Krot * node_->rot.y();
        } else if (d >= h) {
            // Fully submerged: constant buoyant force (a_wl*height == volume), no further
            // stiffness contribution -- matches real cBuoyElement's own d>=height branch exactly
            // (buoy_element.cpp:238-243).
            F_int_local(2) = water_density_ * g * props_.volume;
        }
        // d <= 0 (fully emerged): K, F stay zero.
    }

    /**
     * @brief Structural mass (`weight/g`) plus added mass (`rho_water*volume*Ca` per axis,
     * translation only) and structural rotational inertia -- mirrors real `calc_mass_vector`
     * (`buoy_element.cpp:345-375`), simplified to a diagonal matrix (real also rotates the
     * translational added-mass tensor by the buoy's own orientation; this port's fixed identity
     * frame makes that a no-op).
     */
    Eigen::MatrixXd mass_matrix(double rho_water) const override {
        Eigen::MatrixXd M = Eigen::MatrixXd::Zero(6, 6);
        double structural_mass = props_.weight / 9.81;
        M(0, 0) = structural_mass + rho_water * props_.volume * props_.Ca.x();
        M(1, 1) = structural_mass + rho_water * props_.volume * props_.Ca.y();
        M(2, 2) = structural_mass + rho_water * props_.volume * props_.Ca.z();
        M(3, 3) = props_.moment_inertia.x();
        M(4, 4) = props_.moment_inertia.y();
        M(5, 5) = props_.moment_inertia.z();
        return M;
    }

private:
    int id_;
    Node3D* node_;
    BuoyProps props_;
    double water_surface_z_;
    double water_density_;
};

} // namespace risersim

#endif
