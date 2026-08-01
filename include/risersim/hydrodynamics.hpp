#ifndef RISERSIM_HYDRODYNAMICS_HPP
#define RISERSIM_HYDRODYNAMICS_HPP

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif

#include <Eigen/Dense>
#include <vector>
#include <cmath>
#include <cstdlib>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace risersim {

struct WaveWaveComponent {
    double frequency; // omega_k (rad/s)
    double wavenumber;// k_k (rad/m)
    double amplitude; // a_k (m)
    double phase;     // phi_k (rad)
};

class JONSWAPSpectrum {
public:
    double Hs;     // Significant wave height (m)
    double Tp;     // Peak wave period (s)
    double gamma;  // Peak enhancement factor (default 3.3)
    double depth;  // Water depth (m)

    JONSWAPSpectrum(double hs, double tp, double d, double g_factor = 3.3)
        : Hs(hs), Tp(tp), depth(d), gamma(g_factor) {}

    double evaluate(double omega) const {
        if (omega <= 0.0) return 0.0;
        double omega_p = 2.0 * M_PI / Tp;
        double g = 9.81;

        double sigma = (omega <= omega_p) ? 0.07 : 0.09;
        double r = std::exp(-std::pow(omega - omega_p, 2.0) / (2.0 * sigma * sigma * omega_p * omega_p));

        double alpha = 5.06 * std::pow(Hs / (Tp * Tp), 2.0) * (1.0 - 0.287 * std::log(gamma));
        double S_pm = (alpha * g * g / std::pow(omega, 5.0)) * std::exp(-1.25 * std::pow(omega_p / omega, 4.0));

        return S_pm * std::pow(gamma, r);
    }

    std::vector<WaveWaveComponent> generate_wave_components(int N_components = 50, double omega_max = 3.0) const {
        std::vector<WaveWaveComponent> components;
        double d_omega = omega_max / N_components;
        double g = 9.81;

        for (int i = 1; i <= N_components; ++i) {
            double w = i * d_omega;
            double S_w = evaluate(w);
            double amp = std::sqrt(2.0 * S_w * d_omega);

            // Wavenumber k from dispersion relation: w^2 = g * k * tanh(k * depth)
            double k = w * w / g; // Deep water approximation for initial guess
            for (int iter = 0; iter < 5; ++iter) {
                k = w * w / (g * std::tanh(k * depth));
            }

            double phase = static_cast<double>(rand()) / RAND_MAX * 2.0 * M_PI;

            components.push_back({w, k, amp, phase});
        }

        return components;
    }
};

class AiryWaveKinematics {
public:
    std::vector<WaveWaveComponent> components;
    double depth;

    AiryWaveKinematics(const std::vector<WaveWaveComponent>& comps, double d)
        : components(comps), depth(d) {}

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

class MorisonForce {
public:
    double Cd; // Drag coefficient (default 1.0)
    double Cm; // Inertia coefficient (default 2.0)
    double D;  // Outer diameter (m)
    double rho_water; // Seawater density (kg/m^3)

    MorisonForce(double cd = 1.0, double cm = 2.0, double diameter = 0.25, double rho = 1025.0)
        : Cd(cd), Cm(cm), D(diameter), rho_water(rho) {}

    // Calculate Morison Hydrodynamic Force per unit length (N/m)
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
        double Area_outer = M_PI * D * D / 4.0;
        Eigen::Vector3d f_inertia = rho_water * Area_outer * (Cm * a_fluid_perp - (Cm - 1.0) * a_struct_perp);

        return f_drag + f_inertia;
    }
};

} // namespace risersim

#endif
