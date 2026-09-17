#include "dve/ai/named_tasks.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace dve::ai {
namespace {

bool is_within(const std::filesystem::path& root,const std::filesystem::path& candidate){
    auto a=root.begin(),b=candidate.begin();for(;a!=root.end();++a,++b)if(b==candidate.end()||*a!=*b)return false;return true;
}

std::filesystem::path safe_working_directory(const std::filesystem::path& root,const std::filesystem::path& relative,std::string* error){
    if(relative.is_absolute()){if(error)*error="task working directory must be project-relative";return {};}
    std::error_code ec;auto result=std::filesystem::weakly_canonical(root/relative,ec);
    if(ec||!is_within(root,result)){if(error)*error="task working directory escapes the project root or does not exist";return {};}
    return result;
}

void append_bounded(std::string& destination,std::string_view chunk,std::size_t limit,bool* truncated){
    if(destination.size()>=limit){*truncated=true;return;}
    const auto count=std::min(chunk.size(),limit-destination.size());destination.append(chunk.substr(0,count));if(count<chunk.size())*truncated=true;
}

#if !defined(_WIN32)
std::optional<std::filesystem::path> resolve_executable(std::string_view executable,std::string* error){
    const std::filesystem::path requested(executable);
    if(requested.has_parent_path()){
        if(!requested.is_absolute()){if(error)*error="task executable with a path must be absolute";return std::nullopt;}
        if(access(requested.c_str(),X_OK)!=0){if(error)*error="task executable is missing or not executable";return std::nullopt;}
        return requested;
    }
    const char* rawPath=std::getenv("PATH");const std::string path=rawPath?rawPath:"/usr/local/bin:/usr/bin:/bin";
    std::size_t begin=0;
    while(begin<=path.size()){
        const auto end=path.find(':',begin);const auto stop=end==std::string::npos?path.size():end;
        const std::filesystem::path directory=stop==begin?std::filesystem::current_path():std::filesystem::path(path.substr(begin,stop-begin));
        const auto candidate=directory/requested;
        if(access(candidate.c_str(),X_OK)==0)return std::filesystem::absolute(candidate);
        if(end==std::string::npos)break;
        begin=end+1;
    }
    if(error)*error="task executable was not found in PATH";
    return std::nullopt;
}

std::vector<std::string> clean_environment(){
    static constexpr std::array<std::string_view,14> allowed{
        "PATH","HOME","TMPDIR","TMP","TEMP","LANG","LC_ALL","LC_CTYPE",
        "CC","CXX","CMAKE_GENERATOR","CMAKE_PREFIX_PATH","PKG_CONFIG_PATH","SDKROOT"};
    std::vector<std::string> result;result.reserve(allowed.size()+1);
    for(const auto name:allowed)if(const char* value=std::getenv(std::string(name).c_str());value&&*value)
        result.emplace_back(std::string(name)+"="+value);
    if(std::none_of(result.begin(),result.end(),[](const std::string& value){return value.starts_with("PATH=");}))
        result.emplace_back("PATH=/usr/local/bin:/usr/bin:/bin");
    result.emplace_back("DVE_AI_VALIDATION_TASK=1");
    return result;
}

AiNamedTaskResult run_posix(const AiNamedTaskDescriptor& task,const std::filesystem::path& cwd,
                            const std::filesystem::path& executable,const std::vector<std::string>& environment){
    AiNamedTaskResult result;int pipefd[2]{};if(pipe(pipefd)!=0){result.error="could not create task output pipe";return result;}
    const auto started=std::chrono::steady_clock::now();const pid_t pid=fork();
    if(pid<0){close(pipefd[0]);close(pipefd[1]);result.error="could not fork validation task";return result;}
    if(pid==0){
        (void)setpgid(0,0);
        (void)dup2(pipefd[1],STDOUT_FILENO);(void)dup2(pipefd[1],STDERR_FILENO);close(pipefd[0]);close(pipefd[1]);
        if(chdir(cwd.c_str())!=0)_exit(126);
        std::vector<std::string> storage;storage.reserve(task.arguments.size()+1);storage.push_back(executable.string());storage.insert(storage.end(),task.arguments.begin(),task.arguments.end());
        std::vector<char*> argv;argv.reserve(storage.size()+1);for(auto& value:storage)argv.push_back(value.data());argv.push_back(nullptr);
        std::vector<std::string> environmentStorage=environment;std::vector<char*> envp;envp.reserve(environmentStorage.size()+1);for(auto& value:environmentStorage)envp.push_back(value.data());envp.push_back(nullptr);
        execve(executable.c_str(),argv.data(),envp.data());_exit(errno==ENOENT?127:126);
    }
    (void)setpgid(pid,pid);
    result.launched=true;close(pipefd[1]);const int flags=fcntl(pipefd[0],F_GETFL,0);(void)fcntl(pipefd[0],F_SETFL,flags|O_NONBLOCK);
    bool childExited=false;int status=0;std::array<char,8192> buffer{};
    while(!childExited){
        for(;;){const ssize_t count=read(pipefd[0],buffer.data(),buffer.size());if(count>0)append_bounded(result.output,std::string_view(buffer.data(),static_cast<std::size_t>(count)),task.maximumOutputBytes,&result.outputTruncated);else break;}
        const pid_t waited=waitpid(pid,&status,WNOHANG);if(waited==pid){childExited=true;break;}
        const auto elapsed=std::chrono::steady_clock::now()-started;if(elapsed>=task.timeout){result.timedOut=true;(void)kill(-pid,SIGTERM);std::this_thread::sleep_for(std::chrono::milliseconds(100));if(waitpid(pid,&status,WNOHANG)==0)(void)kill(-pid,SIGKILL);(void)waitpid(pid,&status,0);childExited=true;break;}
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    for(;;){const ssize_t count=read(pipefd[0],buffer.data(),buffer.size());if(count>0)append_bounded(result.output,std::string_view(buffer.data(),static_cast<std::size_t>(count)),task.maximumOutputBytes,&result.outputTruncated);else break;}close(pipefd[0]);
    result.duration=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started);
    if(result.timedOut){result.exitCode=-1;result.error="validation task exceeded its deadline";return result;}
    if(WIFEXITED(status))result.exitCode=WEXITSTATUS(status);else if(WIFSIGNALED(status))result.exitCode=128+WTERMSIG(status);else result.exitCode=-1;
    result.succeeded=result.exitCode==0;if(!result.succeeded)result.error="validation task exited with code "+std::to_string(result.exitCode);return result;
}
#endif

} // namespace

