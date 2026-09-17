#include "dve/audio/event_graph.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <sstream>

namespace dve::audio {

std::uint32_t audio_parameter_hash(std::string_view value) noexcept {
    std::uint32_t hash = 2166136261U;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 16777619U;
    }
    return hash;
}

float AudioEventParameters::get(std::string_view name, float fallback) const noexcept {
    const auto found = values.find(std::string(name));
    return found == values.end() ? fallback : found->second;
}

std::string_view audio_event_node_type_name(AudioEventNodeType type) noexcept {
    static constexpr std::array<std::string_view, 14> names{
        "sample", "synth_note", "random_no_repeat", "sequence", "layer", "delay", "gain",
        "bus", "switch", "scatter", "cooldown", "blend", "loop", "stream"};
    const auto index = static_cast<std::size_t>(type);
    return index < names.size() ? names[index] : std::string_view{"unknown"};
}

namespace {

std::optional<AudioEventNodeType> parse_type(std::string_view name) {
    for (std::uint32_t i = 0; i <= static_cast<std::uint32_t>(AudioEventNodeType::Stream); ++i) {
        const auto type = static_cast<AudioEventNodeType>(i);
        if (audio_event_node_type_name(type) == name) return type;
    }
    return std::nullopt;
}

void diagnostic(AudioEventCompileReport& report, AudioEventDiagnosticSeverity severity,
                std::uint32_t node, std::string message, std::uint32_t edge = UINT32_MAX) {
    report.diagnostics.push_back({severity, node, edge, std::move(message)});
}

bool is_control_node(AudioEventNodeType type) noexcept {
    return type != AudioEventNodeType::Sample && type != AudioEventNodeType::Stream && type != AudioEventNodeType::SynthNote;
}

std::size_t required_children(AudioEventNodeType type) noexcept {
    switch (type) {
        case AudioEventNodeType::Delay:
        case AudioEventNodeType::Gain:
        case AudioEventNodeType::Bus:
        case AudioEventNodeType::Scatter:
        case AudioEventNodeType::Cooldown:
        case AudioEventNodeType::Loop: return 1U;
        case AudioEventNodeType::Switch:
        case AudioEventNodeType::Blend: return 2U;
        default: return 0U;
    }
}

float apply_curve(float value, AudioBlendCurve curve) noexcept {
    value = std::clamp(value, 0.0F, 1.0F);
    switch (curve) {
        case AudioBlendCurve::SmoothStep: return value * value * (3.0F - 2.0F * value);
        case AudioBlendCurve::EqualPower: return value;
        case AudioBlendCurve::Linear: return value;
    }
    return value;
}

} // namespace

