/**
 * @file vessel_offset.hpp
 * @brief Prescribed top-node (vessel) offset for static offset analyses.
 */
#ifndef RISERSIM_VESSEL_OFFSET_HPP
#define RISERSIM_VESSEL_OFFSET_HPP

#include <Eigen/Dense>

namespace risersim {

/** @brief Named directions for a vessel offset, relative to the anchor. */
enum class OffsetMode {
    Near,   ///< Vessel moves towards anchor (-X).
    Far,    ///< Vessel moves away from anchor (+X).
    Cross,  ///< Vessel moves laterally (+Y).
    Custom  ///< Custom (dX, dY, dZ) vector.
};

/** @brief A prescribed displacement applied to the top node, used by StaticAnalysis::solve_vessel_offset(). */
class VesselOffset {
public:
    OffsetMode mode;
    Eigen::Vector3d offset_disp; ///< Imposed offset displacement on top node (m).

    /**
     * @brief Builds an offset along one of the named directions.
     * @param m Offset direction.
     * @param magnitude Offset magnitude (m); sign is derived from `m` (ignored for `Custom`).
     */
    VesselOffset(OffsetMode m = OffsetMode::Far, double magnitude = 10.0) : mode(m) {
        offset_disp.setZero();
        switch (m) {
            case OffsetMode::Near:
                offset_disp.x() = -std::abs(magnitude);
                break;
            case OffsetMode::Far:
                offset_disp.x() = std::abs(magnitude);
                break;
            case OffsetMode::Cross:
                offset_disp.y() = std::abs(magnitude);
                break;
            case OffsetMode::Custom:
                break;
        }
    }

    /** @brief Builds a custom (dx, dy, dz) offset vector. */
    VesselOffset(double dx, double dy, double dz) : mode(OffsetMode::Custom) {
        offset_disp = Eigen::Vector3d(dx, dy, dz);
    }
};

} // namespace risersim

#endif
