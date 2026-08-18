/**
 * @file element_scalar.hpp
 * @brief Generic 6-DOF nonlinear spring/connector element, mirroring ANFLEX's `cScalar`.
 */
#ifndef RISERSIM_ELEMENT_SCALAR_HPP
#define RISERSIM_ELEMENT_SCALAR_HPP

#include "risersim/element.hpp"
#include "risersim/element_beam.hpp" // reuses CorotationalBeam3D::build_frame_from_chord (real-ANFLEX-matching local axis)
#include "risersim/curve_function.hpp"
#include "risersim/rotation_utils.hpp"
#include <Eigen/Dense>

namespace risersim {

/**
 * @brief Per-local-DOF force/moment-vs-relative-displacement curves for a ScalarElement, mirroring
 * ANFLEX's `cScalar` (`scalar.h`/`scalar.cpp`): 6 independent nonlinear springs (translational
 * X/Y/Z, rotational RX/RY/RZ), each a curve evaluated at that DOF's own relative
 * displacement/rotation -- covers both a simple linear spring (`PiecewiseLinearCurve::linear(k)`)
 * and a genuinely nonlinear one (e.g. a real ESDV valve's force curve, `ESDV/ESDV.aml`'s
 * `%SCALAR.FUNCTION_ID`). Real ANFLEX's flexjoint (`%MATERIAL.FLEXIBLE_JOINT` + `FINITE_ELEMENT
 * 'scalar'`, `Curso/Exemplo_01/Exemplo_01b.aml`) is this same element with the rotational curves
 * set and the translational ones effectively rigid (a very high linear stiffness) -- there's no
 * separate flexjoint element family to build, this one covers it.
 */
struct ScalarProps {
    PiecewiseLinearCurve curve_x, curve_y, curve_z;    ///< Force vs. relative translation (N vs. m).
    PiecewiseLinearCurve curve_rx, curve_ry, curve_rz; ///< Moment vs. relative rotation (N.m vs. rad).

    ScalarProps()
        : curve_x(PiecewiseLinearCurve::zero()), curve_y(PiecewiseLinearCurve::zero()),
          curve_z(PiecewiseLinearCurve::zero()), curve_rx(PiecewiseLinearCurve::zero()),
          curve_ry(PiecewiseLinearCurve::zero()), curve_rz(PiecewiseLinearCurve::zero()) {}
};

/**
 * @brief Generic 6-DOF nonlinear spring connecting two nodes, mirroring ANFLEX's `cScalar`.
 *
 * Real `cScalar` builds its local frame from 3 auxiliary reference points read from the `.DAT`
 * (`scalar.cpp:108-189`) -- risersim's JSON schema has no such concept yet, so this uses a
 * SIMPLIFICATION: the local frame is `CorotationalBeam3D::build_frame_from_chord()` applied to
 * the element's own initial chord (node2 - node1), fixed at construction (not re-derived every
 * iteration like the beam's corotational ghost frame) -- appropriate for the small-to-moderate
 * relative rotations a spring/flexjoint connector actually sees, and reuses the exact axis
 * convention already verified against real ANFLEX ground truth (`element_beam.cpp`,
 * `build_frame_from_chord()`'s own doc comment) rather than inventing a second, different one.
 *
 * Relative translation is `R * (node2.disp - node1.disp)`. Relative rotation is extracted via
 * proper rotation composition (not naive vector subtraction -- see `rotation_utils.hpp`):
 * `R_rel = rodrigues(node2.rot) * rodrigues(node1.rot)^T`, then expressed in the local frame and
 * converted back to an axis-angle vector. Both together are `s` (6,): each `props()` curve is
 * evaluated at its own component of `s` for the local generalized force, and at its derivative for
 * the local tangent stiffness -- then rotated into global 12-DOF form the same way
 * `CorotationalBeam3D::compute_corotational_forces()` already does (`K_global = T^T*K_local*T`).
 *
 * Contributes no mass (`mass_matrix()` returns zero) -- matches real `cScalar`, a massless
 * connector; no hydrodynamic loading either (weight/buoyancy/current loops in `static_integrator.
 * cpp`/`dynamic_analysis.cpp` skip any non-`CorotationalBeam3D` element, correct here since a
 * spring/flexjoint has no physical envelope in real ANFLEX either).
 */
class ScalarElement : public Element {
public:
    ScalarElement(int id, Node3D* node1, Node3D* node2, const ScalarProps& props)
        : id_(id), node1_(node1), node2_(node2), props_(props) {
        Eigen::Vector3d chord = node2_->coords - node1_->coords;
        double len = chord.norm();
        // Coincident nodes (a zero-length connector, a real and common case for a flexjoint
        // sitting exactly at a single physical point): chord direction is undefined, fall back to
        // global X -- an arbitrary but harmless choice, since a zero-length element's local X axis
        // never gets exercised by any real relative displacement along it either way.
        Eigen::Vector3d ex = (len > 1.0e-9) ? (chord / len).eval() : Eigen::Vector3d(1.0, 0.0, 0.0);
        local_frame_ = CorotationalBeam3D::build_frame_from_chord(ex);
    }

