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
