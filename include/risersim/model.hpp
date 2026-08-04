/**
 * @file model.hpp
 * @brief The riser/mooring model aggregate: owns all nodes and elements.
 */
#ifndef RISERSIM_MODEL_HPP
#define RISERSIM_MODEL_HPP

#include "risersim/node.hpp"
#include "risersim/element_beam.hpp"
#include <vector>
#include <memory>
#include <utility>

namespace risersim {

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
