#ifndef RISERSIM_SEABED_HPP
#define RISERSIM_SEABED_HPP

#include <Eigen/Dense>
#include <cmath>

namespace risersim {

class SeabedInteraction {
public:
    double seabed_depth;   // Z coordinate of the seabed (e.g. -100.0 m)
    double stiffness_z;    // Vertical soil stiffness (N/m^2 or N/m per node, default 1e5)
    double friction_coeff; // Lateral/axial friction coefficient mu (default 0.5)
    double elastic_deflection_limit; // Deslocamento que mobiliza o atrito limite (m)
    double contact_ramp_zone;        // Faixa de penetração onde a rigidez normal é suavizada (m)

    SeabedInteraction(double depth = -100.0, double kz = 1.0e5, double mu = 0.5, double u_limit = 0.05, double ramp = 0.05)
        : seabed_depth(depth), stiffness_z(kz), friction_coeff(mu), elastic_deflection_limit(u_limit), contact_ramp_zone(ramp) {}

    // Calculate normal reaction force and stiffness contribution for a node.
    // Suaviza a transição de contato numa pequena faixa (contact_ramp_zone) em vez de
    // um degrau abrupto em pen=0: força e rigidez tangente ficam contínuas em pen=0 e
    // em pen=contact_ramp_zone, evitando "chattering" quando um nó cruza o limiar do
    // solo repetidamente entre iterações do Newton-Raphson (comum quando muitos nós
    // começam praticamente encostados no fundo, como neste modelo).
    void calculate_seabed_reaction(double z_current, double& f_normal, double& k_normal) const {
        double pen = seabed_depth - z_current; // > 0 quando penetrando
        if (pen <= 0.0) {
            f_normal = 0.0;
            k_normal = 0.0;
        } else if (contact_ramp_zone > 1.0e-9 && pen < contact_ramp_zone) {
            // Rampa quadrática: f(0)=0, f'(0)=0, contínua com o regime linear em pen=ramp
            k_normal = (stiffness_z / contact_ramp_zone) * pen;
            f_normal = 0.5 * (stiffness_z / contact_ramp_zone) * pen * pen;
        } else {
            f_normal = stiffness_z * (pen - 0.5 * contact_ramp_zone);
            k_normal = stiffness_z;
        }
    }

    // Mola de atrito de Coulomb elástico-plástica (1D), na mesma linha do
    // ANFLEX real (soil_uncoupled.cpp: calc_unidimensional_friction): regime
    // elástico linear k = mu*|Fn|/u_limite até o limite de atrito mu*|Fn|,
    // depois disso a força satura e a rigidez tangente cai a zero.
    // disp: deslocamento horizontal acumulado do nó (m).
    void calculate_friction_1d(double f_normal, double disp, double& f_friction, double& k_friction) const {
        double fl = friction_coeff * std::fabs(f_normal);
        if (fl < 1.0e-9 || elastic_deflection_limit < 1.0e-9) {
            f_friction = 0.0;
            k_friction = 0.0;
            return;
        }
        k_friction = fl / elastic_deflection_limit;
        f_friction = k_friction * disp;
        if (std::fabs(f_friction) > fl) {
            f_friction = (f_friction > 0.0) ? fl : -fl;
            k_friction = 0.0;
        }
    }
};

} // namespace risersim

#endif
