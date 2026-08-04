#ifndef RISERSIM_RCM_REORDER_HPP
#define RISERSIM_RCM_REORDER_HPP

#include "risersim/model.hpp"
#include <vector>
#include <queue>
#include <algorithm>
#include <unordered_map>

namespace risersim {

// Reverse Cuthill-McKee (RCM): reduz a banda da matriz de conectividade,
// fiel ao ANFLEX real (cuthill_mckee_reorderer.cpp -- lá via Boost Graph
// Library; aqui uma implementação direta, sem dependências externas, para
// não trazer o Boost só por causa disso). O ANFLEX aplica isso por padrão
// (model_builder_dat.cpp: reorderer default = "reverse_cuthill_mckee") antes
// de montar o sistema, independente do solver escolhido depois.
//
// Retorna a ORDEM em que os nós de model.nodes devem ser processados para
// numerar os graus de liberdade (não reordena model.nodes em si -- só
// devolve os índices na ordem RCM, para uso em assign_equation_numbers()).
inline std::vector<int> compute_rcm_order(const RiserModel& model) {
    int n = static_cast<int>(model.nodes.size());
    std::vector<int> order;
    order.reserve(n);
    if (n == 0) return order;

    std::unordered_map<Node3D*, int> index_of;
    index_of.reserve(n);
    for (int i = 0; i < n; ++i) index_of[model.nodes[i]] = i;

    std::vector<std::vector<int>> adj(n);
    for (auto* elem : model.elements) {
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

    // Candidatos a ponto de partida, do menor grau para o maior -- aproxima
    // um nó "periférico" sem um algoritmo pseudo-periférico completo (ex.:
    // George-Liu), suficiente para a topologia tipicamente quase-1D de
    // linhas de ancoragem/risers.
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
