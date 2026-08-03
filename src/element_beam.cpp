#include "risersim/element_beam.hpp"

namespace risersim {

Eigen::Matrix<double, 12, 12> CorotationalBeam3D::local_material_stiffness() const {
    Eigen::Matrix<double, 12, 12> K = Eigen::Matrix<double, 12, 12>::Zero();
    double L = current_length();
    if (L <= 0.0) L = initial_length;

    double E = props.E;
    double G = props.G;
    double A = props.A;
    double Iy = props.IY;
    double Iz = props.IZ;
    double J = props.J;

    double EA_L = E * A / L;
    double GJ_L = G * J / L;

    double k12EIz_L3 = 12.0 * E * Iz / (L * L * L);
    double k6EIz_L2  = 6.0 * E * Iz / (L * L);
    double k4EIz_L   = 4.0 * E * Iz / L;
    double k2EIz_L   = 2.0 * E * Iz / L;

    double k12EIy_L3 = 12.0 * E * Iy / (L * L * L);
    double k6EIy_L2  = 6.0 * E * Iy / (L * L);
    double k4EIy_L   = 4.0 * E * Iy / L;
    double k2EIy_L   = 2.0 * E * Iy / L;

    // Node 1 & Node 2 Axial
    K(0, 0) =  EA_L; K(0, 6) = -EA_L;
    K(6, 0) = -EA_L; K(6, 6) =  EA_L;

    // Node 1 & Node 2 Torsion
    K(3, 3) =  GJ_L; K(3, 9) = -GJ_L;
    K(9, 3) = -GJ_L; K(9, 9) =  GJ_L;

    // Bending XY (z-axis rotation)
    K(1, 1) =  k12EIz_L3; K(1, 5) =  k6EIz_L2; K(1, 7) = -k12EIz_L3; K(1, 11) =  k6EIz_L2;
    K(5, 1) =   k6EIz_L2; K(5, 5) =   k4EIz_L; K(5, 7) =  -k6EIz_L2; K(5, 11) =   k2EIz_L;
    K(7, 1) = -k12EIz_L3; K(7, 5) = -k6EIz_L2; K(7, 7) =  k12EIz_L3; K(7, 11) = -k6EIz_L2;
    K(11,1) =   k6EIz_L2; K(11,5) =   k2EIz_L; K(11,7) =  -k6EIz_L2; K(11,11) =   k4EIz_L;

    // Bending XZ (y-axis rotation)
    K(2, 2) =  k12EIy_L3; K(2, 4) = -k6EIy_L2; K(2, 8) = -k12EIy_L3; K(2, 10) = -k6EIy_L2;
    K(4, 2) =  -k6EIy_L2; K(4, 4) =   k4EIy_L; K(4, 8) =   k6EIy_L2; K(4, 10) =   k2EIy_L;
    K(8, 2) = -k12EIy_L3; K(8, 4) =  k6EIy_L2; K(8, 8) =  k12EIy_L3; K(8, 10) =  k6EIy_L2;
    K(10,2) =  -k6EIy_L2; K(10,4) =   k2EIy_L; K(10,8) =   k6EIy_L2; K(10,10) =   k4EIy_L;

    return K;
}

Eigen::Matrix<double, 12, 12> CorotationalBeam3D::local_geometric_stiffness() const {
    Eigen::Matrix<double, 12, 12> Kg = Eigen::Matrix<double, 12, 12>::Zero();
    double L = current_length();
    if (L <= 0.0) L = initial_length;

    // Use Effective Tension T_eff = T_true + p_e*A_e - p_i*A_i
    double P = tension_effective;
    double P_L = P / L;

    double c1 = 6.0 / 5.0 * P_L;
    double c2 = L / 10.0 * P_L;
    double c3 = 2.0 * L * L / 15.0 * P_L;
    double c4 = -L * L / 30.0 * P_L;

    // Bending XY
    Kg(1, 1) =  c1; Kg(1, 5) =  c2; Kg(1, 7) = -c1; Kg(1, 11) =  c2;
    Kg(5, 1) =  c2; Kg(5, 5) =  c3; Kg(5, 7) = -c2; Kg(5, 11) =  c4;
    Kg(7, 1) = -c1; Kg(7, 5) = -c2; Kg(7, 7) =  c1; Kg(7, 11) = -c2;
    Kg(11,1) =  c2; Kg(11,5) =  c4; Kg(11,7) = -c2; Kg(11,11) =  c3;

    // Bending XZ
    Kg(2, 2) =  c1; Kg(2, 4) = -c2; Kg(2, 8) = -c1; Kg(2, 10) = -c2;
    Kg(4, 2) = -c2; Kg(4, 4) =  c3; Kg(4, 8) =  c2; Kg(4, 10) =  c4;
    Kg(8, 2) = -c1; Kg(8, 4) =  c2; Kg(8, 8) =  c1; Kg(8, 10) =  c2;
    Kg(10,2) = -c2; Kg(10,4) =  c4; Kg(10,8) =  c2; Kg(10,10) =  c3;

    return Kg;
}

Eigen::Matrix<double, 12, 12> CorotationalBeam3D::local_mass_matrix(double rho_water) const {
    Eigen::Matrix<double, 12, 12> M = Eigen::Matrix<double, 12, 12>::Zero();
    double L = current_length();
    if (L <= 0.0) L = initial_length;

    double m_lin = total_linear_mass(rho_water);
    double mL_420 = m_lin * L / 420.0;

    // Axial lump / consistent terms
    M(0, 0) = m_lin * L / 3.0; M(0, 6) = m_lin * L / 6.0;
    M(6, 0) = m_lin * L / 6.0; M(6, 6) = m_lin * L / 3.0;

    // Transverse Bending XY Consistent Mass
    M(1, 1) = 156.0 * mL_420; M(1, 5) = 22.0 * L * mL_420; M(1, 7) = 54.0 * mL_420; M(1, 11) = -13.0 * L * mL_420;
    M(5, 1) = 22.0 * L * mL_420; M(5, 5) = 4.0 * L * L * mL_420; M(5, 7) = 13.0 * L * mL_420; M(5, 11) = -3.0 * L * L * mL_420;
    M(7, 1) = 54.0 * mL_420; M(7, 5) = 13.0 * L * mL_420; M(7, 7) = 156.0 * mL_420; M(7, 11) = -22.0 * L * mL_420;
    M(11,1) = -13.0 * L * mL_420; M(11,5) = -3.0 * L * L * mL_420; M(11,7) = -22.0 * L * mL_420; M(11,11) = 4.0 * L * L * mL_420;

    // Transverse Bending XZ Consistent Mass
    M(2, 2) = 156.0 * mL_420; M(2, 4) = -22.0 * L * mL_420; M(2, 8) = 54.0 * mL_420; M(2, 10) = 13.0 * L * mL_420;
    M(4, 2) = -22.0 * L * mL_420; M(4, 4) = 4.0 * L * L * mL_420; M(4, 8) = -13.0 * L * mL_420; M(4, 10) = -3.0 * L * L * mL_420;
    M(8, 2) = 54.0 * mL_420; M(8, 4) = -13.0 * L * mL_420; M(8, 8) = 156.0 * mL_420; M(8, 10) = 22.0 * L * mL_420;
    M(10,2) = 13.0 * L * mL_420; M(10,4) = -3.0 * L * L * mL_420; M(10,8) = 22.0 * L * mL_420; M(10,10) = 4.0 * L * L * mL_420;

    return M;
}

Eigen::Matrix<double, 12, 12> CorotationalBeam3D::transformation_matrix() const {
    Eigen::Matrix<double, 12, 12> T = Eigen::Matrix<double, 12, 12>::Zero();
    Eigen::Vector3d dx = node2->current_coords() - node1->current_coords();
    double L = dx.norm();
    
    Eigen::Vector3d ex = dx / L;
    Eigen::Vector3d ez_temp(0, 0, 1);
    if (std::abs(ex.dot(ez_temp)) > 0.99) {
        ez_temp = Eigen::Vector3d(0, 1, 0);
    }
    Eigen::Vector3d ey = ez_temp.cross(ex).normalized();
    Eigen::Vector3d ez = ex.cross(ey).normalized();

    Eigen::Matrix3d R;
    R.row(0) = ex.transpose();
    R.row(1) = ey.transpose();
    R.row(2) = ez.transpose();

    T.block<3, 3>(0, 0) = R;
    T.block<3, 3>(3, 3) = R;
    T.block<3, 3>(6, 6) = R;
    T.block<3, 3>(9, 9) = R;

    return T;
}

CorotationalBeam3D::StressAndCurvatureResults CorotationalBeam3D::compute_stress_and_curvature(
    const CorotationalBeam3D* prev_elem,
    const CorotationalBeam3D* next_elem,
    double yield_stress_MPa) const {
    StressAndCurvatureResults res;

    double L = current_length();
    if (L <= 0.0) L = initial_length;

    // 1. Calculate 3D Geometric Curvature from adjacent element orientation vectors
    Eigen::Vector3d ex_curr = (node2->current_coords() - node1->current_coords()).normalized();
    double kappa_geom = 0.0;
    int count = 0;

    if (prev_elem) {
        Eigen::Vector3d ex_prev = (prev_elem->node2->current_coords() - prev_elem->node1->current_coords()).normalized();
        double dot_val = std::max(-1.0, std::min(1.0, ex_prev.dot(ex_curr)));
        double d_theta = std::acos(dot_val);
        double L_avg = 0.5 * (prev_elem->current_length() + L);
        if (L_avg > 0.0) {
            kappa_geom += d_theta / L_avg;
            count++;
        }
    }

    if (next_elem) {
        Eigen::Vector3d ex_next = (next_elem->node2->current_coords() - next_elem->node1->current_coords()).normalized();
        double dot_val = std::max(-1.0, std::min(1.0, ex_curr.dot(ex_next)));
        double d_theta = std::acos(dot_val);
        double L_avg = 0.5 * (L + next_elem->current_length());
        if (L_avg > 0.0) {
            kappa_geom += d_theta / L_avg;
            count++;
        }
    }

    // Curvatura geométrica média entre elementos adjacentes
    if (count > 1) {
        kappa_geom /= static_cast<double>(count);
    }
    res.curvature = kappa_geom;

    // Rigidez fletora EI (N.m²)
    double EI_eff = (props.EI > 0.0) ? props.EI : (props.E * props.IY);

    // Momento fletor M = EI * kappa (N.m)
    double M_total_Nm = EI_eff * res.curvature;
    res.bending_moment_kNm = M_total_Nm / 1000.0;

    if (res.curvature > 1.0e-7) {
        res.bend_radius = 1.0 / res.curvature;
    } else {
        res.bend_radius = 9999.0;
    }

    double yield_Pa = yield_stress_MPa * 1.0e6;
    res.mbr_min = (props.E * props.D_outer) / (2.0 * yield_Pa);
    res.mbr_safety_factor = res.bend_radius / (res.mbr_min > 0.01 ? res.mbr_min : 1.0);

    double A_struct = (props.A > 0.0) ? props.A : (M_PI * (props.D_outer * props.D_outer - props.D_inner * props.D_inner) / 4.0);
    double sigma_axial = tension_effective / A_struct;  // Pode ser positivo (tração) ou negativo (compressão)
    double r_outer = props.D_outer / 2.0;

    // Tensão fletora na fibra externa (My/I)
    double I_geom = M_PI * (std::pow(props.D_outer, 4) - std::pow(props.D_inner, 4)) / 64.0;
    double sigma_bending = (M_total_Nm * r_outer) / (I_geom > 1.0e-12 ? I_geom : 1.0e-5);

    // Tensão circunferencial (hoop stress) para tubo de parede fina: σ_h = (p_i * r_i - p_e * r_o) / t
    double r_inner = props.D_inner / 2.0;
    double wall_thickness = r_outer - r_inner;
    double sigma_hoop = 0.0;
    if (wall_thickness > 1.0e-6) {
        sigma_hoop = (p_i * r_inner - p_e * r_outer) / wall_thickness;
    }

    // Tensão axial combinada nas duas fibras extremas (tração e compressão por flexão)
    double sigma_x_tension     = sigma_axial + std::abs(sigma_bending);  // Fibra tracionada
    double sigma_x_compression = sigma_axial - std::abs(sigma_bending);  // Fibra comprimida

    // Von Mises biaxial: σ_vm = √(σ_x² - σ_x·σ_h + σ_h²)
    // Avalia ambas as fibras e usa o pior caso
    double vm_tension = std::sqrt(sigma_x_tension * sigma_x_tension
                                  - sigma_x_tension * sigma_hoop
                                  + sigma_hoop * sigma_hoop);
    double vm_compression = std::sqrt(sigma_x_compression * sigma_x_compression
                                      - sigma_x_compression * sigma_hoop
                                      + sigma_hoop * sigma_hoop);
    double von_mises_Pa = std::max(vm_tension, vm_compression);
    res.von_mises_MPa = von_mises_Pa / 1.0e6;

    return res;
}

} // namespace risersim
