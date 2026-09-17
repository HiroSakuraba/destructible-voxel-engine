#include "dve/ai/project_patch.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

#include "dve/ai/assistant.hpp"

namespace dve::ai {
namespace {

struct TextLines {
    std::vector<std::string> lines;
    std::string newline{"\n"};
    bool finalNewline{};
};
struct HunkLine { char operation{}; std::string text; bool noNewline{}; };
struct Hunk { std::size_t oldStart{}; std::size_t oldCount{}; std::size_t newStart{}; std::size_t newCount{}; std::vector<HunkLine> lines; };
struct FilePatch { std::string oldPath; std::string newPath; std::vector<Hunk> hunks; };
struct PreparedFile {
    std::string path;
    bool existed{};
    bool remove{};
    std::string before;
    std::string after;
    std::string beforeHash;
    std::string afterHash;
    std::size_t added{};
    std::size_t removed{};
};

bool is_within(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    auto a=root.begin(), b=candidate.begin();
    for(;a!=root.end();++a,++b) if(b==candidate.end()||*a!=*b) return false;
    return true;
}

bool allowed_text_extension(const std::filesystem::path& path) {
    static constexpr std::array<std::string_view, 25> allowed{
        ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl",
        ".cmake", ".txt", ".md", ".json", ".lua", ".py", ".sh", ".bat", ".ps1",
        ".hlsl", ".hlsli", ".glsl", ".vert", ".frag", ".yaml", ".yml"};
    const auto ext=path.extension().string();
    return std::find(allowed.begin(),allowed.end(),ext)!=allowed.end()||path.filename()=="CMakeLists.txt";
}

std::optional<std::filesystem::path> resolve_path(const std::filesystem::path& root, std::string_view relative,
                                                  bool writing, std::string* error) {
    if(relative.empty()||relative=="/dev/null") { if(error)*error="patch path is missing"; return std::nullopt; }
    std::filesystem::path rel(relative);
    if(rel.is_absolute()) { if(error)*error="absolute patch paths are not permitted"; return std::nullopt; }
    std::error_code ec;
    auto normalized=std::filesystem::weakly_canonical(root/rel,ec);
    if(ec&&writing) {
        ec.clear();
        auto parent=std::filesystem::weakly_canonical((root/rel).parent_path(),ec);
        if(!ec) normalized=parent/rel.filename();
    }
    if(ec||!is_within(root,normalized)) { if(error)*error="patch path escapes the project root or cannot be resolved"; return std::nullopt; }
    return normalized;
}

std::vector<std::string> split_patch_lines(std::string_view text) {
    std::vector<std::string> result;
    std::size_t begin=0;
    while(begin<text.size()) {
        const auto end=text.find('\n',begin);
        const auto stop=end==std::string_view::npos?text.size():end;
        std::string line(text.substr(begin,stop-begin));
        if(!line.empty()&&line.back()=='\r') line.pop_back();
        result.push_back(std::move(line));
        if(end==std::string_view::npos) break;
        begin=end+1;
    }
    return result;
}

TextLines split_text(std::string_view text, std::string* error) {
    TextLines result;
    const bool hasCrLf=text.find("\r\n")!=std::string_view::npos;
    std::string withoutCrLf(text);
    if(hasCrLf) {
        std::size_t pos=0;
        while((pos=withoutCrLf.find("\r\n",pos))!=std::string::npos) { withoutCrLf.replace(pos,2,"\n"); ++pos; }
        if(withoutCrLf.find('\r')!=std::string::npos) { if(error)*error="mixed or unsupported line endings"; return {}; }
        result.newline="\r\n";
    } else if(text.find('\r')!=std::string_view::npos) {
        if(error)*error="classic Mac line endings are not supported";
        return {};
    }
    result.finalNewline=!withoutCrLf.empty()&&withoutCrLf.back()=='\n';
    std::size_t begin=0;
    while(begin<withoutCrLf.size()) {
        const auto end=withoutCrLf.find('\n',begin);
        if(end==std::string::npos) { result.lines.emplace_back(withoutCrLf.substr(begin)); break; }
        result.lines.emplace_back(withoutCrLf.substr(begin,end-begin));
        begin=end+1;
    }
    if(result.finalNewline&&!result.lines.empty()&&result.lines.back().empty()) result.lines.pop_back();
    return result;
}

std::string join_text(const TextLines& value) {
    std::string result;
    for(std::size_t i=0;i<value.lines.size();++i) {
        if(i) result+=value.newline;
        result+=value.lines[i];
    }
    if(value.finalNewline&&!value.lines.empty()) result+=value.newline;
    return result;
}

std::string clean_diff_path(std::string line, std::string_view prefix) {
    if(!line.starts_with(prefix)) return {};
    line.erase(0,prefix.size());
    const auto tab=line.find('\t'); if(tab!=std::string::npos) line.resize(tab);
    const auto space=line.find(' '); if(space!=std::string::npos) line.resize(space);
    if(line.starts_with("a/")||line.starts_with("b/")) line.erase(0,2);
    return line;
}

bool parse_range(std::string_view text, std::size_t* start, std::size_t* count) {
    if(text.empty()) return false;
    const auto comma=text.find(',');
    try {
        *start=static_cast<std::size_t>(std::stoull(std::string(text.substr(0,comma))));
        *count=comma==std::string_view::npos?1U:static_cast<std::size_t>(std::stoull(std::string(text.substr(comma+1))));
    } catch(...) { return false; }
    return true;
}

bool parse_hunk_header(std::string_view line,Hunk* hunk) {
    if(!line.starts_with("@@ -")) return false;
    const auto oldEnd=line.find(' ',4); if(oldEnd==std::string_view::npos) return false;
    const auto plus=line.find('+',oldEnd); if(plus==std::string_view::npos) return false;
    const auto newEnd=line.find(' ',plus); if(newEnd==std::string_view::npos) return false;
    return parse_range(line.substr(4,oldEnd-4),&hunk->oldStart,&hunk->oldCount)&&
           parse_range(line.substr(plus+1,newEnd-plus-1),&hunk->newStart,&hunk->newCount);
}

std::optional<std::vector<FilePatch>> parse_patch(std::string_view patch,std::string* error) {
    const auto lines=split_patch_lines(patch);
    std::vector<FilePatch> files;
    std::size_t i=0;
    while(i<lines.size()) {
        while(i<lines.size()&&!lines[i].starts_with("--- ")) ++i;
        if(i==lines.size()) break;
        FilePatch file;
        file.oldPath=clean_diff_path(lines[i],"--- ");
        if(++i>=lines.size()||!lines[i].starts_with("+++ ")) { if(error)*error="missing +++ file header"; return std::nullopt; }
        file.newPath=clean_diff_path(lines[i],"+++ ");
        ++i;
        while(i<lines.size()&&!lines[i].starts_with("--- ")) {
            if(lines[i].empty()) { ++i; continue; }
            Hunk hunk;
            if(!parse_hunk_header(lines[i],&hunk)) { if(error)*error="unexpected line outside a patch hunk: "+lines[i]; return std::nullopt; }
            ++i;
            std::size_t oldSeen=0,newSeen=0;
            while(i<lines.size()&&!lines[i].starts_with("@@ -")&&!lines[i].starts_with("--- ")) {
                if(lines[i]=="\\ No newline at end of file") {
                    if(hunk.lines.empty()) { if(error)*error="newline marker has no preceding patch line"; return std::nullopt; }
                    hunk.lines.back().noNewline=true; ++i; continue;
                }
                if(lines[i].empty()) { if(error)*error="empty patch line is missing a context marker"; return std::nullopt; }
                const char op=lines[i][0];
                if(op!=' '&&op!='+'&&op!='-') { if(error)*error="invalid patch line marker"; return std::nullopt; }
                hunk.lines.push_back({op,lines[i].substr(1),false});
                if(op!='+') ++oldSeen;
                if(op!='-') ++newSeen;
                ++i;
            }
            if(oldSeen!=hunk.oldCount||newSeen!=hunk.newCount) { if(error)*error="hunk line counts do not match its header"; return std::nullopt; }
            file.hunks.push_back(std::move(hunk));
        }
        if(file.hunks.empty()) { if(error)*error="file patch contains no hunks"; return std::nullopt; }
        if(file.oldPath.empty()||file.newPath.empty()) { if(error)*error="invalid file header path"; return std::nullopt; }
        if(file.oldPath=="/dev/null"&&file.newPath=="/dev/null") { if(error)*error="both patch paths are /dev/null"; return std::nullopt; }
        if(file.oldPath!="/dev/null"&&file.newPath!="/dev/null"&&file.oldPath!=file.newPath) { if(error)*error="rename patches are not supported; use delete plus create"; return std::nullopt; }
        files.push_back(std::move(file));
    }
    if(files.empty()) { if(error)*error="patch contains no file sections"; return std::nullopt; }
    return files;
}

std::optional<TextLines> apply_hunks(const TextLines& original,const FilePatch& patch,std::size_t* added,std::size_t* removed,std::string* error) {
    TextLines result; result.newline=original.newline; result.finalNewline=original.finalNewline;
    std::size_t source=0;
    for(const auto& hunk:patch.hunks) {
        const std::size_t target=hunk.oldStart==0?0:hunk.oldStart-1;
        if(target<source||target>original.lines.size()) { if(error)*error="hunk starts outside the source file or overlaps an earlier hunk"; return std::nullopt; }
        result.lines.insert(result.lines.end(),original.lines.begin()+static_cast<std::ptrdiff_t>(source),original.lines.begin()+static_cast<std::ptrdiff_t>(target));
        source=target;
        for(const auto& line:hunk.lines) {
            if(line.operation==' '||line.operation=='-') {
                if(source>=original.lines.size()||original.lines[source]!=line.text) { if(error)*error="patch context does not match the exact source text"; return std::nullopt; }
            }
            if(line.operation==' ') { result.lines.push_back(original.lines[source]); ++source; }
            else if(line.operation=='-') { ++source; ++*removed; }
            else { result.lines.push_back(line.text); ++*added; }
        }
        if(source!=target+hunk.oldCount) { if(error)*error="hunk consumed an unexpected number of source lines"; return std::nullopt; }
        if(target+hunk.oldCount==original.lines.size()) {
            const auto it=std::find_if(hunk.lines.rbegin(),hunk.lines.rend(),[](const HunkLine& l){return l.operation!='-';});
            if(it!=hunk.lines.rend()) result.finalNewline=!it->noNewline;
        }
    }
    result.lines.insert(result.lines.end(),original.lines.begin()+static_cast<std::ptrdiff_t>(source),original.lines.end());
    if(result.lines.empty()) result.finalNewline=false;
    return result;
}

std::optional<std::string> read_file(const std::filesystem::path& path,std::size_t limit,bool* existed,std::string* error) {
    std::error_code ec;
    *existed=std::filesystem::exists(path,ec)&&!ec;
    if(!*existed) return std::string{};
    if(!std::filesystem::is_regular_file(path,ec)||ec) { if(error)*error="patch target is not a regular file"; return std::nullopt; }
    const auto size=std::filesystem::file_size(path,ec);
    if(ec||size>limit) { if(error)*error="patch target exceeds the permitted file size"; return std::nullopt; }
    std::ifstream in(path,std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)),{});
    if(!in.good()&&!in.eof()) { if(error)*error="patch target could not be read"; return std::nullopt; }
    if(text.find('\0')!=std::string::npos) { if(error)*error="binary files cannot be patched"; return std::nullopt; }
    return text;
}

