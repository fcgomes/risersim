/**
 * @file seabed.hpp
 * @brief Seabed vertical reaction and Coulomb friction models, modeled after ANFLEX's `soil.cpp`/`soil_uncoupled.cpp`.
 */
#ifndef RISERSIM_SEABED_HPP
#define RISERSIM_SEABED_HPP

#include <Eigen/Dense>
#include <cmath>

namespace risersim {

/**
 * @brief Which soil-friction formulation to use, mirroring ANFLEX's `eSoilModel`
 * (`soil.h`: COUPLED/UNCOUPLED/PYTZ -- risersim only implements the first two).
 *
 * Real models pick one explicitly: Exemplo_01a uses `%SOIL.UNCOUPLED`, Exemplo_02a uses
 * `%OPTION.SOIL.COUPLED`. They are physically different friction laws (see
 * `calculate_friction_1d` vs. `calculate_friction_coupled`), not just an implementation detail.
 */
enum class SoilModel { Uncoupled, Coupled };

/**
 * @brief Vertical (normal) reaction and axial/lateral Coulomb friction at the seabed.
 */
class SeabedInteraction {
public:
    SeabedInteraction(double depth = -100.0, double kz = 1.0e5, double mu = 0.5, double u_limit = 0.05, double f_ult = 1.0e7)
        : soil_model_(SoilModel::Uncoupled), seabed_depth_(depth), stiffness_z_(kz), friction_coeff_(mu),
          elastic_deflection_limit_(u_limit), ultimate_bearing_force_(f_ult),
          axial_friction_(mu), lateral_friction_(mu),
          axial_elastic_deflection_limit_(u_limit), lateral_elastic_deflection_limit_(u_limit) {}

    /** @brief Which friction law calc_friction_* in analysis.cpp should use. */
    SoilModel soil_model() const { return soil_model_; }
    void set_soil_model(SoilModel m) { soil_model_ = m; }

    /** @brief Z coordinate of the seabed (e.g. -100.0 m). */
    double seabed_depth() const { return seabed_depth_; }
    void set_seabed_depth(double z) { seabed_depth_ = z; }

    /** @brief Initial vertical soil stiffness k_inicial (N/m per node, default 1e5). */
    double stiffness_z() const { return stiffness_z_; }
    void set_stiffness_z(double kz) { stiffness_z_ = kz; }

    /** @brief Default/fallback friction coefficient mu (used if axial/lateral aren't overridden).
     *
     * Note: `axial_friction`/`lateral_friction` only default to this value at construction time
     * (or when explicitly set via set_axial_friction()/set_lateral_friction()) -- calling
     * set_friction_coeff() later does NOT retroactively update them. */
    double friction_coeff() const { return friction_coeff_; }
    void set_friction_coeff(double mu) { friction_coeff_ = mu; }

    /** @brief Default/fallback displacement (m) that mobilizes the friction limit. */
    double elastic_deflection_limit() const { return elastic_deflection_limit_; }
    void set_elastic_deflection_limit(double u_limit) { elastic_deflection_limit_ = u_limit; }

    /** @brief Ultimate vertical bearing capacity per node f_ultima (N). */
    double ultimate_bearing_force() const { return ultimate_bearing_force_; }
    void set_ultimate_bearing_force(double f_ult) { ultimate_bearing_force_ = f_ult; }

    /**
     * @brief Axial (along the line) friction coefficient -- see the class-level note on
     * axial/lateral vs. friction_coeff().
     */
    double axial_friction() const { return axial_friction_; }
    void set_axial_friction(double mu) { axial_friction_ = mu; }

    /** @brief Lateral (perpendicular, horizontal plane) friction coefficient. */
    double lateral_friction() const { return lateral_friction_; }
    void set_lateral_friction(double mu) { lateral_friction_ = mu; }

    /** @brief Axial elastic deflection limit (m). */
    double axial_elastic_deflection_limit() const { return axial_elastic_deflection_limit_; }
    void set_axial_elastic_deflection_limit(double u_limit) { axial_elastic_deflection_limit_ = u_limit; }

    /** @brief Lateral elastic deflection limit (m). */
    double lateral_elastic_deflection_limit() const { return lateral_elastic_deflection_limit_; }
    void set_lateral_elastic_deflection_limit(double u_limit) { lateral_elastic_deflection_limit_ = u_limit; }

