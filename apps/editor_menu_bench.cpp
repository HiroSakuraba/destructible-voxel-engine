// Single-threaded, warmed registry microbenchmark. Counts ordinary operator new
// calls inside each query, including result copies. Timings are observational;
// this is not a display-latency measurement or a hardware-independent threshold.
#include "dve/editor_workspace.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <vector>

namespace {
std::atomic<std::size_t> allocations{};
}

void* operator new(std::size_t size) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* pointer = std::malloc(size ? size : 1)) return pointer;
    throw std::bad_alloc();
}
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }

int main() {
    const auto registry = dve::editor::EditorMenuRegistry::make_default();
    const char* queries[]{"move", "rotate", "save", "collision", "anchor", "viewport", "scale", "simulate"};
    auto benchmark = [&](const char* name, auto operation) {
        for (int i = 0; i < 32; ++i) (void)operation(i);
        std::vector<double> times;
        times.reserve(1000);
        std::size_t totalAllocations = 0;
        std::size_t items = 0;
        for (int i = 0; i < 1000; ++i) {
            const auto before = allocations.load(std::memory_order_relaxed);
            const auto start = std::chrono::steady_clock::now();
            items += operation(i);
            const auto end = std::chrono::steady_clock::now();
            totalAllocations += allocations.load(std::memory_order_relaxed) - before;
            times.push_back(std::chrono::duration<double, std::micro>(end - start).count());
        }
        std::sort(times.begin(), times.end());
        std::cout << name << ": allocations/call=" << static_cast<double>(totalAllocations) / 1000.0
                  << " p50_us=" << times[499] << " p99_us=" << times[989]
                  << " items=" << items << '\n';
    };
    benchmark("search changing query", [&](int i) { return registry.search(queries[i % 8], 8).size(); });
    benchmark("search repeated query", [&](int) { return registry.search("collision", 8).size(); });
    benchmark("Tools menu", [&](int) { return registry.menu("Tools", true).size(); });
}
