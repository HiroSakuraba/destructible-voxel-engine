#include "dve/rhi/pipeline_preparation.hpp"
#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>
namespace dve::rhi {
namespace {
void failure(std::string* output, const char* message) {
    if(output) *output=message;
}
}
PipelinePreparationQueue::Job* PipelinePreparationQueue::find(
    std::uint64_t epoch, std::string_view key) noexcept {
    const auto it=std::find_if(jobs_.begin(),jobs_.end(),[&](const Job& job){
        return job.epoch==epoch && job.key==key;
    });
    return it==jobs_.end()?nullptr:&*it;
}
const PipelinePreparationQueue::Job* PipelinePreparationQueue::find(
    std::uint64_t epoch, std::string_view key) const noexcept {
    const auto it=std::find_if(jobs_.begin(),jobs_.end(),[&](const Job& job){
        return job.epoch==epoch && job.key==key;
    });
    return it==jobs_.end()?nullptr:&*it;
}
bool PipelinePreparationQueue::stage_graphics(
    std::uint64_t epoch,std::string key,GraphicsPipelineDesc desc,std::string* error) {
    if(epoch==0U || key.empty() || key.size()>256U || jobs_.size()>=kMaximumJobs ||
       find(epoch,key)) {
        failure(error,"PSO-2 invalid, duplicate, or over-capacity graphics preparation");
        return false;
    }
    jobs_.push_back({epoch,std::move(key),PipelinePreparationState::Pending,
                     std::move(desc),{},{}});
    return true;
}
bool PipelinePreparationQueue::stage_compute(
    std::uint64_t epoch,std::string key,ComputePipelineDesc desc,std::string* error) {
    if(epoch==0U || key.empty() || key.size()>256U || jobs_.size()>=kMaximumJobs ||
       find(epoch,key)) {
        failure(error,"PSO-2 invalid, duplicate, or over-capacity compute preparation");
        return false;
    }
    jobs_.push_back({epoch,std::move(key),PipelinePreparationState::Pending,
                     std::move(desc),{},{}});
    return true;
}
PipelinePreparationProgress PipelinePreparationQueue::pump(
    std::uint32_t maxPipelines, std::chrono::microseconds softBudget) {
    PipelinePreparationProgress out{};
    const auto start=std::chrono::steady_clock::now();
    const auto limit=std::max(softBudget,std::chrono::microseconds{0});
    for(auto& job:jobs_) {
        if(job.state!=PipelinePreparationState::Pending)continue;
        if(out.attempted>=maxPipelines)break;
        if(out.attempted>0U && std::chrono::steady_clock::now()-start>=limit) {
            out.hitSoftBudget=true;break;
        }
        ++out.attempted;
        std::string error;
        try {
            if(const auto* graphics=std::get_if<GraphicsPipelineDesc>(&job.desc)) {
                const auto handle=device_.create_graphics_pipeline(*graphics,&error);
                if(handle)job.prepared=handle;
            } else {
                const auto handle=device_.create_compute_pipeline(
                    std::get<ComputePipelineDesc>(job.desc),&error);
                if(handle)job.prepared=handle;
            }
        } catch(const std::exception& e) {
            error=e.what();
        } catch(...) {
            error="unknown graphics API exception";
        }
        if(std::holds_alternative<std::monostate>(job.prepared)) {
            job.state=PipelinePreparationState::Failed;
            job.failure=error.empty()?"driver refused pipeline":std::move(error);
            ++out.failed;
        } else {
            job.state=PipelinePreparationState::Ready;
            ++out.compiled;
        }
    }
    for(const auto& job:jobs_)
        if(job.state==PipelinePreparationState::Pending)++out.pending;
    out.elapsedNanoseconds=static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-start).count());
    if(out.pending && out.attempted && std::chrono::steady_clock::now()-start>=limit)
        out.hitSoftBudget=true;
    return out;
}
PipelinePreparationState PipelinePreparationQueue::state(
    std::uint64_t epoch,std::string_view key) const noexcept {
    const auto* job=find(epoch,key);
    return job?job->state:PipelinePreparationState::Missing;
}
PipelinePreparationSummary PipelinePreparationQueue::summary(std::uint64_t epoch) const noexcept {
    PipelinePreparationSummary out{};
    std::uint32_t count=0U;
    for(const auto& job:jobs_) {
        if(job.epoch!=epoch)continue;
        ++count;
        switch(job.state) {
        case PipelinePreparationState::Pending:++out.pending;break;
        case PipelinePreparationState::Ready:++out.ready;break;
        case PipelinePreparationState::Failed:++out.failed;break;
        case PipelinePreparationState::Published:++out.published;break;
        case PipelinePreparationState::Missing:break;
        }
    }
    out.allReady=count>0U && !out.pending && !out.failed;
    return out;
}
GraphicsPipelineHandle PipelinePreparationQueue::take_graphics(
    std::uint64_t epoch,std::string_view key) noexcept {
    auto* job=find(epoch,key);
    if(!job || job->state!=PipelinePreparationState::Ready)return {};
    const auto* handle=std::get_if<GraphicsPipelineHandle>(&job->prepared);
    if(!handle)return {};
    const auto result=*handle;
    job->prepared=std::monostate{};
    job->state=PipelinePreparationState::Published;
    return result;
}
ComputePipelineHandle PipelinePreparationQueue::take_compute(
    std::uint64_t epoch,std::string_view key) noexcept {
    auto* job=find(epoch,key);
    if(!job || job->state!=PipelinePreparationState::Ready)return {};
    const auto* handle=std::get_if<ComputePipelineHandle>(&job->prepared);
    if(!handle)return {};
    const auto result=*handle;
    job->prepared=std::monostate{};
    job->state=PipelinePreparationState::Published;
    return result;
}
std::string_view PipelinePreparationQueue::failure_reason(
    std::uint64_t epoch,std::string_view key) const noexcept {
    const auto* job=find(epoch,key);
    return job?std::string_view(job->failure):std::string_view{};
}
void PipelinePreparationQueue::retire(Job& job) noexcept {
    try {
        if(const auto* graphics=std::get_if<GraphicsPipelineHandle>(&job.prepared))
            (void)device_.destroy_graphics_pipeline(*graphics,nullptr);
        else if(const auto* compute=std::get_if<ComputePipelineHandle>(&job.prepared))
            (void)device_.destroy_compute_pipeline(*compute,nullptr);
    } catch(...) {} // Tear-down must never throw or destroy a transferred handle.
    job.prepared=std::monostate{};
}
void PipelinePreparationQueue::discard_epoch(std::uint64_t epoch) noexcept {
    for(auto& job:jobs_)if(job.epoch==epoch)retire(job);
    jobs_.erase(std::remove_if(jobs_.begin(),jobs_.end(),[&](const Job& job){
        return job.epoch==epoch;
    }),jobs_.end());
}
void PipelinePreparationQueue::clear() noexcept {
    for(auto& job:jobs_)retire(job);
    jobs_.clear();
}
} // namespace dve::rhi
