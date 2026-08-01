#ifndef RISERSIM_SEABED_HPP
#define RISERSIM_SEABED_HPP

#include <Eigen/Dense>
#include <cmath>

namespace risersim {

class SeabedInteraction {
public:
    double seabed_depth;   // Z coordinate of the seabed (e.g. -100.0 m)
    double stiffness_z;    // Vertical soil stiffness (N/m^2 or N/m per node, default 1e5)
    double friction_coeff; // Lateral/axial friction coefficient mu (default 0.5)

    SeabedInteraction(double depth = -100.0, double kz = 1.0e5, double mu = 0.5)
        : seabed_depth(depth), stiffness_z(kz), friction_coeff(mu) {}

    // Calculate normal reaction force and stiffness contribution for a node
    void calculate_seabed_reaction(double z_current, double& f_normal, double& k_normal) const {
        if (z_current < seabed_depth) {
            double penetration = seabed_depth - z_current;
            f_normal = stiffness_z * penetration; // Upward force (+Z)
            k_normal = stiffness_z;                // Tangent stiffness
        } else {
            f_normal = 0.0;
            k_normal = 0.0;
        }
    }
};

} // namespace risersim

#endif