std::optional<std::vector<PreparedFile>> prepare(const std::filesystem::path& root,std::string_view patch,
        const std::map<std::string,std::string,std::less<>>& hashes,ProjectPatchLimits limits,std::string* error) {
    if(patch.size()>limits.maximumPatchBytes) { if(error)*error="patch exceeds the configured byte limit"; return std::nullopt; }
    auto parsed=parse_patch(patch,error); if(!parsed) return std::nullopt;
    if(parsed->size()>limits.maximumFiles) { if(error)*error="patch touches too many files"; return std::nullopt; }
    std::vector<PreparedFile> prepared; prepared.reserve(parsed->size());
    for(const auto& file:*parsed) {
        const bool creating=file.oldPath=="/dev/null";
        const bool deleting=file.newPath=="/dev/null";
        const std::string path=creating?file.newPath:file.oldPath;
        std::string pathError; auto absolute=resolve_path(root,path,true,&pathError);
        if(!absolute) { if(error)*error=path+": "+pathError; return std::nullopt; }
        if(!allowed_text_extension(*absolute)) { if(error)*error=path+": file extension is not in the source/text allowlist"; return std::nullopt; }
        bool existed=false; auto before=read_file(*absolute,limits.maximumFileBytes,&existed,error); if(!before) return std::nullopt;
        if(creating&&existed) { if(error)*error=path+": create patch target already exists"; return std::nullopt; }
        if(!creating&&!existed) { if(error)*error=path+": patch target does not exist"; return std::nullopt; }
        const auto expected=hashes.find(path);
        if(expected==hashes.end()) { if(error)*error=path+": exact base hash is required"; return std::nullopt; }
        const std::string currentHash=to_hex(fnv1a64(*before));
        if(expected->second!=currentHash) { if(error)*error=path+": stale base hash; current hash is "+currentHash; return std::nullopt; }
        std::string splitError; auto original=split_text(*before,&splitError);
        if(!splitError.empty()) { if(error)*error=path+": "+splitError; return std::nullopt; }
        std::size_t added=0,removed=0; auto next=apply_hunks(original,file,&added,&removed,error); if(!next) { if(error&&!error->starts_with(path+":"))*error=path+": "+*error; return std::nullopt; }
        std::string after=deleting?std::string{}:join_text(*next);
        if(after.size()>limits.maximumFileBytes) { if(error)*error=path+": patched file exceeds the configured byte limit"; return std::nullopt; }
        prepared.push_back({path,existed,deleting,std::move(*before),std::move(after),currentHash,{},added,removed});
        prepared.back().afterHash=to_hex(fnv1a64(prepared.back().after));
    }
    std::sort(prepared.begin(),prepared.end(),[](const PreparedFile& a,const PreparedFile& b){return a.path<b.path;});
    for(std::size_t i=1;i<prepared.size();++i) if(prepared[i-1].path==prepared[i].path) { if(error)*error="patch contains duplicate file sections for "+prepared[i].path; return std::nullopt; }
    return prepared;
}

