/**
 * @file current_profile.hpp
 * @brief Steady ocean current profile: real tabulated depth/velocity/angle data when available,
 * power-law fallback when only a single point is known.
 */
#ifndef RISERSIM_CURRENT_PROFILE_HPP
#define RISERSIM_CURRENT_PROFILE_HPP

#include "risersim/config.hpp"
#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <numbers>
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
    double v_surface;     ///< Current velocity at the sea surface (m/s) -- fallback path only.
    double seabed_depth;  ///< Seabed Z coordinate (m, e.g. -80.0 in the "surface=0" convention, or
                           ///< ~0.0 for a real XML/H5-derived model, see `water_surface_z` below).
    double heading_deg;   ///< Current direction in degrees (0 = +X, 90 = +Y) -- fallback path only.
    double power_exponent;///< Power law exponent alpha (default 1/7 = 0.1428) -- fallback path only.
    double Cd;            ///< Drag coefficient (default 1.0).

    /**
     * @brief Z coordinate of the sea surface, in the SAME frame as `seabed_depth` and the
     * model's node Z coordinates -- NOT assumed to be 0.
     *
     * Defaults to `0.0`, matching risersim's synthetic-model convention (surface at z=0,
     * seabed at negative z, e.g. `seabed_depth=-80.0`) -- so any caller that never sets this
     * (the synthetic fallback path, older callers) keeps its exact previous behavior.
     *
     * Real XML/H5-derived models do NOT use that convention: `ModelBuilder` aligns
     * `seabed_depth_z` with the nodes' own real (unshifted) Z coordinates
     * (`model_builder.cpp`), which for the ANFLEX AML's native frame sit near `z=0` at the
     * SEABED, with the water surface at `z=+water_depth` (e.g. Exemplo_01a: seabed z≈0,
     * surface z≈265) -- the opposite orientation. `get_velocity()`/`get_heading()` need this
     * value to compute "how far below the surface is z" correctly regardless of which frame the
     * caller's nodes actually live in -- see `mapa_classes_anflex_estatica.md` for how assuming
     * `water_surface_z=0` for a real model silently collapsed the whole depth-varying current
     * profile to a single, constant, surface-only value along the entire riser.
     */
    double water_surface_z = 0.0;

    /**
     * @brief Real tabulated profile, sorted ascending by depth below the surface (0 = surface,
     * positive downward -- same convention as the input JSON's environmental.current arrays).
     * Empty by default; populated via set_profile() when the source data has it.
     *
     * Named `depth_below_surface_m`, not `depths_m`, on purpose: the source data (ANFLEX
     * XML/AML) measures depth the OPPOSITE way (0 = seabed, increasing toward the surface) --
     * a same-name-different-convention collision between the JSON field and the XML field is
     * exactly what let a real bug (the whole current profile silently collapsing to the surface
     * value for every depth) go unnoticed for a long time, see
     * docs/mapa_classes_anflex_estatica.md and docs/mapa_aml_exemplos_e_web_interface.md
     * ("Auditoria de conversões de valor", achado 3). Keep the convention in the name when this
     * field is touched again.
     */
    std::vector<double> depth_below_surface_m;
    std::vector<double> velocities_ms;
    std::vector<double> angles_deg;

    CurrentProfile(double v_surf = 1.5, double seabed_z = -80.0, double heading = 90.0, double alpha = 0.1428, double cd = 1.0)
        : v_surface(v_surf), seabed_depth(seabed_z), heading_deg(heading), power_exponent(alpha), Cd(cd) {}

    /** @brief Installs the real tabulated profile (arbitrary point count, sorted ascending by depth). */
    void set_profile(std::vector<double> depths, std::vector<double> vels, std::vector<double> angles) {
        depth_below_surface_m = std::move(depths);
        velocities_ms = std::move(vels);
        angles_deg = std::move(angles);
    }

    /**
     * @brief Current velocity magnitude at depth z.
     *
     * Interpolates the real tabulated profile when 2+ points are available; otherwise falls back
     * to the power-law formula between seabed (0) and surface (`v_surface`).
     * @param z Elevation (m), in the same frame as `water_surface_z`/`seabed_depth` (NOT assumed
     *          to be 0 at the surface -- see `water_surface_z`).
     * @return Velocity magnitude (m/s).
     */
    double get_velocity(double z) const {
        double depth_from_surface = std::max(0.0, water_surface_z - z);
        if (depth_below_surface_m.size() >= 2) {
            return interp1(depth_below_surface_m, velocities_ms, depth_from_surface);
        }

        if (z >= water_surface_z) return v_surface;
        if (z <= seabed_depth) return 0.0;

        double total_water_depth = water_surface_z - seabed_depth;
        double height_above_seabed = z - seabed_depth;

        double ratio = (total_water_depth > 1.0e-9) ? (height_above_seabed / total_water_depth) : 1.0;
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
        double depth_from_surface = std::max(0.0, water_surface_z - z);
        if (angles_deg.size() >= 2 && angles_deg.size() == depth_below_surface_m.size()) {
            return interp1(depth_below_surface_m, angles_deg, depth_from_surface);
        }
        return heading_deg;
    }

    /**
     * @brief Static (quadratic) drag force per unit length at depth z, projected perpendicular to
     * the element's current axis (no axial/tangential drag term -- same simplification already
     * used by the dynamic wave Morison force, `MorisonForce::calculate_force_per_length` in
     * `hydrodynamics.hpp`, "standard practice for risers/mooring lines").
     *
     * Real ANFLEX (`cMorison::calc_distributed_load`, `anf_analysis/src/morison.cpp:49-124`)
     * projects the relative fluid velocity onto normal/tangential element directions before
     * applying the quadratic drag law -- a full 3D vector, not a fixed-heading force applied
     * uniformly regardless of the element's local orientation. This class used to do the latter
     * (`F = f_mag * (cos(heading), sin(heading), 0)`, ignoring `elem_axis` entirely), which
     * over-applies drag on steep/inclined sections of the suspended catenary span (the current's
     * full magnitude counted even where the element runs nearly parallel to it) -- confirmed via
     * a static NODAL DISPLACEMENTS comparison against the real `.SAI` (Near/Transverse/Cross,
     * `Exemplo_01c`): the seabed-lying portion matched to ~0.1m, but the suspended portion was off
     * by up to 8m horizontally, worst for Near. See docs/roadmap.md, Eixo 2a.
     *
     * @param z Elevation (m).
     * @param D_outer Outer diameter used for the projected area (m).
     * @param rho_water Seawater density (kg/m^3).
     * @param elem_axis Unit vector along the element's current axis.
     * @return Drag force per unit length (N/m), perpendicular to `elem_axis`.
     */
    Eigen::Vector3d get_drag_force_per_length(double z, double D_outer, double rho_water,
                                               const Eigen::Vector3d& elem_axis) const {
        double v = get_velocity(z);
        double rad = get_heading(z) * std::numbers::pi / 180.0;
        Eigen::Vector3d v_fluid(v * std::cos(rad), v * std::sin(rad), 0.0);

        Eigen::Vector3d v_perp = v_fluid - (v_fluid.dot(elem_axis)) * elem_axis;

        return 0.5 * rho_water * Cd * D_outer * v_perp.norm() * v_perp;
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
