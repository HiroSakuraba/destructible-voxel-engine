// Synthetic, deterministic CPU microbenchmark, not a frame-time or GPU benchmark.
#include "dve/region_dirty_index.hpp"
#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

using namespace dve;
static std::uint64_t elapsed_ns(std::chrono::steady_clock::time_point start) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now()-start).count());
}
int main() {
    constexpr std::size_t count=100'000;
    RegionCatalog catalog;
    RegionDirtyIndex index(catalog,{1'000'000,1'000'000});
    std::mt19937_64 rng(1701);
    std::vector<AppliedBrickEdit> edits;
    edits.reserve(count);
    for(std::size_t i=0;i<count;++i) {
        const auto bx=static_cast<std::int32_t>(rng()%128U)-64;
        const auto bz=static_cast<std::int32_t>(rng()%128U)-64;
        AppliedBrickEdit edit{{bx,0,bz},{},static_cast<std::uint32_t>(1+i/128U)};
        edit.changedMask.set(static_cast<std::uint16_t>(rng()%512U));
        edits.push_back(edit);
    }
    const auto start=std::chrono::steady_clock::now();
    for(const auto& edit : edits) {
        const auto result=index.submit(1,0,22,edit,region_dirty_channel(RegionDirtyConsumer::Gpu));
        if(result==RegionDirtySubmitResult::CapacityExceeded || result==RegionDirtySubmitResult::Invalid) return 2;
    }
    const auto submitNs=elapsed_ns(start);
    const auto unique=index.task_count();
    const auto leaseStart=std::chrono::steady_clock::now();
    auto tickets=index.lease_box(1,0,{-4,0,-4},{4,0,4},unique);
    if(!tickets) return 3;
    const auto leaseNs=elapsed_ns(leaseStart);
    for(const auto& ticket:*tickets)
        if(index.complete(ticket,true)!=RegionDirtyCompletion::Retired) return 4;
    if(index.task_count()!=0) return 5;
    std::cout << "{\"experiment\":\"r3_region_dirty\",\"events\":" << count
              << ",\"unique_tasks\":" << unique
              << ",\"submit_ns_per_event\":" << submitNs/count
              << ",\"lease_ns_per_ticket\":" << (tickets->empty()?0:leaseNs/tickets->size())
              << ",\"completed\":" << tickets->size() << "}\n";
}
