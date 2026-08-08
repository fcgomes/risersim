/**
 * @file model.hpp
 * @brief The riser/mooring model aggregate: owns all nodes and elements.
 */
#ifndef RISERSIM_MODEL_HPP
#define RISERSIM_MODEL_HPP

#include "risersim/node.hpp"
#include "risersim/element_beam.hpp"
#include "risersim/seabed.hpp"
#include "risersim/vessel_offset.hpp"
#include "risersim/vessel_motion.hpp"
#include <vector>
#include <string>
#include <memory>
#include <utility>

namespace risersim {

/**
 * @brief Environmental configuration (seabed/current/wave/water) parsed from the input JSON's
 * `environmental` block -- owned by RiserModel so it's the single place this data lives, instead
 * of scattered local variables in ModelBuilder (model_builder.cpp) that get assigned piecemeal
 * onto StaticAnalysis/DynamicAnalysis/SeabedInteraction/CurrentProfile after construction.
 */
struct EnvironmentalConfig {
    bool seabed_enabled = true;
    double seabed_depth_z = -100.0;      ///< Real Z of the seabed (aligned to the model's nodes, not the raw depth_m).
    double water_surface_z = 0.0;
    double seabed_stiffness = 1.0e5;
    double seabed_friction = 0.5;        ///< Isotropic fallback used when axial/lateral aren't given.
    double seabed_axial_friction = -1.0;      ///< -1 sentinel = "use seabed_friction".
    double seabed_lateral_friction = -1.0;
    double seabed_axial_elastic_limit = -1.0;
    double seabed_lateral_elastic_limit = -1.0;
    SoilModel soil_model = SoilModel::Uncoupled;

    bool current_enabled = false;
    /// 0 = superfície, positivo pra baixo -- convenção OPOSTA à do XML/AML de origem (0 = leito).
    /// Nome deliberadamente explícito, não "current_depths_m" -- ver CurrentProfile::depth_below_surface_m.
    std::vector<double> current_depth_below_surface_m;
    std::vector<double> current_velocities_ms;
    std::vector<double> current_angles_deg;

    /**
     * @brief Real load-ramp curve for current force, read from the ANFLEX XML's
     * `Currents/<case>/static_function_id` function (`x` normalized to fraction-of-total-steps
     * `[0,1]`, `y` the load fraction `[0,1]`). Empty when the source JSON doesn't carry this
     * data (synthetic models, older JSONs) -- StaticAnalysis falls back to the same half-cosine
     * ramp used for weight in that case. Real ANFLEX never ramps self-weight (its
     * `m_has_gravitational_load` gate is never set anywhere in the source), only current uses a
     * ramp -- and it's a curve of its own, independent from any structural load ramp. See
     * docs/mapa_classes_anflex_estatica.md.
     */
    std::vector<double> current_ramp_x;
    std::vector<double> current_ramp_y;

    // Onda: guardado aqui já (o JSON já traz esses dados reais), mas ainda não consumido pela
    // física da análise dinâmica -- ver docs/mapa_classes_anflex_estatica.md, fase 3 do plano de
    // eliminação de dados hardcoded (JONSWAP/Morison real fica pra uma rodada própria).
    std::string wave_type = "regular";
    double wave_period_s = 10.0;
    double wave_amplitude_m = 2.5;
    double wave_angle_deg = 0.0;
    double wave_gamma = 3.3;

    double water_density = 1025.0;

