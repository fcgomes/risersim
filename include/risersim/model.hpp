/**
 * @file model.hpp
 * @brief The riser/mooring model aggregate: owns all nodes and elements.
 */
#ifndef RISERSIM_MODEL_HPP
#define RISERSIM_MODEL_HPP

#include "risersim/node.hpp"
#include "risersim/element_beam.hpp"
#include <vector>

namespace risersim {

/**
 * @brief Owning container of a riser/mooring model's nodes and elements, equivalent to ANFLEX's `cDomain`.
 *
 * Owns its Node3D/CorotationalBeam3D instances via raw pointer + manual `delete` in clear()
 * (copy disabled, move enabled) rather than `std::unique_ptr`, for compatibility with the
 * pybind11 bindings layer, which hands out raw pointers into `nodes`/`elements` for Python-side
 * introspection.
 */
class RiserModel {
public:
    std::vector<Node3D*> nodes;
    std::vector<CorotationalBeam3D*> elements;

    RiserModel() = default;

    /** @brief Releases all owned nodes and elements. */
    ~RiserModel() {
        clear();
    }

    // Copy disabled to avoid accidental double-free of owned pointers.
    RiserModel(const RiserModel&) = delete;
    RiserModel& operator=(const RiserModel&) = delete;

    // Move semantics allowed.
    RiserModel(RiserModel&& other) noexcept
        : nodes(std::move(other.nodes)), elements(std::move(other.elements)) {}

    RiserModel& operator=(RiserModel&& other) noexcept {
        if (this != &other) {
            clear();
            nodes = std::move(other.nodes);
            elements = std::move(other.elements);
        }
        return *this;
    }

    /** @brief Deletes all owned nodes and elements and empties both containers. */
    void clear() {
        for (auto* elem : elements) {
            delete elem;
        }
        elements.clear();

        for (auto* node : nodes) {
            delete node;
        }
        nodes.clear();
    }
};

} // namespace risersim

#endif // RISERSIM_MODEL_HPP