AudioEventCompileReport compile_audio_event_report(const AudioEventAsset& asset) {
    AudioEventCompileReport report;
    const auto& graph = asset.graph;
    if (asset.version != kAudioEventAssetVersion) {
        diagnostic(report, AudioEventDiagnosticSeverity::Error, UINT32_MAX,
                   "unsupported .dveaudio version " + std::to_string(asset.version));
        return report;
    }
    if (graph.nodes.empty() || graph.root >= graph.nodes.size()) {
        diagnostic(report, AudioEventDiagnosticSeverity::Error, UINT32_MAX, "audio event has no valid root");
        return report;
    }
    if (graph.nodes.size() > 256U) {
        diagnostic(report, AudioEventDiagnosticSeverity::Error, UINT32_MAX, "audio event exceeds 256 nodes");
        return report;
    }
    if (!asset.editorNodes.empty() && asset.editorNodes.size() != graph.nodes.size()) {
        diagnostic(report, AudioEventDiagnosticSeverity::Error, UINT32_MAX,
                   "editor node layout count does not match graph node count");
        return report;
    }
    for (std::uint32_t i = 0; i < asset.editorNodes.size(); ++i) {
        if (!std::isfinite(asset.editorNodes[i].x) || !std::isfinite(asset.editorNodes[i].y)) {
            diagnostic(report, AudioEventDiagnosticSeverity::Error, i,
                       "editor node position is not finite");
            return report;
        }
    }

    std::vector<std::uint8_t> marks(graph.nodes.size());
    std::function<bool(std::uint32_t)> visit = [&](std::uint32_t nodeIndex) {
        if (nodeIndex >= graph.nodes.size()) return false;
        if (marks[nodeIndex] == 1U) {
            diagnostic(report, AudioEventDiagnosticSeverity::Error, nodeIndex, "cycle reaches this node");
            return false;
        }
        if (marks[nodeIndex] == 2U) return true;
        marks[nodeIndex] = 1U;
        const auto& node = graph.nodes[nodeIndex];
        for (std::uint32_t edge = 0; edge < node.children.size(); ++edge) {
            const auto child = node.children[edge];
            if (child >= graph.nodes.size()) {
                diagnostic(report, AudioEventDiagnosticSeverity::Error, nodeIndex,
                           "child index is out of range", edge);
                return false;
            }
            if (!visit(child)) return false;
        }
        marks[nodeIndex] = 2U;
        return true;
    };
    if (!visit(graph.root)) return report;

    for (std::uint32_t i = 0; i < graph.nodes.size(); ++i) {
        const auto& node = graph.nodes[i];
        if (is_control_node(node.type) && node.children.empty())
            diagnostic(report, AudioEventDiagnosticSeverity::Error, i, "control node has no children");
        const std::size_t required = required_children(node.type);
        if (required != 0U && node.children.size() != required)
            diagnostic(report, AudioEventDiagnosticSeverity::Error, i,
                       std::string(audio_event_node_type_name(node.type)) + " requires " +
                       std::to_string(required) + " child nodes");
        if (node.type == AudioEventNodeType::Sample && !node.sample)
            diagnostic(report, AudioEventDiagnosticSeverity::Warning, i, "sample node has an empty sample id");
        if (node.type == AudioEventNodeType::Stream && !node.stream)
            diagnostic(report, AudioEventDiagnosticSeverity::Warning, i, "stream node has an empty stream id");
        if ((node.type == AudioEventNodeType::Switch || node.type == AudioEventNodeType::Blend) && node.parameter.empty())
            diagnostic(report, AudioEventDiagnosticSeverity::Error, i, "parameterized node has no parameter name");
        if (node.type == AudioEventNodeType::Blend && node.value2 <= node.threshold)
            diagnostic(report, AudioEventDiagnosticSeverity::Error, i, "blend upper range must exceed lower range");
        if ((node.type == AudioEventNodeType::Scatter || node.type == AudioEventNodeType::Loop) && node.count == 0U)
            diagnostic(report, AudioEventDiagnosticSeverity::Error, i, "repeat count must be nonzero");
        if (node.type == AudioEventNodeType::Cooldown && node.value < 0.0F)
            diagnostic(report, AudioEventDiagnosticSeverity::Error, i, "cooldown cannot be negative");
        if (marks[i] == 0U)
            diagnostic(report, AudioEventDiagnosticSeverity::Warning, i, "node is unreachable from the root");
    }
    if (std::any_of(report.diagnostics.begin(), report.diagnostics.end(), [](const auto& item) {
            return item.severity == AudioEventDiagnosticSeverity::Error;
        })) return report;

    CompiledAudioEvent result;
    result.name = graph.name;
    result.root = graph.root;
    result.deterministicSeed = asset.deterministicSeed == 0U ? 0x9e3779b9U : asset.deterministicSeed;
    result.noRepeatHistory = std::clamp(asset.noRepeatHistory, 1U, 16U);
    result.nodes.reserve(graph.nodes.size());
    for (const AudioEventNode& source : graph.nodes) {
        CompiledAudioEventNode node;
        node.type = source.type;
        node.childOffset = static_cast<std::uint32_t>(result.children.size());
        node.childCount = static_cast<std::uint16_t>(source.children.size());
        node.sample = source.sample;
        node.stream = source.stream;
        node.loop = source.loop;
        node.bus = source.bus;
        node.priority = source.priority;
        node.note = source.note;
        node.velocity = std::clamp(source.velocity, 0.0F, 1.0F);
        node.durationSeconds = std::clamp(source.durationSeconds, 0.0F, 120.0F);
        node.value = std::isfinite(source.value) ? source.value : 0.0F;
        node.value2 = std::isfinite(source.value2) ? source.value2 : 0.0F;
        node.count = std::clamp(source.count, 1U, 32U);
        node.parameterHash = audio_parameter_hash(source.parameter);
        node.threshold = std::isfinite(source.threshold) ? source.threshold : 0.0F;
        node.curve = source.curve;
        result.nodes.push_back(node);
        result.children.insert(result.children.end(), source.children.begin(), source.children.end());
    }
    report.event = std::move(result);
    return report;
}

