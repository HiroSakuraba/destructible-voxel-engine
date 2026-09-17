#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "dve/replication_contract.hpp"

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
}

int main() {
    try {
        using namespace dve;
        const Float3 source{123.4567F, -2.3456F, 0.0044F};
        const auto quantized = quantize_position_millimeters(source);
        const Float3 restored = dequantize_position_meters(quantized);
        require(std::abs(restored.x - 123.457F) < 0.0011F, "position quantization drifted");
        const auto qv = quantize_velocity_centimeters({400.0F, -400.0F, 1.234F});
        require(qv.xCentimetersPerSecond == 32767 && qv.yCentimetersPerSecond == -32768,
                "velocity saturation failed");

        GameplayReplicationFrame frame;
        frame.serverTick = 240;
        frame.baselineTick = 232;
        frame.characters.push_back({17, quantize_position_millimeters({1.0F,2.0F,3.0F}),
            quantize_velocity_centimeters({4.0F,5.0F,6.0F}), 0x03U, 9});
        frame.triggers.push_back({41, false, true});
        frame.destructionEdits.push_back({88, 1001, quantize_position_millimeters({3,4,5}), 750, 19, 0xAABBCCDDULL});
        std::string error;
        const auto bytes = encode_gameplay_replication_frame(frame, &error);
        require(!bytes.empty(), error.c_str());
        const auto decoded = decode_gameplay_replication_frame(bytes, &error);
        require(decoded.has_value(), error.c_str());
        require(decoded->stable_hash() == frame.stable_hash(), "replication round trip changed stable hash");
        require(decoded->characters.front().support == 9, "support object was not preserved");
        require(decoded->destructionEdits.front().expectedContentHash == 0xAABBCCDDULL,
                "destruction repair hash was not preserved");


        ReplicationIdentityRegistry identities;
        require(identities.bind_object(10, 100, &error), error.c_str());
        require(identities.bind_object(10, 100, &error), "idempotent object binding failed");
        require(identities.ensure_object(10) == 100, "explicit object network id changed");
        require(!identities.bind_trigger(4, 100, &error), "cross-kind duplicate network id was accepted");
        const auto triggerNetwork = identities.ensure_trigger(4);
        require(triggerNetwork != 0 && triggerNetwork != 100, "trigger identity allocation collided");
        require(identities.local_object(100) == 10, "reverse object identity lookup failed");
        require(identities.local_trigger(triggerNetwork) == 4, "reverse trigger identity lookup failed");

        GameDamageEvent damage;
        damage.objectId = 10;
        damage.worldCenter = {1.25F, 2.5F, 3.75F};
        damage.radius = 0.75F;
        DestructionReplicationJournal journal(identities);
        GameDamageEvent invalidDamage;
        require(journal.record(invalidDamage, 0, 0) == 0, "invalid destruction event was journaled");
        require(journal.record(damage, 7, 0x11223344ULL) == 1, "first destruction sequence was not one");
        damage.worldCenter.x += 1.0F;
        require(journal.record(damage, 8, 0x55667788ULL) == 2, "second destruction sequence was not two");
        require(journal.after(1).size() == 1U && journal.after(1).front().editSequence == 2,
                "destruction journal range query failed");
        journal.acknowledge_through(1);
        require(journal.after(0).size() == 1U, "destruction acknowledgement did not trim old edits");

        std::vector<std::uint8_t> truncated(bytes.begin(), bytes.end() - 1);
        require(!decode_gameplay_replication_frame(truncated, &error), "truncated packet was accepted");
        auto trailing = bytes; trailing.push_back(0U);
        require(!decode_gameplay_replication_frame(trailing, &error), "packet with trailing bytes was accepted");
        frame.destructionEdits.push_back({88, 1000, quantize_position_millimeters({1,1,1}), 1, 0, 0});
        require(!frame.validate(&error), "out-of-order destruction sequence was accepted");

        std::cout << "replication contract tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "replication contract tests failed: " << exception.what() << '\n';
        return 1;
    }
}
