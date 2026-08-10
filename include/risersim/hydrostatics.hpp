/**
 * @file hydrostatics.hpp
 * @brief Partial-submersion buoyancy force + tangent stiffness for a beam element crossing the
 * free surface -- ported from real ANFLEX's cNL_Hidrostatics (trunk/src/nl_hidrostatic.{h,cpp}).
 *
 * risersim's force assembly (static AND dynamic) used to treat buoyancy as a CONSTANT per
 * element regardless of Z position -- an element straddling, fully above, or fully below the
 * water surface all got the same full-submerged buoyancy force, and zero associated tangent
 * stiffness. Real ANFLEX instead scales buoyancy by the actual submerged cross-sectional area
 * (a circular segment, not a step function) and supplies the matching analytic stiffness -- see
 * docs/roadmap.md item 1b for the investigation that traced a dynamic Newton divergence to
 * exactly this gap: a node crossing Z=0 mid-simulation, on an otherwise smooth trajectory, had
 * no restoring stiffness gradient right at the crossing, and the residual exploded there.
 *
 * Two deliberate simplifications relative to real ANFLEX, both scoped to "Fase 1" of that
 * investigation (see docs/roadmap.md item 1b) -- revisit if/when per-end wave elevation is wired
 * up (Fase 4, alongside hydrodynamics.hpp's Morison force):
 *  - **Flat water surface**: `compute()` takes a single `water_surface_z` shared by both ends.
 *    Real ANFLEX allows a different instantaneous water line per end (real wave elevation can
 *    differ slightly across an element's span). Not needed until a spatial wave field exists.
 *  - **Centerline-Z submersion depth**: `h[i] = water_surface_z - z_end[i]` here, the vertical
 *    clearance of each END NODE's centerline point. Real ANFLEX's `distance_to_water()` instead
 *    measures the perpendicular distance from the tube's outer surface to the water line along
 *    the element's own inclined axis, which differs slightly from a pure vertical Z difference
 *    for a steeply inclined element. For a moderately-inclined catenary riser element the
 *    difference is small; a smooth, correctly-signed stiffness right at the crossing (the actual
 *    fix target) doesn't depend on this refinement.
 */
#ifndef RISERSIM_HYDROSTATICS_HPP
#define RISERSIM_HYDROSTATICS_HPP

#include "risersim/config.hpp"
#include <algorithm>

namespace risersim {

/**
 * @brief Buoyancy force + tangent stiffness at each end of a submerged/partially-submerged
 * cylindrical beam element, as a function of each end's depth below the (flat) water surface.
 *
 * Five regimes, exactly mirroring real ANFLEX's cNL_Hidrostatics::calc_nl_hidrostatics(): both
 * ends dry (zero force/stiffness), both ends fully submerged (constant force -- matches
 * risersim's PREVIOUS constant-buoyancy behavior exactly in this one regime, zero stiffness,
 * since submersion doesn't change here), one end fully wet / other fully dry (linear
 * length-based transfer coefficients alpha0/alpha1), and both ends partially wet (submerged-area-
 * and area-derivative-weighted transfer). Only the last two regimes -- an element actually
 * straddling or near the surface -- differ from today's constant-buoyancy behavior.
 */
class Hydrostatics {
public:
    /**
     * @param diameter Outer diameter of the cylindrical element (m).
     * @param length Real 3D chord length of the element (m) -- current_length(), not a
     *               horizontal projection (see the file-level scope note about simplifying
     *               ANFLEX's general sloped-water-line geometry).
     * @param fluid_density Seawater density (kg/m^3).
     */
    Hydrostatics(double diameter, double length, double fluid_density)
        : radius_(0.5 * diameter), length_(length), fluid_density_(fluid_density) {}

