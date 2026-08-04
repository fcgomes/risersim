/**
 * @file rotation_utils.hpp
 * @brief Finite 3D rotation helpers shared by the corotational element and analysis code.
 */
#ifndef RISERSIM_ROTATION_UTILS_HPP
#define RISERSIM_ROTATION_UTILS_HPP

#include <Eigen/Dense>

namespace risersim {

/**
 * @brief Converts a rotation vector (axis * angle, radians) to its SO(3) rotation matrix via Rodrigues' formula.
 *
 * @param theta Rotation vector; `theta.norm()` is the angle, `theta.normalized()` the axis.
 * @return The corresponding 3x3 rotation matrix (identity for a near-zero angle).
 */
inline Eigen::Matrix3d rodrigues(const Eigen::Vector3d& theta) {
    double angle = theta.norm();
    if (angle < 1.0e-14) return Eigen::Matrix3d::Identity();
    return Eigen::AngleAxisd(angle, theta / angle).toRotationMatrix();
}

/**
 * @brief Properly composes two finite rotations expressed as rotation vectors.
 *
 * Equivalent to ANFLEX's `pseudo_sum()` (`integrator.cpp`), computed via matrix/quaternion
 * composition rather than naive vector addition -- necessary because 3D rotations don't
 * commute and don't add linearly for large angles. (Naive addition is exactly what the
 * original risersim bug did: feeding accumulated total rotation into the bending stiffness
 * instead of the local/deformational rotation -- see `docs/mapa_classes_anflex_estatica.md`.)
 *
 * `delta_theta` is applied *after* `old_theta` (spatial/global composition, matching
 * `pseudo_sum`'s convention: the increment quaternion is composed with the prior one).
 *
 * @param old_theta   Prior accumulated rotation vector.
 * @param delta_theta Incremental rotation vector to compose on top of `old_theta`.
 * @return The composed rotation vector.
 */
inline Eigen::Vector3d compose_rotations(const Eigen::Vector3d& old_theta, const Eigen::Vector3d& delta_theta) {
    Eigen::Matrix3d R_new = rodrigues(delta_theta) * rodrigues(old_theta);
    Eigen::AngleAxisd aa(R_new);
    return aa.angle() * aa.axis();
}

} // namespace risersim

#endif