AiNamedTaskRunner::AiNamedTaskRunner(std::filesystem::path projectRoot,std::vector<AiNamedTaskDescriptor> tasks):tasks_(std::move(tasks)){
    std::error_code ec;root_=std::filesystem::weakly_canonical(projectRoot,ec);if(ec)root_=std::filesystem::absolute(projectRoot);
    std::sort(tasks_.begin(),tasks_.end(),[](const auto& a,const auto& b){return a.name<b.name;});
    tasks_.erase(std::unique(tasks_.begin(),tasks_.end(),[](const auto& a,const auto& b){return a.name==b.name;}),tasks_.end());
}
const AiNamedTaskDescriptor* AiNamedTaskRunner::find(std::string_view name) const noexcept{for(const auto& task:tasks_)if(task.name==name)return &task;return nullptr;}
JsonValue AiNamedTaskRunner::list_json() const{JsonValue::Array rows;for(const auto& task:tasks_)rows.emplace_back(JsonValue::Object{{"name",task.name},{"title",task.title},{"description",task.description},{"writes_artifacts",task.writesArtifacts},{"timeout_seconds",static_cast<double>(task.timeout.count())}});return JsonValue::Object{{"tasks",std::move(rows)}};}
AiNamedTaskResult AiNamedTaskRunner::run(std::string_view name) const{
    const auto failure=[](std::string message){AiNamedTaskResult result;result.error=std::move(message);return result;};
    const auto* task=find(name);if(!task)return failure("unknown validation task: "+std::string(name));
    std::string error;const auto cwd=safe_working_directory(root_,task->workingDirectory,&error);if(cwd.empty())return failure(std::move(error));
#if defined(_WIN32)
    return failure("direct named-task execution is not yet implemented on Windows");
#else
    const auto executable=resolve_executable(task->executable,&error);if(!executable)return failure(std::move(error));
    return run_posix(*task,cwd,*executable,clean_environment());
#endif
}

