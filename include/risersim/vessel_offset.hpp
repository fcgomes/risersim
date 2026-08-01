#ifndef RISERSIM_VESSEL_OFFSET_HPP
#define RISERSIM_VESSEL_OFFSET_HPP

#include <Eigen/Dense>

namespace risersim {

enum class OffsetMode {
    Near,   // Vessel moves towards anchor (-X)
    Far,    // Vessel moves away from anchor (+X)
    Cross,  // Vessel moves laterally (+Y)
    Custom  // Custom (dX, dY, dZ) vector
};

class VesselOffset {
public:
    OffsetMode mode;
    Eigen::Vector3d offset_disp; // Imposed offset displacement on top node (m)

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

    VesselOffset(double dx, double dy, double dz) : mode(OffsetMode::Custom) {
        offset_disp = Eigen::Vector3d(dx, dy, dz);
    }
};

} // namespace risersim

#endif
