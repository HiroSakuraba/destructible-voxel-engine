#include "dve/player/split_rc_probe_layout.hpp"
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dve::player;
#define REQUIRE(x) do { if (!(x)) throw std::runtime_error("split RC probe layout: " #x); } while (false)

int main() {
    try {
        SplitRcProbeLayoutCache cache;
        std::vector<std::uint32_t> keys, slots, initialKeys, initialSlots;
        std::string error;
        // Four levels at a negative world coordinate use the same 8/16/24-bit
        // signed-cell encoding as split_rc_probe_assign_cs, including zero marker.
        std::array<SplitRcProbeCandidate,5> candidates{};
        for(int i=0;i<4;++i) {
            candidates[i].positionSpacing={-0.75F,0.125F,0.5F,0.25F};
            candidates[i].levelSheet={i,0,0,0};
        }
        candidates[4]=candidates[0]; // duplicate maps to same slot
        REQUIRE(cache.assign(candidates,keys,slots,&error));
        REQUIRE(slots.size()==5 && keys.size()>=1024);
        REQUIRE(slots[0]==slots[4]);
        REQUIRE(keys[slots[0]]!=0);
        for(std::size_t i=1;i<4;++i)REQUIRE(keys[slots[i]]!=keys[slots[0]]);
        initialKeys=keys;initialSlots=slots;
        REQUIRE(cache.assign(candidates,keys,slots,&error));
        REQUIRE(cache.reused_last_call());
        REQUIRE(keys==initialKeys && slots==initialSlots);
        candidates[4].positionSpacing[0]=0.875F;
        REQUIRE(cache.assign(candidates,keys,slots,&error));
        REQUIRE(!cache.reused_last_call());
        REQUIRE(slots[0]!=slots[4]);
        candidates[4].levelSheet[1]=6;
        REQUIRE(!cache.assign(candidates,keys,slots,&error));
        REQUIRE(!cache.reused_last_call());
        candidates[4]=candidates[0];
        REQUIRE(cache.assign(candidates,keys,slots,&error));
        REQUIRE(!cache.reused_last_call());
        // A plane is allowed only in signed 8-bit index range.
        candidates[0].positionSpacing[0]=32.5F;
        REQUIRE(!cache.assign(candidates,keys,slots,&error));
        // Changed spacing or surface sheet must invalidate a previous layout.
        candidates[0]=candidates[4];
        REQUIRE(cache.assign(candidates,keys,slots,&error));
        candidates[0].levelSheet[1]=1;
        REQUIRE(cache.assign(candidates,keys,slots,&error));
        REQUIRE(!cache.reused_last_call());
        REQUIRE(slots[0]!=slots[4]);
        cache.invalidate();
        REQUIRE(cache.assign(candidates,keys,slots,&error));
        REQUIRE(!cache.reused_last_call());
        std::cout<<"CPU world-space probe layout and invalidation PASS\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr<<e.what()<<'\n';return 1;
    }
}