std::vector<AiNamedTaskDescriptor> default_dve_validation_tasks(){
    using namespace std::chrono_literals;
    return {
        {"build.ai_bridge","Build AI bridge tests","Build the focused AI assistant test executable in the bounded headless build directory.","cmake",{"--build","build/ai-headless","--target","dve_ai_assistant_tests","-j","2"},".",600s,512U<<10U,true},
        {"build.playable_runtime","Build playable runtime","Build the character, trigger, replication, exact polygon collision, network simulation, and demonstration targets.","cmake",{"--build","build/ai-headless","--target","dve_gameplay_runtime_tests","dve_replication_contract_tests","dve_network_simulation_tests","dve_network_runtime_tests","dve_network_transport_tests","dve_udp_multiprocess_tests","dve_polygon_geometry_tests","dve_playable_runtime_v145_demo","dve_network_simulation_v146_demo","dve_network_runtime_v147_demo","dve_network_transport_v148_demo","-j","2"},".",600s,512U<<10U,true},
        {"build.gabor_volume","Build Gabor volumes","Build the Gabor cooker, runtime world, GPU frame-plan contract, and focused tests.","cmake",{"--build","build/ai-headless","--target","dve_cook_gabor","dve_gabor_volume_tests","dve_runtime_gabor_volume_tests","-j","2"},".",600s,512U<<10U,true},
#if defined(DVE_ENABLE_FLUODDITY)
        {"build.fluoddity","Build Fluoddity foundation","Build the Fluoddity preset cooker, native asset tests, and runtime planning tests.","cmake",{"--build","build/ai-headless","--target","dve_cook_fluoddity","dve_fluoddity_tests","dve_runtime_fluoddity_tests","-j","2"},".",600s,512U<<10U,true},
#endif
        {"build.text3d","Build 3D text","Build the Slug cooker, runtime world, analytic face/side renderer, and focused 3D-text tests.","cmake",{"--build","build/ai-headless","--target","dve_cook_text3d","dve_text3d_tests","dve_text3d_render_tests","dve_runtime_text3d_tests","-j","2"},".",600s,512U<<10U,true},
        {"configure.headless_hybrid","Configure headless HYBRID build","Configure the dependency-minimal HYBRID test build without editor, audio, Vulkan, Lua, or Jolt.","cmake",{"-S",".","-B","build/ai-headless","-DDVE_GEOMETRY_MODE=HYBRID","-DDVE_BUILD_EDITOR=OFF","-DDVE_BUILD_NATIVE_EDITOR=OFF","-DDVE_BUILD_AUDIO_SYNTH=OFF","-DDVE_ENABLE_VULKAN_BUFFER_BACKEND=OFF","-DDVE_ENABLE_LUA=OFF","-DDVE_ENABLE_JOLT=OFF","-DDVE_BUILD_BENCHMARK=OFF","-DDVE_BUILD_ASSET_COOKER=OFF","-DDVE_BUILD_ASSET_PIPELINE_TESTS=OFF"},".",300s,512U<<10U,true},
        {"test.ai_bridge","Test AI bridge","Run only the AI assistant regression suite from the bounded headless build directory.","ctest",{"--test-dir","build/ai-headless","--output-on-failure","-R","^dve_ai_assistant_tests$"},".",300s,512U<<10U,true},
        {"test.game_world","Test game world","Run the focused game-world regression suite.","ctest",{"--test-dir","build/ai-headless","--output-on-failure","-R","^dve_game_world_tests$"},".",300s,512U<<10U,true},
        {"test.gameplay_runtime","Test playable runtime","Run the character-controller, player, trigger, moving-platform, destruction-recovery, and replay suite.","ctest",{"--test-dir","build/ai-headless","--output-on-failure","-R","^dve_gameplay_runtime_tests$"},".",300s,512U<<10U,true},
        {"test.replication_contract","Test replication contract","Run the deterministic gameplay-state and destruction-journal codec suite.","ctest",{"--test-dir","build/ai-headless","--output-on-failure","-R","^dve_replication_contract_tests$"},".",300s,512U<<10U,true},
        {"test.network_simulation","Test lossy network simulation","Run reliable ordering, unreliable sequencing, fragmentation, repair, and prediction reconciliation tests.","ctest",{"--test-dir","build/ai-headless","--output-on-failure","-R","^dve_network_simulation_tests$"},".",300s,512U<<10U,true},
        {"test.network_runtime","Test network runtime integration","Run executed prediction replay, authoritative brick repair, collision rebuild, and socket-neutral transport tests.","ctest",{"--test-dir","build/ai-headless","--output-on-failure","-R","^dve_network_runtime_tests$"},".",300s,512U<<10U,true},
        {"test.network_transport","Test authenticated UDP transport","Run loopback UDP authentication, reliable fragmentation, server input validation, remote interpolation, and late-join checkpoint tests.","ctest",{"--test-dir","build/ai-headless","--output-on-failure","-R","^dve_network_transport_tests$"},".",300s,512U<<10U,true},
        {"test.network_transport_processes","Test UDP across processes","Run the separate-process localhost authenticated UDP request/response smoke test.","ctest",{"--test-dir","build/ai-headless","--output-on-failure","-R","^dve_udp_multiprocess_tests$"},".",300s,512U<<10U,true},
        {"test.polygon_character_collision","Test polygon character collision","Run exact triangle-level capsule and polygon-controller regression tests.","ctest",{"--test-dir","build/ai-headless","--output-on-failure","-R","^dve_polygon_geometry_tests$"},".",300s,512U<<10U,true},
        {"test.gabor_volume","Test Gabor volumes","Run import, DGABOR migration, runtime instancing, transactional GPU upload, frame planning, temporal reset, shadow scheduling, and preview tests.","ctest",{"--test-dir","build/ai-headless","--output-on-failure","-R","^dve_(runtime_)?gabor_volume_tests$"},".",300s,512U<<10U,true},
#if defined(DVE_ENABLE_FLUODDITY)
        {"test.fluoddity","Test Fluoddity foundation","Run preset migration, Fourier compatibility, DFLUODDITY integrity, budget, and runtime frame-planning tests.","ctest",{"--test-dir","build/ai-headless","--output-on-failure","-R","^dve_(runtime_)?fluoddity_tests$"},".",300s,512U<<10U,true},
#endif
        {"test.text3d","Test 3D text","Run bounded TrueType parsing, Slug atlas construction, runtime instancing, transactional atlas upload, face/side recording, preview, and DTEXT round-trip tests.","ctest",{"--test-dir","build/ai-headless","--output-on-failure","-R","^dve_(runtime_)?text3d(_render)?_tests$"},".",300s,512U<<10U,true},
        {"test.full_headless","Test full headless suite","Run all tests available in the bounded headless build directory.","ctest",{"--test-dir","build/ai-headless","--output-on-failure"},".",900s,1U<<20U,true},
        {"verify.release_manifest","Verify release manifest","Verify all payload hashes and release metadata without modifying the project.","python3",{"tools/dve_release_manifest.py","--verify","--root","."},".",120s,256U<<10U,false},
    };
}

} // namespace dve::ai
