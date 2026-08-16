/**
 * @file vessel_motion.hpp
 * @brief Real RAO + JONSWAP "equivalent harmonic" top-motion, mirroring ANFLEX's
 * `cEquivalentHarmonic`/`cHybridMovement` (`trunk/libs/anf_movements`).
 */
#ifndef RISERSIM_VESSEL_MOTION_HPP
#define RISERSIM_VESSEL_MOTION_HPP

#include "risersim/config.hpp"
#include <Eigen/Dense>
#include <array>
#include <vector>

namespace risersim {

/** @brief The 6 rigid-body DOFs, in the order ANFLEX's RAO tables and this module use throughout. */
enum class VesselDof { Surge = 0, Sway = 1, Heave = 2, Roll = 3, Pitch = 4, Yaw = 5 };

/**
 * @brief Raw RAO table + JONSWAP + geometry data, straight from the real XML/H5 (via
 * `xml_h5_reader.py::extract_vessel_motion_raw()`), with zero physics applied yet -- mirrors
 * `CurrentProfile`'s split (raw table in, `get_velocity()` interpolates) rather than doing the
 * interpolation/spectral math in Python. `VesselMotion` below is what actually consumes this.
 */
struct VesselMotionConfig {
    bool enabled = false;

    /// Global CM position (m), straight from the XML's `Rao/movement_center`. Combined with
    /// `offset_m` (added directly) to form the vessel CM's true global position -- see
    /// `offset_m` below and `VesselMotion`'s constructor.
    std::array<double, 3> cm_position_m{0.0, 0.0, 0.0};

    /// Small additive correction (m) to `cm_position_m`, straight from the XML's `Rao/offset`.
    /// Confirmed against the real ANFLEX source (`model_builder_dat.cpp:4373-4386`,
    /// `anf_movements.cpp:42-85`, `save-dat.cpp`) that this is the SAME quantity the real loader
    /// calls `static_offset` and adds to `movement_center` to get the CM's true global position
    /// (`cm_position[i] += cm_offset[i]`) -- it is NOT a ready-made local CM->attachment-point
    /// delta. The real local offset used for the rigid-body transfer is
    /// `(attachment point's real global position) - (movement_center + offset)`, rotated into
    /// the vessel-local frame -- see `VesselMotion`'s constructor and
    /// docs/mapa_aml_exemplos_e_web_interface.md for the two rounds of investigation that led
    /// here (an earlier version of this module misread `offset` as already being that local
    /// delta, which produced an implausibly small lever arm and was temporarily disabled
    /// entirely before this was confirmed).
    std::array<double, 3> offset_m{0.0, 0.0, 0.0};

    /// Reference system angle (deg), rotating the vessel-local RAO axes into the global frame.
    /// Used as-is from the XML, NOT run through the DAT parser's `(360-angle)*DEG_TO_RAD`
    /// "anggeo" reinterpretation -- that conversion is specific to raw DAT/user-entered angles;
    /// the XML export is assumed to already store it in the convention this module expects.
    double refsys_angle_deg = 0.0;

    /// Which DOF's spectral response sets the single equivalent frequency shared by all 6 DOFs.
    VesselDof maximization_dof = VesselDof::Heave;

    /// RAO table: `n_headings` values in `headings_deg` (ascending, spanning a full 0-360 circle
    /// in the real data, so plain linear interpolation needs no wraparound handling),
    /// `n_freq` values in `frequencies_rad_s` (ascending), and `amplitude[dof]`/`phase_deg[dof]`
    /// each a `[n_headings][n_freq]` table.
    std::vector<double> headings_deg;
    std::vector<double> frequencies_rad_s;
    std::array<std::vector<std::vector<double>>, 6> amplitude;
    std::array<std::vector<std::vector<double>>, 6> phase_deg;