std::optional<CompiledAudioEvent> compile_audio_event(const AudioEventAsset& asset, std::string* error) {
    AudioEventCompileReport report = compile_audio_event_report(asset);
    if (!report.event && error) {
        const auto found = std::find_if(report.diagnostics.begin(), report.diagnostics.end(), [](const auto& item) {
            return item.severity == AudioEventDiagnosticSeverity::Error;
        });
        *error = found == report.diagnostics.end() ? "audio event compilation failed" : found->message;
    }
    return std::move(report.event);
}

std::optional<CompiledAudioEvent> compile_audio_event(const AudioEventGraph& graph, std::string* error) {
    AudioEventAsset asset;
    asset.graph = graph;
    return compile_audio_event(asset, error);
}

namespace {

struct ExecutionContext {
    const CompiledAudioEvent& event;
    const AudioEventParameters& parameters;
    AudioEventInstanceState& state;
    AudioEventExecution output{};
    std::uint32_t sampleRate{};
    std::uint64_t absoluteStartFrame{};

    float parameter(std::uint32_t hash, float fallback) const noexcept {
        for (const auto& [name, value] : parameters.values)
            if (audio_parameter_hash(name) == hash) return value;
        return fallback;
    }

    void emit(AudioEventAction action) noexcept {
        if (output.count >= output.actions.size()) { output.truncated = true; return; }
        output.actions[output.count++] = action;
    }

    std::uint32_t random(std::uint32_t limit) noexcept {
        state.randomState ^= state.randomState << 13U;
        state.randomState ^= state.randomState >> 17U;
        state.randomState ^= state.randomState << 5U;
        return limit == 0U ? 0U : state.randomState % limit;
    }

    float random_unit() noexcept {
        return static_cast<float>(random(1U << 24U)) / static_cast<float>(1U << 24U);
    }

