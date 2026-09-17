#include "dve/ai/openai_responses.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <system_error>
#include <algorithm>
#include <atomic>
#include <cmath>

namespace dve::ai {
namespace {
std::string default_key(){const char* key=std::getenv("OPENAI_API_KEY");return key?std::string(key):std::string{};}
std::string shell_quote(std::string_view value){std::string result="'";for(const char c:value){if(c=='\'')result+="'\\''";else result.push_back(c);}result+='\'';return result;}
std::string curl_config_quote(std::string_view value){std::string result;result.reserve(value.size()+8U);for(const char c:value){if(c=='\\'||c=='"')result.push_back('\\');if(c=='\n'||c=='\r')continue;result.push_back(c);}return result;}
std::filesystem::path unique_file(const std::filesystem::path& directory,std::string_view prefix,std::string_view extension=".json"){static std::mt19937_64 rng{std::random_device{}()};return directory/(std::string(prefix)+"-"+std::to_string(rng())+std::string(extension));}
JsonValue function_tool(const AiToolDescriptor& tool){return JsonValue::Object{{"type","function"},{"name",tool.name},{"description",tool.description},{"parameters",tool.inputSchema},{"strict",false}};}
std::uint64_t token_count(const JsonValue* value){if(!value||!value->is_number())return 0;const double number=value->as_number();if(!std::isfinite(number)||number<0.0)return 0;return static_cast<std::uint64_t>(number);}
std::string next_openai_actor(){static std::atomic_uint64_t next{1};return "openai-session-"+std::to_string(next.fetch_add(1));}
}
CurlAiHttpTransport::CurlAiHttpTransport(std::filesystem::path temporaryDirectory,
                                             std::chrono::seconds connectTimeout,
                                             std::chrono::seconds requestTimeout,
                                             std::size_t retryCount)
    :temporaryDirectory_(std::move(temporaryDirectory)),
     connectTimeout_(std::max(std::chrono::seconds(1),connectTimeout)),
     requestTimeout_(std::max(std::chrono::seconds(1),requestTimeout)),
     retryCount_(std::min<std::size_t>(retryCount,5)){
    if(temporaryDirectory_.empty())temporaryDirectory_=std::filesystem::temp_directory_path();
}
AiHttpResponse CurlAiHttpTransport::post_json(std::string_view url,std::string_view bearerToken,std::string_view body){
    if(url.substr(0,8)!="https://") return {0,{},"OpenAI endpoint must use HTTPS"};
    if(bearerToken.empty()) return {0,{},"OPENAI_API_KEY is not configured"};
    std::error_code ec;
    std::filesystem::create_directories(temporaryDirectory_,ec);
    if(ec) return {0,{},"could not create secure OpenAI temporary directory"};
    const auto sessionDirectory=unique_file(temporaryDirectory_,"dve-openai-session","");
    if(!std::filesystem::create_directory(sessionDirectory,ec)||ec)
        return {0,{},"could not create private OpenAI request directory"};
    std::filesystem::permissions(sessionDirectory,std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace,ec);
    if(ec){std::filesystem::remove_all(sessionDirectory,ec);return {0,{},"could not secure OpenAI request directory"};}
    const auto request=sessionDirectory/"request.json";
    const auto response=sessionDirectory/"response.json";
    const auto status=sessionDirectory/"status.txt";
    const auto config=sessionDirectory/"curl.cfg";
    auto cleanup=[&](){ec.clear();std::filesystem::remove_all(sessionDirectory,ec);};
    {std::ofstream out(request,std::ios::binary|std::ios::trunc);out<<body;if(!out){cleanup();return {0,{},"could not create OpenAI request file"};}}
    {
        std::ofstream out(config,std::ios::binary|std::ios::trunc);
        out << "silent\nshow-error\nrequest = \"POST\"\n"
            << "connect-timeout = " << connectTimeout_.count() << "\n"
            << "max-time = " << requestTimeout_.count() << "\n"
            << "retry = " << retryCount_ << "\n"
            << "retry-delay = 1\nretry-all-errors\n"
            << "url = \"" << curl_config_quote(url) << "\"\n"
            << "header = \"Authorization: Bearer " << curl_config_quote(bearerToken) << "\"\n"
            << "header = \"Content-Type: application/json\"\n"
            << "data-binary = \"@" << curl_config_quote(request.string()) << "\"\n"
            << "output = \"" << curl_config_quote(response.string()) << "\"\n"
            << "write-out = \"%{http_code}\"\n";
        if(!out){cleanup();return {0,{},"could not create secure curl configuration"};}
    }
    std::filesystem::permissions(config,std::filesystem::perms::owner_read|std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace,ec);
    ec.clear();
    const std::string command="curl --config "+shell_quote(config.string())+" > "+shell_quote(status.string());
    const int code=std::system(command.c_str());
    std::ifstream bodyIn(response,std::ios::binary);
    std::string responseBody((std::istreambuf_iterator<char>(bodyIn)),{});
    std::ifstream statusIn(status);int httpStatus=0;statusIn>>httpStatus;
    cleanup();
    if(code!=0) return {httpStatus,std::move(responseBody),"curl transport failed"};
    return {httpStatus,std::move(responseBody),{}};
}
OpenAiResponsesClient::OpenAiResponsesClient(DveAiBridge& bridge,std::unique_ptr<IAiHttpTransport> transport,OpenAiResponsesConfig config,ApiKeyProvider keyProvider):bridge_(bridge),transport_(std::move(transport)),config_(std::move(config)),keyProvider_(std::move(keyProvider)),actorId_(next_openai_actor()){if(!keyProvider_)keyProvider_=default_key;}
JsonValue OpenAiResponsesClient::build_request(std::string_view userText,std::optional<std::string_view> previousResponseId) const {
    JsonValue::Array tools;for(const auto& tool:bridge_.registry().tools())tools.emplace_back(function_tool(tool));JsonValue::Object request{{"model",config_.model},{"instructions",config_.instructions},{"input",std::string(userText)},{"tools",std::move(tools)},{"parallel_tool_calls",false},{"store",true}};if(previousResponseId&&!previousResponseId->empty())request.emplace("previous_response_id",std::string(*previousResponseId));return request;
}
OpenAiTurnResult OpenAiResponsesClient::send(std::string_view userText,std::optional<std::string_view> previousResponseId){
    OpenAiTurnResult final;
    const auto fail=[&final](std::string message){final.ok=false;final.error=std::move(message);return final;};
    if(!transport_) return fail("OpenAI transport is not attached");
    const std::string key=keyProvider_();
    if(key.empty()) return fail("OPENAI_API_KEY is not configured");
    JsonValue request=build_request(userText,previousResponseId);
    for(std::size_t turn=0;turn<config_.maximumToolTurns;++turn){
        const auto http=transport_->post_json(config_.endpoint,key,stringify_json(request,false));
        if(!http.error.empty())return fail(http.error);
        if(http.statusCode<200||http.statusCode>=300)return fail("OpenAI HTTP "+std::to_string(http.statusCode)+": "+http.body);
        const auto parsed=parse_json(http.body);
        if(!parsed.value||!parsed.value->is_object())return fail("OpenAI response JSON is invalid: "+parsed.error);
        const auto* id=parsed.value->find("id");if(id&&id->is_string())final.responseId=std::string(id->as_string());
        if(const auto* model=parsed.value->find("model");model&&model->is_string())final.model=std::string(model->as_string());
        if(const auto* usage=parsed.value->find("usage");usage&&usage->is_object()){
            final.usage.inputTokens+=token_count(usage->find("input_tokens"));
            final.usage.outputTokens+=token_count(usage->find("output_tokens"));
            final.usage.totalTokens+=token_count(usage->find("total_tokens"));
        }
        JsonValue::Array outputs;bool called=false;
        const auto* output=parsed.value->find("output");
        if(output&&output->is_array())for(const auto& item:output->as_array()){
            if(!item.is_object())continue;
            const auto* type=item.find("type");if(!type||!type->is_string())continue;
            if(type->as_string()=="message"){
                const auto* content=item.find("content");
                if(content&&content->is_array())for(const auto& part:content->as_array()){
                    const auto* partType=part.find("type");const auto* text=part.find("text");
                    if(partType&&partType->is_string()&&partType->as_string()=="output_text"&&text&&text->is_string()){
                        if(!final.text.empty())final.text+='\n';
                        final.text+=text->as_string();
                    }
                }
            } else if(type->as_string()=="function_call"){
                called=true;
                const auto* name=item.find("name");const auto* arguments=item.find("arguments");const auto* callId=item.find("call_id");
                if(!name||!name->is_string()||!callId||!callId->is_string())return fail("OpenAI function call is missing name or call_id");
                JsonValue args=JsonValue::Object{};
                if(arguments&&arguments->is_string()){
                    const auto argumentJson=parse_json(arguments->as_string());
                    if(!argumentJson.value)return fail("OpenAI tool arguments are invalid JSON");
                    args=*argumentJson.value;
                }
                const auto toolResult=bridge_.registry().call(name->as_string(),args,config_.approvalPolicy,{},actorId_);
                if(toolResult.status==AiCallStatus::ApprovalRequired){
                    for(const auto& pending:bridge_.registry().approvals().pending())
                        if(!pending.resolved&&pending.id==toolResult.approvalId)final.pendingApprovals.push_back(pending);
                    final.ok=true;return final;
                }
                outputs.emplace_back(JsonValue::Object{
                    {"type","function_call_output"},{"call_id",std::string(callId->as_string())},
                    {"output",stringify_json(JsonValue::Object{{"status",std::string(to_string(toolResult.status))},{"message",toolResult.message},{"content",toolResult.content}},false)}});
            }
        }
        if(!called){final.ok=true;return final;}
        JsonValue nextBase=build_request("",std::nullopt);
        JsonValue tools=nextBase.find("tools")?*nextBase.find("tools"):JsonValue::Array{};
        request=JsonValue::Object{{"model",config_.model},{"instructions",config_.instructions},{"previous_response_id",final.responseId},{"input",std::move(outputs)},{"tools",std::move(tools)},{"parallel_tool_calls",false},{"store",true}};
    }
    return fail("maximum OpenAI tool turns exceeded");
}

} // namespace dve::ai
