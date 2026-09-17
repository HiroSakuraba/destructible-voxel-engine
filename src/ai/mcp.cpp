#include "dve/ai/mcp.hpp"

#include <atomic>

namespace dve::ai {
namespace {
JsonValue make_error(const JsonValue* id,int code,std::string message,JsonValue data=JsonValue{}){
    JsonValue::Object result{{"jsonrpc","2.0"},{"id",id?*id:JsonValue(nullptr)},
        {"error",JsonValue::Object{{"code",code},{"message",std::move(message)}}}};
    if(!data.is_null()) result["error"]["data"]=std::move(data);
    return result;
}
JsonValue text_content(std::string text){return JsonValue::Array{JsonValue::Object{{"type","text"},{"text",std::move(text)}}};}
std::string next_mcp_actor(){static std::atomic_uint64_t next{1};return "mcp-session-"+std::to_string(next.fetch_add(1));}
}
DveMcpProtocol::DveMcpProtocol(DveAiBridge& bridge,AiApprovalPolicy policy):bridge_(bridge),policy_(policy),actorId_(next_mcp_actor()){}
std::string DveMcpProtocol::handle(std::string_view message){
    const auto parsed=parse_json(message);if(!parsed.value)return stringify_json(make_error(nullptr,-32700,"Parse error",JsonValue::Object{{"offset",static_cast<double>(parsed.errorOffset)},{"detail",parsed.error}}));
    if(!parsed.value->is_object())return stringify_json(make_error(nullptr,-32600,"Invalid Request"));
    const JsonValue* id=parsed.value->find("id");const JsonValue* method=parsed.value->find("method");
    if(!method||!method->is_string())return stringify_json(make_error(id,-32600,"method is required"));
    if(method->as_string()=="notifications/initialized"){
        if(initializeReceived_) initialized_=true;
        return {};
    }
    if(!id)return {};
    JsonValue response;
    try{response=result_for(*parsed.value);}catch(const std::exception& e){response=make_error(id,-32603,e.what());}
    return stringify_json(response,false);
}
JsonValue DveMcpProtocol::result_for(const JsonValue& request){
    const JsonValue* id=request.find("id");const std::string method(request.find("method")->as_string());const JsonValue* params=request.find("params");
    if(method=="initialize"){
        if(initializeReceived_) return make_error(id,-32600,"MCP session is already initialized");
        if(params&&params->is_object()){
            if(const auto* requested=params->find("protocolVersion");requested&&requested->is_string()&&requested->as_string()!="2025-11-25")
                return make_error(id,-32602,"Unsupported MCP protocol version",JsonValue::Object{{"supported",JsonValue::Array{"2025-11-25"}},{"requested",std::string(requested->as_string())}});
        }
        initializeReceived_=true;
        return JsonValue::Object{{"jsonrpc","2.0"},{"id",*id},{"result",JsonValue::Object{{"protocolVersion","2025-11-25"},{"capabilities",JsonValue::Object{{"tools",JsonValue::Object{{"listChanged",false}}},{"resources",JsonValue::Object{{"subscribe",false},{"listChanged",false}}},{"prompts",JsonValue::Object{{"listChanged",false}}}}},{"serverInfo",JsonValue::Object{{"name","dve-engine"},{"title","Destructible Voxel Engine"},{"version","1.49.0"}}},{"instructions","Inspect first. Mutating and destructive tools require explicit DVE approval unless the host policy permits them."}}}};
    }
    if(!initialized_)return make_error(id,-32002,"MCP session is not initialized");
    if(method=="ping")return JsonValue::Object{{"jsonrpc","2.0"},{"id",*id},{"result",JsonValue::Object{}}};
    if(method=="tools/list"){
        JsonValue::Array tools;for(const auto& d:bridge_.registry().tools())tools.emplace_back(JsonValue::Object{{"name",d.name},{"title",d.title},{"description",d.description},{"inputSchema",d.inputSchema},{"annotations",JsonValue::Object{{"readOnlyHint",d.risk==AiToolRisk::ReadOnly},{"destructiveHint",d.risk==AiToolRisk::Destructive},{"idempotentHint",d.idempotent},{"openWorldHint",false}}}});
        return JsonValue::Object{{"jsonrpc","2.0"},{"id",*id},{"result",JsonValue::Object{{"tools",std::move(tools)}}}};
    }
    if(method=="tools/call"){
        if(!params||!params->is_object()) return make_error(id,-32602,"params object is required");
        const auto* name=params->find("name");
        if(!name||!name->is_string()) return make_error(id,-32602,"tool name is required");
        JsonValue arguments=JsonValue::Object{};
        if(const auto* a=params->find("arguments")) arguments=*a;
        std::string approval;if(arguments.is_object()){if(const auto* a=arguments.find("_dve_approval_id"))approval=std::string(a->as_string());arguments.as_object().erase("_dve_approval_id");}
        const auto result=bridge_.registry().call(name->as_string(),arguments,policy_,approval,actorId_);const bool error=result.status!=AiCallStatus::Completed&&result.status!=AiCallStatus::ApprovalRequired;
        JsonValue::Object body{{"content",text_content(result.message.empty()?stringify_json(result.content,true):result.message)},{"structuredContent",result.content},{"isError",error}};
        if(!result.approvalId.empty())body.emplace("_meta",JsonValue::Object{{"dve/approvalId",result.approvalId}});
        return JsonValue::Object{{"jsonrpc","2.0"},{"id",*id},{"result",std::move(body)}};
    }
    if(method=="resources/list"){
        JsonValue::Array resources{
            JsonValue::Object{{"uri","dve://project/summary"},{"name","project-summary"},{"title","DVE project summary"},{"mimeType","application/json"}},
            JsonValue::Object{{"uri","dve://scene/objects"},{"name","scene-objects"},{"title","Live scene objects"},{"mimeType","application/json"}},
            JsonValue::Object{{"uri","dve://validation/tasks"},{"name","validation-tasks"},{"title","Allowlisted validation tasks"},{"mimeType","application/json"}},
            JsonValue::Object{{"uri","dve://ai/pending-approvals"},{"name","pending-approvals"},{"title","Pending AI change proposals"},{"mimeType","application/json"}},
            JsonValue::Object{{"uri","dve://ai/audit"},{"name","ai-audit"},{"title","AI action audit"},{"mimeType","application/json"}}};
        if(bridge_.registry().find("dve.editor.context"))resources.emplace_back(JsonValue::Object{{"uri","dve://editor/context"},{"name","editor-context"},{"title","Open editor context"},{"mimeType","application/json"}});
        if(bridge_.registry().find("dve.editor.list_settings"))resources.emplace_back(JsonValue::Object{{"uri","dve://editor/settings"},{"name","editor-settings"},{"title","Editor settings"},{"mimeType","application/json"}});
        if(bridge_.registry().find("dve.editor.live_mcp_status"))resources.emplace_back(JsonValue::Object{{"uri","dve://editor/live-mcp"},{"name","live-editor-mcp"},{"title","Live editor MCP host status"},{"mimeType","application/json"}});
        return JsonValue::Object{{"jsonrpc","2.0"},{"id",*id},{"result",JsonValue::Object{{"resources",std::move(resources)}}}};
    }
    if(method=="resources/read"){
        const auto* uri=params?params->find("uri"):nullptr;if(!uri||!uri->is_string())return make_error(id,-32602,"resource uri is required");AiToolCallResult value;
        if(uri->as_string()=="dve://project/summary")value=bridge_.registry().call("dve.project.summary",JsonValue::Object{},AiApprovalPolicy::ReadOnlyOnly,{},"mcp-resource");
        else if(uri->as_string()=="dve://scene/objects")value=bridge_.registry().call("dve.scene.list_objects",JsonValue::Object{},AiApprovalPolicy::ReadOnlyOnly,{},"mcp-resource");
        else if(uri->as_string()=="dve://validation/tasks")value=bridge_.registry().call("dve.project.list_validation_tasks",JsonValue::Object{},AiApprovalPolicy::ReadOnlyOnly,{},"mcp-resource");
        else if(uri->as_string()=="dve://ai/pending-approvals")value=bridge_.registry().call("dve.ai.pending_approvals",JsonValue::Object{},AiApprovalPolicy::ReadOnlyOnly,{},"mcp-resource");
        else if(uri->as_string()=="dve://ai/audit")value=bridge_.registry().call("dve.ai.audit",JsonValue::Object{},AiApprovalPolicy::ReadOnlyOnly,{},"mcp-resource");
        else if(uri->as_string()=="dve://editor/context")value=bridge_.registry().call("dve.editor.context",JsonValue::Object{},AiApprovalPolicy::ReadOnlyOnly,{},"mcp-resource");
        else if(uri->as_string()=="dve://editor/settings")value=bridge_.registry().call("dve.editor.list_settings",JsonValue::Object{},AiApprovalPolicy::ReadOnlyOnly,{},"mcp-resource");
        else if(uri->as_string()=="dve://editor/live-mcp")value=bridge_.registry().call("dve.editor.live_mcp_status",JsonValue::Object{},AiApprovalPolicy::ReadOnlyOnly,{},"mcp-resource");
        else return make_error(id,-32002,"resource not found");
        return JsonValue::Object{{"jsonrpc","2.0"},{"id",*id},{"result",JsonValue::Object{{"contents",JsonValue::Array{JsonValue::Object{{"uri",std::string(uri->as_string())},{"mimeType","application/json"},{"text",stringify_json(value.content,true)}}}}}}};
    }
    if(method=="prompts/list"){
        JsonValue::Array prompts{
            JsonValue::Object{{"name","inspect-project"},{"title","Inspect DVE project"},{"description","Inspect project and scene before proposing changes"}},
            JsonValue::Object{{"name","implement-feature"},{"title","Implement engine feature"},{"description","Inspect relevant files, propose a bounded plan, then request approval for writes"},{"arguments",JsonValue::Array{JsonValue::Object{{"name","feature"},{"description","Feature request"},{"required",true}}}}},
            JsonValue::Object{{"name","review-scene"},{"title","Review live scene"},{"description","Inspect live scene objects and identify issues without changing them"}}};
        return JsonValue::Object{{"jsonrpc","2.0"},{"id",*id},{"result",JsonValue::Object{{"prompts",std::move(prompts)}}}};
    }
    if(method=="prompts/get"){
        const auto* name=params?params->find("name"):nullptr;if(!name||!name->is_string())return make_error(id,-32602,"prompt name is required");std::string text;
        if(name->as_string()=="inspect-project")text="Inspect dve://project/summary and the relevant files. Report evidence before suggesting changes.";
        else if(name->as_string()=="review-scene")text="Inspect dve://scene/objects. Identify composition, performance, camera, material, and gameplay issues. Do not mutate the scene.";
        else if(name->as_string()=="implement-feature"){const auto* arguments=params->find("arguments");const std::string feature=arguments&&arguments->find("feature")?std::string(arguments->find("feature")->as_string()):"the requested feature";text="Implement "+feature+". Inspect first, keep changes bounded, use exact base hashes and dve.project.preview_patch before dve.project.apply_patch, then run the narrowest named validation task after approval.";}
        else return make_error(id,-32602,"unknown prompt");
        return JsonValue::Object{{"jsonrpc","2.0"},{"id",*id},{"result",JsonValue::Object{{"description","DVE assistant workflow"},{"messages",JsonValue::Array{JsonValue::Object{{"role","user"},{"content",JsonValue::Object{{"type","text"},{"text",text}}}}}}}}};
    }
    return make_error(id,-32601,"Method not found");
}
} // namespace dve::ai
