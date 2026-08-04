/**
 * @file rcm_reorder.hpp
 * @brief Reverse Cuthill-McKee bandwidth-reduction reordering for equation numbering.
 */
#ifndef RISERSIM_RCM_REORDER_HPP
#define RISERSIM_RCM_REORDER_HPP

#include "risersim/model.hpp"
#include <vector>
#include <queue>
#include <algorithm>
#include <unordered_map>

namespace risersim {

/**
 * @brief Computes a Reverse Cuthill-McKee (RCM) node visiting order to reduce the bandwidth of the assembled system matrix.
 *
 * Mirrors ANFLEX's real default (`cuthill_mckee_reorderer.cpp`, via the Boost Graph Library
 * there; here a direct, dependency-free implementation, to avoid pulling in Boost just for
 * this). ANFLEX applies this by default (`model_builder_dat.cpp`: reorderer default =
 * `"reverse_cuthill_mckee"`) before assembling the system, independent of the chosen solver
 * backend.
 *
 * Starting nodes are chosen from lowest to highest degree (a cheap approximation of a
 * peripheral node, without a full pseudo-peripheral algorithm such as George-Liu) -- sufficient
 * for the typically near-1D topology of riser/mooring lines. Neighbors are enqueued in ascending
 * degree order within each BFS level. The resulting Cuthill-McKee order is reversed to get RCM.
 *
 * @param model The model whose node connectivity (via `model.elements`) defines the graph.
 * @return The order in which `model.nodes` should be processed to number DOFs (indices into
 *         `model.nodes`; does *not* reorder `model.nodes` itself). Intended for
 *         Analysis::assign_equation_numbers().
 */
inline std::vector<int> compute_rcm_order(const RiserModel& model) {
    int n = static_cast<int>(model.nodes.size());
    std::vector<int> order;
    order.reserve(n);
    if (n == 0) return order;

    std::unordered_map<Node3D*, int> index_of;
    index_of.reserve(n);
    for (int i = 0; i < n; ++i) index_of[model.nodes[i].get()] = i;

    std::vector<std::vector<int>> adj(n);
    for (const auto& elem : model.elements) {
        auto it1 = index_of.find(elem->node1);
        auto it2 = index_of.find(elem->node2);
        if (it1 == index_of.end() || it2 == index_of.end()) continue;
        int a = it1->second, b = it2->second;
        adj[a].push_back(b);
        adj[b].push_back(a);
    }
    for (auto& neighbors : adj) {
        std::sort(neighbors.begin(), neighbors.end());
        neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
    }

    // Starting-node candidates, from lowest to highest degree -- approximates a "peripheral"
    // node without a full pseudo-peripheral algorithm (e.g. George-Liu), sufficient for the
    // typically near-1D topology of riser/mooring lines.
    std::vector<int> by_degree(n);
    for (int i = 0; i < n; ++i) by_degree[i] = i;
    std::sort(by_degree.begin(), by_degree.end(),
              [&](int a, int b) { return adj[a].size() < adj[b].size(); });

    std::vector<bool> visited(n, false);
    for (int start : by_degree) {
        if (visited[start]) continue;

        std::queue<int> q;
        visited[start] = true;
        q.push(start);

        while (!q.empty()) {
            int u = q.front();
            q.pop();
            order.push_back(u);

            std::vector<int> next;
            for (int v : adj[u]) {
                if (!visited[v]) {
                    visited[v] = true;
                    next.push_back(v);
                }
            }
            std::sort(next.begin(), next.end(),
                      [&](int a, int b) { return adj[a].size() < adj[b].size(); });
            for (int v : next) q.push(v);
        }
    }

    std::reverse(order.begin(), order.end()); // Cuthill-McKee -> Reverse Cuthill-McKee
    return order;
}

} // namespace risersim

#endif
