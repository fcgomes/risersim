/**
 * @file element_beam.hpp
 * @brief Corotational 3D Euler-Bernoulli beam element, the core structural model of risersim.
 */
#ifndef RISERSIM_ELEMENT_BEAM_HPP
#define RISERSIM_ELEMENT_BEAM_HPP

#include "risersim/config.hpp"
#include "risersim/node.hpp"
#include "risersim/rotation_utils.hpp"
#include "risersim/element.hpp"
#include <Eigen/Dense>

namespace risersim {

/** @brief Cross-section and material properties of a beam element (pipe geometry, elasticity, added mass). */
struct BeamMaterialProps {
    double E;          ///< Young's modulus (Pa).
    double G;          ///< Shear modulus (Pa).
    double A;          ///< Structural cross-section area (m^2).
    double IY;         ///< Moment of inertia Y (m^4).
    double IZ;         ///< Moment of inertia Z (m^4).
    double J;          ///< Torsional constant (m^4).
    double rho;        ///< Structural mass density (kg/m^3).
    double EI;         ///< Bending stiffness EI (N.m^2).

    double D_outer;    ///< Outer diameter (m).
    double D_inner;    ///< Inner diameter (m).
    double rho_fluid;  ///< Internal fluid density (kg/m^3).
    double Ca;         ///< Hydrodynamic added mass coefficient (default 1.0).
    double Cd;         ///< Hydrodynamic drag coefficient (default 1.0) -- distinct from Ca; previously the current-drag code reused Ca in its place.

    BeamMaterialProps()
        : E(2.1e11), G(8.0e10), A(0.015), IY(5.0e-5), IZ(5.0e-5), J(1.0e-4), rho(7850.0), EI(1.05e7),
          D_outer(0.25), D_inner(0.20), rho_fluid(800.0), Ca(1.0), Cd(1.0) {}
};

/**
 * @brief Corotational 3D beam element (2 nodes, 6 DOFs/node), equivalent to ANFLEX's `cBeam` (`beam.cpp`).
 *
 * Large displacements/rotations are handled by the corotational framework: a co-rotating
 * "ghost frame" (see compute_corotational_forces()) separates rigid-body motion from the small
 * local/deformational strain that actually feeds the linear beam stiffness. This mirrors
 * ANFLEX's `calc_transformation_mt`/`calc_relative_rotations`/`calc_fe` pipeline
 * (`element.cpp`/`matrix.cpp`/`beam.cpp`) rather than a naive total-Lagrangian formulation.
 *
 * Implements the Element interface directly (mirroring how ANFLEX's `cBeam` derives from
 * `cElement`), rather than through a separate wrapper class: an earlier version of the
 * modernization roadmap (`docs/mapa_classes_anflex_estatica.md`) envisioned a `BeamElement`
 * wrapper specifically to bolt on the fixed reference triad, but that triad ended up being
 * added directly to this class instead (as the fix for the corotational divergence bug), so
 * a separate wrapper would have had nothing left to do beyond forwarding calls.
 */
class CorotationalBeam3D : public Element {
public:
    int id;
    Node3D* node1;
    Node3D* node2;
    BeamMaterialProps props;

    double initial_length;
    double tension_true;      ///< True axial wall tension T_true.
    double tension_effective; ///< T_eff = T_true + p_e*A_e - p_i*A_i.
    double p_i;               ///< Internal fluid pressure (Pa).
    double p_e;               ///< External hydrostatic pressure (Pa).
    double net_upward_buoyancy; ///< Net upward buoyancy force per meter from modules (N/m).

    /**
     * @brief Fixed reference triads at both ends, computed once in the initial (t=0) configuration.
     *
     * Equivalent to ANFLEX's `TELNOAORIG`/`TELNOBORIG` (`calc_init_rot_mt`, `beam.cpp:301`).
     * Without a manual pre-curvature input (`curvature1`/`curvature2` in ANFLEX's `.dat` format),
     * both triads coincide with the initial-chord frame. Each node's local/deformational rotation
     * is measured against this *fixed* reference every iteration -- not against the current chord
     * (which changes every iteration and would mix rigid-body rotation with elastic deformation).
     * This is the fix for the divergence bug documented in `docs/mapa_classes_anflex_estatica.md`:
     * feeding the total accumulated rotation into the bending stiffness collapses convergence far
     * earlier than ANFLEX for long chains.
     */
    Eigen::Matrix3d node1_init_triad;
    Eigen::Matrix3d node2_init_triad;

