#pragma once
#include "dve/types.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
namespace dve {
struct Bitset512 {
 std::array<std::uint64_t,8> words{};
 static Bitset512 full(){ Bitset512 b; b.words.fill(~std::uint64_t{0}); return b; }
 bool none() const {for(auto x: words) if(x) return false;return true;}
 void set(unsigned i){words[i>>6]|=std::uint64_t{1}<<(i&63U);}
 Bitset512& operator|=(const Bitset512& b){for(size_t i=0;i<8;++i)words[i]|=b.words[i]; return *this;}
 bool operator==(const Bitset512&) const=default;
};
struct AppliedBrickEdit {BrickKey key{};Bitset512 changedMask{};std::uint32_t generation{};};
class Brick {std::uint32_t g_{};public:explicit Brick(std::uint32_t g):g_(g){} std::uint32_t generation()const{return g_;}};
class VoxelObject {
public: std::map<BrickKey,Brick> bricks; const Brick* find_brick(BrickKey key)const {auto it=bricks.find(key);return it==bricks.end()?nullptr:&it->second;}
};
}
