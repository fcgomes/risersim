#ifndef RISERSIM_SNAPSHOT_HPP
#define RISERSIM_SNAPSHOT_HPP

#include <vector>
#include <Eigen/Core>

namespace risersim {

struct StepSnapshot {
    int step_index;
    double load_factor;
    std::vector<Eigen::Vector3d> node_coords;
    std::vector<double> element_tensions_kN;
    std::vector<double> element_bending_moments_kNm;
    std::vector<double> element_curvatures;
    std::vector<double> element_von_mises_MPa;
    std::vector<double> element_mbr_safety_factors;
};

} // namespace risersim

#endif