    CorotationalBeam3D(int elem_id, Node3D* n1, Node3D* n2, const BeamMaterialProps& p, double L_unstretched = 0.0)
        : id(elem_id), node1(n1), node2(n2), props(p), tension_true(0.0), tension_effective(0.0), p_i(0.0), p_e(0.0), net_upward_buoyancy(0.0) {
        initial_length = (L_unstretched > 0.0) ? L_unstretched : (node2->coords - node1->coords).norm();
        Eigen::Vector3d ex0 = (node2->coords - node1->coords).normalized();
        node1_init_triad = build_frame_from_chord(ex0);
        node2_init_triad = node1_init_triad;
    }

    /** @brief Current (deformed) element length: distance between the current node positions. */
    double current_length() const {
        return (node2->current_coords() - node1->current_coords()).norm();
    }

    /** @brief Cross-sectional area enclosed by the outer diameter (m^2). */
    double outer_area() const {
        return std::numbers::pi * props.D_outer * props.D_outer / 4.0;
    }

    /** @brief Cross-sectional area enclosed by the inner (bore) diameter (m^2). */
    double inner_area() const {
        return std::numbers::pi * props.D_inner * props.D_inner / 4.0;
    }

    /**
     * @brief Total linear mass per meter: pipe wall + internal fluid + hydrodynamic added mass.
     * @param rho_water Seawater density used for the added-mass term (kg/m^3).
     */
    double total_linear_mass(double rho_water = 1025.0) const {
        double m_pipe = props.rho * props.A;
        double m_fluid = props.rho_fluid * inner_area();
        double m_added = rho_water * outer_area() * props.Ca;
        return m_pipe + m_fluid + m_added;
    }

    /**
     * @brief Recomputes and returns the effective tension T_eff = T_true + p_e*A_e - p_i*A_i.
     *
     * `T_true` is derived from engineering axial strain (`current_length()` vs. `initial_length`)
     * times `E*A`. Updates tension_true and tension_effective as a side effect.
     */
    double update_effective_tension() {
        double delta_L = current_length() - initial_length;
        double strain = delta_L / initial_length;
        tension_true = props.E * props.A * strain;
        tension_effective = tension_true + p_e * outer_area() - p_i * inner_area();
        return tension_effective;
    }

    /** @brief Local (element-frame) material (elastic) stiffness matrix (12x12), standard 3D Euler-Bernoulli beam. */
    Eigen::Matrix<double, 12, 12> local_material_stiffness() const;

    /** @brief Local (element-frame) geometric stiffness matrix (12x12), the tension-dependent stress-stiffening term. */
    Eigen::Matrix<double, 12, 12> local_geometric_stiffness() const;

    /**
     * @brief Local (element-frame) consistent mass matrix (12x12), including internal fluid and added mass.
     * @param rho_water Seawater density used for the added-mass term (kg/m^3).
     */
    Eigen::Matrix<double, 12, 12> local_mass_matrix(double rho_water = 1025.0) const;

    /**
     * @brief 3D transformation matrix (12x12) built purely from the current chord direction.
     *
     * Used only for the mass matrix, where local rotation doesn't enter the formulation (unlike
     * the stiffness/internal-force path, which needs the corotational ghost frame -- see
     * compute_corotational_forces()).
     */
    Eigen::Matrix<double, 12, 12> transformation_matrix() const;

    /**
     * @brief Global (assembled-frame) consistent mass matrix (12x12): `T^T * M_local * T`.
     * @param rho_water Seawater density used for the added-mass term (kg/m^3).
     */
    Eigen::Matrix<double, 12, 12> global_mass(double rho_water = 1025.0) const {
        Eigen::Matrix<double, 12, 12> T = transformation_matrix();
        Eigen::Matrix<double, 12, 12> M_local = local_mass_matrix(rho_water);
        return T.transpose() * M_local * T;
    }

