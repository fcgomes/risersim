/**
 * @file test_element.cpp
 * @brief Validates that CorotationalBeam3D's Element interface overrides (assemble(), mass_matrix(),
 * node(), num_nodes()) agree bit-for-bit with the original fixed-size methods they wrap
 * (compute_corotational_forces(), global_mass()) -- per roadmap step 5 in
 * docs/mapa_classes_anflex_estatica.md, which asks for exactly this comparison before trusting
 * the new interface.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "risersim/node.hpp"
#include "risersim/element_beam.hpp"
#include "risersim/element_scalar.hpp"
#include "risersim/element_truss.hpp"
#include "risersim/element_winch.hpp"
#include "risersim/element.hpp"

using namespace risersim;

namespace {

/// A deliberately non-trivial sample configuration: an inclined, stretched, rotated element --
/// exercises the corotational ghost-frame math, not just a degenerate straight/unrotated case.
CorotationalBeam3D* build_sample_element(Node3D*& n1, Node3D*& n2) {
    n1 = new Node3D(1, 0.0, 0.0, 0.0);
    n2 = new Node3D(2, 8.0, 3.0, -4.0);

    // Non-zero displacement and rotation on both nodes, so the ghost frame and local
    // rotations are non-trivial (not just the identity/zero-rotation special case).
    n1->disp = Eigen::Vector3d(0.05, -0.02, 0.03);
    n1->rot  = Eigen::Vector3d(0.01, 0.02, -0.015);
    n2->disp = Eigen::Vector3d(-0.03, 0.06, 0.01);
    n2->rot  = Eigen::Vector3d(-0.02, 0.01, 0.025);

    BeamMaterialProps props; // defaults
    return new CorotationalBeam3D(1, n1, n2, props);
}

} // namespace

TEST_CASE("Element::node()/num_nodes() match CorotationalBeam3D's node1/node2", "[element]") {
    Node3D *n1, *n2;
    auto* elem = build_sample_element(n1, n2);

    REQUIRE(elem->num_nodes() == 2);
    REQUIRE(elem->node(0) == elem->node1());
    REQUIRE(elem->node(1) == elem->node2());
    REQUIRE(elem->node(0) == n1);
    REQUIRE(elem->node(1) == n2);

    delete elem;
    delete n1;
    delete n2;
}

TEST_CASE("Element::assemble() matches compute_corotational_forces() bit-for-bit", "[element]") {
    Node3D *n1, *n2;
    auto* elem = build_sample_element(n1, n2);

    Eigen::Matrix<double, 12, 12> K_ref;
    Eigen::Matrix<double, 12, 1> F_ref;
    elem->compute_corotational_forces(K_ref, F_ref);

    Eigen::MatrixXd K_iface;
    Eigen::VectorXd F_iface;
    static_cast<Element*>(elem)->assemble(K_iface, F_iface);

    REQUIRE(K_iface.rows() == 12);
    REQUIRE(K_iface.cols() == 12);
    REQUIRE(F_iface.size() == 12);

    for (int i = 0; i < 12; ++i) {
        for (int j = 0; j < 12; ++j) {
            REQUIRE(K_iface(i, j) == K_ref(i, j));
        }
        REQUIRE(F_iface(i) == F_ref(i));
    }

    delete elem;
    delete n1;
    delete n2;
}

TEST_CASE("Element::mass_matrix() matches global_mass() bit-for-bit", "[element]") {
    Node3D *n1, *n2;
    auto* elem = build_sample_element(n1, n2);

    const double rho_water = 1025.0;
    Eigen::Matrix<double, 12, 12> M_ref = elem->global_mass(rho_water);
    Eigen::MatrixXd M_iface = static_cast<Element*>(elem)->mass_matrix(rho_water);

    REQUIRE(M_iface.rows() == 12);
    REQUIRE(M_iface.cols() == 12);
    for (int i = 0; i < 12; ++i) {
        for (int j = 0; j < 12; ++j) {
            REQUIRE(M_iface(i, j) == M_ref(i, j));
        }
    }

    delete elem;
    delete n1;
    delete n2;
}

TEST_CASE("ScalarElement: node()/num_nodes() match node1()/node2()", "[element][scalar]") {
    Node3D n1(1, 0.0, 0.0, 0.0), n2(2, 2.0, 0.0, 0.0);
    ScalarProps props;
    ScalarElement elem(1, &n1, &n2, props);

    REQUIRE(elem.num_nodes() == 2);
    REQUIRE(elem.node(0) == elem.node1());
    REQUIRE(elem.node(1) == elem.node2());
    REQUIRE(elem.node(0) == &n1);
    REQUIRE(elem.node(1) == &n2);
}

TEST_CASE("ScalarElement: at rest (zero relative displacement), force is zero", "[element][scalar]") {
    Node3D n1(1, 0.0, 0.0, 0.0), n2(2, 2.0, 0.0, 0.0);
    ScalarProps props;
    props.curve_x = PiecewiseLinearCurve::linear(1000.0);
    props.curve_ry = PiecewiseLinearCurve::linear(500.0);
    ScalarElement elem(1, &n1, &n2, props);

    Eigen::MatrixXd K;
    Eigen::VectorXd F;
    static_cast<Element*>(&elem)->assemble(K, F);

    REQUIRE(K.rows() == 12);
    REQUIRE(F.size() == 12);
    for (int i = 0; i < 12; ++i) REQUIRE(F(i) == Catch::Approx(0.0).margin(1.0e-9));
}

// Chord along global +X: CorotationalBeam3D::build_frame_from_chord((1,0,0)) is exactly
// row0=(1,0,0), i.e. local X coincides with global X (verified against the same real-ANFLEX-
// matching formula this element reuses) -- lets this test predict the axial component directly
// without depending on the local frame's Y/Z sign convention.
TEST_CASE("ScalarElement: axial elongation along the chord produces the classic spring internal-force pair", "[element][scalar]") {
    Node3D n1(1, 0.0, 0.0, 0.0), n2(2, 2.0, 0.0, 0.0);
    const double k = 1000.0; // N/m
    const double du = 0.01;  // m, node2 pulled away from node1 along the chord
    n2.disp = Eigen::Vector3d(du, 0.0, 0.0);

    ScalarProps props;
    props.curve_x = PiecewiseLinearCurve::linear(k);
    ScalarElement elem(1, &n1, &n2, props);

    Eigen::MatrixXd K;
    Eigen::VectorXd F;
    static_cast<Element*>(&elem)->assemble(K, F);

    // Classic 2-point spring internal-force pair (same pattern a truss/bar element's
    // K=[[k,-k],[-k,k]] produces): F1 = -k*(u2-u1), F2 = +k*(u2-u1), along the chord's own +X.
    const double expected = k * du; // 10 N
    REQUIRE(F(0) == Catch::Approx(-expected).margin(1.0e-6));
    REQUIRE(F(1) == Catch::Approx(0.0).margin(1.0e-9));
    REQUIRE(F(2) == Catch::Approx(0.0).margin(1.0e-9));
    REQUIRE(F(6) == Catch::Approx(expected).margin(1.0e-6));
    REQUIRE(F(7) == Catch::Approx(0.0).margin(1.0e-9));
    REQUIRE(F(8) == Catch::Approx(0.0).margin(1.0e-9));
    // No rotation curves set -> zero moment at both nodes.
    for (int i : {3, 4, 5, 9, 10, 11}) REQUIRE(F(i) == Catch::Approx(0.0).margin(1.0e-9));

    REQUIRE(K.rows() == 12);
    REQUIRE(K.cols() == 12);
    for (int i = 0; i < 12; ++i)
        for (int j = 0; j < 12; ++j)
            REQUIRE(K(i, j) == Catch::Approx(K(j, i)).margin(1.0e-9)); // symmetric for a locally-linear curve
}

TEST_CASE("ScalarElement: mass_matrix() is always zero (massless connector)", "[element][scalar]") {
    Node3D n1(1, 0.0, 0.0, 0.0), n2(2, 2.0, 0.0, 0.0);
    ScalarProps props;
    ScalarElement elem(1, &n1, &n2, props);

    Eigen::MatrixXd M = static_cast<Element*>(&elem)->mass_matrix(1025.0);
    REQUIRE(M.rows() == 12);
    REQUIRE(M.cols() == 12);
    REQUIRE(M.isZero());
}

TEST_CASE("TrussElement: node()/num_nodes() match node1()/node2()", "[element][truss]") {
    Node3D n1(1, 0.0, 0.0, 0.0), n2(2, 2.0, 0.0, 0.0);
    TrussProps props;
    TrussElement elem(1, &n1, &n2, props);

    REQUIRE(elem.num_nodes() == 2);
    REQUIRE(elem.node(0) == elem.node1());
    REQUIRE(elem.node(1) == elem.node2());
    REQUIRE(elem.node(0) == &n1);
    REQUIRE(elem.node(1) == &n2);
}

TEST_CASE("TrussElement: at rest (zero strain, no pre-tension), internal force is zero but axial stiffness isn't", "[element][truss]") {
    Node3D n1(1, 0.0, 0.0, 0.0), n2(2, 2.0, 0.0, 0.0);
    TrussProps props;
    props.E = 1000.0;
    props.A = 2.0;
    TrussElement elem(1, &n1, &n2, props);

    static_cast<Element*>(&elem)->update_effective_tension();
    Eigen::MatrixXd K;
    Eigen::VectorXd F;
    static_cast<Element*>(&elem)->assemble(K, F);

    REQUIRE(K.rows() == 12);
    REQUIRE(F.size() == 12);
    for (int i = 0; i < 12; ++i) REQUIRE(F(i) == Catch::Approx(0.0).margin(1.0e-9));

    // EAXL = E*A/L = 1000*2/2 = 1000 -- axial (X-X) stiffness at both ends, B term is zero
    // (axial_force == 0 at rest), matching real cTruss::calc_stiff_mt's EAXL*cos⊗cos + B*I.
    REQUIRE(K(0, 0) == Catch::Approx(1000.0).margin(1.0e-6));
    REQUIRE(K(6, 6) == Catch::Approx(1000.0).margin(1.0e-6));
    REQUIRE(K(0, 6) == Catch::Approx(-1000.0).margin(1.0e-6));
    // No rotational stiffness at all -- real cTruss has no rotational DOF (m_num_dof=3).
    for (int i : {3, 4, 5, 9, 10, 11})
        for (int j = 0; j < 12; ++j)
            REQUIRE(K(i, j) == Catch::Approx(0.0).margin(1.0e-9));
}

// Chord along global +X, node2 pulled directly along it -- direction cosines stay exactly
// (1,0,0), so this test can predict the axial force/force-pair without any local-frame ambiguity.
TEST_CASE("TrussElement: axial elongation produces the classic 2-point bar internal-force pair", "[element][truss]") {
    Node3D n1(1, 0.0, 0.0, 0.0), n2(2, 2.0, 0.0, 0.0);
    const double du = 0.01; // m, node2 pulled away from node1 along the chord
    n2.disp = Eigen::Vector3d(du, 0.0, 0.0);

    TrussProps props;
    props.E = 1000.0;
    props.A = 2.0; // E*A = 2000
    TrussElement elem(1, &n1, &n2, props);

    static_cast<Element*>(&elem)->update_effective_tension();
    const double expected = (du / 2.0) * (props.E * props.A); // strain * E*A = 10 N
    REQUIRE(elem.axial_force() == Catch::Approx(expected).margin(1.0e-6));

    Eigen::MatrixXd K;
    Eigen::VectorXd F;
    static_cast<Element*>(&elem)->assemble(K, F);

    // Node1 pulled toward node2 (+X), node2 pulled toward node1 (-X) -- both ends pulled
    // inward under tension, same pattern (and sign convention) as real cTruss::calc_internal_forces.
    REQUIRE(F(0) == Catch::Approx(expected).margin(1.0e-6));
    REQUIRE(F(1) == Catch::Approx(0.0).margin(1.0e-9));
    REQUIRE(F(2) == Catch::Approx(0.0).margin(1.0e-9));
    REQUIRE(F(6) == Catch::Approx(-expected).margin(1.0e-6));
    REQUIRE(F(7) == Catch::Approx(0.0).margin(1.0e-9));
    REQUIRE(F(8) == Catch::Approx(0.0).margin(1.0e-9));
    for (int i : {3, 4, 5, 9, 10, 11}) REQUIRE(F(i) == Catch::Approx(0.0).margin(1.0e-9));
}

TEST_CASE("TrussElement: initial_tension adds a constant offset to the axial force", "[element][truss]") {
    Node3D n1(1, 0.0, 0.0, 0.0), n2(2, 2.0, 0.0, 0.0); // zero strain
    TrussProps props;
    props.E = 1000.0;
    props.A = 2.0;
    props.initial_tension = 500.0;
    TrussElement elem(1, &n1, &n2, props);

    static_cast<Element*>(&elem)->update_effective_tension();
    REQUIRE(elem.axial_force() == Catch::Approx(500.0).margin(1.0e-9));
}

TEST_CASE("TrussElement: mass_matrix() is a lumped translational mass, no rotational inertia", "[element][truss]") {
    Node3D n1(1, 0.0, 0.0, 0.0), n2(2, 2.0, 0.0, 0.0);
    TrussProps props;
    props.rho = 8000.0;
    props.A = 0.01;
    TrussElement elem(1, &n1, &n2, props); // L = 2.0

    Eigen::MatrixXd M = static_cast<Element*>(&elem)->mass_matrix(1025.0);
    const double expected_lumped = 0.5 * props.rho * props.A * 2.0; // rho*A*L/2 = 80 kg
    REQUIRE(M(0, 0) == Catch::Approx(expected_lumped).margin(1.0e-9));
    REQUIRE(M(1, 1) == Catch::Approx(expected_lumped).margin(1.0e-9));
    REQUIRE(M(2, 2) == Catch::Approx(expected_lumped).margin(1.0e-9));
    REQUIRE(M(6, 6) == Catch::Approx(expected_lumped).margin(1.0e-9));
    for (int i : {3, 4, 5, 9, 10, 11}) REQUIRE(M(i, i) == Catch::Approx(0.0).margin(1.0e-9));
}

TEST_CASE("WinchElement: default payout curve (constant 1.0) behaves exactly like TrussElement", "[element][winch]") {
    Node3D n1(1, 0.0, 0.0, 0.0), n2(2, 2.0, 0.0, 0.0);
    TrussProps props;
    props.E = 1000.0;
    props.A = 2.0;
    WinchElement winch(1, &n1, &n2, props);

    REQUIRE(winch.reference_length() == Catch::Approx(2.0).margin(1.0e-9));
    winch.set_time(5.0); // constant curve -- time shouldn't matter
    REQUIRE(winch.reference_length() == Catch::Approx(2.0).margin(1.0e-9));
}

TEST_CASE("WinchElement: payout curve shortens the reference length and changes the axial force at rest", "[element][winch]") {
    Node3D n1(1, 0.0, 0.0, 0.0), n2(2, 2.0, 0.0, 0.0); // deformed length stays 2.0 (no disp)
    TrussProps props;
    props.E = 1000.0;
    props.A = 2.0; // E*A = 2000
    PiecewiseLinearCurve payout({0.0, 1.0}, {1.0, 0.5}); // pays in to half length by t=1
    WinchElement winch(1, &n1, &n2, props, payout);

    winch.set_time(1.0);
    REQUIRE(winch.reference_length() == Catch::Approx(1.0).margin(1.0e-9)); // 2.0 * 0.5

    static_cast<Element*>(&winch)->update_effective_tension();
    // strain = (L_def - L_ref)/L_ref = (2.0 - 1.0)/1.0 = 1.0 -- the deformed length now exceeds
    // the (shortened) unstretched reference length, so the winch is under tension even though the
    // nodes themselves never moved.
    const double expected = 1.0 * (props.E * props.A);
    REQUIRE(winch.axial_force() == Catch::Approx(expected).margin(1.0e-6));
}
