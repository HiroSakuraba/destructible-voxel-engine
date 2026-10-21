#pragma once
#include "dve/editor_nurbs.hpp"
#include "dve/editor_nurbs_seam.hpp"
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
namespace dve::editor {
// One worker, one running snapshot, one newest pending snapshot. Never owns UI/document pointers.
class NurbsTessellationQueue {
public:
    struct Result {
        std::uint64_t generation{};
        NurbsSource source;
        std::unique_ptr<EditNurbsCommand> command;
        std::unique_ptr<PreparedNurbsSeamEdit> seamCommand;
        std::string error;
    };
    NurbsTessellationQueue();
    ~NurbsTessellationQueue();
    std::uint64_t submit(std::shared_ptr<const EditorObject>,NurbsSource);
    std::uint64_t submit(std::shared_ptr<const NurbsSeamDragSnapshot>,EditorObjectId,NurbsSource);
    void cancel();
    [[nodiscard]] std::optional<Result> poll();
private:
    struct Request { std::uint64_t generation;std::shared_ptr<const EditorObject> object;NurbsSource source;std::stop_source stop;std::shared_ptr<const NurbsSeamDragSnapshot> seam;EditorObjectId edited{}; };
    void run(std::stop_token);
    std::mutex mutex_;
    std::condition_variable_any ready_;
    std::optional<Request> pending_;
    std::optional<Result> result_;
    std::stop_source activeStop_;
    std::uint64_t generation_{};
    std::jthread worker_;
};
}
