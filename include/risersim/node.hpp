/**
 * @file node.hpp
 * @brief 3D structural node: position, displacement/rotation state, and per-node friction state.
 */
#ifndef RISERSIM_NODE_HPP
#define RISERSIM_NODE_HPP

#include <Eigen/Dense>
#include <vector>

namespace risersim {

/**
 * @brief A 3D node with 6 DOFs (translation + rotation), equivalent to ANFLEX's `cNode`.
 */
class Node3D {
public:
    int id;
    Eigen::Vector3d coords;      ///< Initial position (X, Y, Z).
    Eigen::Vector3d disp;        ///< Displacement vector (u_x, u_y, u_z).
    Eigen::Vector3d rot;         ///< Total accumulated rotation vector (theta_x, theta_y, theta_z).

    /**
     * @brief Accumulated seabed friction force (axial, lateral), persistent across iterations.
     *
     * Part of the incremental elastic-plastic friction model -- see
     * SeabedInteraction::calculate_friction_1d(). Equivalent to ANFLEX's `m_forces[0]`/`m_forces[1]`.
     */
    Eigen::Vector2d friction_force = Eigen::Vector2d::Zero();

    /**
     * @brief Displacement increment (X, Y) from the last Newton-Raphson update.
     *
     * Consumed as `du` by the next SeabedInteraction::calculate_friction_1d() call, then reset
     * to zero. Equivalent to ANFLEX's `get_delta_dx()`/`get_delta_dy()`.
     */
    Eigen::Vector2d delta_disp_xy = Eigen::Vector2d::Zero();

    std::vector<int> eq_numbers; ///< Global equation numbers for the 6 DOFs (-1 if constrained).

    Node3D(int node_id, double x, double y, double z)
        : id(node_id), coords(x, y, z), disp(0, 0, 0), rot(0, 0, 0), eq_numbers(6, -1) {}

    /** @brief Current (deformed) position: initial coordinates plus displacement. */
    Eigen::Vector3d current_coords() const {
        return coords + disp;
    }
};

} // namespace risersim

#endif
