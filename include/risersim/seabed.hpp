/**
 * @file seabed.hpp
 * @brief Seabed vertical reaction and Coulomb friction models, modeled after ANFLEX's `soil.cpp`/`soil_uncoupled.cpp`.
 */
#ifndef RISERSIM_SEABED_HPP
#define RISERSIM_SEABED_HPP

#include <Eigen/Dense>
#include <cmath>

namespace risersim {

/**
 * @brief Vertical (normal) reaction and axial/lateral Coulomb friction at the seabed.
 */
class SeabedInteraction {
public:
    double seabed_depth;   ///< Z coordinate of the seabed (e.g. -100.0 m).
    double stiffness_z;    ///< Initial vertical soil stiffness k_inicial (N/m per node, default 1e5).
    double friction_coeff; ///< Default/fallback friction coefficient mu (used if axial/lateral aren't overridden).
    double elastic_deflection_limit; ///< Default/fallback displacement (m) that mobilizes the friction limit.
    double ultimate_bearing_force;   ///< Ultimate vertical bearing capacity per node f_ultima (N).

    /**
     * @brief Direction-specific friction coefficients and elastic deflection limits.
     *
     * Axial (along the line) and lateral (perpendicular, horizontal plane) are kept distinct,
     * mirroring ANFLEX's `soil.h` (`m_axial_friction`/`m_lateral_friction`,
     * `m_axial_elastic_deflection_limit`/`m_lateral_elastic_deflection_limit`). Real models can
     * have very different values per direction -- e.g. `Exemplo_01a_A1.xml` uses axial =
     * 0.92/0.03 m vs. lateral = 0.95/0.279 m. Default to `friction_coeff`/`elastic_deflection_limit`;
     * the caller may overwrite them with a model's real values.
     */
    double axial_friction;
    double lateral_friction;
    double axial_elastic_deflection_limit;
    double lateral_elastic_deflection_limit;

    SeabedInteraction(double depth = -100.0, double kz = 1.0e5, double mu = 0.5, double u_limit = 0.05, double f_ult = 5000.0)
        : seabed_depth(depth), stiffness_z(kz), friction_coeff(mu), elastic_deflection_limit(u_limit), ultimate_bearing_force(f_ult),
          axial_friction(mu), lateral_friction(mu),
          axial_elastic_deflection_limit(u_limit), lateral_elastic_deflection_limit(u_limit) {}

    /**
     * @brief Computes the normal (vertical) seabed reaction force and its tangent stiffness.
     *
     * Nonlinear hyperbolic model, the same functional form used by real offshore geotechnical
     * P-Y/T-Z curves: `f(pen) = pen / (1/k_inicial + pen/f_ultima)`. Tangent stiffness starts at
     * `k_inicial = stiffness_z` (soft) and saturates asymptotically toward `f_ultima` as
     * penetration grows -- instead of an abrupt jump to full linear stiffness the instant
     * `pen > 0`, which caused contact "chattering"/ill-conditioning when many nodes touch down
     * nearly simultaneously (common when the seabed already intersects much of the mesh in the
     * initial configuration).
     *
     * @param z_current Current Z coordinate of the node.
     * @param[out] f_normal Normal reaction force (0 if not penetrating).
     * @param[out] k_normal Tangent stiffness df/dpen (0 if not penetrating).
     */
    void calculate_seabed_reaction(double z_current, double& f_normal, double& k_normal) const {
        double pen = seabed_depth - z_current; // > 0 quando penetrando
        if (pen <= 0.0) {
            f_normal = 0.0;
            k_normal = 0.0;
            return;
        }
        double a = 1.0 / stiffness_z;
        double b = 1.0 / ultimate_bearing_force;
        double denom = a + b * pen;
        f_normal = pen / denom;
        k_normal = a / (denom * denom); // df/dpen
    }

    /**
     * @brief Incremental elastic-plastic 1D Coulomb friction spring.
     *
     * Mirrors ANFLEX's real model (`soil_uncoupled.cpp::calc_unidimensional_friction`): linear
     * elastic regime with `k = mu*|Fn|/u_limit`, but the force is accumulated incrementally from
     * the persistent state carried over from the previous iteration (`f_state`, in/out --
     * equivalent to ANFLEX's `m_forces[0]`/`m_forces[1]`) using *this iteration's* displacement
     * increment (`du`, equivalent to `get_delta_dx()`) -- not the total accumulated displacement.
     *
     * Using the total displacement (an earlier version of this method) makes the force saturate
     * permanently once the total displacement since t=0 exceeds the elastic limit, even if the
     * node isn't actually slipping right now -- physically wrong, and a real cause of the contact
     * "chattering" found when combining seabed + current loading (see
     * `docs/mapa_classes_anflex_estatica.md`).
     *
     * When contact is lost (`f_normal == 0`), the caller must reset `f_state` to zero.
     * `mu`/`u_limit` are passed explicitly (rather than reading `friction_coeff`/
     * `elastic_deflection_limit` directly) so this function can be called once for the axial
     * direction and once for the lateral direction, with distinct values each time.
     *
     * @param mu Friction coefficient for this direction (axial_friction or lateral_friction).
     * @param u_limit Elastic deflection limit for this direction.
     * @param f_normal Current normal reaction force (defines the plastic cap `mu*|f_normal|`).
     * @param du This iteration's displacement increment along this direction.
     * @param[in,out] f_state Persistent friction force state for this direction.
     * @param[out] k_friction Tangent stiffness (0 once the plastic cap is reached).
     */
    void calculate_friction_1d(double mu, double u_limit, double f_normal, double du, double& f_state, double& k_friction) const {
        double fl = mu * std::fabs(f_normal);
        if (fl < 1.0e-9 || u_limit < 1.0e-9) {
            f_state = 0.0;
            k_friction = 0.0;
            return;
        }
        k_friction = fl / u_limit;
        f_state += k_friction * du;
        if (std::fabs(f_state) > fl) {
            f_state = (f_state > 0.0) ? fl : -fl;
            k_friction = 0.0;
        }
    }
};

} // namespace risersim

#endif
