#include "dve/editor_nurbs_tessellation.hpp"
namespace dve::editor {
NurbsTessellationQueue::NurbsTessellationQueue():worker_([this](std::stop_token stop){run(stop);}){}
NurbsTessellationQueue::~NurbsTessellationQueue(){cancel();worker_.request_stop();ready_.notify_all();}
std::uint64_t NurbsTessellationQueue::submit(std::shared_ptr<const EditorObject> object,NurbsSource source){
    std::lock_guard lock(mutex_);activeStop_.request_stop();result_.reset();
    const auto generation=++generation_;
    pending_=Request{generation,std::move(object),std::move(source),{}};ready_.notify_one();return generation;
}
std::uint64_t NurbsTessellationQueue::submit(std::shared_ptr<const NurbsSeamDragSnapshot> snapshot,EditorObjectId edited,NurbsSource source){
    std::lock_guard lock(mutex_);activeStop_.request_stop();result_.reset();
    const auto generation=++generation_;
    pending_=Request{generation,{},std::move(source),{},std::move(snapshot),edited};ready_.notify_one();return generation;
}
void NurbsTessellationQueue::cancel(){
    std::lock_guard lock(mutex_);++generation_;activeStop_.request_stop();pending_.reset();result_.reset();
}
std::optional<NurbsTessellationQueue::Result> NurbsTessellationQueue::poll(){
    std::lock_guard lock(mutex_);auto result=std::move(result_);result_.reset();return result;
}
void NurbsTessellationQueue::run(std::stop_token stop){
    while(!stop.stop_requested()){
        std::optional<Request> request;
        {
            std::unique_lock lock(mutex_);
            if(!ready_.wait(lock,stop,[&]{return pending_.has_value();}))return;
            request=std::move(pending_);pending_.reset();activeStop_=request->stop;
        }
        Result result;result.generation=request->generation;result.source=std::move(request->source);
        try {
            if(request->seam)result.seamCommand=PreparedNurbsSeamEdit::prepare(request->seam,request->edited,result.source,&result.error,request->stop.get_token());
            else if(request->object)result.command=EditNurbsCommand::prepare(*request->object,result.source,&result.error,request->stop.get_token());
            else result.error="Missing NURBS snapshot";
        }
        catch(const std::exception& error){result.error=error.what();}
        catch(...){result.error="NURBS tessellation failed";}
        std::lock_guard lock(mutex_);
        if(!stop.stop_requested()&&!request->stop.stop_requested()&&result.generation==generation_)result_=std::move(result);
    }
}
}