JsonValue summary_json(const std::vector<PreparedFile>& files) {
    JsonValue::Array rows;
    for(const auto& f:files) rows.emplace_back(JsonValue::Object{{"path",f.path},{"operation",f.remove?"delete":f.existed?"modify":"create"},{"before_fnv1a64",f.beforeHash},{"after_fnv1a64",f.afterHash},{"before_bytes",static_cast<double>(f.before.size())},{"after_bytes",static_cast<double>(f.after.size())},{"lines_added",static_cast<double>(f.added)},{"lines_removed",static_cast<double>(f.removed)}});
    return JsonValue::Object{{"files",std::move(rows)},{"file_count",static_cast<double>(files.size())}};
}

bool write_atomic(const std::filesystem::path& path,std::string_view content,std::string_view suffix,std::string* error) {
    std::error_code ec; std::filesystem::create_directories(path.parent_path(),ec);
    if(ec) { if(error)*error="could not create destination directory"; return false; }
    auto temp=path; temp += std::string(".dve-ai-")+std::string(suffix)+".tmp";
    { std::ofstream out(temp,std::ios::binary|std::ios::trunc); out.write(content.data(),static_cast<std::streamsize>(content.size())); out.flush(); if(!out){std::filesystem::remove(temp,ec);if(error)*error="temporary write failed";return false;} }
#if defined(_WIN32)
    if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
        const auto code=GetLastError();std::filesystem::remove(temp,ec);
        if(error)*error="atomic replacement failed with Win32 error "+std::to_string(code);return false;
    }