    /**
     * @brief Computes buoyancy force + tangent stiffness at both ends from each end's Z and the
     * (flat) water surface Z. Results retrievable via end_force()/end_stiffness() afterward.
     * @param z_end Z coordinate of each end (m) -- z_end[0]/z_end[1] matching node1/node2.
     * @param water_surface_z Z coordinate of the (flat) water surface (m).
     */
    void compute(const double z_end[2], double water_surface_z) {
        double h[2];
        for (int i = 0; i < 2; ++i) h[i] = water_surface_z - z_end[i]; // >0 == submerged by that much

        const double diam = 2.0 * radius_;
        if (h[0] <= 0.0 && h[1] <= 0.0) {
            // Both ends dry.
            force_[0] = force_[1] = 0.0;
            stiffness_[0] = stiffness_[1] = 0.0;
        } else if (h[0] >= diam && h[1] >= diam) {
            // Both ends fully submerged -- same constant force risersim always used to apply
            // everywhere; zero stiffness (nothing here depends on Z once fully submerged).
            double b_force = length_ * fluid_density_ * (std::numbers::pi * radius_ * radius_) * 0.5;
            force_[0] = force_[1] = b_force;
            stiffness_[0] = stiffness_[1] = 0.0;
        } else if (h[0] >= diam && h[1] <= 0.0) {
            // End 0 fully submerged, end 1 fully dry.
            double b_stiff = fluid_density_ * (std::numbers::pi * radius_ * radius_);
            double b_force = length_ * b_stiff;
            double ls = submerged_length(z_end, water_surface_z);
            double alpha0 = (ls / length_) * (1.0 - ls / (2.0 * length_));
            double alpha1 = 0.5 * (ls / length_) * (ls / length_);
            force_[0] = b_force * alpha0; force_[1] = b_force * alpha1;
            stiffness_[0] = b_stiff * alpha0; stiffness_[1] = b_stiff * alpha1;
        } else if (h[0] <= 0.0 && h[1] >= diam) {
            // End 1 fully submerged, end 0 fully dry -- mirror of the above.
            double b_stiff = fluid_density_ * (std::numbers::pi * radius_ * radius_);
            double b_force = length_ * b_stiff;
            double ls = submerged_length(z_end, water_surface_z);
            double alpha0 = 0.5 * (ls / length_) * (ls / length_);
            double alpha1 = (ls / length_) * (1.0 - ls / (2.0 * length_));
            force_[0] = b_force * alpha0; force_[1] = b_force * alpha1;
            stiffness_[0] = b_stiff * alpha0; stiffness_[1] = b_stiff * alpha1;
        } else {
            // Both ends partially wet -- area/derivative assumed to vary linearly along the
            // element (same assumption real ANFLEX makes here).
            double a0 = submerged_area(h[0]), a1 = submerged_area(h[1]);
            double s0 = section_stiffness(h[0]), s1 = section_stiffness(h[1]);
            double alpha0 = (a0 + a1 > 1.0e-12) ? (2.0 * a0 + a1) / (3.0 * (a0 + a1)) : 0.5;
            double alpha1 = 1.0 - alpha0;
            double b_force = fluid_density_ * length_ * (a0 + a1) * 0.5;
            double b_stiff = fluid_density_ * length_ * (s0 + s1) * 0.5;
            force_[0] = b_force * alpha0; force_[1] = b_force * alpha1;
            stiffness_[0] = b_stiff * alpha0; stiffness_[1] = b_stiff * alpha1;
        }
    }

    /** @brief Buoyancy force (N) at end i (0 or 1) from the last compute() call. */
    double end_force(int i_end) const { return force_[i_end]; }
    /** @brief Buoyancy tangent stiffness (N/m, diagonal-only -- see file-level note in the
     * callers about how this is wired into K) at end i (0 or 1) from the last compute() call. */
    double end_stiffness(int i_end) const { return stiffness_[i_end]; }

private:
    double radius_, length_, fluid_density_;
    double force_[2] = {0.0, 0.0};
    double stiffness_[2] = {0.0, 0.0};

    /** @brief Submerged circular-segment cross-sectional area for a submersion depth h (0 = dry,
     * 2*radius = fully submerged) -- ported verbatim from cNL_Hidrostatics::get_submerged_area. */
    double submerged_area(double h) const {
        double r = radius_, r2 = r * r;
        if (h <= 0.0) return 0.0;
        if (h <= r) {
            return r2 * std::asin(std::sqrt(2.0 * h * r - h * h) / r)
                 - std::sqrt(h * (2.0 * r - h) * (h - r) * (h - r));
        }
        if (h < 2.0 * r) {
            return std::numbers::pi * r2 - r2 * std::asin(std::sqrt(2.0 * h * r - h * h) / r)
                 + std::sqrt(h * (2.0 * r - h) * (h - r) * (h - r));
        }
        return std::numbers::pi * r2;
    }

    /** @brief d(submerged_area)/dh -- ported verbatim from
     * cNL_Hidrostatics::get_section_stiffness. */
    double section_stiffness(double h) const {
        double r = radius_, r2 = r * r;
        if (h <= 0.0) return 0.0;
        if (h < 2.0 * r) {
            return r2 * (1.0 - std::cos(2.0 * std::asin(std::sqrt(2.0 * h * r - h * h) / r)))
                 / std::sqrt(2.0 * h * r - h * h);
        }
        return 0.0;
    }

    /** @brief Fraction of the element's real 3D chord length below the (flat) water surface --
     * simplified from ANFLEX's general sloped-water-line formula (see file-level scope note):
     * direct linear interpolation along Z between the two given ends. Only called in the
     * one-end-fully-wet/one-end-fully-dry regimes, where exactly one crossing point exists. */
    double submerged_length(const double z_end[2], double water_surface_z) const {
        double z0 = z_end[0], z1 = z_end[1];
        if (std::abs(z1 - z0) < 1.0e-9) return length_; // degenerate (near-horizontal at the line) -- treat as fully wet, matches this regime's physical limit as z1->z0
        double t = std::clamp((water_surface_z - z0) / (z1 - z0), 0.0, 1.0);
        double length_from_end0 = t * length_;
        return (z0 < water_surface_z) ? length_from_end0 : (length_ - length_from_end0);
    }
};

} // namespace risersim

#endif // RISERSIM_HYDROSTATICS_HPP