    /**
     * @brief Global tangent stiffness matrix (12x12), for introspection (Python bindings/diagnostics).
     *
     * Convenience wrapper around compute_corotational_forces(); the internal solver calls that
     * function directly instead, since it computes K and F_int together and avoids recomputing
     * the ghost frame twice.
     */
    Eigen::Matrix<double, 12, 12> global_stiffness() const {
        Eigen::Matrix<double, 12, 12> K;
        Eigen::Matrix<double, 12, 1> F_unused;
        compute_corotational_forces(K, F_unused);
        return K;
    }

    /**
     * @brief Builds an orthonormal frame {ex, ey, ez} (rows = local axes in global coordinates) from a chord direction.
     *
     * Shared by the fixed reference triad (computed once at t=0, see node1_init_triad) and by
     * transformation_matrix() (used only for the mass matrix).
     * @param ex Unit vector along the desired local x-axis (element chord direction).
     */
    static Eigen::Matrix3d build_frame_from_chord(const Eigen::Vector3d& ex);

    /**
     * @brief Computes the element's tangent stiffness and internal force via the full corotational pipeline.
     *
     * Extracts each node's local/deformational rotation as the mismatch between its current
     * orientation and the element's corotating reference (Crisfield's "ghost frame", built from
     * the mean/half-relative rotation of the two nodes), and uses that -- not the total
     * accumulated rotation -- to evaluate internal force and stiffness. Replicates ANFLEX's
     * `calc_transformation_mt` / `calc_relative_rotations` / `calc_fe`
     * (`element.cpp:215`, `matrix.cpp:499`, `beam.cpp:1276`).
     *
     * @param[out] K_global 12x12 tangent stiffness in global coordinates.
     * @param[out] F_int_global 12x1 internal (resisting) force vector in global coordinates.
     */
    void compute_corotational_forces(Eigen::Matrix<double, 12, 12>& K_global,
                                      Eigen::Matrix<double, 12, 1>& F_int_global) const;

    // --- Element interface ---------------------------------------------------------------

    /** @brief Always 2 for a beam element. */
    int num_nodes() const override { return 2; }

    /** @brief `node(0)` is node1, `node(1)` is node2. */
    Node3D* node(int local_index) const override { return local_index == 0 ? node1 : node2; }

    /**
     * @brief Element::assemble() implementation: thin dynamically-sized wrapper around compute_corotational_forces().
     *
     * Does not duplicate or re-derive any math -- copies the same fixed-size 12x12/12x1 result
     * into dynamically-sized matrices, so this and compute_corotational_forces() always agree
     * bit-for-bit (validated in `tests/test_element.cpp`).
     */
    void assemble(Eigen::MatrixXd& K_local, Eigen::VectorXd& F_int_local) const override;

    /**
     * @brief Element::mass_matrix() implementation: thin dynamically-sized wrapper around global_mass().
     * @param rho_water Seawater density used for the added-mass term (kg/m^3).
     */
    Eigen::MatrixXd mass_matrix(double rho_water) const override;

    /** @brief Bending moment, curvature, minimum-bend-radius check, and combined von Mises stress at an element. */
    struct StressAndCurvatureResults {
        double curvature;          ///< Local curvature kappa (1/m).
        double bending_moment_kNm; ///< Bending moment M (kN*m).
        double bend_radius;        ///< Actual bend radius R = 1/kappa (m).
        double mbr_min;            ///< Minimum Bend Radius threshold (m).
        double mbr_safety_factor;  ///< Safety factor R_actual / MBR_min.
        double von_mises_MPa;      ///< Combined von Mises stress (MPa).
    };

    /**
     * @brief Computes bending/curvature/stress results for this element, using neighbors for a smoother curvature estimate.
     * @param prev_elem Element sharing node1 with this one, or nullptr at a chain end.
     * @param next_elem Element sharing node2 with this one, or nullptr at a chain end.
     * @param yield_stress_MPa Material yield stress used to normalize the von Mises result (MPa).
     */
    StressAndCurvatureResults compute_stress_and_curvature(
        const CorotationalBeam3D* prev_elem = nullptr,
        const CorotationalBeam3D* next_elem = nullptr,
        double yield_stress_MPa = 350.0) const;
};

} // namespace risersim

#endif
