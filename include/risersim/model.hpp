/**
 * @file model.hpp
 * @brief The riser/mooring model aggregate: owns all nodes and elements.
 */
#ifndef RISERSIM_MODEL_HPP
#define RISERSIM_MODEL_HPP

#include "risersim/node.hpp"
#include "risersim/element.hpp"
#include "risersim/element_beam.hpp"
#include "risersim/element_scalar.hpp"
#include "risersim/seabed.hpp"
#include "risersim/vessel_offset.hpp"
#include "risersim/vessel_motion.hpp"
#include <vector>
#include <string>
#include <memory>
#include <utility>
#include <map>

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
    // HHT-alpha (Alfa-Method) time integration. gamma=0.5-alpha, beta=0.25*(1-alpha)^2
    // (bcalfa.f:100-101). alpha=0 (default here) recovers plain Newmark average acceleration
    // (gamma=0.5, beta=0.25). Real ANFLEX's own default is -0.1 (bldanagr3.f:77) -- tested
    // directly against the Far load case (docs/roadmap.md, Eixo 2a, Atualizacao 16): confirmed
    // implemented correctly (monotonic, sign-correct response), but empirically alpha=-0.1
    // makes that case's known non-convergence slightly WORSE (fails at step 430 vs. 446 for
    // alpha=0), with no compensating benefit on any case that already converges (Near/Transverse/
    // Cross unaffected either way). Kept configurable (real, correct, general improvement over
    // the previous undocumented ad hoc gamma=0.55/beta=0.28) but defaulted to 0.0 since -0.1
    // doesn't help anything in this codebase and measurably hurts the one hard case.
    double hht_alpha = 0.0;
    // LSTEPITER real (badina.f:2163-2171): quantas vezes um passo dinamico pode subdividir dt pela
    // metade (recursivamente) quando o Newton nao converge, antes de desistir e parar o laco de
    // tempo. 0 = comportamento antigo (sem retry, falha imediata). Default 4 = dt tao fino quanto
    // dt_s/16.
    int dynamic_max_step_halvings = 4;
    // Default true -- see the matching field's doc comment in dynamic_analysis.hpp for why (a
    // non-converged step's state is physically meaningless, so is everything built on it).
    bool stop_on_first_non_convergence = true;
};

/**
 * @brief One reference/coordinate frame a line can attach to -- mirrors real ANFLEX's `cReference`
 * hierarchy (`interfaces/src/reference.h`), where a floating body (`cFloating`, `%FLOATING.SHIP`
 * in the AML) is itself just a `cReference` subtype alongside the one fixed/global frame
 * (`cGlobalRef`). Only `Floating` references carry a meaningful `vessel_motion` -- `Global`
 * exists so an anchor-to-seabed connection can use the exact same `ConnectionInfo` shape as a
 * floating-body attachment (same real-ANFLEX design: an anchor is a `cConnection` pointing at
 * `cGlobalRef`, not a structurally special case).
 */
enum class ReferenceType { Global, Floating };

/**
 * @brief One entry in `RiserModel::references` -- see ReferenceType. `vessel_motion` is only
 * consulted when `type == Floating`; for `Global` it's left default (`enabled=false`).
 */
struct ReferenceInfo {
    int id = 0;
    ReferenceType type = ReferenceType::Global;
    std::string name;
    VesselMotionConfig vessel_motion;
};

/**
 * @brief One entry in `RiserModel::connections` -- mirrors real ANFLEX's `cConnection`
 * (`interfaces/src/connection.h`): which `ReferenceInfo` (by `reference_id`) a line attaches to.
 * `node_id` is the field the C++ solver actually consumes: it bridges to the `Node3D` whose
 * global position was already resolved upstream (Python's `_resolve_connection_global()`/
 * `_synthesize_mesh()`, or a hand-built model) -- the solver does NOT re-derive a node's position
 * from a reference frame's origin/azimuth plus a local offset, since that would duplicate the
 * (MoorPy-backed) catenary-geometry solving that already happens once, upstream, in Python.
 */
struct ConnectionInfo {
    int id = 0;
    int reference_id = 0;
    int node_id = -1;
};

