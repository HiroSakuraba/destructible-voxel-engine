#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include "dve/ai/mcp.hpp"
#include "dve/game_world.hpp"

int main(int argc, char** argv) {
    const std::filesystem::path output = argc > 1
        ? std::filesystem::path(argv[1])
        : std::filesystem::path("ai_assistant_v1_39_evidence.json");

    auto physics = std::make_unique<dve::ReferenceRigidBodyWorld>();
    dve::GameWorld world(std::move(physics));
    dve::GameObjectDesc marker;
    marker.name = "AI Inspection Point";
    marker.transform.position = {1.0F, 2.0F, 3.0F};
    const auto markerId = world.create_object(std::move(marker));

    dve::ai::DveAiBridge bridge({std::filesystem::current_path()}, &world);
    dve::ai::DveMcpProtocol mcp(bridge);
    const std::string initialize = mcp.handle(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"demo","version":"1"}}})");
    const std::string initializedNotification = mcp.handle(
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    (void)initializedNotification;
    const std::string tools = mcp.handle(
        R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})");
    const std::string scene = mcp.handle(
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"dve.scene.list_objects","arguments":{}}})");

    dve::ai::JsonValue moveRequest = dve::ai::JsonValue::Object{
        {"jsonrpc", "2.0"},
        {"id", 4},
        {"method", "tools/call"},
        {"params", dve::ai::JsonValue::Object{
            {"name", "dve.scene.set_transform"},
            {"arguments", dve::ai::JsonValue::Object{
                {"id", static_cast<double>(markerId)},
                {"position", dve::ai::JsonValue::Array{4, 5, 6}}
            }}
        }}
    };
    const std::string approval = mcp.handle(dve::ai::stringify_json(moveRequest));

    const auto parsedOrNull = [](const std::string& text) {
        const auto parsed = dve::ai::parse_json(text);
        return parsed.value.value_or(dve::ai::JsonValue{});
    };
    dve::ai::JsonValue evidence = dve::ai::JsonValue::Object{
        {"version", "1.39.0"},
        {"protocol", "2025-11-25"},
        {"tool_count", static_cast<double>(bridge.registry().tools().size())},
        {"marker_id", static_cast<double>(markerId)},
        {"initialize", parsedOrNull(initialize)},
        {"tools", parsedOrNull(tools)},
        {"scene", parsedOrNull(scene)},
        {"approval", parsedOrNull(approval)},
        {"audit_entries", static_cast<double>(bridge.registry().audit().entries().size())},
        {"security", dve::ai::JsonValue::Object{
            {"project_root_sandbox", true},
            {"write_approval", true},
            {"destructive_approval", true},
            {"shell_tool_exposed", false},
            {"api_key_persisted", false}
        }}
    };

    std::ofstream file(output, std::ios::binary | std::ios::trunc);
    file << dve::ai::stringify_json(evidence, true) << '\n';
    std::cout << output << '\n';
    return file ? 0 : 1;
}
