/**
 * @file element_beam.cpp
 * @brief Implementation of CorotationalBeam3D: local stiffness/mass matrices and the corotational force/stiffness pipeline.
 */
#include "risersim/element_beam.hpp"

namespace risersim {

Eigen::Matrix<double, 12, 12> CorotationalBeam3D::local_material_stiffness() const {
    Eigen::Matrix<double, 12, 12> K = Eigen::Matrix<double, 12, 12>::Zero();
    double L = current_length();
    if (L <= 0.0) L = initial_length();

    double E = props().E;
    double G = props().G;
    double A = props().A;
    double Iy = props().IY;
    double Iz = props().IZ;
    double J = props().J;

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
    if (L <= 0.0) L = initial_length();

    // Use Effective Tension T_eff = T_true + p_e*A_e - p_i*A_i
    double P = tension_effective();
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
    if (L <= 0.0) L = initial_length();

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

Eigen::Matrix3d CorotationalBeam3D::build_frame_from_chord(const Eigen::Vector3d& ex) {
    // Replicates real ANFLEX's own local-axis convention exactly (`nMathUtils::calc_angbeta` +
    // `calc_local_mt_rot`, anf_analysis/src/math_utils.cpp:419-500) instead of an axis-picking
    // heuristic of our own. Found and fixed 2026-08-18 by comparing this function's row(1) (fed
    // into the birth-twist curvature projection, see discrete_signed_curvature() in
    // model_builder.cpp) against ANFLEX's own `tec_line`-generated CURV1/CURV2 ground truth
    // (Exemplo_01c's includes/L1_beam.ele): the old "least-aligned global axis" heuristic picked a
    // genuinely different Y direction than ANFLEX's, undershooting real curvature by ~35% on
    // average and flipping its sign on 6% of elements.
    //
    // This is the ONE function used to build a frame from a bare chord direction, called both by
    // the constructor (t=0, seeding node1_init_triad_/node2_init_triad_) and by
    // transformation_matrix() (rotates the local mass matrix into global coords each step) --
    // fixing it here keeps both self-consistent automatically. The ongoing corotational tracking
    // in compute_corotational_forces() never calls this function past t=0 -- it only composes
    // rotations forward from the birth triad -- so this change cannot introduce a mismatch
    // between the "birth" frame and the "current" frame.
    //
    // `ex` is already a unit vector (L=1): calc_angbeta/calc_local_mt_rot are scale-invariant
    // (every term is a ratio of dx/dy/dz/L), so using ex's own components directly instead of raw
    // node deltas gives identical results.
    double dx = ex.x(), dy = ex.y(), dz = ex.z();
    double pr = std::sqrt(dx * dx + dz * dz);

    double ang_beta = 0.0;
    double dcxy = std::sqrt(dx * dx + dy * dy);
    if (dcxy != 0.0) {
        double sen_teta = -dy / dcxy;
        double cos_teta = -dx / dcxy;
        double c1, s1, c2, s2;
        if (pr >= 1.0e-10) {
            c1 = dx / pr; s1 = dz / pr; c2 = pr; s2 = dy;
        } else {
            c1 = 1.0; s1 = 0.0; c2 = 0.0; s2 = 1.0;
        }
        double aux1 = s1 * sen_teta;
        double aux2 = c1 * s2 * sen_teta + c2 * cos_teta;
        if (std::abs(aux2) > 0.0) {
            ang_beta = (aux2 > 0.0) ? std::atan(aux1 / aux2) : std::atan(aux1 / aux2) + std::numbers::pi;
        } else {
            ang_beta = (s1 * s2 > 0.0) ? 3.0 * std::numbers::pi / 2.0 : std::numbers::pi / 2.0;
        }
    }

    double cosb = std::cos(ang_beta), sinb = std::sin(ang_beta);
    Eigen::Matrix3d R;
    R.row(0) = ex.transpose();
    if (pr >= 1.0e-10) {
        double c1 = dx / pr, s1 = dz / pr, c2 = pr, s2 = dy;
        R(1, 0) = -s1 * sinb - c1 * s2 * cosb;
        R(1, 1) =  c2 * cosb;
        R(1, 2) =  c1 * sinb - s1 * s2 * cosb;
        R(2, 0) =  c1 * s2 * sinb - s1 * cosb;
        R(2, 1) = -c2 * sinb;
        R(2, 2) =  c1 * cosb + s1 * s2 * sinb;
    } else {
        double s2 = dy;
        R(1, 0) = -cosb * s2;
        R(1, 1) = 0.0;
        R(1, 2) = sinb;
        R(2, 0) = sinb * s2;
        R(2, 1) = 0.0;
        R(2, 2) = cosb;
    }
    return R;
}

Eigen::Matrix<double, 12, 12> CorotationalBeam3D::transformation_matrix() const {
    Eigen::Matrix<double, 12, 12> T = Eigen::Matrix<double, 12, 12>::Zero();
    Eigen::Vector3d dx = node2()->current_coords() - node1()->current_coords();
    double L = dx.norm();
    Eigen::Vector3d ex = dx / L;
    Eigen::Matrix3d R = build_frame_from_chord(ex);

    T.block<3, 3>(0, 0) = R;
    T.block<3, 3>(3, 3) = R;
    T.block<3, 3>(6, 6) = R;
    T.block<3, 3>(9, 9) = R;

    return T;
}

/**
 * @brief Extracts the local (small-angle) rotation mismatch of triad A relative to triad B.
 *
 * Replicates ANFLEX's `calc_relative_rotations`, `i_form=0` ("traditional ANFLEX formulation"),
 * `matrix.cpp:518-528`.
 * @param A The node's current triad.
 * @param B The element's corotating reference (ghost frame).
 * @return Approximate small-angle rotation vector (A relative to B).
 */
static Eigen::Vector3d extract_relative_rotation(const Eigen::Matrix3d& A, const Eigen::Matrix3d& B) {
    Eigen::Vector3d defor;
    defor[0] = A.row(1).dot(B.row(2));
    defor[1] = A.row(2).dot(B.row(0));
    defor[2] = A.row(0).dot(B.row(1));
    return defor;
}

void CorotationalBeam3D::compute_corotational_forces(Eigen::Matrix<double, 12, 12>& K_global,
                                                       Eigen::Matrix<double, 12, 1>& F_int_global) const {
    Eigen::Vector3d dx = node2()->current_coords() - node1()->current_coords();
    double L = dx.norm();
    Eigen::Vector3d ex = L > 0.0 ? (dx / L).eval() : Eigen::Vector3d(1, 0, 0);

    // Each node's current triad = fixed reference triad (t=0) composed with the node's
    // TOTAL accumulated rotation (real ANFLEX's calc_init_rot_mt + update_transformations_matrices:
    // m_node_tm = m_node_init_tm * trans(gen_mat_3d(rot_total))).
    Eigen::Matrix3d node1_tm = node1_init_triad() * rodrigues(node1()->rot).transpose();
    Eigen::Matrix3d node2_tm = node2_init_triad() * rodrigues(node2()->rot).transpose();

    // Crisfield's ghost frame: the "mean" rotation between the two nodes' triads,
    // re-orthogonalized so its x-axis coincides exactly with the current chord
    // (real ANFLEX's element.cpp:215-257).
    Eigen::Matrix3d Rrel = node2_tm.transpose() * node1_tm;
    Eigen::AngleAxisd aa_rel(Rrel);
    Eigen::Vector3d gamma_half = 0.5 * aa_rel.angle() * aa_rel.axis();
    Eigen::Matrix3d halfR = rodrigues(gamma_half);
    Eigen::Matrix3d P = halfR * node1_tm.transpose();
    Eigen::Vector3d PX = P.col(0), PY = P.col(1), PZ = P.col(2);

    double A1 = PX.dot(ex), A2 = PY.dot(ex), A3 = PZ.dot(ex);
    Eigen::Vector3d I2 = PY - (A2 / (1.0 + A1)) * (PX + ex);
    Eigen::Vector3d I3 = PZ - (A3 / (1.0 + A1)) * (PX + ex);

    Eigen::Matrix3d ghost;
    ghost.row(0) = ex.transpose();
    ghost.row(1) = I2.transpose();
    ghost.row(2) = I3.transpose();

    // Local/deformational rotations (mismatch between each node's triad and the ghost
    // frame) -- this, not the total accumulated rotation, is what actually bends the element.
    Eigen::Vector3d local_rot1 = extract_relative_rotation(node1_tm, ghost);
    Eigen::Vector3d local_rot2 = extract_relative_rotation(node2_tm, ghost);

    Eigen::Matrix<double, 12, 1> local_disp = Eigen::Matrix<double, 12, 1>::Zero();
    local_disp.segment<3>(3) = local_rot1;
    local_disp.segment<3>(9) = local_rot2;
    local_disp[6] = L - initial_length();

    Eigen::Matrix<double, 12, 12> T = Eigen::Matrix<double, 12, 12>::Zero();
    T.block<3, 3>(0, 0) = ghost;
    T.block<3, 3>(3, 3) = ghost;
    T.block<3, 3>(6, 6) = ghost;
    T.block<3, 3>(9, 9) = ghost;

    Eigen::Matrix<double, 12, 12> K_local = local_material_stiffness() + local_geometric_stiffness();

    K_global = T.transpose() * K_local * T;
    F_int_global = T.transpose() * (K_local * local_disp);
}

void CorotationalBeam3D::assemble(Eigen::MatrixXd& K_local, Eigen::VectorXd& F_int_local) const {
    Eigen::Matrix<double, 12, 12> K;
    Eigen::Matrix<double, 12, 1> F;
    compute_corotational_forces(K, F);
    K_local = K;
    F_int_local = F;
}

Eigen::MatrixXd CorotationalBeam3D::mass_matrix(double rho_water) const {
    return global_mass(rho_water);
}

CorotationalBeam3D::StressAndCurvatureResults CorotationalBeam3D::compute_stress_and_curvature(
    const CorotationalBeam3D* prev_elem,
    const CorotationalBeam3D* next_elem,
    double yield_stress_MPa) const {
    StressAndCurvatureResults res;

    double L = current_length();
    if (L <= 0.0) L = initial_length();

    // 1. Calculate 3D Geometric Curvature from adjacent element orientation vectors
    Eigen::Vector3d ex_curr = (node2()->current_coords() - node1()->current_coords()).normalized();
    double kappa_geom = 0.0;
    int count = 0;

    if (prev_elem) {
        Eigen::Vector3d ex_prev = (prev_elem->node2()->current_coords() - prev_elem->node1()->current_coords()).normalized();
        double dot_val = std::max(-1.0, std::min(1.0, ex_prev.dot(ex_curr)));
        double d_theta = std::acos(dot_val);
        double L_avg = 0.5 * (prev_elem->current_length() + L);
        if (L_avg > 0.0) {
            kappa_geom += d_theta / L_avg;
            count++;
        }
    }

    if (next_elem) {
        Eigen::Vector3d ex_next = (next_elem->node2()->current_coords() - next_elem->node1()->current_coords()).normalized();
        double dot_val = std::max(-1.0, std::min(1.0, ex_curr.dot(ex_next)));
        double d_theta = std::acos(dot_val);
        double L_avg = 0.5 * (L + next_elem->current_length());
        if (L_avg > 0.0) {
            kappa_geom += d_theta / L_avg;
            count++;
        }
    }

    // Average geometric curvature between adjacent elements
    if (count > 1) {
        kappa_geom /= static_cast<double>(count);
    }
    res.curvature = kappa_geom;

    // Bending stiffness EI (N.m^2)
    double EI_eff = (props().EI > 0.0) ? props().EI : (props().E * props().IY);

    // Bending moment M = EI * kappa (N.m)
    double M_total_Nm = EI_eff * res.curvature;
    res.bending_moment_kNm = M_total_Nm / 1000.0;

    if (res.curvature > 1.0e-7) {
        res.bend_radius = 1.0 / res.curvature;
    } else {
        res.bend_radius = 9999.0;
    }

    double yield_Pa = yield_stress_MPa * 1.0e6;
    res.mbr_min = (props().E * props().D_outer) / (2.0 * yield_Pa);
    res.mbr_safety_factor = res.bend_radius / (res.mbr_min > 0.01 ? res.mbr_min : 1.0);

    double A_struct = (props().A > 0.0) ? props().A : (std::numbers::pi * (props().D_outer * props().D_outer - props().D_inner * props().D_inner) / 4.0);
    double sigma_axial = tension_effective() / A_struct;  // Can be positive (tension) or negative (compression)
    double r_outer = props().D_outer / 2.0;

    // Bending stress at the outer fiber (My/I)
    double I_geom = std::numbers::pi * (std::pow(props().D_outer, 4) - std::pow(props().D_inner, 4)) / 64.0;
    double sigma_bending = (M_total_Nm * r_outer) / (I_geom > 1.0e-12 ? I_geom : 1.0e-5);

    // Hoop stress for a thin-walled pipe: sigma_h = (p_i * r_i - p_e * r_o) / t
    double r_inner = props().D_inner / 2.0;
    double wall_thickness = r_outer - r_inner;
    double sigma_hoop = 0.0;
    if (wall_thickness > 1.0e-6) {
        sigma_hoop = (p_i() * r_inner - p_e() * r_outer) / wall_thickness;
    }

    // Combined axial stress at both extreme fibers (tension and compression from bending)
    double sigma_x_tension     = sigma_axial + std::abs(sigma_bending);  // Tension-side fiber
    double sigma_x_compression = sigma_axial - std::abs(sigma_bending);  // Compression-side fiber

    // Biaxial von Mises: sigma_vm = sqrt(sigma_x^2 - sigma_x*sigma_h + sigma_h^2)
    // Evaluates both fibers and takes the worst case
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
