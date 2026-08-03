#ifndef RISERSIM_MODEL_HPP
#define RISERSIM_MODEL_HPP

#include "risersim/node.hpp"
#include "risersim/element_beam.hpp"
#include <vector>

namespace risersim {

class RiserModel {
public:
    std::vector<Node3D*> nodes;
    std::vector<CorotationalBeam3D*> elements;

    RiserModel() = default;

    // Destrutor cuida da liberação correta de memória
    ~RiserModel() {
        clear();
    }

    // Desabilitar cópia para evitar liberação dupla acidental
    RiserModel(const RiserModel&) = delete;
    RiserModel& operator=(const RiserModel&) = delete;

    // Permitir move semantics
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
