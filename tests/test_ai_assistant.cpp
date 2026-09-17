#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <map>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

#include "dve/ai/mcp.hpp"
#include "dve/ai/openai_responses.hpp"
#include "dve/game_world.hpp"
#if defined(DVE_ENABLE_FLUODDITY)
#include "dve/fluoddity.hpp"
#endif
#include "dve/editor_ai_assistant.hpp"

namespace {
int failures=0;
#define CHECK(...) do { if(!(__VA_ARGS__)){std::cerr<<"FAIL "<<__FILE__<<':'<<__LINE__<<"  " #__VA_ARGS__ "\n";++failures;} } while(false)

using namespace dve;
using namespace dve::ai;

std::filesystem::path make_temp(){auto path=std::filesystem::temp_directory_path()/"dve-ai-tests";std::filesystem::remove_all(path);std::filesystem::create_directories(path/"src");std::ofstream(path/"src"/"sample.cpp")<<"int value = 1;\n";return path;}

class MockTransport final : public IAiHttpTransport {
public:
    std::vector<AiHttpResponse> responses;
    std::vector<std::string> requests;
    std::chrono::milliseconds delay{};
    AiHttpResponse post_json(std::string_view,std::string_view,std::string_view body) override {
        if(delay.count()>0)std::this_thread::sleep_for(delay);
        requests.emplace_back(body); if(responses.empty()) return {500,{},"no mock response"};auto result=responses.front();responses.erase(responses.begin());return result;
    }
};

void test_json(){auto parsed=parse_json(R"({"a":[1,true,"x"],"b":null})");CHECK(parsed.value.has_value());CHECK(parsed.value->find("a")&&parsed.value->find("a")->as_array().size()==3);CHECK(parse_json(stringify_json(*parsed.value)).value.has_value());CHECK(!parse_json("{]").value.has_value());}

void test_project_sandbox_and_approval(){const auto root=make_temp();DveAiBridge bridge({root});auto read=bridge.registry().call("dve.project.read_text",JsonValue::Object{{"path","src/sample.cpp"}},AiApprovalPolicy::AskForChanges,{});CHECK(read.status==AiCallStatus::Completed);CHECK(read.content.find("content")->as_string().find("value")!=std::string_view::npos);
    auto escape=bridge.registry().call("dve.project.read_text",JsonValue::Object{{"path","../escape.txt"}},AiApprovalPolicy::AskForChanges,{});CHECK(escape.status!=AiCallStatus::Completed);
    auto write=bridge.registry().call("dve.project.write_text",JsonValue::Object{{"path","src/sample.cpp"},{"content","int value = 2;\n"}},AiApprovalPolicy::AskForChanges,{});CHECK(write.status==AiCallStatus::ApprovalRequired);CHECK(!write.approvalId.empty());CHECK(bridge.registry().approvals().approve(write.approvalId));auto approved=bridge.registry().call("dve.project.write_text",JsonValue::Object{{"path","src/sample.cpp"},{"content","int value = 2;\n"}},AiApprovalPolicy::AskForChanges,write.approvalId);CHECK(approved.status==AiCallStatus::Completed);std::ifstream in(root/"src"/"sample.cpp");std::string text((std::istreambuf_iterator<char>(in)),{});CHECK(text=="int value = 2;\n");
}


void test_approval_is_bound_to_exact_arguments(){
    const auto root=make_temp();DveAiBridge bridge({root});
    JsonValue original=JsonValue::Object{{"path","src/sample.cpp"},{"content","int value = 2;\n"}};
    JsonValue altered=JsonValue::Object{{"path","src/sample.cpp"},{"content","int value = 999;\n"}};
    auto pending=bridge.registry().call("dve.project.write_text",original,AiApprovalPolicy::AskForChanges,{},"client-a");
    CHECK(pending.status==AiCallStatus::ApprovalRequired);CHECK(bridge.registry().approvals().approve(pending.approvalId));
    auto otherClient=bridge.registry().call("dve.project.write_text",original,AiApprovalPolicy::AskForChanges,pending.approvalId,"client-b");
    CHECK(otherClient.status==AiCallStatus::ApprovalRequired);
    auto replay=bridge.registry().call("dve.project.write_text",altered,AiApprovalPolicy::AskForChanges,pending.approvalId,"client-a");
    CHECK(replay.status==AiCallStatus::ApprovalRequired);
    std::ifstream before(root/"src"/"sample.cpp");std::string unchanged((std::istreambuf_iterator<char>(before)),{});CHECK(unchanged=="int value = 1;\n");
    auto approved=bridge.registry().call("dve.project.write_text",original,AiApprovalPolicy::AskForChanges,pending.approvalId,"client-a");
    CHECK(approved.status==AiCallStatus::Completed);
}

void test_transactional_patch_and_rollback(){
    const auto root=make_temp();DveAiBridge bridge({root});
    const std::string patch=
        "--- a/src/sample.cpp\n"
        "+++ b/src/sample.cpp\n"
        "@@ -1,1 +1,1 @@\n"
        "-int value = 1;\n"
        "+int value = 2;\n"
        "--- /dev/null\n"
        "+++ b/src/new.hpp\n"
        "@@ -0,0 +1,2 @@\n"
        "+#pragma once\n"
        "+inline constexpr int created = 7;\n";
    JsonValue hashes=JsonValue::Object{{"src/sample.cpp",to_hex(fnv1a64("int value = 1;\n"))},{"src/new.hpp",to_hex(fnv1a64(""))}};
    JsonValue args=JsonValue::Object{{"patch",patch},{"base_hashes",hashes}};
    auto preview=bridge.registry().call("dve.project.preview_patch",args,AiApprovalPolicy::AskForChanges,{});
    CHECK(preview.status==AiCallStatus::Completed);CHECK(preview.content.find("file_count")->as_number()==2.0);
    auto pending=bridge.registry().call("dve.project.apply_patch",args,AiApprovalPolicy::AskForChanges,{});
    CHECK(pending.status==AiCallStatus::ApprovalRequired);CHECK(bridge.registry().approvals().approve(pending.approvalId));
    auto applied=bridge.registry().call("dve.project.apply_patch",args,AiApprovalPolicy::AskForChanges,pending.approvalId);
    CHECK(applied.status==AiCallStatus::Completed);const std::string transaction(applied.content.find("transaction_id")->as_string());CHECK(!transaction.empty());
    std::ifstream changed(root/"src"/"sample.cpp");std::string changedText((std::istreambuf_iterator<char>(changed)),{});CHECK(changedText=="int value = 2;\n");
    std::ifstream created(root/"src"/"new.hpp");std::string createdText((std::istreambuf_iterator<char>(created)),{});CHECK(createdText=="#pragma once\ninline constexpr int created = 7;\n");
    JsonValue rollbackArgs=JsonValue::Object{{"transaction_id",transaction}};
    auto rollbackPending=bridge.registry().call("dve.project.rollback_patch",rollbackArgs,AiApprovalPolicy::AskForChanges,{});
    CHECK(rollbackPending.status==AiCallStatus::ApprovalRequired);CHECK(bridge.registry().approvals().approve(rollbackPending.approvalId));
    auto rolled=bridge.registry().call("dve.project.rollback_patch",rollbackArgs,AiApprovalPolicy::AskForChanges,rollbackPending.approvalId);
    CHECK(rolled.status==AiCallStatus::Completed);CHECK(!std::filesystem::exists(root/"src"/"new.hpp"));
    std::ifstream restored(root/"src"/"sample.cpp");std::string restoredText((std::istreambuf_iterator<char>(restored)),{});CHECK(restoredText=="int value = 1;\n");
}

void test_patch_rejects_stale_hash_and_post_change_rollback(){
    const auto root=make_temp();DveAiBridge bridge({root});
    const std::string patch="--- a/src/sample.cpp\n+++ b/src/sample.cpp\n@@ -1,1 +1,1 @@\n-int value = 1;\n+int value = 3;\n";
    JsonValue staleArgs=JsonValue::Object{{"patch",patch},{"base_hashes",JsonValue::Object{{"src/sample.cpp","0000000000000000"}}}};
    auto stale=bridge.registry().call("dve.project.preview_patch",staleArgs,AiApprovalPolicy::AskForChanges,{});CHECK(stale.status==AiCallStatus::InvalidArguments);
    JsonValue args=JsonValue::Object{{"patch",patch},{"base_hashes",JsonValue::Object{{"src/sample.cpp",to_hex(fnv1a64("int value = 1;\n"))}}}};
    auto pending=bridge.registry().call("dve.project.apply_patch",args,AiApprovalPolicy::AskForChanges,{});CHECK(bridge.registry().approvals().approve(pending.approvalId));auto applied=bridge.registry().call("dve.project.apply_patch",args,AiApprovalPolicy::AskForChanges,pending.approvalId);CHECK(applied.status==AiCallStatus::Completed);
    std::ofstream(root/"src"/"sample.cpp",std::ios::trunc)<<"int independently_changed = 4;\n";
    JsonValue rollback=JsonValue::Object{{"transaction_id",std::string(applied.content.find("transaction_id")->as_string())}};auto rp=bridge.registry().call("dve.project.rollback_patch",rollback,AiApprovalPolicy::AskForChanges,{});CHECK(bridge.registry().approvals().approve(rp.approvalId));auto refused=bridge.registry().call("dve.project.rollback_patch",rollback,AiApprovalPolicy::AskForChanges,rp.approvalId);CHECK(refused.status==AiCallStatus::Failed);
}

void test_named_validation_task(){
#if !defined(_WIN32)
    const auto root=make_temp();DveAiBridgeOptions options;options.projectRoot=root;options.enableDefaultValidationTasks=false;options.validationTasks.push_back({"test.echo","Echo validation","Deterministic test task","/bin/echo",{"validation-ok"},".",std::chrono::seconds(5),4096,false});DveAiBridge bridge(std::move(options));
    auto list=bridge.registry().call("dve.project.list_validation_tasks",JsonValue::Object{},AiApprovalPolicy::ReadOnlyOnly,{});CHECK(list.status==AiCallStatus::Completed);CHECK(list.content.find("tasks")->as_array().size()==1);
    JsonValue args=JsonValue::Object{{"name","test.echo"}};auto pending=bridge.registry().call("dve.project.run_validation_task",args,AiApprovalPolicy::AskForChanges,{});CHECK(pending.status==AiCallStatus::ApprovalRequired);CHECK(bridge.registry().approvals().approve(pending.approvalId));auto result=bridge.registry().call("dve.project.run_validation_task",args,AiApprovalPolicy::AskForChanges,pending.approvalId);CHECK(result.status==AiCallStatus::Completed);CHECK(result.content.find("output")->as_string().find("validation-ok")!=std::string_view::npos);
    (void)setenv("OPENAI_API_KEY","must-not-reach-validation-child",1);
    DveAiBridgeOptions envOptions;envOptions.projectRoot=root;envOptions.enableDefaultValidationTasks=false;envOptions.validationTasks.push_back({"test.env","Environment validation","Print sanitized task environment","/usr/bin/env",{},".",std::chrono::seconds(5),16384,false});DveAiBridge envBridge(std::move(envOptions));
    JsonValue envArgs=JsonValue::Object{{"name","test.env"}};auto envPending=envBridge.registry().call("dve.project.run_validation_task",envArgs,AiApprovalPolicy::AskForChanges,{});CHECK(envBridge.registry().approvals().approve(envPending.approvalId));auto envResult=envBridge.registry().call("dve.project.run_validation_task",envArgs,AiApprovalPolicy::AskForChanges,envPending.approvalId);CHECK(envResult.status==AiCallStatus::Completed);CHECK(envResult.content.find("output")->as_string().find("OPENAI_API_KEY")==std::string_view::npos);
    (void)unsetenv("OPENAI_API_KEY");
#endif
}


void test_text3d_tool_registration(){
    const auto root=make_temp();
    DveAiBridge bridge({root});
    const auto* tool=bridge.registry().find("dve.project.cook_text3d");
    CHECK(tool!=nullptr);
    if(tool){
        CHECK(tool->risk==AiToolRisk::Mutating);
        const auto* properties=tool->inputSchema.find("properties");
        CHECK(properties&&properties->find("font_path")!=nullptr);
        CHECK(properties&&properties->find("text")!=nullptr);
        CHECK(properties&&properties->find("output_path")!=nullptr);
    }
}


void test_gabor_tool_registration_and_cook(){
    const auto root=make_temp();std::filesystem::create_directories(root/"assets/volumes");
    std::ofstream ply(root/"assets/volumes/cloud.ply");
    ply<<"ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nproperty float y\nproperty float z\n"
       <<"property float scale_0\nproperty float scale_1\nproperty float scale_2\n"
       <<"property float rot_0\nproperty float rot_1\nproperty float rot_2\nproperty float rot_3\n"
       <<"property float opacities\nproperty float omega\nproperty float extent\n"
       <<"property float albedo_0\nproperty float albedo_1\nproperty float albedo_2\nend_header\n"
       <<"0 0 0 -1 -1 -1 1 0 0 0 1 0 3 0.8 0.9 1\n";ply.close();
    DveAiBridge bridge({root});const auto* tool=bridge.registry().find("dve.project.cook_gabor");CHECK(tool&&tool->risk==AiToolRisk::Mutating);
    JsonValue args=JsonValue::Object{{"source_path","assets/volumes/cloud.ply"},{"output_path","assets/volumes/cloud.dgabor"},{"preview_path","assets/volumes/cloud.ppm"}};
    auto pending=bridge.registry().call("dve.project.cook_gabor",args,AiApprovalPolicy::AskForChanges,{},"gabor-client");CHECK(pending.status==AiCallStatus::ApprovalRequired);CHECK(bridge.registry().approvals().approve(pending.approvalId));
    auto cooked=bridge.registry().call("dve.project.cook_gabor",args,AiApprovalPolicy::AskForChanges,pending.approvalId,"gabor-client");CHECK(cooked.status==AiCallStatus::Completed);CHECK(std::filesystem::is_regular_file(root/"assets/volumes/cloud.dgabor"));CHECK(std::filesystem::is_regular_file(root/"assets/volumes/cloud.ppm"));
}


#if defined(DVE_ENABLE_FLUODDITY)
void test_fluoddity_tool_registration_and_cook(){
    const auto root=make_temp();
    std::filesystem::create_directories(root/"assets/fluoddity");
    std::ofstream preset(root/"assets/fluoddity/minimal.json");
    preset<<"{\"version\":7,\"settings\":{\"num_cohorts\":8,\"rule_seed\":0.25},\"rule\":[";
    for(std::size_t index=0;index<kFluoddityRuleFloatCount;++index){if(index!=0)preset<<',';preset<<"0.0";}
    preset<<"]}";preset.close();
    DveAiBridge bridge({root});
    const auto* tool=bridge.registry().find("dve.project.cook_fluoddity");
    CHECK(tool&&tool->risk==AiToolRisk::Mutating);
    JsonValue args=JsonValue::Object{{"source_path","assets/fluoddity/minimal.json"},{"output_path","assets/fluoddity/minimal.dfluoddity"}};
    auto pending=bridge.registry().call("dve.project.cook_fluoddity",args,AiApprovalPolicy::AskForChanges,{},"fluoddity-client");
    CHECK(pending.status==AiCallStatus::ApprovalRequired);
    CHECK(bridge.registry().approvals().approve(pending.approvalId));
    auto cooked=bridge.registry().call("dve.project.cook_fluoddity",args,AiApprovalPolicy::AskForChanges,pending.approvalId,"fluoddity-client");
    CHECK(cooked.status==AiCallStatus::Completed);
    CHECK(std::filesystem::is_regular_file(root/"assets/fluoddity/minimal.dfluoddity"));
    const auto loaded=read_dfluoddity(root/"assets/fluoddity/minimal.dfluoddity");
    CHECK(loaded&&loaded.asset->cohortCount==8U);
    const auto* semantics=cooked.content.find("trail_semantics");
    CHECK(semantics&&semantics->as_string()=="velocity_rgb_alpha_zero");
}
#endif

void test_scene_tools(){const auto root=make_temp();GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());GameObjectDesc marker;marker.name="Marker";marker.transform.position={1,2,3};const auto id=world.create_object(std::move(marker));DveAiBridge bridge({root},&world);auto list=bridge.registry().call("dve.scene.list_objects",JsonValue::Object{},AiApprovalPolicy::ReadOnlyOnly,{});CHECK(list.status==AiCallStatus::Completed);CHECK(list.content.find("objects")->as_array().size()==1);
    JsonValue args=JsonValue::Object{{"id",static_cast<double>(id)},{"position",JsonValue::Array{4,5,6}}};auto pending=bridge.registry().call("dve.scene.set_transform",args,AiApprovalPolicy::AskForChanges,{});CHECK(pending.status==AiCallStatus::ApprovalRequired);CHECK(bridge.registry().approvals().approve(pending.approvalId));auto moved=bridge.registry().call("dve.scene.set_transform",args,AiApprovalPolicy::AskForChanges,pending.approvalId);CHECK(moved.status==AiCallStatus::Completed);CHECK(world.position(id)->x==4.0F);
}