#else
    std::filesystem::rename(temp,path,ec);
    if(ec){std::filesystem::remove(temp,ec);if(error)*error="atomic replacement failed";return false;}
#endif
    return true;
}

std::string transaction_id(std::string_view patch) {
    static std::atomic_uint64_t counter{1};
    const auto ticks=static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    return "patch-"+to_hex(fnv1a64(patch)).substr(0,12)+"-"+to_hex(ticks).substr(4)+"-"+std::to_string(counter.fetch_add(1));
}

bool restore_files(const std::filesystem::path& root,const std::vector<PreparedFile>& files,std::string_view suffix,std::string* error) {
    for(const auto& f:files) {
        auto path=root/f.path;
        std::error_code ec;
        if(!f.existed) {
            const bool removed=std::filesystem::remove(path,ec);
            if(ec||(std::filesystem::exists(path,ec)&&!removed)){if(error)*error="could not remove transaction-created file "+f.path;return false;}
        } else if(!write_atomic(path,f.before,suffix,error)) return false;
    }
    return true;
}

bool current_matches_before(const std::filesystem::path& root,const PreparedFile& file,std::size_t limit,std::string* error){
    bool exists=false;std::string readError;auto current=read_file(root/file.path,limit,&exists,&readError);
    if(!current){if(error)*error=file.path+": "+readError;return false;}
    if(exists!=file.existed||to_hex(fnv1a64(*current))!=file.beforeHash){
        if(error)*error=file.path+": file changed after patch preview/preparation";
        return false;
    }
    return true;
}

} // namespace

