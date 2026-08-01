#ifndef RISERSIM_BUOYANCY_AND_RESTRICTOR_HPP
#define RISERSIM_BUOYANCY_AND_RESTRICTOR_HPP

#include "risersim/element_beam.hpp"
#include <vector>

namespace risersim {

// Class for intermediate buoyancy modules (creates Lazy Wave S-shape)
class BuoyancyModule {
public:
    double D_buoyancy;       // Outer diameter of the buoyancy module (m)
    double net_upward_force; // Net extra upward buoyancy force per meter (N/m)

    BuoyancyModule(double d_buoy = 0.80, double net_force = 1200.0)
        : D_buoyancy(d_buoy), net_upward_force(net_force) {}

    void apply_to_element(CorotationalBeam3D* elem) const {
        elem->props.D_outer = D_buoyancy;
    }
};

// Class for Bend Restrictor (protects riser near platform top by increasing EI)
class BendRestrictor {
public:
    double stiffness_multiplier; // Factor to increase flexural rigidity EI (e.g. 5.0x)

    BendRestrictor(double factor = 5.0) : stiffness_multiplier(factor) {}

    void apply_to_element(CorotationalBeam3D* elem) const {
        elem->props.IY *= stiffness_multiplier;
        elem->props.IZ *= stiffness_multiplier;
    }
};

} // namespace risersim

#endif