    /**
     * @brief Computes the normal (vertical) seabed reaction force and its tangent stiffness.
     *
     * Nonlinear hyperbolic model, `f(pen) = pen / (1/k_inicial + pen/f_ultima)`, saturating
     * toward `ultimate_bearing_force` as penetration grows, instead of a plain linear spring
     * (`f = k*pen`, unbounded) like real ANFLEX's `cUncoupledSoil`/`cCoupledSoil`
     * (`soil_uncoupled.cpp:90-91`/`soil_coupled.cpp:92-93`).
     *
     * Tried switching to the real-ANFLEX-matching plain linear spring directly (no saturation)
     * and it made things measurably WORSE on the real soil+current divergence case: once a
     * Newton iterate lets a node penetrate deeply during a bad step, an unbounded linear force
     * grows without limit and the blow-up gets far more catastrophic (residual reaching ~1e30
     * instead of ~1e12) -- see `mapa_classes_anflex_estatica.md`. So the saturation is being kept
     * as a deliberate risersim-only safety net (real ANFLEX apparently never lets a bad iterate
     * get this far in the first place, for reasons still under investigation), but
     * `ultimate_bearing_force` itself was previously a hardcoded 5000 N default that was never
     * actually read from the input JSON -- far too small for a real riser's weight, letting nodes
     * sink multiple meters into the seabed once that cap was reached instead of being held up.
     * Raised to a much larger default (1e7 N) so it acts purely as a runaway-prevention ceiling,
     * not a physically-meaningful bearing capacity that binds under normal loading.
     *
     * @param z_current Current Z coordinate of the node.
     * @param[out] f_normal Normal reaction force (0 if not penetrating).
     * @param[out] k_normal Tangent stiffness df/dpen (0 if not penetrating).
     */
    void calculate_seabed_reaction(double z_current, double& f_normal, double& k_normal) const {
        double pen = seabed_depth_ - z_current; // > 0 quando penetrando
        if (pen <= 0.0) {
            f_normal = 0.0;
            k_normal = 0.0;
            return;
        }
        double a = 1.0 / stiffness_z_;
        double b = 1.0 / ultimate_bearing_force_;
        double denom = a + b * pen;
        f_normal = pen / denom;
        k_normal = a / (denom * denom); // df/dpen
    }

    /**
     * @brief Incremental elastic-plastic 1D Coulomb friction spring.
     *
     * Mirrors ANFLEX's real model (`soil_uncoupled.cpp::calc_unidimensional_friction`): linear
     * elastic regime with `k = mu*|Fn|/u_limit`, but the force is accumulated incrementally from
     * the persistent state carried over from the previous iteration (`f_state`, in/out --
     * equivalent to ANFLEX's `m_forces[0]`/`m_forces[1]`) using *this iteration's* displacement
     * increment (`du`, equivalent to `get_delta_dx()`) -- not the total accumulated displacement.
     *
     * Using the total displacement (an earlier version of this method) makes the force saturate
     * permanently once the total displacement since t=0 exceeds the elastic limit, even if the
     * node isn't actually slipping right now -- physically wrong, and a real cause of the contact
     * "chattering" found when combining seabed + current loading (see
     * `docs/mapa_classes_anflex_estatica.md`).
     *
     * When contact is lost (`f_normal == 0`), the caller must reset `f_state` to zero.
     * `mu`/`u_limit` are passed explicitly (rather than reading `friction_coeff`/
     * `elastic_deflection_limit` directly) so this function can be called once for the axial
     * direction and once for the lateral direction, with distinct values each time.
     *
     * @param mu Friction coefficient for this direction (axial_friction or lateral_friction).
     * @param u_limit Elastic deflection limit for this direction.
     * @param f_normal Current normal reaction force (defines the plastic cap `mu*|f_normal|`).
     * @param du This iteration's displacement increment along this direction.
     * @param[in,out] f_state Persistent friction force state for this direction.
     * @param[out] k_friction Tangent stiffness -- always the full elastic slope `mu*|f_normal|/u_limit`,
     *                        never reduced even once the plastic cap is reached (matches real ANFLEX,
     *                        see the note inside the function body).
     */
    void calculate_friction_1d(double mu, double u_limit, double f_normal, double du, double& f_state, double& k_friction) const {
        double fl = mu * std::fabs(f_normal);
        if (fl < 1.0e-9 || u_limit < 1.0e-9) {
            f_state = 0.0;
            k_friction = 0.0;
            return;
        }
        // Mirrors ANFLEX's real cUncoupledSoil::calc_unidimensional_friction
        // (soil_uncoupled.cpp:144-173): the tangent stiffness `k` is always the full elastic
        // slope `fl/u_limit`, computed once and NEVER reduced -- even once the trial force is
        // pulled back onto the Coulomb cap below. Only the force is corrected via return-mapping;
        // the stiffness never softens. This was previously a small residual fraction here
        // (friction_residual_stiffness_fraction), a deliberate regularization to avoid a
        // discontinuous Newton tangent -- but real ANFLEX has no such softening and still
        // converges, while risersim's softened tangent was the leading suspect for why the
        // global K loses conditioning exactly when many nodes saturate simultaneously (see
        // mapa_classes_anflex_estatica.md).
        k_friction = fl / u_limit;
        f_state += k_friction * du;
        if (std::fabs(f_state) > fl) {
            f_state = (f_state > 0.0) ? fl : -fl;
        }
    }