    void run(std::uint32_t index, std::uint64_t frameOffset, float gain, AudioBusId inheritedBus,
             std::size_t depth) noexcept {
        if (depth > 64U || index >= event.nodes.size() || output.truncated) return;
        const auto& node = event.nodes[index];
        auto child = [&](std::uint32_t childIndex) -> std::uint32_t {
            return event.children[node.childOffset + childIndex];
        };
        switch (node.type) {
            case AudioEventNodeType::Sample:
                emit({AudioEventActionType::PlaySample, frameOffset, node.sample,
                      node.bus == AudioBusId::Master ? inheritedBus : node.bus, node.priority, 0U, 0.0F, gain});
                break;
            case AudioEventNodeType::Stream: {
                AudioEventAction action;
                action.type = AudioEventActionType::PlayStream;
                action.frameOffset = frameOffset;
                action.bus = node.bus == AudioBusId::Master ? inheritedBus : node.bus;
                action.priority = node.priority;
                action.gain = gain;
                action.stream = node.stream;
                action.loop = node.loop;
                emit(action);
                break;
            }
            case AudioEventNodeType::SynthNote: {
                const float velocity = std::clamp(node.velocity * gain, 0.0F, 1.0F);
                emit({AudioEventActionType::SynthNoteOn, frameOffset, {}, inheritedBus, node.priority,
                      node.note, velocity, gain});
                emit({AudioEventActionType::SynthNoteOff,
                      frameOffset + static_cast<std::uint64_t>(node.durationSeconds * static_cast<float>(sampleRate)),
                      {}, inheritedBus, node.priority, node.note, 0.0F, gain});
                break;
            }
            case AudioEventNodeType::Layer:
                for (std::uint32_t i = 0; i < node.childCount; ++i)
                    run(child(i), frameOffset, gain, inheritedBus, depth + 1U);
                break;
            case AudioEventNodeType::RandomNoRepeat: {
                const std::size_t slot = index % state.lastRandomChoice.size();
                const std::uint32_t historyLimit = std::min<std::uint32_t>(
                    {event.noRepeatHistory, node.childCount > 0U ? node.childCount - 1U : 0U, 16U});
                auto was_recent = [&](std::uint32_t candidate) noexcept {
                    for (std::uint32_t i = 0; i < state.randomHistoryCount[slot]; ++i)
                        if (state.randomHistory[slot][i] == candidate) return true;
                    return false;
                };
                std::uint32_t choice = random(node.childCount);
                for (std::uint32_t attempt = 0; attempt < node.childCount * 2U && was_recent(choice); ++attempt)
                    choice = random(node.childCount);
                if (was_recent(choice)) {
                    for (std::uint32_t candidate = 0; candidate < node.childCount; ++candidate)
                        if (!was_recent(candidate)) { choice = candidate; break; }
                }
                state.lastRandomChoice[slot] = choice;
                if (historyLimit > 0U) {
                    const std::uint8_t cursor = state.randomHistoryCursor[slot];
                    state.randomHistory[slot][cursor] = choice;
                    state.randomHistoryCursor[slot] = static_cast<std::uint8_t>((cursor + 1U) % historyLimit);
                    state.randomHistoryCount[slot] = static_cast<std::uint8_t>(
                        std::min<std::uint32_t>(historyLimit, state.randomHistoryCount[slot] + 1U));
                }
                run(child(choice), frameOffset, gain, inheritedBus, depth + 1U);
                break;
            }
            case AudioEventNodeType::Sequence: {
                const std::size_t slot = index % state.sequenceCursor.size();
                const std::uint32_t choice = state.sequenceCursor[slot]++ % node.childCount;
                run(child(choice), frameOffset, gain, inheritedBus, depth + 1U);
                break;
            }
            case AudioEventNodeType::Delay:
                run(child(0), frameOffset + static_cast<std::uint64_t>(std::max(0.0F, node.value) * static_cast<float>(sampleRate)),
                    gain, inheritedBus, depth + 1U);
                break;
            case AudioEventNodeType::Gain:
                run(child(0), frameOffset, gain * std::max(0.0F, node.value), inheritedBus, depth + 1U);
                break;
            case AudioEventNodeType::Bus:
                run(child(0), frameOffset, gain, node.bus, depth + 1U);
                break;
            case AudioEventNodeType::Switch: {
                const bool selected = parameter(node.parameterHash, 0.0F) >= node.threshold;
                run(child(selected ? 1U : 0U), frameOffset, gain, inheritedBus, depth + 1U);
                break;
            }
            case AudioEventNodeType::Scatter: {
                const float low = std::max(0.0F, std::min(node.value, node.value2));
                const float high = std::max(low, std::max(node.value, node.value2));
                for (std::uint32_t i = 0; i < node.count; ++i) {
                    const float delay = low + (high - low) * random_unit();
                    run(child(0), frameOffset + static_cast<std::uint64_t>(delay * static_cast<float>(sampleRate)), gain,
                        inheritedBus, depth + 1U);
                }
                break;
            }
            case AudioEventNodeType::Cooldown: {
                const std::size_t slot = index % state.lastTriggerFrame.size();
                const std::uint64_t now = absoluteStartFrame + frameOffset;
                const std::uint64_t cooldown = static_cast<std::uint64_t>(std::max(0.0F, node.value) * static_cast<float>(sampleRate));
                const std::uint64_t last = state.lastTriggerFrame[slot];
                if (last != std::numeric_limits<std::uint64_t>::max() && now >= last && now - last < cooldown) {
                    ++output.suppressedByCooldown;
                    break;
                }
                state.lastTriggerFrame[slot] = now;
                run(child(0), frameOffset, gain, inheritedBus, depth + 1U);
                break;
            }
            case AudioEventNodeType::Blend: {
                const float denominator = std::max(1.0e-6F, node.value2 - node.threshold);
                float t = (parameter(node.parameterHash, node.threshold) - node.threshold) / denominator;
                t = apply_curve(t, node.curve);
                float a = 1.0F - t;
                float b = t;
                if (node.curve == AudioBlendCurve::EqualPower) {
                    a = std::cos(t * 1.57079632679F);
                    b = std::sin(t * 1.57079632679F);
                }
                if (a > 1.0e-5F) run(child(0), frameOffset, gain * a, inheritedBus, depth + 1U);
                if (b > 1.0e-5F) run(child(1), frameOffset, gain * b, inheritedBus, depth + 1U);
                break;
            }
            case AudioEventNodeType::Loop: {
                const std::uint64_t interval = static_cast<std::uint64_t>(std::max(0.0F, node.value) * static_cast<float>(sampleRate));
                for (std::uint32_t i = 0; i < node.count; ++i)
                    run(child(0), frameOffset + interval * i, gain, inheritedBus, depth + 1U);
                break;
            }
        }
    }
};

} // namespace

