#include "dve/ai/assistant.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>

#include "dve/game_world.hpp"
#include "dve/gabor_volume.hpp"
#if defined(DVE_ENABLE_FLUODDITY)
#include "dve/fluoddity.hpp"
#endif
#include "dve/text3d.hpp"
#include "dve/ai/named_tasks.hpp"
#include "dve/ai/project_patch.hpp"

namespace dve::ai {
namespace {

std::string lowercase(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

std::string utc_now() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm value{};
#if defined(_WIN32)
    gmtime_s(&value, &t);
#else
    gmtime_r(&t, &value);
#endif
    std::ostringstream out;
    out << std::put_time(&value, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

JsonValue object_schema(JsonValue::Object properties, std::vector<std::string> required = {}) {
    JsonValue::Array requiredValues;
    for (auto& value : required) requiredValues.emplace_back(std::move(value));
    return JsonValue::Object{{"type", "object"}, {"properties", std::move(properties)},
                             {"required", std::move(requiredValues)}, {"additionalProperties", false}};
}
JsonValue string_schema(std::string description = {}) {
    JsonValue::Object result{{"type", "string"}};
    if (!description.empty()) result.emplace("description", std::move(description));
    return result;
}
JsonValue number_schema(std::string description = {}) {
    JsonValue::Object result{{"type", "number"}};
    if (!description.empty()) result.emplace("description", std::move(description));
    return result;
}
JsonValue string_map_schema(std::string description = {}) {
    JsonValue::Object result{{"type", "object"}, {"additionalProperties", JsonValue::Object{{"type", "string"}}}};
    if (!description.empty()) result.emplace("description", std::move(description));
    return result;
}
JsonValue bool_schema(std::string description = {}) {
    JsonValue::Object result{{"type", "boolean"}};
    if (!description.empty()) result.emplace("description", std::move(description));
    return result;
}
JsonValue vec3_schema(std::string description) {
    return JsonValue::Object{{"type", "array"}, {"description", std::move(description)},
                             {"items", JsonValue::Object{{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}};
}
JsonValue quat_schema() {
    return JsonValue::Object{{"type", "array"}, {"description", "Quaternion [x,y,z,w]"},
                             {"items", JsonValue::Object{{"type", "number"}}}, {"minItems", 4}, {"maxItems", 4}};
}

bool is_within(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    auto rootIt = root.begin();
    auto candidateIt = candidate.begin();
    for (; rootIt != root.end(); ++rootIt, ++candidateIt) {
        if (candidateIt == candidate.end() || *rootIt != *candidateIt) return false;
    }
    return true;
}

std::optional<std::uint64_t> exact_u64(const JsonValue* value) {
    if (!value || !value->is_number()) return std::nullopt;
    const double number = value->as_number();
    if (number < 0.0 || number > 9007199254740991.0 || std::floor(number) != number) return std::nullopt;
    return static_cast<std::uint64_t>(number);
}

std::optional<Float3> read_vec3(const JsonValue* value) {
    if (!value || !value->is_array() || value->as_array().size() != 3) return std::nullopt;
    const auto& a = value->as_array();
    if (!a[0].is_number() || !a[1].is_number() || !a[2].is_number()) return std::nullopt;
    const double x=a[0].as_number(), y=a[1].as_number(), z=a[2].as_number();
    if (!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)) return std::nullopt;
    return Float3{static_cast<float>(x),static_cast<float>(y),static_cast<float>(z)};
}
std::optional<Quaternion> read_quat(const JsonValue* value) {
    if (!value || !value->is_array() || value->as_array().size() != 4) return std::nullopt;
    const auto& a=value->as_array(); for(const auto& x:a) if(!x.is_number()||!std::isfinite(x.as_number())) return std::nullopt;
    return Quaternion{static_cast<float>(a[0].as_number()),static_cast<float>(a[1].as_number()),
                      static_cast<float>(a[2].as_number()),static_cast<float>(a[3].as_number())};
}
JsonValue vec3_json(Float3 value) { return JsonValue::Array{value.x,value.y,value.z}; }
JsonValue quat_json(Quaternion value) { return JsonValue::Array{value.x,value.y,value.z,value.w}; }

std::string summarize_arguments(const JsonValue& args) {
    std::string text=stringify_json(args,false);
    if(text.size()>320) text.resize(320), text+="...";
    return text;
}

bool allowed_text_extension(const std::filesystem::path& path) {
    static constexpr std::array<std::string_view, 25> allowed{
        ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl",
        ".cmake", ".txt", ".md", ".json", ".lua", ".py", ".sh", ".bat", ".ps1",
        ".hlsl", ".hlsli", ".glsl", ".vert", ".frag", ".yaml", ".yml"};
    const std::string extension=path.extension().string();
    return std::find(allowed.begin(),allowed.end(),extension)!=allowed.end() || path.filename()=="CMakeLists.txt";
}

} // namespace

std::string_view to_string(AiToolRisk risk) noexcept {
    switch(risk){case AiToolRisk::ReadOnly:return "read_only";case AiToolRisk::Mutating:return "mutating";case AiToolRisk::Destructive:return "destructive";}return "unknown";
}
std::string_view to_string(AiCallStatus status) noexcept {
    switch(status){case AiCallStatus::Completed:return "completed";case AiCallStatus::ApprovalRequired:return "approval_required";case AiCallStatus::Denied:return "denied";case AiCallStatus::InvalidArguments:return "invalid_arguments";case AiCallStatus::Failed:return "failed";}return "unknown";
}
std::uint64_t fnv1a64(std::string_view text) noexcept { std::uint64_t h=1469598103934665603ULL;for(unsigned char c:text){h^=c;h*=1099511628211ULL;}return h; }
std::string to_hex(std::uint64_t value){std::ostringstream out;out<<std::hex<<std::setfill('0')<<std::setw(16)<<value;return out.str();}

AiAuditLog::AiAuditLog(std::filesystem::path path):path_(std::move(path)){}
void AiAuditLog::append(AiAuditEntry entry){
    if(entry.sequence==0) entry.sequence=nextSequence_++;
    if(entry.utcTimestamp.empty()) entry.utcTimestamp=utc_now();
    entries_.push_back(entry);
    if(path_.empty()) return;
    std::error_code ec; std::filesystem::create_directories(path_.parent_path(),ec);
    std::ofstream out(path_,std::ios::app|std::ios::binary); if(!out)return;
    JsonValue line=JsonValue::Object{{"sequence",entry.sequence},{"timestamp",entry.utcTimestamp},{"actor",entry.actor},
        {"tool",entry.tool},{"risk",std::string(to_string(entry.risk))},{"status",std::string(to_string(entry.status))},{"summary",entry.summary}};
    out<<stringify_json(line,false)<<'\n';
}
JsonValue AiAuditLog::as_json() const { JsonValue::Array rows; for(const auto& e:entries_)rows.emplace_back(JsonValue::Object{{"sequence",e.sequence},{"timestamp",e.utcTimestamp},{"actor",e.actor},{"tool",e.tool},{"risk",std::string(to_string(e.risk))},{"status",std::string(to_string(e.status))},{"summary",e.summary}});return rows; }

std::string AiApprovalStore::request(std::string tool,std::string actor,AiToolRisk risk,JsonValue arguments,std::string summary){
    const std::string id="approval-"+std::to_string(nextId_++);requests_.push_back({id,std::move(tool),std::move(actor),risk,std::move(arguments),std::move(summary),false,false});return id;
}
bool AiApprovalStore::approve(std::string_view id){for(auto& r:requests_)if(r.id==id&&!r.resolved){r.approved=true;r.resolved=true;return true;}return false;}
bool AiApprovalStore::deny(std::string_view id){for(auto& r:requests_)if(r.id==id&&!r.resolved){r.approved=false;r.resolved=true;return true;}return false;}
bool AiApprovalStore::consume_approved(std::string_view id,std::string_view tool,const JsonValue& arguments,std::string_view actor){
    const std::string canonical=stringify_json(arguments,false);
    for(auto& r:requests_)if(r.id==id&&r.tool==tool&&r.actor==actor&&r.resolved&&r.approved&&stringify_json(r.arguments,false)==canonical){r.approved=false;return true;}
    return false;
}
bool AiApprovalStore::consume_approved_for(std::string_view tool,const JsonValue& arguments,std::string_view actor){
    const std::string canonical=stringify_json(arguments,false);
    for(auto& r:requests_)if(r.tool==tool&&r.actor==actor&&r.resolved&&r.approved&&stringify_json(r.arguments,false)==canonical){r.approved=false;return true;}
    return false;
}

bool AiToolRegistry::add(AiToolDescriptor descriptor,Handler handler,std::string* error){
    if(descriptor.name.empty()||!handler){if(error)*error="tool name and handler are required";return false;}
    if(handlers_.contains(descriptor.name)){if(error)*error="duplicate AI tool: "+descriptor.name;return false;}
    handlers_.emplace(descriptor.name,std::move(handler));descriptors_.push_back(std::move(descriptor));return true;
}
const AiToolDescriptor* AiToolRegistry::find(std::string_view name) const noexcept {for(const auto& d:descriptors_)if(d.name==name)return &d;return nullptr;}
void AiToolRegistry::set_audit_path(std::filesystem::path path){AiAuditLog replacement(std::move(path));for(const auto& e:audit_.entries())replacement.append(e);audit_=std::move(replacement);}
AiToolCallResult AiToolRegistry::call(std::string_view name,const JsonValue& arguments,AiApprovalPolicy policy,std::string_view approvalId,std::string_view actor){
    const AiToolDescriptor* descriptor=find(name); if(!descriptor)return {AiCallStatus::Failed,{},"unknown tool: "+std::string(name),{}};
    const auto handler=handlers_.find(name); if(handler==handlers_.end())return {AiCallStatus::Failed,{},"tool handler is unavailable",{}};
    const bool denied=policy==AiApprovalPolicy::DenyAll || (policy==AiApprovalPolicy::ReadOnlyOnly&&descriptor->risk!=AiToolRisk::ReadOnly);
    if(denied){AiToolCallResult r{AiCallStatus::Denied,{},"tool is denied by the active policy",{}};audit_.append({0,{},std::string(actor),std::string(name),descriptor->risk,r.status,r.message});return r;}
    const bool autoAllowed=descriptor->risk==AiToolRisk::ReadOnly || policy==AiApprovalPolicy::AllowChanges;
    const bool explicitlyApproved=!approvalId.empty()&&approvals_.consume_approved(approvalId,name,arguments,actor);
    const bool matchingApproval=approvalId.empty()&&approvals_.consume_approved_for(name,arguments,actor);
    if(!autoAllowed && !explicitlyApproved && !matchingApproval){
        const std::string summary=std::string(descriptor->title)+" "+summarize_arguments(arguments);
        const std::string id=approvals_.request(std::string(name),std::string(actor),descriptor->risk,arguments,summary);
        AiToolCallResult r{AiCallStatus::ApprovalRequired,JsonValue::Object{{"approval_required",true},{"approval_id",id},{"tool",std::string(name)},{"summary",summary}},"approval required",id};
        audit_.append({0,{},std::string(actor),std::string(name),descriptor->risk,r.status,summary}); return r;
    }
    AiToolCallResult result;
    try{result=handler->second(arguments);}catch(const std::exception& e){result={AiCallStatus::Failed,{},e.what(),{}};}
    audit_.append({0,{},std::string(actor),std::string(name),descriptor->risk,result.status,result.message.empty()?summarize_arguments(arguments):result.message});
    return result;
}

DveAiBridge::DveAiBridge(DveAiBridgeOptions options,GameWorld* world):options_(std::move(options)),world_(world){
    if(options_.projectRoot.empty())options_.projectRoot=std::filesystem::current_path();
    std::error_code ec;root_=std::filesystem::weakly_canonical(options_.projectRoot,ec);if(ec)root_=std::filesystem::absolute(options_.projectRoot);
    std::vector<AiNamedTaskDescriptor> tasks;
    if(options_.enableDefaultValidationTasks) tasks=default_dve_validation_tasks();
    tasks.insert(tasks.end(),options_.validationTasks.begin(),options_.validationTasks.end());
    taskRunner_=std::make_unique<AiNamedTaskRunner>(root_,std::move(tasks));
    registry_.set_audit_path(root_/".dve"/"ai_audit.jsonl");
    register_project_tools();register_patch_tools();register_task_tools();register_scene_tools();register_text3d_tools();register_gabor_tools();
#if defined(DVE_ENABLE_FLUODDITY)
    register_fluoddity_tools();
#endif
}
JsonValue DveAiBridge::project_summary() const {
    std::size_t files=0;std::uintmax_t bytes=0;std::error_code ec;
    for(std::filesystem::recursive_directory_iterator it(root_,std::filesystem::directory_options::skip_permission_denied,ec),end;it!=end&&!ec;++it){
        if(it.depth()>6){it.disable_recursion_pending();continue;} if(it->path().filename()==".git"||it->path().filename()=="build"){if(it->is_directory())it.disable_recursion_pending();continue;}
        if(it->is_regular_file()){++files;bytes+=it->file_size(ec);ec.clear();}
    }
    return JsonValue::Object{{"project_root",root_.generic_string()},{"file_count",static_cast<double>(files)},{"bytes",static_cast<double>(bytes)},
        {"geometry_profile",
#if defined(DVE_GEOMETRY_MODE_VOXEL)
        "VOXEL"
#elif defined(DVE_GEOMETRY_MODE_POLYGON)
        "POLYGON"
#else
        "HYBRID"
#endif
        },{"scene_attached",world_!=nullptr},{"tool_count",static_cast<double>(registry_.tools().size())}};
}
std::optional<std::filesystem::path> DveAiBridge::resolve_project_path(std::string_view relative,bool writing,std::string* error) const {
    if(relative.empty()){if(error)*error="path is required";return std::nullopt;}
    std::filesystem::path rel(relative); if(rel.is_absolute()){if(error)*error="absolute paths are not permitted";return std::nullopt;}
    std::error_code ec;const auto candidate=std::filesystem::weakly_canonical(root_/rel,ec);
    std::filesystem::path normalized=candidate;
    if(ec&&writing){ec.clear();const auto parent=std::filesystem::weakly_canonical((root_/rel).parent_path(),ec);if(!ec)normalized=parent/rel.filename();}
    if(ec||!is_within(root_,normalized)){if(error)*error="path escapes the project root or cannot be resolved";return std::nullopt;}
    return normalized;
}
void DveAiBridge::register_project_tools(){
    registry_.add({"dve.project.summary","Inspect project","Return project root, size, profile, scene attachment, and tool counts",object_schema({}),AiToolRisk::ReadOnly,true},
        [this](const JsonValue&){return AiToolCallResult{AiCallStatus::Completed,project_summary(),"project summary read",{}};});
    registry_.add({"dve.project.list_files","List project files","List bounded project-relative paths without following paths outside the project",
        object_schema({{"path",string_schema("Relative directory; defaults to project root")},{"limit",number_schema("Maximum entries, 1-500")}}),AiToolRisk::ReadOnly,true},
        [this](const JsonValue& args){
            const std::string_view rel=args.find("path")?args.find("path")->as_string():std::string_view{};std::string error;auto path=resolve_project_path(rel.empty()?".":rel,false,&error);if(!path)return AiToolCallResult{AiCallStatus::InvalidArguments,{},error,{}};
            const std::size_t limit=static_cast<std::size_t>(std::clamp(args.find("limit")?args.find("limit")->as_number(100):100.0,1.0,500.0));JsonValue::Array rows;std::error_code ec;
            for(std::filesystem::directory_iterator it(*path,std::filesystem::directory_options::skip_permission_denied,ec),end;it!=end&&!ec&&rows.size()<limit;++it){const auto relative=std::filesystem::relative(it->path(),root_,ec);if(ec){ec.clear();continue;}rows.emplace_back(JsonValue::Object{{"path",relative.generic_string()},{"directory",it->is_directory(ec)},{"bytes",it->is_regular_file(ec)?static_cast<double>(it->file_size(ec)):0.0}});ec.clear();}
            return AiToolCallResult{AiCallStatus::Completed,JsonValue::Object{{"entries",std::move(rows)}},"project files listed",{}};
        });
    registry_.add({"dve.project.read_text","Read project text file","Read a UTF-8 text file inside the project root with a strict byte limit",
        object_schema({{"path",string_schema("Project-relative path")}}, {"path"}),AiToolRisk::ReadOnly,true},
        [this](const JsonValue& args){std::string error;auto path=resolve_project_path(args.find("path")?args.find("path")->as_string():"",false,&error);if(!path)return AiToolCallResult{AiCallStatus::InvalidArguments,{},error,{}};std::error_code ec;const auto size=std::filesystem::file_size(*path,ec);if(ec)return AiToolCallResult{AiCallStatus::Failed,{},"file cannot be read",{}};if(size>options_.maximumReadBytes)return AiToolCallResult{AiCallStatus::Denied,{},"file exceeds AI read limit",{}};std::ifstream in(*path,std::ios::binary);std::string text((std::istreambuf_iterator<char>(in)),{});return AiToolCallResult{AiCallStatus::Completed,JsonValue::Object{{"path",std::filesystem::relative(*path,root_).generic_string()},{"content",text},{"fnv1a64",to_hex(fnv1a64(text))},{"bytes",static_cast<double>(text.size())}},"project text read",{}};});
    registry_.add({"dve.project.write_text","Write project text file","Atomically replace a permitted source/text file inside the project root. Optional expected hash prevents stale overwrites.",
        object_schema({{"path",string_schema("Project-relative source/text path")},{"content",string_schema("Complete replacement text")},{"expected_fnv1a64",string_schema("Optional current content hash")}}, {"path","content"}),AiToolRisk::Mutating,false},
        [this](const JsonValue& args){if(!options_.allowSourceWrites)return AiToolCallResult{AiCallStatus::Denied,{},"source writes are disabled",{}};const std::string_view content=args.find("content")?args.find("content")->as_string():"";if(content.size()>options_.maximumWriteBytes)return AiToolCallResult{AiCallStatus::Denied,{},"content exceeds AI write limit",{}};std::string error;auto path=resolve_project_path(args.find("path")?args.find("path")->as_string():"",true,&error);if(!path)return AiToolCallResult{AiCallStatus::InvalidArguments,{},error,{}};if(!allowed_text_extension(*path))return AiToolCallResult{AiCallStatus::Denied,{},"file extension is not in the source/text allowlist",{}};
            std::string existing;{std::ifstream in(*path,std::ios::binary);if(in)existing.assign(std::istreambuf_iterator<char>(in),{});}if(const auto* expected=args.find("expected_fnv1a64");expected&&expected->is_string()&&!expected->as_string().empty()&&expected->as_string()!=to_hex(fnv1a64(existing)))return AiToolCallResult{AiCallStatus::Failed,JsonValue::Object{{"current_fnv1a64",to_hex(fnv1a64(existing))}},"stale file hash; inspect before overwriting",{}};
            std::error_code ec;std::filesystem::create_directories(path->parent_path(),ec);if(ec)return AiToolCallResult{AiCallStatus::Failed,{},"could not create destination directory",{}};auto temporary=*path; temporary += ".dve-ai-tmp";{std::ofstream out(temporary,std::ios::binary|std::ios::trunc);out.write(content.data(),static_cast<std::streamsize>(content.size()));out.flush();if(!out)return AiToolCallResult{AiCallStatus::Failed,{},"temporary write failed",{}};}std::filesystem::rename(temporary,*path,ec);if(ec){std::filesystem::remove(*path,ec);ec.clear();std::filesystem::rename(temporary,*path,ec);}if(ec){std::filesystem::remove(temporary);return AiToolCallResult{AiCallStatus::Failed,{},"atomic replacement failed",{}};}
            return AiToolCallResult{AiCallStatus::Completed,JsonValue::Object{{"path",std::filesystem::relative(*path,root_).generic_string()},{"fnv1a64",to_hex(fnv1a64(content))},{"bytes",static_cast<double>(content.size())}},"project text written",{}};
        });
    registry_.add({"dve.ai.audit","Read AI audit log","Return the bounded in-memory audit history for this editor session",object_schema({}),AiToolRisk::ReadOnly,true},
        [this](const JsonValue&){return AiToolCallResult{AiCallStatus::Completed,JsonValue::Object{{"entries",registry_.audit().as_json()}},"audit read",{}};});
    registry_.add({"dve.ai.pending_approvals","Read pending AI approvals","Return unresolved approval proposals with exact argument fingerprints.",object_schema({}),AiToolRisk::ReadOnly,true},
        [this](const JsonValue&){JsonValue::Array rows;for(const auto& pending:registry_.approvals().pending())if(!pending.resolved)rows.emplace_back(JsonValue::Object{{"id",pending.id},{"tool",pending.tool},{"actor",pending.actor},{"risk",std::string(to_string(pending.risk))},{"summary",pending.summary},{"arguments",pending.arguments},{"arguments_fnv1a64",to_hex(fnv1a64(stringify_json(pending.arguments,false)))}});return AiToolCallResult{AiCallStatus::Completed,JsonValue::Object{{"approvals",std::move(rows)}},"pending approvals read",{}};});
}

void DveAiBridge::register_patch_tools(){
    const JsonValue patchSchema=object_schema({
        {"patch",string_schema("Unified diff containing one or more project-relative text-file changes")},
        {"base_hashes",string_map_schema("Exact FNV-1a 64-bit hashes keyed by every touched project-relative path")}},
        {"patch","base_hashes"});
    registry_.add({"dve.project.preview_patch","Preview source patch",
        "Validate a unified diff against exact base hashes and return changed-file and line summaries without writing.",
        patchSchema,AiToolRisk::ReadOnly,true},
        [this](const JsonValue& args){
            const auto* patch=args.find("patch");const auto* hashes=args.find("base_hashes");
            if(!patch||!patch->is_string()||!hashes||!hashes->is_object())return AiToolCallResult{AiCallStatus::InvalidArguments,{},"patch and base_hashes are required",{}};
            std::map<std::string,std::string,std::less<>> values;for(const auto& [path,value]:hashes->as_object())if(value.is_string())values.emplace(path,std::string(value.as_string()));
            const auto result=preview_project_patch(root_,patch->as_string(),values,{options_.maximumWriteBytes*4U,options_.maximumWriteBytes,64});
            return result.ok?AiToolCallResult{AiCallStatus::Completed,result.content,"patch preview validated",{}}:AiToolCallResult{AiCallStatus::InvalidArguments,result.content,result.error,{}};
        });
    registry_.add({"dve.project.apply_patch","Apply source patch",
        "Apply a validated unified diff transactionally across multiple files and retain a byte-exact rollback bundle.",
        patchSchema,AiToolRisk::Mutating,false},
        [this](const JsonValue& args){
            if(!options_.allowSourceWrites)return AiToolCallResult{AiCallStatus::Denied,{},"source writes are disabled",{}};
            const auto* patch=args.find("patch");const auto* hashes=args.find("base_hashes");
            if(!patch||!patch->is_string()||!hashes||!hashes->is_object())return AiToolCallResult{AiCallStatus::InvalidArguments,{},"patch and base_hashes are required",{}};
            std::map<std::string,std::string,std::less<>> values;for(const auto& [path,value]:hashes->as_object())if(value.is_string())values.emplace(path,std::string(value.as_string()));
            const auto result=apply_project_patch(root_,patch->as_string(),values,{options_.maximumWriteBytes*4U,options_.maximumWriteBytes,64});
            return result.ok?AiToolCallResult{AiCallStatus::Completed,result.content,"patch transaction committed",{}}:AiToolCallResult{AiCallStatus::Failed,result.content,result.error,{}};
        });
    registry_.add({"dve.project.rollback_patch","Rollback source patch",
        "Restore byte-exact originals from one AI patch rollback bundle, refusing if any patched file changed afterward.",
        object_schema({{"transaction_id",string_schema("Transaction ID returned by dve.project.apply_patch")}}, {"transaction_id"}),
        AiToolRisk::Destructive,false},
        [this](const JsonValue& args){const auto* id=args.find("transaction_id");if(!id||!id->is_string())return AiToolCallResult{AiCallStatus::InvalidArguments,{},"transaction_id is required",{}};const auto result=rollback_project_patch(root_,id->as_string());return result.ok?AiToolCallResult{AiCallStatus::Completed,result.content,"patch transaction rolled back",{}}:AiToolCallResult{AiCallStatus::Failed,result.content,result.error,{}};});
}
void DveAiBridge::register_task_tools(){
    registry_.add({"dve.project.list_validation_tasks","List validation tasks",
        "List the fixed project validation tasks available to the assistant. No arbitrary executable or arguments are accepted.",
        object_schema({}),AiToolRisk::ReadOnly,true},
        [this](const JsonValue&){return AiToolCallResult{AiCallStatus::Completed,taskRunner_?taskRunner_->list_json():JsonValue::Object{{"tasks",JsonValue::Array{}}},"validation tasks listed",{}};});
    registry_.add({"dve.project.run_validation_task","Run validation task",
        "Run one named allowlisted project task with a fixed executable, fixed arguments, project-root confinement, deadline, and bounded combined log.",
        object_schema({{"name",string_schema("Exact task name from dve.project.list_validation_tasks")}}, {"name"}),AiToolRisk::Mutating,false},
        [this](const JsonValue& args){const auto* name=args.find("name");if(!name||!name->is_string())return AiToolCallResult{AiCallStatus::InvalidArguments,{},"validation task name is required",{}};if(!taskRunner_||!taskRunner_->find(name->as_string()))return AiToolCallResult{AiCallStatus::InvalidArguments,{},"unknown validation task",{}};const auto result=taskRunner_->run(name->as_string());JsonValue content=JsonValue::Object{{"name",std::string(name->as_string())},{"launched",result.launched},{"succeeded",result.succeeded},{"timed_out",result.timedOut},{"exit_code",result.exitCode},{"duration_ms",static_cast<double>(result.duration.count())},{"output",result.output},{"output_truncated",result.outputTruncated}};return result.succeeded?AiToolCallResult{AiCallStatus::Completed,std::move(content),"validation task passed",{}}:AiToolCallResult{AiCallStatus::Failed,std::move(content),result.error.empty()?"validation task failed":result.error,{}};});
}

void DveAiBridge::register_text3d_tools(){
    registry_.add({"dve.project.cook_text3d","Cook 3D text",
        "Cook a project-local glyf-based TrueType font and UTF-8 text into a Slug-backed .dtext asset with extruded side geometry and an optional deterministic preview.",
        object_schema({{"font_path",string_schema("Project-relative .ttf font path")},
                       {"text",string_schema("UTF-8 text")},
                       {"output_path",string_schema("Project-relative .dtext output path")},
                       {"preview_path",string_schema("Optional project-relative .ppm preview path")},
                       {"size_meters",number_schema("Em size in meters")},
                       {"depth_meters",number_schema("Extrusion depth in meters")},
                       {"letter_spacing_em",number_schema("Additional spacing in em units")},
                       {"face_material_id",number_schema("Positive face material ID")},
                       {"side_material_id",number_schema("Positive extrusion-side material ID")},
                       {"alignment",string_schema("left, center, or right")},
                       {"fill_rule",string_schema("nonzero or evenodd")},
                       {"bands",number_schema("Horizontal and vertical Slug bands, 1-255")}},
                      {"font_path","text","output_path"}),AiToolRisk::Mutating,false},
        [this](const JsonValue& args){
            std::string error;
            const auto* fontValue=args.find("font_path");const auto* textValue=args.find("text");const auto* outputValue=args.find("output_path");
            if(!fontValue||!fontValue->is_string()||!textValue||!textValue->is_string()||!outputValue||!outputValue->is_string())
                return AiToolCallResult{AiCallStatus::InvalidArguments,{},"font_path, text, and output_path are required",{}};
            auto font=resolve_project_path(fontValue->as_string(),false,&error);if(!font)return AiToolCallResult{AiCallStatus::InvalidArguments,{},error,{}};
            auto output=resolve_project_path(outputValue->as_string(),true,&error);if(!output)return AiToolCallResult{AiCallStatus::InvalidArguments,{},error,{}};
            if(font->extension()!=".ttf")return AiToolCallResult{AiCallStatus::InvalidArguments,{},"only glyf-based .ttf fonts are supported",{}};
            if(output->extension()!=".dtext")return AiToolCallResult{AiCallStatus::InvalidArguments,{},"output_path must end in .dtext",{}};
            Text3DCookOptions options;
            if(const auto* value=args.find("size_meters")){const double n=value->as_number(1.0);if(!std::isfinite(n)||n<=0.0||n>1000.0)return AiToolCallResult{AiCallStatus::InvalidArguments,{},"size_meters is invalid",{}};options.style.emSizeMeters=static_cast<float>(n);}
            if(const auto* value=args.find("depth_meters")){const double n=value->as_number(0.12);if(!std::isfinite(n)||n<0.0||n>1000.0)return AiToolCallResult{AiCallStatus::InvalidArguments,{},"depth_meters is invalid",{}};options.style.extrusionDepthMeters=static_cast<float>(n);}
            if(const auto* value=args.find("letter_spacing_em")){const double n=value->as_number(0.0);if(!std::isfinite(n)||n<-2.0||n>10.0)return AiToolCallResult{AiCallStatus::InvalidArguments,{},"letter_spacing_em is invalid",{}};options.style.letterSpacingEm=static_cast<float>(n);}
            if(const auto* value=args.find("face_material_id")){const double n=value->as_number(1.0);if(!std::isfinite(n)||n<1.0||n>4294967295.0||std::floor(n)!=n)return AiToolCallResult{AiCallStatus::InvalidArguments,{},"face_material_id is invalid",{}};options.style.faceMaterialId=static_cast<std::uint32_t>(n);}
            if(const auto* value=args.find("side_material_id")){const double n=value->as_number(2.0);if(!std::isfinite(n)||n<1.0||n>4294967295.0||std::floor(n)!=n)return AiToolCallResult{AiCallStatus::InvalidArguments,{},"side_material_id is invalid",{}};options.style.sideMaterialId=static_cast<std::uint32_t>(n);}
            if(const auto* value=args.find("alignment");value&&value->is_string()){const std::string v=lowercase(value->as_string());if(v=="left")options.style.alignment=Text3DHorizontalAlignment::Left;else if(v=="center"||v=="centre")options.style.alignment=Text3DHorizontalAlignment::Center;else if(v=="right")options.style.alignment=Text3DHorizontalAlignment::Right;else return AiToolCallResult{AiCallStatus::InvalidArguments,{},"alignment must be left, center, or right",{}};}
            if(const auto* value=args.find("fill_rule");value&&value->is_string()){const std::string v=lowercase(value->as_string());if(v=="nonzero"||v=="non-zero")options.style.fillRule=Text3DFillRule::NonZero;else if(v=="evenodd"||v=="even-odd")options.style.fillRule=Text3DFillRule::EvenOdd;else return AiToolCallResult{AiCallStatus::InvalidArguments,{},"fill_rule must be nonzero or evenodd",{}};}
            if(const auto* value=args.find("bands")){const double n=value->as_number(8.0);if(!std::isfinite(n)||n<1.0||n>255.0||std::floor(n)!=n)return AiToolCallResult{AiCallStatus::InvalidArguments,{},"bands must be an integer from 1 to 255",{}};options.style.horizontalBands=options.style.verticalBands=static_cast<std::uint16_t>(n);}
            const auto cooked=cook_text3d(*font,textValue->as_string(),options);if(!cooked)return AiToolCallResult{AiCallStatus::Failed,{},cooked.error,{}};
            std::filesystem::create_directories(output->parent_path());if(!write_dtext(*output,cooked.asset,&error))return AiToolCallResult{AiCallStatus::Failed,{},error,{}};
            std::string previewRelative;
            if(const auto* value=args.find("preview_path");value&&value->is_string()&&!value->as_string().empty()){auto preview=resolve_project_path(value->as_string(),true,&error);if(!preview)return AiToolCallResult{AiCallStatus::InvalidArguments,{},error,{}};if(preview->extension()!=".ppm")return AiToolCallResult{AiCallStatus::InvalidArguments,{},"preview_path must end in .ppm",{}};std::filesystem::create_directories(preview->parent_path());if(!write_text3d_preview_ppm(*preview,cooked.asset,{},&error))return AiToolCallResult{AiCallStatus::Failed,{},error,{}};previewRelative=std::filesystem::relative(*preview,root_).generic_string();}
            return AiToolCallResult{AiCallStatus::Completed,JsonValue::Object{{"output_path",std::filesystem::relative(*output,root_).generic_string()},{"preview_path",previewRelative},{"glyph_instances",static_cast<double>(cooked.asset.glyphInstances.size())},{"curves",static_cast<double>(cooked.asset.atlas.curveTexels.size()/2U)},{"side_triangles",static_cast<double>(cooked.asset.sideMesh.indices.size()/3U)},{"content_hash",to_hex(cooked.asset.contentHash)}},"3D text asset cooked",{}};
        });
}


void DveAiBridge::register_gabor_tools(){
    registry_.add({"dve.project.cook_gabor","Cook Gabor volume",
        "Import a project-local Gabor Fields PLY or pyramid directory, or validate an existing .dgabor asset, then write a native .dgabor asset and optional deterministic preview.",
        object_schema({{"source_path",string_schema("Project-relative .ply, .dgabor, or pyramid directory")},
                       {"output_path",string_schema("Project-relative .dgabor output path")},
                       {"preview_path",string_schema("Optional project-relative .ppm preview path")},
                       {"density_normalization",number_schema("Nonnegative density multiplier")},
                       {"position_scale",number_schema("Positive imported position scale")},
                       {"opacity_scale",number_schema("Nonnegative imported opacity scale")},
                       {"maximum_primitives",number_schema("Maximum imported primitives, 1-1000000")}},
                      {"source_path","output_path"}),AiToolRisk::Mutating,false},
        [this](const JsonValue& args){
            std::string error;
            const auto* sourceValue=args.find("source_path"); const auto* outputValue=args.find("output_path");
            if(!sourceValue||!sourceValue->is_string()||!outputValue||!outputValue->is_string())
                return AiToolCallResult{AiCallStatus::InvalidArguments,{},"source_path and output_path are required",{}};
            auto source=resolve_project_path(sourceValue->as_string(),false,&error);
            if(!source)return AiToolCallResult{AiCallStatus::InvalidArguments,{},error,{}};
            auto output=resolve_project_path(outputValue->as_string(),true,&error);
            if(!output)return AiToolCallResult{AiCallStatus::InvalidArguments,{},error,{}};
            if(output->extension()!=".dgabor")return AiToolCallResult{AiCallStatus::InvalidArguments,{},"output_path must end in .dgabor",{}};
            GaborPlyImportOptions options;
            if(const auto* v=args.find("density_normalization")){const double n=v->as_number(1.0);if(!std::isfinite(n)||n<0.0||n>1000000.0)return AiToolCallResult{AiCallStatus::InvalidArguments,{},"density_normalization is invalid",{}};options.densityNormalization=static_cast<float>(n);}
            if(const auto* v=args.find("position_scale")){const double n=v->as_number(1.0);if(!std::isfinite(n)||n<=0.0||n>1000000.0)return AiToolCallResult{AiCallStatus::InvalidArguments,{},"position_scale is invalid",{}};options.positionScale=static_cast<float>(n);}
            if(const auto* v=args.find("opacity_scale")){const double n=v->as_number(1.0);if(!std::isfinite(n)||n<0.0||n>1000000.0)return AiToolCallResult{AiCallStatus::InvalidArguments,{},"opacity_scale is invalid",{}};options.opacityScale=static_cast<float>(n);}
            if(const auto* v=args.find("maximum_primitives")){const double n=v->as_number(1000000.0);if(!std::isfinite(n)||n<1.0||n>1000000.0||std::floor(n)!=n)return AiToolCallResult{AiCallStatus::InvalidArguments,{},"maximum_primitives must be an integer from 1 to 1000000",{}};options.maximumPrimitives=static_cast<std::size_t>(n);}
            GaborImportResult imported;
            if(std::filesystem::is_directory(*source)) imported=import_gabor_pyramid(*source,options);
            else if(source->extension()==".ply") imported=import_gabor_ply(*source,options);
            else if(source->extension()==".dgabor") imported=read_dgabor(*source);
            else return AiToolCallResult{AiCallStatus::InvalidArguments,{},"source_path must be .ply, .dgabor, or a pyramid directory",{}};
            if(!imported)return AiToolCallResult{AiCallStatus::Failed,{},imported.error,{}};
            std::filesystem::create_directories(output->parent_path());
            if(!write_dgabor(*output,*imported.asset,&error))return AiToolCallResult{AiCallStatus::Failed,{},error,{}};
            std::string previewRelative;
            if(const auto* v=args.find("preview_path");v&&v->is_string()&&!v->as_string().empty()){
                auto preview=resolve_project_path(v->as_string(),true,&error);if(!preview)return AiToolCallResult{AiCallStatus::InvalidArguments,{},error,{}};
                if(preview->extension()!=".ppm")return AiToolCallResult{AiCallStatus::InvalidArguments,{},"preview_path must end in .ppm",{}};
                std::filesystem::create_directories(preview->parent_path());
                if(!render_gabor_preview_ppm(*preview,*imported.asset,{},&error))return AiToolCallResult{AiCallStatus::Failed,{},error,{}};
                previewRelative=std::filesystem::relative(*preview,root_).generic_string();
            }
            std::uint16_t maximumLod=0;for(const auto& primitive:imported.asset->primitives)maximumLod=std::max(maximumLod,primitive.lodLevel);
            JsonValue::Array warnings;for(const auto& warning:imported.warnings)warnings.emplace_back(warning);
            return AiToolCallResult{AiCallStatus::Completed,JsonValue::Object{
                {"output_path",std::filesystem::relative(*output,root_).generic_string()},
                {"preview_path",previewRelative},{"primitives",static_cast<double>(imported.asset->primitives.size())},
                {"lod_levels",static_cast<double>(maximumLod+1U)},{"content_hash",to_hex(imported.asset->contentHash)},
                {"warnings",std::move(warnings)}},"Gabor volume cooked",{}};
        });
}


#if defined(DVE_ENABLE_FLUODDITY)
void DveAiBridge::register_fluoddity_tools(){
    registry_.add({"dve.project.cook_fluoddity","Cook Fluoddity rule",
        "Import a project-local Fluoddity version-7 JSON preset or validate an existing .dfluoddity asset, then write a deterministic native .dfluoddity rule asset.",
        object_schema({{"source_path",string_schema("Project-relative .json or .dfluoddity source path")},
                       {"output_path",string_schema("Project-relative .dfluoddity output path")}},
                      {"source_path","output_path"}),AiToolRisk::Mutating,false},
        [this](const JsonValue& args){
            std::string error;
            const auto* sourceValue=args.find("source_path");
            const auto* outputValue=args.find("output_path");
            if(!sourceValue||!sourceValue->is_string()||!outputValue||!outputValue->is_string())
                return AiToolCallResult{AiCallStatus::InvalidArguments,{},"source_path and output_path are required",{}};
            auto source=resolve_project_path(sourceValue->as_string(),false,&error);
            if(!source)return AiToolCallResult{AiCallStatus::InvalidArguments,{},error,{}};
            auto output=resolve_project_path(outputValue->as_string(),true,&error);
            if(!output)return AiToolCallResult{AiCallStatus::InvalidArguments,{},error,{}};
            if(output->extension()!=".dfluoddity")
                return AiToolCallResult{AiCallStatus::InvalidArguments,{},"output_path must end in .dfluoddity",{}};

            FluoddityImportResult imported;
            if(source->extension()==".json") imported=import_fluoddity_preset_json(*source);
            else if(source->extension()==".dfluoddity") imported=read_dfluoddity(*source);
            else return AiToolCallResult{AiCallStatus::InvalidArguments,{},"source_path must end in .json or .dfluoddity",{}};
            if(!imported)return AiToolCallResult{AiCallStatus::Failed,{},imported.error,{}};

            if(source->extension()==".json") {
                imported.asset->sourcePreset = std::filesystem::relative(*source,root_).generic_string();
                imported.asset->recompute_hash();
            }
            std::filesystem::create_directories(output->parent_path());
            if(!write_dfluoddity(*output,*imported.asset,&error))
                return AiToolCallResult{AiCallStatus::Failed,{},error,{}};
            const FluoddityImportResult verified=read_dfluoddity(*output);
            if(!verified||verified.asset->contentHash!=imported.asset->contentHash)
                return AiToolCallResult{AiCallStatus::Failed,{},verified.error.empty()?"post-write verification failed":verified.error,{}};

            JsonValue::Array warnings;
            for(const std::string& warning:imported.warnings) warnings.emplace_back(warning);
            const auto memory_json=[](const FluoddityMemoryEstimate& memory){
                return JsonValue::Object{{"particle_bytes",static_cast<double>(memory.particleBytes)},
                                         {"trail_bytes",static_cast<double>(memory.trailBytes)},
                                         {"accumulation_bytes",static_cast<double>(memory.accumulationBytes)},
                                         {"support_bytes",static_cast<double>(memory.supportingBytes)},
                                         {"total_bytes",static_cast<double>(memory.totalBytes)}};
            };
            return AiToolCallResult{AiCallStatus::Completed,JsonValue::Object{
                {"output_path",std::filesystem::relative(*output,root_).generic_string()},
                {"name",imported.asset->name},
                {"source_version",static_cast<double>(imported.asset->sourceVersion)},
                {"cohorts",static_cast<double>(imported.asset->cohortCount)},
                {"rule_floats",static_cast<double>(kFluoddityRuleFloatCount)},
                {"content_hash",to_hex(imported.asset->contentHash)},
                {"trail_semantics","velocity_rgb_alpha_zero"},
                {"low_memory",memory_json(estimate_fluoddity_memory(fluoddity_quality_profile(FluoddityQuality::Low),imported.asset->cohortCount))},
                {"medium_memory",memory_json(estimate_fluoddity_memory(fluoddity_quality_profile(FluoddityQuality::Medium),imported.asset->cohortCount))},
                {"high_memory",memory_json(estimate_fluoddity_memory(fluoddity_quality_profile(FluoddityQuality::High),imported.asset->cohortCount))},
                {"warnings",std::move(warnings)}},"Fluoddity rule asset cooked",{}};
        });
}
#endif

void DveAiBridge::register_scene_tools(){
    registry_.add({"dve.scene.list_objects","List scene objects","Return live GameWorld object IDs, names, geometry kinds, positions, and voxel counts",object_schema({}),AiToolRisk::ReadOnly,true},
        [this](const JsonValue&){if(!world_)return AiToolCallResult{AiCallStatus::Failed,{},"no GameWorld is attached",{}};JsonValue::Array objects;for(const auto id:world_->object_ids()){const auto kind=world_->geometry_kind(id);const auto transform=world_->transform(id);JsonValue::Object row{{"id",static_cast<double>(id)},{"name",world_->name_of(id)}};if(kind)row.emplace("geometry",*kind==GameGeometryKind::Voxel?"voxel":*kind==GameGeometryKind::Polygon?"polygon":"marker");if(transform){row.emplace("position",vec3_json(transform->position));row.emplace("rotation",quat_json(transform->rotation));}if(const auto count=world_->voxel_count(id))row.emplace("voxel_count",static_cast<double>(*count));objects.emplace_back(std::move(row));}return AiToolCallResult{AiCallStatus::Completed,JsonValue::Object{{"objects",std::move(objects)}},"scene objects listed",{}};});
    registry_.add({"dve.scene.inspect_object","Inspect scene object","Inspect one live object by exact numeric ID",object_schema({{"id",number_schema("GameObjectId")}}, {"id"}),AiToolRisk::ReadOnly,true},
        [this](const JsonValue& args){if(!world_)return AiToolCallResult{AiCallStatus::Failed,{},"no GameWorld is attached",{}};const auto id=exact_u64(args.find("id"));if(!id||!world_->has_object(*id))return AiToolCallResult{AiCallStatus::InvalidArguments,{},"unknown or invalid object id",{}};const auto t=world_->transform(*id);JsonValue::Object row{{"id",static_cast<double>(*id)},{"name",world_->name_of(*id)}};if(t){row.emplace("position",vec3_json(t->position));row.emplace("rotation",quat_json(t->rotation));}if(const auto v=world_->linear_velocity(*id))row.emplace("linear_velocity",vec3_json(*v));if(const auto c=world_->voxel_count(*id))row.emplace("voxel_count",static_cast<double>(*c));return AiToolCallResult{AiCallStatus::Completed,std::move(row),"scene object inspected",{}};});
    registry_.add({"dve.scene.set_transform","Set scene transform","Set position and/or rotation of a movable live scene object",object_schema({{"id",number_schema("GameObjectId")},{"position",vec3_schema("World position")},{"rotation",quat_schema()}},{"id"}),AiToolRisk::Mutating,false},
        [this](const JsonValue& args){if(!world_)return AiToolCallResult{AiCallStatus::Failed,{},"no GameWorld is attached",{}};const auto id=exact_u64(args.find("id"));if(!id||!world_->has_object(*id))return AiToolCallResult{AiCallStatus::InvalidArguments,{},"unknown object id",{}};bool changed=false;if(const auto* p=args.find("position")){auto value=read_vec3(p);if(!value)return AiToolCallResult{AiCallStatus::InvalidArguments,{},"position must contain three finite numbers",{}};if(!world_->set_position(*id,*value))return AiToolCallResult{AiCallStatus::Failed,{},"object position is not movable",{}};changed=true;}if(const auto* r=args.find("rotation")){auto value=read_quat(r);if(!value)return AiToolCallResult{AiCallStatus::InvalidArguments,{},"rotation must be [x,y,z,w] finite numbers",{}};if(!world_->set_rotation(*id,*value))return AiToolCallResult{AiCallStatus::Failed,{},"object rotation is not movable",{}};changed=true;}if(!changed)return AiToolCallResult{AiCallStatus::InvalidArguments,{},"position or rotation is required",{}};const auto t=world_->transform(*id);return AiToolCallResult{AiCallStatus::Completed,JsonValue::Object{{"id",static_cast<double>(*id)},{"position",vec3_json(t->position)},{"rotation",quat_json(t->rotation)}},"scene transform changed",{}};});
    registry_.add({"dve.scene.spawn_asset","Spawn scene asset","Spawn a project-local .dvox or .dmesh asset into the live world",object_schema({{"path",string_schema("Project-relative .dvox or .dmesh path")},{"name",string_schema()},{"position",vec3_schema("World position")},{"dynamic",bool_schema()},{"structural",bool_schema()}},{"path"}),AiToolRisk::Mutating,false},
        [this](const JsonValue& args){if(!world_)return AiToolCallResult{AiCallStatus::Failed,{},"no GameWorld is attached",{}};std::string error;auto path=resolve_project_path(args.find("path")?args.find("path")->as_string():"",false,&error);if(!path)return AiToolCallResult{AiCallStatus::InvalidArguments,{},error,{}};const auto ext=path->extension().string();if(ext!=".dvox"&&ext!=".dmesh")return AiToolCallResult{AiCallStatus::InvalidArguments,{},"only .dvox and .dmesh assets may be spawned",{}};RigidTransform t{};if(const auto* p=args.find("position")){auto value=read_vec3(p);if(!value)return AiToolCallResult{AiCallStatus::InvalidArguments,{},"invalid position",{}};t.position=*value;}const std::string name=std::string(args.find("name")?args.find("name")->as_string():"");const bool dynamic=args.find("dynamic")?args.find("dynamic")->as_bool(true):true;const bool structural=args.find("structural")?args.find("structural")->as_bool(true):true;const auto id=world_->spawn_asset(*path,name,t,dynamic,structural,&error);if(id==kInvalidGameObjectId)return AiToolCallResult{AiCallStatus::Failed,{},error.empty()?"spawn failed":error,{}};return AiToolCallResult{AiCallStatus::Completed,JsonValue::Object{{"id",static_cast<double>(id)},{"name",world_->name_of(id)}},"scene asset spawned",{}};});
    registry_.add({"dve.scene.delete_object","Delete scene object","Permanently remove one live object. This action is destructive and always approval-gated.",object_schema({{"id",number_schema("GameObjectId")}}, {"id"}),AiToolRisk::Destructive,false},
        [this](const JsonValue& args){if(!world_)return AiToolCallResult{AiCallStatus::Failed,{},"no GameWorld is attached",{}};const auto id=exact_u64(args.find("id"));if(!id||!world_->destroy_object(*id))return AiToolCallResult{AiCallStatus::InvalidArguments,{},"unknown object id",{}};return AiToolCallResult{AiCallStatus::Completed,JsonValue::Object{{"id",static_cast<double>(*id)}},"scene object deleted",{}};});
}

} // namespace dve::ai
