#ifndef RISERSIM_ELEMENT_BEAM_HPP
#define RISERSIM_ELEMENT_BEAM_HPP

#include "risersim/config.hpp"
#include "risersim/node.hpp"
#include "risersim/rotation_utils.hpp"
#include <Eigen/Dense>

namespace risersim {

struct BeamMaterialProps {
    double E;          // Young's modulus (Pa)
    double G;          // Shear modulus (Pa)
    double A;          // Structural cross-section area (m^2)
    double IY;         // Moment of inertia Y (m^4)
    double IZ;         // Moment of inertia Z (m^4)
    double J;          // Torsional constant (m^4)
    double rho;        // Structural mass density (kg/m^3)
    double EI;         // Bending stiffness EI (N.m^2)
    
    double D_outer;    // Outer diameter (m)
    double D_inner;    // Inner diameter (m)
    double rho_fluid;  // Internal fluid density (kg/m^3)
    double Ca;         // Hydrodynamic added mass coefficient (default 1.0)

    BeamMaterialProps()
        : E(2.1e11), G(8.0e10), A(0.015), IY(5.0e-5), IZ(5.0e-5), J(1.0e-4), rho(7850.0), EI(1.05e7),
          D_outer(0.25), D_inner(0.20), rho_fluid(800.0), Ca(1.0) {}
};

class CorotationalBeam3D {
public:
    int id;
    Node3D* node1;
    Node3D* node2;
    BeamMaterialProps props;
    
    double initial_length;
    double tension_true;      // True axial wall tension T_true
    double tension_effective; // T_eff = T_true + p_e*A_e - p_i*A_i
    double p_i;               // Internal fluid pressure (Pa)
    double p_e;               // External hydrostatic pressure (Pa)
    double net_upward_buoyancy; // Net upward buoyancy force per meter from modules (N/m)

    // Triades de referencia fixas nos dois extremos, calculadas UMA VEZ na
    // configuracao inicial (t=0) -- equivalente a TELNOAORIG/TELNOBORIG do
    // ANFLEX real (calc_init_rot_mt, beam.cpp:301). Sem entrada de
    // pre-curvatura manual (curvature1/curvature2 do .dat), as duas coincidem
    // com o referencial da corda inicial. E contra essa referencia fixa que a
    // rotacao local/deformacional de cada no e medida a cada iteracao -- nao
    // contra a corda atual (que muda a cada iteracao e mistura rotacao de
    // corpo rigido com deformacao elastica).
    Eigen::Matrix3d node1_init_triad;
    Eigen::Matrix3d node2_init_triad;

    CorotationalBeam3D(int elem_id, Node3D* n1, Node3D* n2, const BeamMaterialProps& p, double L_unstretched = 0.0)
        : id(elem_id), node1(n1), node2(n2), props(p), tension_true(0.0), tension_effective(0.0), p_i(0.0), p_e(0.0), net_upward_buoyancy(0.0) {
        initial_length = (L_unstretched > 0.0) ? L_unstretched : (node2->coords - node1->coords).norm();
        Eigen::Vector3d ex0 = (node2->coords - node1->coords).normalized();
        node1_init_triad = build_frame_from_chord(ex0);
        node2_init_triad = node1_init_triad;
    }

    double current_length() const {
        return (node2->current_coords() - node1->current_coords()).norm();
    }

    double outer_area() const {
        return M_PI * props.D_outer * props.D_outer / 4.0;
    }

    double inner_area() const {
        return M_PI * props.D_inner * props.D_inner / 4.0;
    }

    // Calculate total linear mass per meter (pipe + internal fluid + added water mass)
    double total_linear_mass(double rho_water = 1025.0) const {
        double m_pipe = props.rho * props.A;
        double m_fluid = props.rho_fluid * inner_area();
        double m_added = rho_water * outer_area() * props.Ca;
        return m_pipe + m_fluid + m_added;
    }

    // Calculate Effective Tension T_eff = T_true + p_e * A_e - p_i * A_i
    double update_effective_tension() {
        double delta_L = current_length() - initial_length;
        double strain = delta_L / initial_length;
        tension_true = props.E * props.A * strain;
        tension_effective = tension_true + p_e * outer_area() - p_i * inner_area();
        return tension_effective;
    }

    // Local Material Stiffness Matrix (12x12)
    Eigen::Matrix<double, 12, 12> local_material_stiffness() const;

    // Local Geometric Stiffness Matrix (12x12)
    Eigen::Matrix<double, 12, 12> local_geometric_stiffness() const;

    // Local Consistent Mass Matrix (12x12)
    Eigen::Matrix<double, 12, 12> local_mass_matrix(double rho_water = 1025.0) const;

    // 3D Transformation Matrix (12x12) -- referencial baseado so na corda atual
    // (usado para a matriz de massa, onde a rotacao local nao entra).
    Eigen::Matrix<double, 12, 12> transformation_matrix() const;

    // Global Element Mass Matrix (12x12)
    Eigen::Matrix<double, 12, 12> global_mass(double rho_water = 1025.0) const {
        Eigen::Matrix<double, 12, 12> T = transformation_matrix();
        Eigen::Matrix<double, 12, 12> M_local = local_mass_matrix(rho_water);
        return T.transpose() * M_local * T;
    }

    // Global Element Stiffness Matrix (12x12) -- conveniencia para introspeccao
    // (Python/diagnostico). Internamente o solver usa compute_corotational_forces
    // diretamente (calcula K e F_int juntos, evitando recomputar o ghost frame).
    Eigen::Matrix<double, 12, 12> global_stiffness() const {
        Eigen::Matrix<double, 12, 12> K;
        Eigen::Matrix<double, 12, 1> F_unused;
        compute_corotational_forces(K, F_unused);
        return K;
    }

    // Constroi um referencial ortonormal {ex,ey,ez} (linhas = eixos locais em
    // coordenadas globais) a partir de uma direcao de corda ex. Compartilhado
    // pela triade de referencia fixa (t=0) e pela transformation_matrix() (usada
    // so para a matriz de massa).
    static Eigen::Matrix3d build_frame_from_chord(const Eigen::Vector3d& ex);

    // Estado corrotacional consistente: extrai a rotacao LOCAL/deformacional de
    // cada no (mismatch entre a orientacao atual do no e o referencial
    // co-rotativo do elemento -- "ghost frame" de Crisfield), e usa isso -- nao
    // a rotacao total acumulada -- para calcular forca interna e rigidez.
    // Replica calc_transformation_mt/calc_relative_rotations/calc_fe do ANFLEX
    // real (element.cpp:215, matrix.cpp:499, beam.cpp:1276).
    void compute_corotational_forces(Eigen::Matrix<double, 12, 12>& K_global,
                                      Eigen::Matrix<double, 12, 1>& F_int_global) const;

    // Item 3: Bending Moment, Curvature, MBR Check & von Mises Combined Stress
    struct StressAndCurvatureResults {
        double curvature;          // Local curvature kappa (1/m)
        double bending_moment_kNm; // Bending moment M (kN*m)
        double bend_radius;        // Actual bend radius R = 1/kappa (m)
        double mbr_min;            // Minimum Bend Radius threshold (m)
        double mbr_safety_factor;  // Safety factor R_actual / MBR_min
        double von_mises_MPa;      // Combined von Mises stress (MPa)
    };

    StressAndCurvatureResults compute_stress_and_curvature(
        const CorotationalBeam3D* prev_elem = nullptr,
        const CorotationalBeam3D* next_elem = nullptr,
        double yield_stress_MPa = 350.0) const;
};

} // namespace risersim

#endif