    /**
     * @brief Incremental elastic-plastic COUPLED Coulomb friction: a single combined
     * axial+lateral yield surface, instead of two independent 1D caps.
     *
     * Mirrors ANFLEX's real `cCoupledSoil::calc_friction` (`soil_coupled.cpp:108-162`), used
     * whenever the model's soil is `%OPTION.SOIL.COUPLED` (e.g. Exemplo_02a) rather than
     * `%SOIL.UNCOUPLED` (e.g. Exemplo_01a, see `calculate_friction_1d`). The combined force
     * magnitude `norm_f = sqrt(Fx^2+Fy^2)` is capped at `sqrt(fl_ax^2+fl_lat^2)` with a shared-
     * lambda radial return -- axial and lateral saturate together, not independently, so a node
     * loaded diagonally (partly along the line, partly from lateral current) reaches its combined
     * limit sooner than two separate per-axis caps would allow. As in the uncoupled model, each
     * direction's tangent stiffness is always the elastic slope (`k = fl/u_limit`) and is never
     * reduced when the force is corrected back onto the yield surface -- only the force/state
     * changes, matching ANFLEX's `m_forces`/`m_ui` exactly.
     *
     * @param f_normal Current normal reaction force (defines the combined cap).
     * @param du_axial, du_lateral This iteration's displacement increments, axial and lateral.
     * @param[in,out] f_axial, f_lateral Persistent friction force state, each direction.
     * @param[out] k_axial, k_lateral Tangent stiffness, each direction (always the elastic slope).
     */
    void calculate_friction_coupled(double f_normal, double du_axial, double du_lateral,
                                     double& f_axial, double& f_lateral,
                                     double& k_axial, double& k_lateral) const {
        double fl_ax = axial_friction_ * std::fabs(f_normal);
        double fl_lat = lateral_friction_ * std::fabs(f_normal);
        k_axial = (axial_elastic_deflection_limit_ > 1.0e-9) ? (fl_ax / axial_elastic_deflection_limit_) : 0.0;
        k_lateral = (lateral_elastic_deflection_limit_ > 1.0e-9) ? (fl_lat / lateral_elastic_deflection_limit_) : 0.0;

        f_axial += k_axial * du_axial;
        f_lateral += k_lateral * du_lateral;

        double norm_f = std::sqrt(f_axial * f_axial + f_lateral * f_lateral);
        double cap = std::sqrt(fl_ax * fl_ax + fl_lat * fl_lat);
        if (norm_f - cap > 1.0e-10 && norm_f > 1.0e-12) {
            double r0 = f_axial / norm_f;
            double r1 = f_lateral / norm_f;
            double denom = r0 * k_axial * r0 + r1 * k_lateral * r1;
            if (denom > 1.0e-12) {
                double lambda = (r0 * k_axial * du_axial + r1 * k_lateral * du_lateral) / denom;
                f_axial -= k_axial * lambda * r0;
                f_lateral -= k_lateral * lambda * r1;
            }
        }
    }

private:
    SoilModel soil_model_;
    double seabed_depth_;   ///< Z coordinate of the seabed (e.g. -100.0 m).
    double stiffness_z_;    ///< Initial vertical soil stiffness k_inicial (N/m per node, default 1e5).
    double friction_coeff_; ///< Default/fallback friction coefficient mu (used if axial/lateral aren't overridden).
    double elastic_deflection_limit_; ///< Default/fallback displacement (m) that mobilizes the friction limit.
    double ultimate_bearing_force_;   ///< Ultimate vertical bearing capacity per node f_ultima (N).

    /**
     * Axial (along the line) and lateral (perpendicular, horizontal plane) are kept distinct,
     * mirroring ANFLEX's `soil.h` (`m_axial_friction`/`m_lateral_friction`,
     * `m_axial_elastic_deflection_limit`/`m_lateral_elastic_deflection_limit`). Real models can
     * have very different values per direction -- e.g. `Exemplo_01a_A1.xml` uses axial =
     * 0.92/0.03 m vs. lateral = 0.95/0.279 m. Default to `friction_coeff_`/`elastic_deflection_limit_`;
     * the caller may overwrite them with a model's real values via the setters above.
     */
    double axial_friction_;
    double lateral_friction_;
    double axial_elastic_deflection_limit_;
    double lateral_elastic_deflection_limit_;
};

} // namespace risersim

#endif