ProjectPatchResult preview_project_patch(const std::filesystem::path& projectRoot,std::string_view patch,
        const std::map<std::string,std::string,std::less<>>& baseHashes,ProjectPatchLimits limits) {
    std::error_code ec; auto root=std::filesystem::weakly_canonical(projectRoot,ec); if(ec) root=std::filesystem::absolute(projectRoot);
    std::string error; auto files=prepare(root,patch,baseHashes,limits,&error);
    if(!files) return {false,{},std::move(error)};
    auto content=summary_json(*files); content["patch_fnv1a64"]=to_hex(fnv1a64(patch)); content["ready_to_apply"]=true;
    return {true,std::move(content),{}};
}

ProjectPatchResult apply_project_patch(const std::filesystem::path& projectRoot,std::string_view patch,
        const std::map<std::string,std::string,std::less<>>& baseHashes,ProjectPatchLimits limits) {
    std::error_code ec; auto root=std::filesystem::weakly_canonical(projectRoot,ec); if(ec) root=std::filesystem::absolute(projectRoot);
    std::string error; auto files=prepare(root,patch,baseHashes,limits,&error);
    if(!files) return {false,{},std::move(error)};
    const std::string id=transaction_id(patch);
    const auto rollbackRoot=root/".dve"/"ai_rollbacks"/id;
    const auto staging=rollbackRoot.string()+".tmp";
    std::filesystem::remove_all(staging,ec); ec.clear(); std::filesystem::create_directories(std::filesystem::path(staging)/"files",ec);
    if(ec) return {false,{},"could not create rollback staging directory"};
    JsonValue::Array manifestFiles;
    for(const auto& f:*files) {
        if(f.existed&&!write_atomic(std::filesystem::path(staging)/"files"/f.path,f.before,"backup",&error)) { std::filesystem::remove_all(staging,ec); return {false,{},"rollback backup failed: "+error}; }
        manifestFiles.emplace_back(JsonValue::Object{{"path",f.path},{"existed",f.existed},{"after_exists",!f.remove},{"before_fnv1a64",f.beforeHash},{"after_fnv1a64",f.afterHash}});
    }
    JsonValue manifest=JsonValue::Object{{"version",2},{"transaction_id",id},{"patch_fnv1a64",to_hex(fnv1a64(patch))},{"files",std::move(manifestFiles)}};
    if(!write_atomic(std::filesystem::path(staging)/"manifest.json",stringify_json(manifest,true),"manifest",&error)) { std::filesystem::remove_all(staging,ec); return {false,{},"rollback manifest failed: "+error}; }
    std::filesystem::create_directories(rollbackRoot.parent_path(),ec); ec.clear(); std::filesystem::rename(staging,rollbackRoot,ec);
    if(ec) { std::filesystem::remove_all(staging,ec); return {false,{},"could not commit rollback bundle"}; }
    std::vector<PreparedFile> applied;
    for(const auto& f:*files) {
        if(!current_matches_before(root,f,limits.maximumFileBytes,&error)){
            std::string restoreError;(void)restore_files(root,applied,id+"-restore",&restoreError);
            return {false,JsonValue::Object{{"transaction_id",id},{"rollback_attempted",!applied.empty()},{"rollback_error",restoreError}},"patch transaction aborted: "+error};
        }
        auto path=root/f.path;bool ok=true;
        if(f.remove){const bool removed=std::filesystem::remove(path,ec);ok=removed&&!ec;ec.clear();if(!ok)error="file deletion failed";}
        else ok=write_atomic(path,f.after,id,&error);
        if(!ok){
            std::string restoreError;(void)restore_files(root,applied,id+"-restore",&restoreError);
            return {false,JsonValue::Object{{"transaction_id",id},{"rollback_attempted",!applied.empty()},{"rollback_error",restoreError}},"patch transaction failed at "+f.path+": "+error};
        }
        applied.push_back(f);
    }
    auto content=summary_json(*files); content["transaction_id"]=id; content["rollback_available"]=true;
    return {true,std::move(content),{}};
}