AudioEventExecution execute_audio_event(const CompiledAudioEvent& event,
                                        const AudioEventParameters& parameters,
                                        AudioEventInstanceState& state,
                                        std::uint32_t sampleRate,
                                        std::uint64_t absoluteStartFrame) noexcept {
    if (!state.initialized) {
        state.randomState = event.deterministicSeed;
        state.lastTriggerFrame.fill(std::numeric_limits<std::uint64_t>::max());
        state.initialized = true;
    }
    ExecutionContext context{event, parameters, state, {}, sampleRate, absoluteStartFrame};
    context.run(event.root, 0U, 1.0F, AudioBusId::Effects, 0U);
    std::sort(context.output.actions.begin(),
              context.output.actions.begin() + static_cast<std::ptrdiff_t>(context.output.count),
              [](const AudioEventAction& a, const AudioEventAction& b) {
                  if (a.frameOffset != b.frameOffset) return a.frameOffset < b.frameOffset;
                  return static_cast<unsigned>(a.type) < static_cast<unsigned>(b.type);
              });
    return context.output;
}

void dispatch_audio_event(const AudioEventExecution& execution, AudioMixer& mixer,
                          const AudioEmitterState& emitter) noexcept {
    const std::uint64_t base = mixer.current_frame();
    for (std::size_t i = 0; i < execution.count; ++i) {
        const AudioEventAction& action = execution.actions[i];
        if (action.type == AudioEventActionType::PlaySample) {
            PlaySampleDesc desc;
            desc.sample = action.sample;
            desc.bus = action.bus;
            desc.priority = action.priority;
            desc.emitter = emitter;
            desc.gain = action.gain;
            desc.sampleFrame = base + action.frameOffset;
            (void)mixer.play_sample(desc);
        } else if (action.type == AudioEventActionType::PlayStream) {
            PlayStreamDesc desc;
            desc.sample = action.stream;
            desc.bus = action.bus;
            desc.gain = action.gain;
            desc.loop = action.loop;
            desc.sampleFrame = base + action.frameOffset;
            (void)mixer.play_stream(desc);
        } else if (action.type == AudioEventActionType::SynthNoteOn) {
            (void)mixer.synthesizer().note_on(action.note, action.velocity, 0U, base + action.frameOffset);
        } else {
            (void)mixer.synthesizer().note_off(action.note, 0.0F, 0U, base + action.frameOffset);
        }
    }
}

bool write_audio_event_asset(const std::filesystem::path& path, const AudioEventAsset& asset,
                             std::string* error) {
    const auto report = compile_audio_event_report(asset);
    if (!report.succeeded()) {
        if (error) {
            const auto found = std::find_if(report.diagnostics.begin(), report.diagnostics.end(), [](const auto& item) {
                return item.severity == AudioEventDiagnosticSeverity::Error;
            });
            *error = found == report.diagnostics.end() ? "audio event is invalid" : found->message;
        }
        return false;
    }
    std::ofstream out(path, std::ios::trunc);
    if (!out) { if (error) *error = "unable to create .dveaudio file"; return false; }
    out << "DVEAUDIO " << asset.version << '\n';
    out << "name " << std::quoted(asset.graph.name) << '\n';
    out << "seed " << asset.deterministicSeed << '\n';
    out << "history " << asset.noRepeatHistory << '\n';
    out << "root " << asset.graph.root << '\n';
    out << "nodes " << asset.graph.nodes.size() << '\n';
    out << std::setprecision(9);
    for (std::size_t i = 0; i < asset.graph.nodes.size(); ++i) {
        const auto& node = asset.graph.nodes[i];
        out << "node " << i << ' ' << audio_event_node_type_name(node.type) << ' '
            << node.sample.value << ' ' << node.stream.value << ' ' << (node.loop ? 1U : 0U) << ' ' << static_cast<unsigned>(node.bus) << ' '
            << static_cast<unsigned>(node.priority) << ' ' << static_cast<unsigned>(node.note) << ' '
            << node.velocity << ' ' << node.durationSeconds << ' ' << node.value << ' ' << node.value2 << ' '
            << node.count << ' ' << node.threshold << ' ' << static_cast<unsigned>(node.curve) << ' '
            << std::quoted(node.parameter) << ' ' << node.children.size();
        for (const auto child : node.children) out << ' ' << child;
        out << '\n';
    }
    out << "editors " << asset.graph.nodes.size() << '\n';
    for (std::size_t i = 0; i < asset.graph.nodes.size(); ++i) {
        AudioEventEditorNode position{};
        if (i < asset.editorNodes.size()) position = asset.editorNodes[i];
        else {
            position.x = static_cast<float>((i % 4U) * 180U + 20U);
            position.y = static_cast<float>((i / 4U) * 80U + 20U);
        }
        out << "editor " << i << ' ' << position.x << ' ' << position.y << '\n';
    }
    out << "end\n";
    if (!out) { if (error) *error = "failed while writing .dveaudio file"; return false; }
    return true;
}

