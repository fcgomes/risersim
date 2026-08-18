/**
 * @file element_winch.hpp
 * @brief Variable-length axial bar (winch/pull-in cable) element, mirroring ANFLEX's `cWinch`.
 */
#ifndef RISERSIM_ELEMENT_WINCH_HPP
#define RISERSIM_ELEMENT_WINCH_HPP

#include "risersim/element_truss.hpp"
#include "risersim/curve_function.hpp"

namespace risersim {

/**
 * @brief A `TrussElement` whose UNSTRETCHED reference length varies with time via a payout curve,
 * mirroring ANFLEX's `cWinch : public cTruss` (`winch.h`/`winch.cpp`) -- used for winch/pull-in
 * operations (`Exemplo_03_D/E`, `ESDV`, `Pull-In`). Real `cWinch` only overrides two things on top
 * of `cTruss`: `get_original_length()` (returns the current, not constant, unstretched length) and
 * `update_axial_strain_and_force()` (evaluates a time function instead of using a fixed
 * `m_original_length`, and has no `initial_tension` term -- see `TrussProps::initial_tension`'s
 * doc comment). Everything else -- `calc_stiff_mt`/`calc_internal_forces`'s formulas, the
 * self-weight/buoyancy/current simplification, the lumped mass -- is inherited from
 * `TrussElement` unchanged, both here and in real ANFLEX.
 *
 * Real `cWinch` has SEPARATE static-analysis and dynamic-analysis payout functions
 * (`m_static_function`/`m_dynamic_function`, `winch.cpp:124-153`) that compose
 * (`length = original * static_function(t) * dynamic_function(t)` once a dynamic run starts).
 * Simplified here to a single `payout_fraction` curve evaluated at whatever pseudo-time
 * `Analysis::current_time` currently holds (load-step fraction during statics, elapsed simulation
 * time during dynamics) -- covers the common case (pay out to a target length, then hold) without
 * the composed static*dynamic curve; a genuinely time-varying payout DURING a dynamic run (a winch
 * actively paying out mid-simulation) is a real but unvalidated-here use case, deferred until a
 * real case needs it.
 */
class WinchElement : public TrussElement {
public:
    /**
     * @param payout_fraction Fraction of `initial_length()` that is the current unstretched
     * reference length, as a function of `Analysis::current_time`. Defaults to
     * `PiecewiseLinearCurve::constant(1.0)` (always fully paid out to the original length -- makes
     * an unconfigured WinchElement behave exactly like a plain TrussElement).
     */
    WinchElement(int id, Node3D* node1, Node3D* node2, const TrussProps& props,
                 PiecewiseLinearCurve payout_fraction = PiecewiseLinearCurve::constant(1.0),
                 double L_unstretched = 0.0)
        : TrussElement(id, node1, node2, props, L_unstretched),
          payout_fraction_(std::move(payout_fraction)), current_time_(0.0) {}

    const PiecewiseLinearCurve& payout_fraction() const { return payout_fraction_; }

    /** @brief `Element::set_time()` override: remembers the analysis's current pseudo-time for reference_length(). */
    void set_time(double current_time) override { current_time_ = current_time; }

    /** @brief `TrussElement::reference_length()` override: `initial_length() * payout_fraction(current_time)`. */
    double reference_length() const override {
        return initial_length_ * payout_fraction_.value_at(current_time_);
    }

private:
    PiecewiseLinearCurve payout_fraction_;
    double current_time_;
};

} // namespace risersim

#endif
