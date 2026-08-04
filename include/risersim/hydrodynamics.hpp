/**
 * @file hydrodynamics.hpp
 * @brief JONSWAP wave spectrum, linear (Airy) wave kinematics, and Morison hydrodynamic loading, for time-domain dynamic analysis.
 */
#ifndef RISERSIM_HYDRODYNAMICS_HPP
#define RISERSIM_HYDRODYNAMICS_HPP

#include "risersim/config.hpp"
#include <Eigen/Dense>
#include <vector>
#include <random>


namespace risersim {

/** @brief A single frequency component of an irregular sea state, generated from a wave spectrum. */
struct WaveWaveComponent {
    double frequency; ///< omega_k (rad/s).
    double wavenumber;///< k_k (rad/m).
    double amplitude; ///< a_k (m).
    double phase;     ///< phi_k (rad).
};

/**
 * @brief JONSWAP wave spectrum: energy density S(omega) and random-phase component decomposition.
 */
class JONSWAPSpectrum {
public:
    double Hs;     ///< Significant wave height (m).
    double Tp;     ///< Peak wave period (s).
    double gamma;  ///< Peak enhancement factor (default 3.3).
    double depth;  ///< Water depth (m).

    JONSWAPSpectrum(double hs, double tp, double d, double g_factor = 3.3)
        : Hs(hs), Tp(tp), depth(d), gamma(g_factor) {}

    /**
     * @brief Evaluates the JONSWAP spectral energy density S(omega).
     * @param omega Angular frequency (rad/s).
     * @return Spectral density at `omega` (m^2*s), 0 for `omega <= 0`.
     */
    double evaluate(double omega) const {
        if (omega <= 0.0) return 0.0;
        double omega_p = 2.0 * std::numbers::pi / Tp;
        double g = 9.81;

        double sigma = (omega <= omega_p) ? 0.07 : 0.09;
        double r = std::exp(-std::pow(omega - omega_p, 2.0) / (2.0 * sigma * sigma * omega_p * omega_p));

        double alpha = 5.06 * std::pow(Hs / (Tp * Tp), 2.0) * (1.0 - 0.287 * std::log(gamma));
        double S_pm = (alpha * g * g / std::pow(omega, 5.0)) * std::exp(-1.25 * std::pow(omega_p / omega, 4.0));

        return S_pm * std::pow(gamma, r);
    }

    /**
     * @brief Discretizes the spectrum into `N_components` random-phase wave components.
     *
     * Amplitude of each component follows `a_k = sqrt(2*S(omega_k)*d_omega)`; wavenumber is
     * solved from the dispersion relation `omega^2 = g*k*tanh(k*depth)` by fixed-point iteration;
     * phase is drawn uniformly from `[0, 2*pi)`.
     *
     * @param N_components Number of frequency components.
     * @param omega_max Upper frequency bound (rad/s).
     * @param seed RNG seed (deterministic by default, for reproducible dynamic runs).
     * @return The generated wave components.
     */
    std::vector<WaveWaveComponent> generate_wave_components(int N_components = 50, double omega_max = 3.0, unsigned int seed = 42) const {
        std::vector<WaveWaveComponent> components;
        double d_omega = omega_max / N_components;
        double g = 9.81;

        std::mt19937 rng(seed);
        std::uniform_real_distribution<double> phase_dist(0.0, 2.0 * std::numbers::pi);

        for (int i = 1; i <= N_components; ++i) {
            double w = i * d_omega;
            double S_w = evaluate(w);
            double amp = std::sqrt(2.0 * S_w * d_omega);

            // Wavenumber k from dispersion relation: w^2 = g * k * tanh(k * depth)
            double k = w * w / g; // Deep water approximation for initial guess
            for (int iter = 0; iter < 5; ++iter) {
                k = w * w / (g * std::tanh(k * depth));
            }

            double phase = phase_dist(rng);

            components.push_back({w, k, amp, phase});
        }

        return components;
    }
};

/**
 * @brief Linear (Airy) wave theory: fluid particle velocity and acceleration from a set of wave components.
 */
class AiryWaveKinematics {
public:
    std::vector<WaveWaveComponent> components;
    double depth;

    AiryWaveKinematics(const std::vector<WaveWaveComponent>& comps, double d)
        : components(comps), depth(d) {}