    /// Movimento real de topo (RAO + JONSWAP "equivalent harmonic") -- só a tabela/parâmetros
    /// crus (ver vessel_motion.hpp); default `enabled=false` preserva o fallback já existente
    /// (onda regular só em Z) pra qualquer JSON sem essa seção -- sintéticos, `aml_reader.py`
    /// (ainda desconectado, Eixo 2a), JSONs antigos.
    VesselMotionConfig vessel_motion;
};

/**
 * @brief Static/dynamic solver options parsed from the input JSON's `analysis_options` block --
 * owned by RiserModel for the same reason as EnvironmentalConfig above.
 */
struct AnalysisOptionsConfig {
    int static_steps = 20;
    int static_max_iterations = 300;
    double static_tolerance = 0.01;
    /// Real ANFLEX's `%ASSEMBLY.USING` -- see StaticAnalysis::use_assembly_phase.
    bool static_use_assembly_phase = true;
    /// Real ANFLEX's `%ANALYSIS_CASE.STATIC.CONVERGENCE_CRITERIUM`/`MAX_UNBALANCED` -- see
    /// StaticAnalysis::enable_unbalanced_criteria.
    bool static_enable_unbalanced_criteria = false;
    double static_unbalanced_force_tol = 1.0;
    double static_unbalanced_moment_tol = 1.0;
    /// risersim-only stabilization, no real-ANFLEX equivalent -- see
    /// StaticAnalysis::enable_step_limiting.
    bool static_enable_step_limiting = false;
    double static_max_translation_step_m = 0.5;
    double static_max_rotation_step_rad = 0.3;
    bool enable_vessel_offset = false;
    OffsetMode offset_mode = OffsetMode::Far;
    double offset_magnitude = 0.0;

    bool dynamic_enabled = true;
    double dynamic_duration_s = 20.0;
    double dynamic_dt_s = 0.05;
    int dynamic_max_iterations = 20;
    double dynamic_tolerance = 1.0e-4;
    double rayleigh_alpha = 0.05;
    double rayleigh_beta = 0.01;
    bool stop_on_first_non_convergence = false;
};

/**
 * @brief Owning container of a riser/mooring model's nodes and elements, equivalent to ANFLEX's `cDomain`.
 *
 * Owns its Node3D/CorotationalBeam3D instances via `std::unique_ptr` (roadmap step 6, see
 * `docs/mapa_classes_anflex_estatica.md`). Other code holds non-owning raw pointers into this
 * storage (e.g. `CorotationalBeam3D::node1`/`node2`, or a `Node3D*` returned by add_node()) --
 * safe as long as those pointers don't outlive the owning RiserModel, exactly like ANFLEX's own
 * `cDomain` node/element arrays. The pybind11 bindings expose `nodes`/`elements` as read-only
 * projections of raw pointers (Python can't safely take ownership away from a `unique_ptr`
 * member's default holder), plus add_node()/add_element() for Python-side model construction --
 * see `bindings.cpp`.
 */
class RiserModel {
public:
    std::vector<std::unique_ptr<Node3D>> nodes;
    std::vector<std::unique_ptr<CorotationalBeam3D>> elements;
    EnvironmentalConfig environmental;
    AnalysisOptionsConfig analysis_options;

    RiserModel() = default;

    // Copy disabled (unique_ptr members aren't copyable); move is the implicitly-generated
    // one, which is already correct here -- no hand-written move ctor/assignment needed.
    RiserModel(const RiserModel&) = delete;
    RiserModel& operator=(const RiserModel&) = delete;
    RiserModel(RiserModel&&) noexcept = default;
    RiserModel& operator=(RiserModel&&) noexcept = default;

    /**
     * @brief Constructs a Node3D owned by this model and returns a non-owning pointer to it.
     * @param args Forwarded to Node3D's constructor.
     */
    template <typename... Args>
    Node3D* add_node(Args&&... args) {
        nodes.push_back(std::make_unique<Node3D>(std::forward<Args>(args)...));
        return nodes.back().get();
    }

    /**
     * @brief Constructs a CorotationalBeam3D owned by this model and returns a non-owning pointer to it.
     * @param args Forwarded to CorotationalBeam3D's constructor.
     */
    template <typename... Args>
    CorotationalBeam3D* add_element(Args&&... args) {
        elements.push_back(std::make_unique<CorotationalBeam3D>(std::forward<Args>(args)...));
        return elements.back().get();
    }

    /** @brief Destroys all owned nodes and elements and empties both containers. */
    void clear() {
        elements.clear();
        nodes.clear();
    }
};

} // namespace risersim

#endif // RISERSIM_MODEL_HPP