    double jonswap_alpha = 0.0;
    double jonswap_gamma = 3.3;
    double jonswap_period_s = 10.0;
    double jonswap_wini_rad_s = 0.2;
    double jonswap_wfin_rad_s = 3.0;
    int jonswap_nwave = 100;
};

/**
 * @brief Computes and holds a real "equivalent harmonic" vessel motion at one attachment point.
 *
 * Standard offshore-engineering reduction of a full spectral response (irregular JONSWAP sea x
 * RAO) to a single harmonic per DOF: all 6 DOFs share one frequency (derived from the
 * `maximization_dof`'s response), each with its own most-probable-maximum amplitude (Rayleigh,
 * narrow-band) and RAO phase at that frequency. Computed once at construction (not per time
 * step) -- see `docs/mapa_aml_exemplos_e_web_interface.md` for the full derivation and the real
 * ANFLEX source files (`equivalent_harmonic.cpp`, `hybrid_movement.cpp`, `jonswap_spectrum.cpp`,
 * `rao_table.cpp`) each formula below mirrors.
 */
class VesselMotion {
public:
    /**
     * @param config Raw RAO/JONSWAP/geometry data (from the input JSON).
     * @param wave_heading_deg Wave propagation direction (deg), same convention as the RAO
     *        table's own heading axis -- the RAO is looked up at `wave_heading_deg -
     *        refsys_angle_deg` (relative heading), mirroring `rao_dir = wave_angle -
     *        floating_angle` in `hybrid_movement.cpp`.
     * @param storm_duration_s Duration used for the Rayleigh most-probable-maximum statistics
     *        (number of response cycles = duration / mean peak-to-peak period). NOT the
     *        analysis' own `duration_s` (how long the time-domain run itself lasts, e.g. 1s in
     *        Exemplo_01a -- would make the Rayleigh statistics degenerate). Real ANFLEX always
     *        calls its equivalent (`cEquivalentHarmonic`) with a hardcoded `10800.0` (a standard
     *        3-hour reference storm, `model_builder_dat.cpp`) regardless of how long the actual
     *        simulated run is -- callers should pass that same literal, not derive it from any
     *        JSON field.
     * @param attachment_point_global Real global position (m) of the riser node the motion is
     *        being computed for, AFTER static analysis (`node->current_coords()`) -- mirrors the
     *        real loader's `node_position` (`model_builder_dat.cpp:4335-4359`, from
     *        `node->get_current_x/y/z()`). Used together with `config.cm_position_m +
     *        config.offset_m` (the CM's true global position) to compute the rigid-body local
     *        offset for the geometric transfer -- see `offset_m`'s doc comment above.
     */
    VesselMotion(const VesselMotionConfig& config, double wave_heading_deg, double storm_duration_s,
                 const Eigen::Vector3d& attachment_point_global);

    /** @brief The single equivalent angular frequency (rad/s) shared by all 6 DOFs. */
    double frequency_rad_s() const { return omega_eq_; }

    /** @brief Most-probable-maximum amplitude (m or rad) of one DOF, before the time ramp. */
    double amplitude(VesselDof dof) const { return amplitude_[static_cast<int>(dof)]; }

    /** @brief RAO phase (rad) of one DOF at the equivalent frequency. */
    double phase_rad(VesselDof dof) const { return phase_rad_[static_cast<int>(dof)]; }

    /**
     * @brief Evaluates the harmonic motion at time `t`, ramped in over the first
     * `ramp_time_s()` seconds (same half-cosine ramp already used for the wave/top motion in
     * `dynamic_analysis.cpp`), transferred to the attachment point and rotated to the global
     * frame.
     * @param time Simulation time (s).
     * @param[out] disp Translational motion (surge/sway/heave -> global X/Y/Z, m).
     * @param[out] rot Rotational motion (roll/pitch/yaw -> global X/Y/Z, rad).
     */
    void get_motion(double time, Eigen::Vector3d& disp, Eigen::Vector3d& rot) const;

    /**
     * @brief Ramp duration (s) applied by get_motion() -- 2 full periods of the equivalent
     * harmonic (`2 * 2*pi/omega_eq_`), computed by the constructor. Confirmed against a real
     * `.SAI`'s "JOINT MOVEMENTS" table (`RAMP` column, exactly `2 * PERIOD` in all 4 real load
     * cases checked -- Cross/Near/Transverse/Far, 4 different periods). An earlier version of
     * this used a fixed 5.0s constant, and a later attempt tried the analysis' own
     * `0.10 * duration_s` (ANFLEX's default for a WAVE's ramp when a load has no explicit
     * override, `model_builder_dat.cpp:461-480`) -- ruled out by the real 70s Far run's total
     * time not matching its 30.7s ramp (30.7/70 = 0.44, not 0.10); period-based is what the real
     * data actually shows, and unlike duration-based it doesn't depend on an unrelated analysis
     * setting (a model with a longer/shorter simulated duration shouldn't ramp faster/slower).
     */
    double ramp_time_s() const { return ramp_time_s_; }

    /// JONSWAP spectral density S(omega) -- see `jonswap_spectrum.cpp`. Public (not just an
    /// implementation detail) so it's directly unit-testable against a hand-computed reference
    /// value, same rationale as exposing it in the real ANFLEX source.
    static double jonswap_spectrum(double omega, double alpha, double gamma, double period_s);

    /// Plain linear interpolation, clamped at the edges (no extrapolation) -- same style as
    /// `CurrentProfile`'s private `interp1`, kept separate here since the RAO tables have a
    /// different shape (2D, complex-valued) and don't share an obvious common helper. Public for
    /// the same testability reason as jonswap_spectrum().
    static double interp_linear(const std::vector<double>& x, const std::vector<double>& y, double xq);

private:
    double omega_eq_ = 0.0;
    std::array<double, 6> amplitude_{};
    std::array<double, 6> phase_rad_{};
    double refsys_angle_rad_ = 0.0;
    double ramp_time_s_ = 0.0; ///< Computed by the constructor -- see ramp_time_s()'s doc comment.
};

} // namespace risersim

#endif