    /**
     * @brief Superposes the fluid velocity and acceleration of all wave components at a point in space/time.
     * @param x Horizontal position (m).
     * @param z Elevation, 0 at the surface, negative below it (m).
     * @param t Time (s).
     * @param[out] vel Fluid velocity vector (m/s); only X/Z components are populated (2D wave kinematics).
     * @param[out] accel Fluid acceleration vector (m/s^2); only X/Z components are populated.
     */
    void calculate_fluid_kinematics(double x, double z, double t, Eigen::Vector3d& vel, Eigen::Vector3d& accel) const {
        vel.setZero();
        accel.setZero();

        for (const auto& comp : components) {
            double theta = comp.wavenumber * x - comp.frequency * t + comp.phase;
            double cos_th = std::cos(theta);
            double sin_th = std::sin(theta);

            double k = comp.wavenumber;
            double a = comp.amplitude;
            double w = comp.frequency;

            double sinh_kd = std::sinh(k * depth);
            if (sinh_kd <= 0.0) continue;

            double cosh_kz = std::cosh(k * (z + depth));
            double sinh_kz = std::sinh(k * (z + depth));

            // Velocity vx, vz
            double vx = a * w * (cosh_kz / sinh_kd) * cos_th;
            double vz = a * w * (sinh_kz / sinh_kd) * sin_th;

            // Acceleration ax, az
            double ax = a * w * w * (cosh_kz / sinh_kd) * sin_th;
            double az = -a * w * w * (sinh_kz / sinh_kd) * cos_th;

            vel.x() += vx;
            vel.z() += vz;

            accel.x() += ax;
            accel.z() += az;
        }
    }
};

/**
 * @brief Morison equation: drag + inertia hydrodynamic force per unit length on a slender member.
 */
class MorisonForce {
public:
    double Cd; ///< Drag coefficient (default 1.0).
    double Cm; ///< Inertia coefficient (default 2.0).
    double D;  ///< Outer diameter (m).
    double rho_water; ///< Seawater density (kg/m^3).

    MorisonForce(double cd = 1.0, double cm = 2.0, double diameter = 0.25, double rho = 1025.0)
        : Cd(cd), Cm(cm), D(diameter), rho_water(rho) {}

    /**
     * @brief Computes the Morison hydrodynamic force per unit length, in the plane perpendicular to the element axis.
     *
     * `F = 0.5*rho*Cd*D*|v_rel_perp|*v_rel_perp + rho*(pi*D^2/4)*(Cm*a_fluid_perp - (Cm-1)*a_struct_perp)`,
     * with relative velocity/absolute accelerations first projected perpendicular to `elem_axis`
     * (axial drag/inertia on a slender member is neglected, standard practice for risers/mooring lines).
     *
     * @param v_fluid Fluid particle velocity (m/s).
     * @param a_fluid Fluid particle acceleration (m/s^2).
     * @param v_struct Structural velocity at the point (m/s).
     * @param a_struct Structural acceleration at the point (m/s^2).
     * @param elem_axis Unit vector along the element's current axis.
     * @return Hydrodynamic force per unit length (N/m).
     */
    Eigen::Vector3d calculate_force_per_length(const Eigen::Vector3d& v_fluid, const Eigen::Vector3d& a_fluid,
                                                const Eigen::Vector3d& v_struct, const Eigen::Vector3d& a_struct,
                                                const Eigen::Vector3d& elem_axis) const {
        // Relative fluid velocity
        Eigen::Vector3d v_rel = v_fluid - v_struct;

        // Project velocity and acceleration perpendicular to element axis
        Eigen::Vector3d v_rel_perp = v_rel - (v_rel.dot(elem_axis)) * elem_axis;
        Eigen::Vector3d a_fluid_perp = a_fluid - (a_fluid.dot(elem_axis)) * elem_axis;
        Eigen::Vector3d a_struct_perp = a_struct - (a_struct.dot(elem_axis)) * elem_axis;

        // Non-linear Drag force: F_drag = 0.5 * rho * Cd * D * |v_rel| * v_rel
        Eigen::Vector3d f_drag = 0.5 * rho_water * Cd * D * v_rel_perp.norm() * v_rel_perp;

        // Inertia force: F_inertia = rho * (pi * D^2 / 4) * (Cm * a_fluid - (Cm - 1) * a_struct)
        double Area_outer = std::numbers::pi * D * D / 4.0;
        Eigen::Vector3d f_inertia = rho_water * Area_outer * (Cm * a_fluid_perp - (Cm - 1.0) * a_struct_perp);

        return f_drag + f_inertia;
    }
};

} // namespace risersim

#endif