void test_mcp(){
    const auto root=make_temp();DveAiBridge bridge({root});DveMcpProtocol mcp(bridge);
    const auto init=parse_json(mcp.handle(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})"));
    CHECK(init.value&&init.value->find("result"));
    const auto notification=mcp.handle(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");(void)notification;
    const auto tools=parse_json(mcp.handle(R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})"));
    CHECK(tools.value&&tools.value->find("result"));
    const auto call=parse_json(mcp.handle(R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"dve.project.summary","arguments":{}}})"));
    CHECK(call.value&&call.value->find("result"));
    const auto resources=parse_json(mcp.handle(R"({"jsonrpc":"2.0","id":4,"method":"resources/list","params":{}})"));
    const auto* resourceResult=resources.value?resources.value->find("result"):nullptr;
    const auto* resourceRows=resourceResult?resourceResult->find("resources"):nullptr;
    CHECK(resourceRows&&resourceRows->is_array()&&resourceRows->as_array().size()>=5);
    const auto validation=parse_json(mcp.handle(R"({"jsonrpc":"2.0","id":5,"method":"resources/read","params":{"uri":"dve://validation/tasks"}})"));
    const auto* validationResult=validation.value?validation.value->find("result"):nullptr;
    const auto* contents=validationResult?validationResult->find("contents"):nullptr;
    CHECK(contents&&contents->is_array()&&!contents->as_array().empty());
    const auto pendingCall=parse_json(mcp.handle(R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"dve.project.write_text","arguments":{"path":"src/sample.cpp","content":"int value = 8;\n"}}})"));
    CHECK(pendingCall.value&&pendingCall.value->find("result"));
    const auto pendingResource=parse_json(mcp.handle(R"({"jsonrpc":"2.0","id":7,"method":"resources/read","params":{"uri":"dve://ai/pending-approvals"}})"));
    const auto* pendingResult=pendingResource.value?pendingResource.value->find("result"):nullptr;
    CHECK(pendingResult&&pendingResult->find("contents"));
}


void test_editor_panel_approval_resume(){
    const auto root=make_temp();
    DveAiBridge bridge({root});
    auto transport=std::make_unique<MockTransport>();
    auto* raw=transport.get();
    const std::string call=R"({"type":"function_call","name":"dve.project.write_text","arguments":"{\"path\":\"src/sample.cpp\",\"content\":\"int value = 9;\\n\"}","call_id":"call_write"})";
    raw->responses.push_back({200,"{\"id\":\"resp_pending\",\"output\":["+call+"]}",{}});
    raw->responses.push_back({200,"{\"id\":\"resp_repeat\",\"output\":["+call+"]}",{}});
    raw->responses.push_back({200,R"({"id":"resp_done","output":[{"type":"message","content":[{"type":"output_text","text":"The file was updated."}]}]})",{}});
    OpenAiResponsesConfig config;config.approvalPolicy=AiApprovalPolicy::AskForChanges;
    auto client=std::make_unique<OpenAiResponsesClient>(bridge,std::move(transport),config,[]{return std::string("test-key");});
    dve::editor::EditorAiAssistantPanel panel;
    panel.attach(std::move(client),&bridge);
    CHECK(panel.send("Update the sample value"));
    CHECK(panel.pending_approvals().size()==1);
    const std::string approval=panel.pending_approvals().front().id;
    CHECK(panel.approve(approval));
    CHECK(panel.pending_approvals().empty());
    CHECK(raw->requests.size()==3);
    std::ifstream in(root/"src"/"sample.cpp");std::string text((std::istreambuf_iterator<char>(in)),{});
    CHECK(text=="int value = 9;\n");
    CHECK(!panel.messages().empty());
    CHECK(panel.messages().back().text=="The file was updated.");
}


void test_editor_panel_async_execution_and_cancellation(){
    const auto root=make_temp();DveAiBridge bridge({root});
    auto transport=std::make_unique<MockTransport>();auto* raw=transport.get();raw->responses.push_back({200,R"({"id":"resp_async","output":[{"type":"message","content":[{"type":"output_text","text":"Async complete."}]}]})",{}});
    auto client=std::make_unique<OpenAiResponsesClient>(bridge,std::move(transport),OpenAiResponsesConfig{},[]{return std::string("test-key");});
    dve::editor::EditorTaskManager tasks(1,4);dve::editor::EditorAiAssistantPanel panel;panel.attach(std::move(client),&bridge,&tasks);
    CHECK(panel.send("Run asynchronously"));CHECK(panel.busy());CHECK(panel.active_task().has_value());
    bool reattachRejected=false;
    try {
        auto replacementTransport=std::make_unique<MockTransport>();
        auto replacementClient=std::make_unique<OpenAiResponsesClient>(bridge,std::move(replacementTransport),OpenAiResponsesConfig{},[]{return std::string("test-key");});
        panel.attach(std::move(replacementClient),&bridge,&tasks);
    } catch(const std::logic_error&) { reattachRejected=true; }
    CHECK(reattachRejected);
    tasks.wait_idle();CHECK(panel.poll());CHECK(!panel.busy());CHECK(panel.messages().back().text=="Async complete.");CHECK(raw->requests.size()==1);

    auto slowTransport=std::make_unique<MockTransport>();slowTransport->delay=std::chrono::milliseconds(50);slowTransport->responses.push_back({200,R"({"id":"resp_cancel","output":[{"type":"message","content":[{"type":"output_text","text":"Too late."}]}]})",{}});
    auto slowClient=std::make_unique<OpenAiResponsesClient>(bridge,std::move(slowTransport),OpenAiResponsesConfig{},[]{return std::string("test-key");});panel.attach(std::move(slowClient),&bridge,&tasks);CHECK(panel.send("Cancel this"));CHECK(panel.cancel());tasks.wait_idle();CHECK(panel.poll());CHECK(!panel.busy());CHECK(panel.last_error()=="assistant request cancelled");
}

void test_openai_tool_loop(){const auto root=make_temp();DveAiBridge bridge({root});auto transport=std::make_unique<MockTransport>();auto* raw=transport.get();raw->responses.push_back({200,R"({"id":"resp_1","output":[{"type":"function_call","name":"dve.project.summary","arguments":"{}","call_id":"call_1"}]})",{}});raw->responses.push_back({200,R"({"id":"resp_2","model":"gpt-test","usage":{"input_tokens":12,"output_tokens":4,"total_tokens":16},"output":[{"type":"message","content":[{"type":"output_text","text":"Project inspected."}]}]})",{}});OpenAiResponsesConfig config;config.approvalPolicy=AiApprovalPolicy::AskForChanges;OpenAiResponsesClient client(bridge,std::move(transport),config,[]{return std::string("test-key");});const auto result=client.send("Inspect this project");CHECK(result.ok);CHECK(result.text=="Project inspected.");CHECK(result.usage.totalTokens==16);CHECK(result.model=="gpt-test");CHECK(raw->requests.size()==2);const auto second=parse_json(raw->requests[1]);CHECK(second.value&&second.value->find("previous_response_id"));}
}

int main(){test_json();test_project_sandbox_and_approval();test_approval_is_bound_to_exact_arguments();test_transactional_patch_and_rollback();test_patch_rejects_stale_hash_and_post_change_rollback();test_named_validation_task();test_text3d_tool_registration();test_gabor_tool_registration_and_cook();
#if defined(DVE_ENABLE_FLUODDITY)
    test_fluoddity_tool_registration_and_cook();
#endif
    test_scene_tools();test_mcp();test_editor_panel_approval_resume();test_editor_panel_async_execution_and_cancellation();test_openai_tool_loop();if(failures==0)std::cout<<"all AI assistant tests passed\n";return failures==0?0:1;}