ProjectPatchResult rollback_project_patch(const std::filesystem::path& projectRoot,std::string_view transactionId) {
    if(transactionId.empty()||transactionId.find('/')!=std::string_view::npos||transactionId.find('\\')!=std::string_view::npos||!transactionId.starts_with("patch-")) return {false,{},"invalid rollback transaction id"};
    std::error_code ec;auto root=std::filesystem::weakly_canonical(projectRoot,ec);if(ec)root=std::filesystem::absolute(projectRoot);
    const auto bundle=root/".dve"/"ai_rollbacks"/std::string(transactionId);
    if(std::filesystem::exists(bundle/"rolled_back",ec))return {false,{},"patch transaction was already rolled back"};
    std::ifstream in(bundle/"manifest.json",std::ios::binary);std::string text((std::istreambuf_iterator<char>(in)),{});
    auto parsed=parse_json(text);if(!parsed.value||!parsed.value->is_object())return {false,{},"rollback manifest is missing or invalid"};
    const auto* manifestId=parsed.value->find("transaction_id");
    if(!manifestId||!manifestId->is_string()||manifestId->as_string()!=transactionId)return {false,{},"rollback manifest transaction ID mismatch"};
    const auto* entries=parsed.value->find("files");if(!entries||!entries->is_array())return {false,{},"rollback manifest has no file list"};
    struct Entry{std::string path;bool existed{};bool afterExists{};std::string beforeHash;std::string afterHash;std::string backup;std::string current;};
    std::vector<Entry> list;list.reserve(entries->as_array().size());
    for(const auto& item:entries->as_array()){
        if(!item.is_object())return {false,{},"rollback manifest entry is invalid"};
        Entry e;
        const auto* path=item.find("path");const auto* existed=item.find("existed");const auto* afterExists=item.find("after_exists");
        const auto* beforeHash=item.find("before_fnv1a64");const auto* afterHash=item.find("after_fnv1a64");
        if(!path||!path->is_string()||!existed||!existed->is_bool()||!afterExists||!afterExists->is_bool()||
           !beforeHash||!beforeHash->is_string()||!afterHash||!afterHash->is_string())return {false,{},"rollback manifest entry is incomplete"};
        e.path=std::string(path->as_string());e.existed=existed->as_bool();e.afterExists=afterExists->as_bool();
        e.beforeHash=std::string(beforeHash->as_string());e.afterHash=std::string(afterHash->as_string());
        std::string pathError;if(!resolve_path(root,e.path,true,&pathError))return {false,{},"rollback path rejected: "+pathError};
        bool currentExists=false;std::string readError;auto current=read_file(root/e.path,64U<<20U,&currentExists,&readError);
        if(!current)return {false,{},readError};
        e.current=std::move(*current);
        if(currentExists!=e.afterExists||to_hex(fnv1a64(e.current))!=e.afterHash)
            return {false,JsonValue::Object{{"path",e.path},{"current_fnv1a64",to_hex(fnv1a64(e.current))},{"expected_fnv1a64",e.afterHash}},"rollback refused because a patched file changed afterward"};
        if(e.existed){
            std::ifstream backup(bundle/"files"/e.path,std::ios::binary);e.backup.assign(std::istreambuf_iterator<char>(backup),{});
            if(!backup.good()&&!backup.eof())return {false,{},"rollback backup could not be read for "+e.path};
            if(to_hex(fnv1a64(e.backup))!=e.beforeHash)return {false,{},"rollback backup hash mismatch for "+e.path};
        }
        list.push_back(std::move(e));
    }
    std::vector<std::size_t> restoredIndices;JsonValue::Array restored;
    const auto recover_after_state=[&](std::string* recoveryError){
        for(auto it=restoredIndices.rbegin();it!=restoredIndices.rend();++it){
            const Entry& e=list[*it];const auto path=root/e.path;
            if(!e.afterExists){std::error_code removeError;std::filesystem::remove(path,removeError);if(removeError&&recoveryError&&recoveryError->empty())*recoveryError="could not recover deleted state for "+e.path;}
            else {std::string writeError;if(!write_atomic(path,e.current,std::string(transactionId)+"-rollback-recover",&writeError)&&recoveryError&&recoveryError->empty())*recoveryError=e.path+": "+writeError;}
        }
    };
    for(std::size_t index=0;index<list.size();++index){
        const Entry& e=list[index];const auto path=root/e.path;bool ok=true;std::string error;
        if(!e.existed){const bool removed=std::filesystem::remove(path,ec);ok=removed&&!ec;ec.clear();if(!ok)error="could not remove transaction-created file";}
        else ok=write_atomic(path,e.backup,std::string(transactionId)+"-rollback",&error);
        if(!ok){std::string recoveryError;recover_after_state(&recoveryError);return {false,JsonValue::Object{{"path",e.path},{"recovery_attempted",!restoredIndices.empty()},{"recovery_error",recoveryError}},"rollback failed for "+e.path+": "+error};}
        restoredIndices.push_back(index);restored.emplace_back(e.path);
    }
    std::ofstream marker(bundle/"rolled_back",std::ios::binary|std::ios::trunc);marker<<"rolled back\n";
    if(!marker)return {false,JsonValue::Object{{"transaction_id",std::string(transactionId)}},"files were restored but rollback completion marker could not be written"};
    return {true,JsonValue::Object{{"transaction_id",std::string(transactionId)},{"restored",std::move(restored)}},{}};
}

} // namespace dve::ai
