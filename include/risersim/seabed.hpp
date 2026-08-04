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

    // Direções axial (ao longo da linha) e lateral (perpendicular, no plano
    // horizontal) distintas, fiel ao ANFLEX real (soil.h: m_axial_friction/
    // m_lateral_friction, m_axial_elastic_deflection_limit/
    // m_lateral_elastic_deflection_limit -- ex.: Exemplo_01a_A1.xml tem
    // axial=0.92/0.03m vs lateral=0.95/0.279m, bem diferentes). Por padrão
    // iguais a friction_coeff/elastic_deflection_limit; o chamador pode
    // sobrescrever com os valores reais do modelo.
    double axial_friction;
    double lateral_friction;
    double axial_elastic_deflection_limit;
    double lateral_elastic_deflection_limit;

    SeabedInteraction(double depth = -100.0, double kz = 1.0e5, double mu = 0.5, double u_limit = 0.05, double f_ult = 5000.0)
        : seabed_depth(depth), stiffness_z(kz), friction_coeff(mu), elastic_deflection_limit(u_limit), ultimate_bearing_force(f_ult),
          axial_friction(mu), lateral_friction(mu),
          axial_elastic_deflection_limit(u_limit), lateral_elastic_deflection_limit(u_limit) {}

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

    // Mola de atrito de Coulomb elástico-plástica (1D) INCREMENTAL, fiel ao
    // ANFLEX real (soil_uncoupled.cpp: calc_unidimensional_friction): regime
    // elástico linear k = mu*|Fn|/u_limite, mas a força é acumulada
    // incrementalmente a partir do estado persistente da iteração anterior
    // (f_state, entra e sai por referência -- equivalente a m_forces[0]/[1]
    // do ANFLEX) usando o incremento de deslocamento DESTA iteração (du,
    // equivalente a get_delta_dx()) -- não o deslocamento total acumulado.
    // Usar o total (versão anterior deste método) faz a força saturar
    // permanentemente assim que o deslocamento total desde t=0 excede o
    // limite elástico, mesmo que o nó não esteja de fato deslizando agora --
    // fisicamente errado e uma causa real do "chattering" de contato
    // encontrado ao combinar solo+corrente (ver mapa_classes_anflex_estatica.md).
    // Quando o contato é perdido (f_normal=0), o chamador deve zerar f_state.
    // mu/u_limit são passados explicitamente (em vez de usar friction_coeff/
    // elastic_deflection_limit direto) para permitir chamar esta função uma
    // vez para a direção axial e outra para a lateral, com valores distintos.
    void calculate_friction_1d(double mu, double u_limit, double f_normal, double du, double& f_state, double& k_friction) const {
        double fl = mu * std::fabs(f_normal);
        if (fl < 1.0e-9 || u_limit < 1.0e-9) {
            f_state = 0.0;
            k_friction = 0.0;
            return;
        }
        k_friction = fl / u_limit;
        f_state += k_friction * du;
        if (std::fabs(f_state) > fl) {
            f_state = (f_state > 0.0) ? fl : -fl;
            k_friction = 0.0;
        }
    }
};

} // namespace risersim

#endif
