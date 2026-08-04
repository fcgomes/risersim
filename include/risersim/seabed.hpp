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
    double ultimate_bearing_force;   // Capacidade de suporte última do solo por nó (N)

    SeabedInteraction(double depth = -100.0, double kz = 1.0e5, double mu = 0.5, double u_limit = 0.05, double f_ult = 5000.0)
        : seabed_depth(depth), stiffness_z(kz), friction_coeff(mu), elastic_deflection_limit(u_limit), ultimate_bearing_force(f_ult) {}

    // Calculate normal reaction force and stiffness contribution for a node.
    // Modelo hiperbólico não-linear (mesma forma funcional das curvas P-Y/T-Z reais de
    // geotecnia offshore): f(pen) = pen / (1/k_inicial + pen/f_ultima). A rigidez tangente
    // começa em k_inicial=stiffness_z (suave) e satura assintoticamente em f_ultima à medida
    // que a penetração cresce — em vez de um salto abrupto para a rigidez linear total assim
    // que pen>0, o que causava "chattering"/mal-condicionamento com muitos nós entrando em
    // contato quase simultaneamente (comum quando o leito marinho já toca boa parte da malha
    // na configuração inicial).
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
