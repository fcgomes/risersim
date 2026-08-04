#ifndef RISERSIM_ROTATION_UTILS_HPP
#define RISERSIM_ROTATION_UTILS_HPP

#include <Eigen/Dense>

namespace risersim {

// Converte um vetor de rotacao (eixo * angulo, em radianos) na matriz de
// rotacao SO(3) correspondente (formula de Rodrigues).
inline Eigen::Matrix3d rodrigues(const Eigen::Vector3d& theta) {
    double angle = theta.norm();
    if (angle < 1.0e-14) return Eigen::Matrix3d::Identity();
    return Eigen::AngleAxisd(angle, theta / angle).toRotationMatrix();
}

// Composicao propria de duas rotacoes finitas representadas como vetores de
// rotacao. Equivalente ao pseudo_sum() do ANFLEX real (integrator.cpp), so
// que via composicao de matrizes/quaternions em vez de soma vetorial ingenua
// -- necessario porque rotacoes 3D nao comutam e nao se somam linearmente
// para angulos grandes (e a soma ingenua e exatamente o que fazia o risersim
// alimentar a rigidez de flexao com "rotacao total acumulada" em vez da
// rotacao local/deformacional, ver mapa_classes_anflex_estatica.md).
// delta_theta e aplicado DEPOIS de old_theta (composicao espacial/global,
// mesma convencao do pseudo_sum: qp = q(incremento) composto com p(antigo)).
inline Eigen::Vector3d compose_rotations(const Eigen::Vector3d& old_theta, const Eigen::Vector3d& delta_theta) {
    Eigen::Matrix3d R_new = rodrigues(delta_theta) * rodrigues(old_theta);
    Eigen::AngleAxisd aa(R_new);
    return aa.angle() * aa.axis();
}

} // namespace risersim

#endif
