/**
 * @file buoyancy_and_restrictor.hpp
 * @brief Localized element property modifiers for buoyancy modules and bend restrictors.
 */
#ifndef RISERSIM_BUOYANCY_AND_RESTRICTOR_HPP
#define RISERSIM_BUOYANCY_AND_RESTRICTOR_HPP

#include "risersim/element_beam.hpp"
#include <vector>

namespace risersim {

/** @brief Intermediate buoyancy module (creates the lazy-wave S-shape) applied to a range of elements. */
class BuoyancyModule {
public:
    double D_buoyancy;       ///< Outer diameter of the buoyancy module (m).
    double net_upward_force; ///< Net extra upward buoyancy force per meter (N/m).

    BuoyancyModule(double d_buoy = 0.80, double net_force = 3600.0)
        : D_buoyancy(d_buoy), net_upward_force(net_force) {}

    /** @brief Overwrites `elem`'s outer diameter and net upward buoyancy with this module's values. */
    void apply_to_element(CorotationalBeam3D* elem) const {
        elem->props().D_outer = D_buoyancy;
        elem->set_net_upward_buoyancy(net_upward_force);
    }
};

/** @brief Bend restrictor (protects the riser near the platform top by locally increasing EI). */
class BendRestrictor {
public:
    double stiffness_multiplier; ///< Factor to increase flexural rigidity EI (e.g. 5.0x).

    BendRestrictor(double factor = 5.0) : stiffness_multiplier(factor) {}

    /** @brief Multiplies `elem`'s IY/IZ moments of inertia by `stiffness_multiplier`. */
    void apply_to_element(CorotationalBeam3D* elem) const {
        elem->props().IY *= stiffness_multiplier;
        elem->props().IZ *= stiffness_multiplier;
    }
};

} // namespace risersim

#endif
