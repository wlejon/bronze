#include "il_cfg_cycles.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace il2mir {

std::vector<uint8_t> cyclic_blocks(const BronzeFunction& fn) {
    // Tarjan's strongly connected components, iteratively: a top level has
    // thousands of blocks.
    const size_t n = fn.blocks.size();
    std::vector<uint8_t> cyclic(n, 0);
    if (n == 0) return cyclic;
    std::unordered_map<uint32_t, uint32_t> index_of;
    index_of.reserve(n);
    for (size_t i = 0; i < n; ++i) index_of.emplace(fn.blocks[i].id, static_cast<uint32_t>(i));
    std::vector<std::vector<uint32_t>> succ(n);
    std::vector<uint8_t> self_edge(n, 0);
    for (size_t i = 0; i < n; ++i) {
        for_each_successor(fn.blocks[i], [&](uint32_t id) {
            const auto it = index_of.find(id);
            if (it == index_of.end()) return;
            succ[i].push_back(it->second);
            if (it->second == i) self_edge[i] = 1;
        });
    }
    constexpr uint32_t kUnvisited = UINT32_MAX;
    std::vector<uint32_t> order(n, kUnvisited), low(n, 0);
    std::vector<uint8_t> on_stack(n, 0);
    std::vector<uint32_t> stack;
    std::vector<std::pair<uint32_t, size_t>> work;
    uint32_t counter = 0;
    for (uint32_t root = 0; root < n; ++root) {
        if (order[root] != kUnvisited) continue;
        work.emplace_back(root, 0);
        while (!work.empty()) {
            auto& [v, next] = work.back();
            if (next == 0 && order[v] == kUnvisited) {
                order[v] = low[v] = counter++;
                stack.push_back(v);
                on_stack[v] = 1;
            }
            if (next < succ[v].size()) {
                const uint32_t w = succ[v][next++];
                if (order[w] == kUnvisited) {
                    work.emplace_back(w, 0);
                } else if (on_stack[w]) {
                    low[v] = std::min(low[v], order[w]);
                }
                continue;
            }
            if (low[v] == order[v]) {
                size_t start = stack.size();
                do {
                    --start;
                } while (stack[start] != v);
                const bool cycle = stack.size() - start > 1 || self_edge[v];
                for (size_t k = start; k < stack.size(); ++k) {
                    on_stack[stack[k]] = 0;
                    cyclic[stack[k]] = cycle ? 1 : 0;
                }
                stack.resize(start);
            }
            const uint32_t done = v;
            work.pop_back();
            if (!work.empty()) {
                const uint32_t parent = work.back().first;
                low[parent] = std::min(low[parent], low[done]);
            }
        }
    }
    return cyclic;
}

} // namespace il2mir
