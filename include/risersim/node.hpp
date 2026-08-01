#ifndef RISERSIM_NODE_HPP
#define RISERSIM_NODE_HPP

#include <Eigen/Dense>
#include <vector>

namespace risersim {

class Node3D {
public:
    int id;
    Eigen::Vector3d coords;      // Initial position (X, Y, Z)
    Eigen::Vector3d disp;        // Displacement vector (u_x, u_y, u_z)
    Eigen::Vector3d rot;         // Rotation vector (theta_x, theta_y, theta_z)
    
    // Global equation numbers for 6 DOFs (-1 if constrained)
    std::vector<int> eq_numbers;

    Node3D(int node_id, double x, double y, double z)
        : id(node_id), coords(x, y, z), disp(0, 0, 0), rot(0, 0, 0), eq_numbers(6, -1) {}

    Eigen::Vector3d current_coords() const {
        return coords + disp;
    }
};

} // namespace risersim

#endif