    int id() const { return id_; }
    Node3D* node1() const { return node1_; }
    Node3D* node2() const { return node2_; }
    const ScalarProps& props() const { return props_; }
    ScalarProps& props() { return props_; }

    int num_nodes() const override { return 2; }
    Node3D* node(int local_index) const override { return local_index == 0 ? node1_ : node2_; }

    void assemble(Eigen::MatrixXd& K_local, Eigen::VectorXd& F_int_local) const override {
        // Relative translation, in local axes.
        Eigen::Vector3d s_trans = local_frame_ * (node2_->disp - node1_->disp);

        // Relative rotation (node1 -> node2), properly composed then expressed in local axes.
        Eigen::Matrix3d R_rel_global = rodrigues(node2_->rot) * rodrigues(node1_->rot).transpose();
        Eigen::Matrix3d R_rel_local = local_frame_ * R_rel_global * local_frame_.transpose();
        Eigen::AngleAxisd aa(R_rel_local);
        Eigen::Vector3d s_rot = aa.angle() * aa.axis();

        double s[6] = {s_trans.x(), s_trans.y(), s_trans.z(), s_rot.x(), s_rot.y(), s_rot.z()};
        const PiecewiseLinearCurve* curves[6] = {&props_.curve_x, &props_.curve_y, &props_.curve_z,
                                                  &props_.curve_rx, &props_.curve_ry, &props_.curve_rz};

        Eigen::Matrix<double, 6, 1> f_local, k_local;
        for (int i = 0; i < 6; ++i) {
            f_local[i] = curves[i]->value_at(s[i]);
            k_local[i] = curves[i]->derivative_at(s[i]);
        }

        // Generalized 12x12/12x1 in local axes: standard "spring between two points" pattern
        // (node1's reference kept at zero, node2 carries the full relative state `s`) -- see this
        // class's doc comment for the derivation/consistency check.
        Eigen::Matrix<double, 12, 12> K_gen = Eigen::Matrix<double, 12, 12>::Zero();
        Eigen::Matrix<double, 12, 1> F_gen = Eigen::Matrix<double, 12, 1>::Zero();
        for (int i = 0; i < 6; ++i) {
            K_gen(i, i) = k_local[i];
            K_gen(i, i + 6) = -k_local[i];
            K_gen(i + 6, i) = -k_local[i];
            K_gen(i + 6, i + 6) = k_local[i];
            F_gen(i) = -f_local[i];
            F_gen(i + 6) = f_local[i];
        }

        Eigen::Matrix<double, 12, 12> T = Eigen::Matrix<double, 12, 12>::Zero();
        T.block<3, 3>(0, 0) = local_frame_;
        T.block<3, 3>(3, 3) = local_frame_;
        T.block<3, 3>(6, 6) = local_frame_;
        T.block<3, 3>(9, 9) = local_frame_;

        K_local = T.transpose() * K_gen * T;
        F_int_local = T.transpose() * F_gen;
    }

    /** @brief No mass modeled (massless connector, matching real `cScalar`). */
    Eigen::MatrixXd mass_matrix(double /*rho_water*/) const override {
        return Eigen::MatrixXd::Zero(12, 12);
    }

private:
    int id_;
    Node3D *node1_, *node2_;
    ScalarProps props_;
    Eigen::Matrix3d local_frame_; ///< Fixed at construction from the initial chord -- see class doc comment.
};

} // namespace risersim

#endif
