/**
 * @file curve_function.hpp
 * @brief Piecewise-linear scalar curve (force/moment vs. displacement/rotation, or length vs.
 * time), shared by any element whose real ANFLEX counterpart reads a curve/function ID from the
 * `.DAT` (`cScalar`'s per-DOF `AnfLoadings::cFunction`, `cWinch`'s payout-vs-time functions) --
 * see `docs/roadmap.md`'s element-type-gap plan.
 */
#ifndef RISERSIM_CURVE_FUNCTION_HPP
#define RISERSIM_CURVE_FUNCTION_HPP

#include <algorithm>
#include <vector>

namespace risersim {

/**
 * @brief A monotonic-x piecewise-linear curve: `value_at(x)` interpolates linearly between
 * control points, `derivative_at(x)` returns the current segment's slope (a genuine tangent
 * stiffness for a nonlinear curve, not a secant approximation). Flat-extrapolates beyond the
 * curve's own domain (holds the boundary value/slope) rather than extrapolating the end segment's
 * line indefinitely -- matches how `CurrentProfile` (`current_profile.hpp`) already treats its
 * own tabulated points, and avoids an unbounded force for a displacement the curve's author never
 * anticipated.
 */
class PiecewiseLinearCurve {
public:
    PiecewiseLinearCurve() = default;

    /** @brief Builds from explicit control points. `x` must be strictly ascending, `x.size() == y.size() >= 2`. */
    PiecewiseLinearCurve(std::vector<double> x, std::vector<double> y)
        : x_(std::move(x)), y_(std::move(y)) {}

    /**
     * @brief Convenience factory: a pure linear spring `y = k*x`, expressed as a 2-point curve
     * spanning a symmetric domain wide enough that no realistic displacement extrapolates past it.
     * @param k Stiffness (force or moment per unit displacement/rotation).
     * @param half_range Domain half-width (displacement/rotation units) -- default is generous
     * for typical riser/mooring displacements (meters) and rotations (radians); pass a larger
     * value explicitly if a specific model's expected range exceeds it.
     */
    static PiecewiseLinearCurve linear(double k, double half_range = 1.0e6) {
        return PiecewiseLinearCurve({-half_range, half_range}, {-k * half_range, k * half_range});
    }

    /** @brief A curve that's always exactly zero (no force/stiffness) -- the default for an unused DOF. */
    static PiecewiseLinearCurve zero() { return linear(0.0); }

    bool empty() const { return x_.size() < 2; }

    /** @brief Interpolated (or boundary-held) value at `x`. Returns 0.0 if the curve has fewer than 2 points. */
    double value_at(double x) const {
        if (empty()) return 0.0;
        if (x <= x_.front()) return y_.front();
        if (x >= x_.back()) return y_.back();
        size_t i = segment_index(x);
        double t = (x - x_[i]) / (x_[i + 1] - x_[i]);
        return y_[i] + t * (y_[i + 1] - y_[i]);
    }

    /** @brief Local slope (tangent stiffness) at `x`. Returns 0.0 if the curve has fewer than 2 points. */
    double derivative_at(double x) const {
        if (empty()) return 0.0;
        double xc = std::clamp(x, x_.front(), x_.back());
        size_t i = (xc >= x_.back()) ? x_.size() - 2 : segment_index(xc);
        return (y_[i + 1] - y_[i]) / (x_[i + 1] - x_[i]);
    }

private:
    size_t segment_index(double x) const {
        // First i such that x_[i+1] >= x -- std::upper_bound finds the first strictly-greater
        // element, one past the segment start we want, hence the -1 (guarded at 0 by the caller's
        // boundary checks above already excluding x <= x_.front()).
        auto it = std::upper_bound(x_.begin(), x_.end(), x);
        size_t idx = static_cast<size_t>(it - x_.begin());
        return idx > 0 ? idx - 1 : 0;
    }

    std::vector<double> x_;
    std::vector<double> y_;
};

} // namespace risersim

#endif