/**
 * @brief One line (riser/mooring) in a possibly-multi-line model -- mirrors real ANFLEX's `cLine`
 * (`interfaces/src/line.h`), which has two independent `cConnection`s (top + anchor). Only
 * `top_connection_id` is consumed by the solver today (resolved to the line's prescribed-motion
 * node); `anchor_connection_id` is documentation/traceability -- the actual fixation still comes
 * from `boundary_conditions.restrained_dofs`, same as a single-line model.
 *
 * `RiserModel::lines` is empty for every model built before this concept existed (hand-built
 * single-line models, JSON without a `lines` array) -- callers (`Simulation`, `StaticAnalysis`,
 * `DynamicAnalysis`) must treat that as "exactly one line, top node = nodes.front(), vessel motion
 * = environmental.vessel_motion", preserving today's behavior with zero migration.
 */
struct LineInfo {
    int id = 0;
    std::string name;
    int top_connection_id = -1;
    int anchor_connection_id = -1;
};

/**
 * @brief Owning container of a riser/mooring model's nodes and elements, equivalent to ANFLEX's `cDomain`.
 *
 * Owns its Node3D/Element instances via `std::unique_ptr` (roadmap step 6, see
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
    RiserModel() = default;

    // Copy disabled (unique_ptr members aren't copyable); move is the implicitly-generated
    // one, which is already correct here -- no hand-written move ctor/assignment needed.
    RiserModel(const RiserModel&) = delete;
    RiserModel& operator=(const RiserModel&) = delete;
    RiserModel(RiserModel&&) noexcept = default;
    RiserModel& operator=(RiserModel&&) noexcept = default;

    /**
     * @brief The model's owned nodes. Read-only: the only ways to grow/empty this are
     * add_node()/clear() -- see the class-level doc comment about the raw pointers other code
     * (e.g. `CorotationalBeam3D::node1`/`node2`) holds into this storage.
     */
    const std::vector<std::unique_ptr<Node3D>>& nodes() const { return nodes_; }

    /**
     * @brief The model's owned elements (any `Element` subtype -- beam, and, as they're added,
     * other structural types). Read-only -- see nodes()'s doc comment, same reasoning. Code that
     * specifically needs a `CorotationalBeam3D`'s own members (props(), tension_effective(), ...)
     * must `dynamic_cast` and skip non-beam elements -- see e.g. `static_integrator.cpp`'s weight/
     * buoyancy/current loop.
     */
    const std::vector<std::unique_ptr<Element>>& elements() const { return elements_; }

    /** @brief Environmental configuration (seabed/current/wave/water) -- see EnvironmentalConfig. */
    const EnvironmentalConfig& environmental() const { return environmental_; }
    EnvironmentalConfig& environmental() { return environmental_; }

    /** @brief Static/dynamic solver options -- see AnalysisOptionsConfig. */
    const AnalysisOptionsConfig& analysis_options() const { return analysis_options_; }
    AnalysisOptionsConfig& analysis_options() { return analysis_options_; }

    /// Multi-line support (docs/roadmap.md, backlog "múltiplas linhas com corpo flutuante
    /// compartilhado") -- empty on every single-line model (see LineInfo's doc comment for the
    /// backward-compat contract). `nodes()`/`elements()` stay ONE flat list regardless (matching
    /// `cDomain`/the HDF5 exporter/RCM equation numbering, all already line-agnostic) -- these
    /// three accessors only add GROUPING metadata on top, they don't change how nodes/elements
    /// are stored.
    const std::vector<ReferenceInfo>& references() const { return references_; }
    std::vector<ReferenceInfo>& references() { return references_; }
    const std::vector<ConnectionInfo>& connections() const { return connections_; }
    std::vector<ConnectionInfo>& connections() { return connections_; }
    const std::vector<LineInfo>& lines() const { return lines_; }
    std::vector<LineInfo>& lines() { return lines_; }

    /**
     * @brief Constructs a Node3D owned by this model and returns a non-owning pointer to it.
     * @param args Forwarded to Node3D's constructor.
     */
    template <typename... Args>
    Node3D* add_node(Args&&... args) {
        nodes_.push_back(std::make_unique<Node3D>(std::forward<Args>(args)...));
        return nodes_.back().get();
    }

    /**
     * @brief Constructs a CorotationalBeam3D owned by this model and returns a non-owning pointer to it.
     * @param args Forwarded to CorotationalBeam3D's constructor.
     */
    template <typename... Args>
    CorotationalBeam3D* add_beam_element(Args&&... args) {
        auto elem = std::make_unique<CorotationalBeam3D>(std::forward<Args>(args)...);
        CorotationalBeam3D* ptr = elem.get();
        elements_.push_back(std::move(elem));
        return ptr;
    }

    /**
     * @brief Constructs a ScalarElement (generic 6-DOF spring/flexjoint, see element_scalar.hpp)
     * owned by this model and returns a non-owning pointer to it.
     * @param args Forwarded to ScalarElement's constructor.
     */
    template <typename... Args>
    ScalarElement* add_scalar_element(Args&&... args) {
        auto elem = std::make_unique<ScalarElement>(std::forward<Args>(args)...);
        ScalarElement* ptr = elem.get();
        elements_.push_back(std::move(elem));
        return ptr;
    }

    /** @brief Destroys all owned nodes and elements and empties both containers. */
    void clear() {
        elements_.clear();
        nodes_.clear();
    }

    /**
     * @brief One line's resolved top-attachment point + the VesselMotionConfig that applies to
     * it -- what Simulation/StaticAnalysis/DynamicAnalysis actually need per line, hiding the
     * references/connections/lines ID resolution behind one call.
     */
    struct LineAttachment {
        Node3D* top_node;
        const VesselMotionConfig* vessel_motion; ///< Never null.
    };

    /**
     * @brief Resolves every line's top node + applicable VesselMotionConfig.
     *
     * `lines` empty (every model built before this concept existed -- hand-built single-line
     * models, JSON without a `lines` array): returns exactly one attachment, `{nodes.front(),
     * &environmental.vessel_motion}` -- today's behavior, unchanged, zero migration needed.
     *
     * `lines` non-empty: for each line, follows `top_connection_id -> ConnectionInfo -> node_id`
     * to the real `Node3D*` (already positioned by whoever built this model -- this method does
     * NOT re-derive geometry from a reference's origin/offset, see ConnectionInfo's doc comment),
     * and `ConnectionInfo::reference_id -> ReferenceInfo` for the VesselMotionConfig to use: that
     * reference's own `vessel_motion` when `type == Floating`, else the model-wide
     * `environmental.vessel_motion` (e.g. an anchor-referenced or otherwise `Global` connection --
     * shouldn't normally be used as a TOP connection, but falls back safely rather than
     * dereferencing nothing). A line whose `top_connection_id`/`node_id` doesn't resolve is
     * skipped (not a partial/garbage entry) -- callers iterate whatever comes back.
     */
    std::vector<LineAttachment> resolve_line_attachments() {
        std::vector<LineAttachment> result;
        if (lines_.empty()) {
            if (!nodes_.empty()) {
                result.push_back({nodes_.front().get(), &environmental_.vessel_motion});
            }
            return result;
        }

        std::map<int, Node3D*> node_by_id;
        for (const auto& n : nodes_) node_by_id[n->id] = n.get();
        std::map<int, const ConnectionInfo*> conn_by_id;
        for (const auto& c : connections_) conn_by_id[c.id] = &c;
        std::map<int, const ReferenceInfo*> ref_by_id;
        for (const auto& r : references_) ref_by_id[r.id] = &r;

        for (const auto& line : lines_) {
            auto cit = conn_by_id.find(line.top_connection_id);
            if (cit == conn_by_id.end()) continue;
            auto nit = node_by_id.find(cit->second->node_id);
            if (nit == node_by_id.end()) continue;

            const VesselMotionConfig* cfg = &environmental_.vessel_motion;
            auto rit = ref_by_id.find(cit->second->reference_id);
            if (rit != ref_by_id.end() && rit->second->type == ReferenceType::Floating) {
                cfg = &rit->second->vessel_motion;
            }
            result.push_back({nit->second, cfg});
        }
        return result;
    }

private:
    std::vector<std::unique_ptr<Node3D>> nodes_;
    std::vector<std::unique_ptr<Element>> elements_;
    EnvironmentalConfig environmental_;
    AnalysisOptionsConfig analysis_options_;
    std::vector<ReferenceInfo> references_;
    std::vector<ConnectionInfo> connections_;
    std::vector<LineInfo> lines_;
};

} // namespace risersim

#endif // RISERSIM_MODEL_HPP
