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

    // Estado persistente da mola de atrito do solo (elástico-plástica
    // incremental, fiel ao ANFLEX real -- ver seabed.hpp:calculate_friction_1d).
    // friction_force: forca de atrito acumulada (x,y), equivalente a m_forces[0]/[1].
    // delta_disp_xy: incremento de deslocamento (x,y) da ULTIMA atualizacao de NR,
    // usado como "du" na proxima chamada -- equivalente a get_delta_dx()/dy().
    Eigen::Vector2d friction_force = Eigen::Vector2d::Zero();
    Eigen::Vector2d delta_disp_xy = Eigen::Vector2d::Zero();

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