std::optional<AudioEventAsset> read_audio_event_asset(const std::filesystem::path& path,
                                                       std::string* error) {
    std::ifstream in(path);
    if (!in) { if (error) *error = "unable to open .dveaudio file"; return std::nullopt; }
    AudioEventAsset asset;
    std::string key;
    std::uint32_t fileVersion{};
    if (!(in >> key >> fileVersion) || key != "DVEAUDIO" || (fileVersion != 2U && fileVersion != 3U && fileVersion != kAudioEventAssetVersion)) {
        if (error) *error = "unsupported or malformed .dveaudio header";
        return std::nullopt;
    }
    asset.version = kAudioEventAssetVersion;
    std::size_t nodeCount{};
    if (!(in >> key) || key != "name" || !(in >> std::quoted(asset.graph.name)) ||
        !(in >> key >> asset.deterministicSeed) || key != "seed" ||
        !(in >> key >> asset.noRepeatHistory) || key != "history" ||
        !(in >> key >> asset.graph.root) || key != "root" ||
        !(in >> key >> nodeCount) || key != "nodes" || nodeCount > 256U) {
        if (error) *error = "malformed .dveaudio metadata";
        return std::nullopt;
    }
    asset.graph.nodes.resize(nodeCount);
    for (std::size_t expected = 0; expected < nodeCount; ++expected) {
        std::size_t index{};
        std::string typeName;
        unsigned bus{}, priority{}, note{}, curve{}, loop{};
        std::size_t childCount{};
        auto& node = asset.graph.nodes[expected];
        bool parsed{};
        if (fileVersion >= 3U) {
            parsed = static_cast<bool>(in >> key >> index >> typeName >> node.sample.value >> node.stream.value >> loop >>
                bus >> priority >> note >> node.velocity >> node.durationSeconds >> node.value >> node.value2 >>
                node.count >> node.threshold >> curve >> std::quoted(node.parameter) >> childCount);
            node.loop = loop != 0U;
        } else {
            parsed = static_cast<bool>(in >> key >> index >> typeName >> node.sample.value >> bus >> priority >> note >>
                node.velocity >> node.durationSeconds >> node.value >> node.value2 >> node.count >>
                node.threshold >> curve >> std::quoted(node.parameter) >> childCount);
        }
        if (!parsed || key != "node" || index != expected || childCount > 256U) {
            if (error) *error = "malformed .dveaudio node " + std::to_string(expected);
            return std::nullopt;
        }
        const auto type = parse_type(typeName);
        if (!type || bus >= kAudioBusCount || priority > 255U || note > 127U || curve > 2U) {
            if (error) *error = "invalid .dveaudio node enum value";
            return std::nullopt;
        }
        node.type = *type;
        node.bus = static_cast<AudioBusId>(bus);
        node.priority = static_cast<AudioPriority>(priority);
        node.note = static_cast<std::uint8_t>(note);
        node.curve = static_cast<AudioBlendCurve>(curve);
        node.children.resize(childCount);
        for (auto& child : node.children) if (!(in >> child)) {
            if (error) *error = "truncated .dveaudio child list";
            return std::nullopt;
        }
    }
    if (fileVersion >= 4U) {
        std::size_t editorCount{};
        if (!(in >> key >> editorCount) || key != "editors" || editorCount != nodeCount) {
            if (error) *error = "malformed .dveaudio editor layout header";
            return std::nullopt;
        }
        asset.editorNodes.resize(editorCount);
        for (std::size_t expected = 0; expected < editorCount; ++expected) {
            std::size_t index{};
            if (!(in >> key >> index >> asset.editorNodes[expected].x >> asset.editorNodes[expected].y) ||
                key != "editor" || index != expected || !std::isfinite(asset.editorNodes[expected].x) ||
                !std::isfinite(asset.editorNodes[expected].y)) {
                if (error) *error = "malformed .dveaudio editor node " + std::to_string(expected);
                return std::nullopt;
            }
        }
    }
    if (!(in >> key) || key != "end") {
        if (error) *error = "missing .dveaudio end marker";
        return std::nullopt;
    }
    const auto report = compile_audio_event_report(asset);
    if (!report.succeeded()) {
        if (error) {
            const auto found = std::find_if(report.diagnostics.begin(), report.diagnostics.end(), [](const auto& item) {
                return item.severity == AudioEventDiagnosticSeverity::Error;
            });
            *error = found == report.diagnostics.end() ? "invalid .dveaudio graph" : found->message;
        }
        return std::nullopt;
    }
    return asset;
}

} // namespace dve::audio
