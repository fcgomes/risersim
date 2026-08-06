/**
 * @file current_profile.hpp
 * @brief Steady ocean current profile: real tabulated depth/velocity/angle data when available,
 * power-law fallback when only a single point is known.
 */
#ifndef RISERSIM_CURRENT_PROFILE_HPP
#define RISERSIM_CURRENT_PROFILE_HPP

#include "risersim/config.hpp"
#include <algorithm>
#include <vector>


namespace risersim {

/**
 * @brief Depth-varying current velocity/heading and per-length steady drag force.
 *
 * Real models (ANFLEX XML/AML) carry a full tabulated profile -- an arbitrary number of
 * (depth, velocity, angle) points -- rather than a single surface value. When that table has 2+
 * points (set via set_profile()), get_velocity()/get_heading() linearly interpolate it directly.
 * The power-law formula (`v_surface * ratio^power_exponent`, fixed `heading_deg`) is kept only as
 * a fallback for the degenerate 1-point (or no-profile) case, where there isn't enough data to
 * interpolate and a synthesized profile shape is the best available approximation.
 */
class CurrentProfile {
public:
    double v_surface;     ///< Current velocity at sea surface z=0 (m/s) -- fallback path only.
    double seabed_depth;  ///< Seabed depth z_seabed (m, e.g. -80.0) -- fallback path only.
    double heading_deg;   ///< Current direction in degrees (0 = +X, 90 = +Y) -- fallback path only.
    double power_exponent;///< Power law exponent alpha (default 1/7 = 0.1428) -- fallback path only.
    double Cd;            ///< Drag coefficient (default 1.0).

    /**
     * @brief Real tabulated profile, sorted ascending by depth_from_surface (0 = surface, positive
     * downward -- same convention as the input JSON's environmental.current arrays). Empty by
     * default; populated via set_profile() when the source data has it.
     */
    std::vector<double> depths_m;
    std::vector<double> velocities_ms;
    std::vector<double> angles_deg;

    CurrentProfile(double v_surf = 1.5, double seabed_z = -80.0, double heading = 90.0, double alpha = 0.1428, double cd = 1.0)
        : v_surface(v_surf), seabed_depth(seabed_z), heading_deg(heading), power_exponent(alpha), Cd(cd) {}

    /** @brief Installs the real tabulated profile (arbitrary point count, sorted ascending by depth). */
    void set_profile(std::vector<double> depths, std::vector<double> vels, std::vector<double> angles) {
        depths_m = std::move(depths);
        velocities_ms = std::move(vels);
        angles_deg = std::move(angles);
    }

    /**
     * @brief Current velocity magnitude at depth z.
     *
     * Interpolates the real tabulated profile when 2+ points are available; otherwise falls back
     * to the power-law formula between seabed (0) and surface (`v_surface`).
     * @param z Elevation (m), 0 at the surface, negative below it.
     * @return Velocity magnitude (m/s).
     */
    double get_velocity(double z) const {
        double depth_from_surface = std::max(0.0, -z);
        if (depths_m.size() >= 2) {
            return interp1(depths_m, velocities_ms, depth_from_surface);
        }

        if (z >= 0.0) return v_surface;
        if (z <= seabed_depth) return 0.0;

        double total_water_depth = std::abs(seabed_depth);
        double height_above_seabed = total_water_depth - depth_from_surface;

        double ratio = height_above_seabed / total_water_depth;
        ratio = std::max(0.0, std::min(1.0, ratio));

        return v_surface * std::pow(ratio, power_exponent);
    }

    /**
     * @brief Current heading at depth z (degrees, 0 = +X, 90 = +Y).
     *
     * Interpolates the real tabulated profile when 2+ angle points are available; otherwise
     * falls back to the single fixed `heading_deg`.
     */
    double get_heading(double z) const {
        double depth_from_surface = std::max(0.0, -z);
        if (angles_deg.size() >= 2 && angles_deg.size() == depths_m.size()) {
            return interp1(depths_m, angles_deg, depth_from_surface);
        }
        return heading_deg;
    }

    /**
     * @brief Static (quadratic) drag force per unit length at depth z, resolved by get_heading(z).
     * @param z Elevation (m).
     * @param D_outer Outer diameter used for the projected area (m).
     * @param rho_water Seawater density (kg/m^3).
     * @param[out] F_drag_x Drag force per meter along X (N/m).
     * @param[out] F_drag_y Drag force per meter along Y (N/m).
     */
    void get_drag_force_per_meter(double z, double D_outer, double rho_water, double& F_drag_x, double& F_drag_y) const {
        double v = get_velocity(z);
        double f_mag = 0.5 * rho_water * Cd * D_outer * v * v;

        double rad = get_heading(z) * std::numbers::pi / 180.0;
        F_drag_x = f_mag * std::cos(rad);
        F_drag_y = f_mag * std::sin(rad);
    }

private:
    /**
     * @brief Linear interpolation of y_table(x_table) at x -- x_table must be sorted ascending.
     * Outside the table's range, holds the nearest edge value (no extrapolation).
     */
    static double interp1(const std::vector<double>& x_table, const std::vector<double>& y_table, double x) {
        const size_t n = x_table.size();
        if (n == 0) return 0.0;
        if (n == 1 || x <= x_table.front()) return y_table.front();
        if (x >= x_table.back()) return y_table.back();
        for (size_t i = 1; i < n; ++i) {
            if (x <= x_table[i]) {
                double x0 = x_table[i - 1], x1 = x_table[i];
                double t = (x1 > x0) ? (x - x0) / (x1 - x0) : 0.0;
                return y_table[i - 1] + t * (y_table[i] - y_table[i - 1]);
            }
        }
        return y_table.back();
    }
};

} // namespace risersim

#endif
