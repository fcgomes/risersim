#ifndef RISERSIM_CURRENT_PROFILE_HPP
#define RISERSIM_CURRENT_PROFILE_HPP

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif

#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace risersim {

class CurrentProfile {
public:
    double v_surface;     // Current velocity at sea surface z=0 (m/s)
    double seabed_depth;  // Seabed depth z_seabed (m, e.g. -80.0)
    double heading_deg;   // Current direction in degrees (0 = +X, 90 = +Y)
    double power_exponent;// Power law exponent alpha (default 1/7 = 0.1428)
    double Cd;            // Drag coefficient (default 1.0)

    CurrentProfile(double v_surf = 1.5, double seabed_z = -80.0, double heading = 90.0, double alpha = 0.1428, double cd = 1.0)
        : v_surface(v_surf), seabed_depth(seabed_z), heading_deg(heading), power_exponent(alpha), Cd(cd) {}

    // Calculate current velocity magnitude at depth z (m/s)
    double get_velocity(double z) const {
        if (z >= 0.0) return v_surface;
        if (z <= seabed_depth) return 0.0;

        double depth_from_surface = std::abs(z);
        double total_water_depth = std::abs(seabed_depth);
        double height_above_seabed = total_water_depth - depth_from_surface;

        double ratio = height_above_seabed / total_water_depth;
        ratio = std::max(0.0, std::min(1.0, ratio));

        return v_surface * std::pow(ratio, power_exponent);
    }

    // Calculate static drag force vector per meter at depth z (N/m)
    void get_drag_force_per_meter(double z, double D_outer, double rho_water, double& F_drag_x, double& F_drag_y) const {
        double v = get_velocity(z);
        double f_mag = 0.5 * rho_water * Cd * D_outer * v * v;

        double rad = heading_deg * M_PI / 180.0;
        F_drag_x = f_mag * std::cos(rad);
        F_drag_y = f_mag * std::sin(rad);
    }
};

} // namespace risersim

#endif
