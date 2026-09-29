// Phase 6 (SYN-016) candidate browser: session persistence, ranking,
// promotion, and the editor Search panel. See patch_search_browser.hpp for
// the ".dvesearch" file-format documentation.
#include "dve/audio/patch_search_browser.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

namespace dve::audio {
namespace {

// --- tiny text helpers -----------------------------------------------------

std::string sanitize_line(std::string value) {
    for (char& c : value)
        if (c == '\n' || c == '\r') c = ' ';
    return value;
}

// Shortest round-trip decimal for a double (bit-identical on parse).
std::string to_text(double value) {
    char buf[32]{};
    auto result = std::to_chars(buf, buf + sizeof(buf), value);
    if (result.ec != std::errc()) return "0";
    return std::string(buf, result.ptr);
}

bool parse_double(std::string_view text, double& out) {
    if (text.empty() || text.size() > 64) return false;
    double value = 0.0;
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size()) return false;
    if (!std::isfinite(value)) return false;
    out = value;
    return true;
}

bool parse_uint64(std::string_view text, std::uint64_t& out) {
    if (text.empty() || text.size() > 20) return false;
    std::uint64_t value = 0;
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size()) return false;
    out = value;
    return true;
}

bool parse_int(std::string_view text, int& out) {
    if (text.empty() || text.size() > 12) return false;
    int value = 0;
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size()) return false;
    out = value;
    return true;
}

// --- target vector field table (order-independent on load) -----------------

struct TargetField {
    const char* name;
    double AudioFeatureVector::* member;
};

constexpr TargetField kTargetFields[] = {
    {"rms", &AudioFeatureVector::rms},
    {"peak", &AudioFeatureVector::peak},
    {"spectral_centroid_hz", &AudioFeatureVector::spectralCentroidHz},
    {"spectral_rolloff_hz", &AudioFeatureVector::spectralRolloffHz},
    {"spectral_flatness", &AudioFeatureVector::spectralFlatness},
    {"zero_crossing_rate", &AudioFeatureVector::zeroCrossingRate},
    {"estimated_pitch_hz", &AudioFeatureVector::estimatedPitchHz},
    {"pitch_confidence", &AudioFeatureVector::pitchConfidence},
    {"harmonic_energy_ratio", &AudioFeatureVector::harmonicEnergyRatio},
    {"attack_seconds", &AudioFeatureVector::attackSeconds},
    {"decay_seconds", &AudioFeatureVector::decaySeconds},
    {"sustain_level", &AudioFeatureVector::sustainLevel},
    {"release_seconds", &AudioFeatureVector::releaseSeconds},
    {"stereo_width", &AudioFeatureVector::stereoWidth},
};

}  // namespace

// --- ranking ---------------------------------------------------------------

void sort_candidates(SearchSession& session) {
    std::stable_sort(session.candidates.begin(), session.candidates.end(),
                     [](const BrowserCandidate& a, const BrowserCandidate& b) {
                         return a.distance < b.distance;
                     });
}

void keep_top_n(SearchSession& session, std::size_t n) {
    sort_candidates(session);
    if (session.candidates.size() > n) session.candidates.resize(n);
}

// --- promotion --------------------------------------------------------------

SynthPreset promote_candidate(const BrowserCandidate& candidate) {
    std::string error;
    if (candidate.preset.validate(&error)) return candidate.preset;
    // Documented fallback: never hand out an invalid preset.
    return SynthPreset::make_default();
}

// --- session persistence ----------------------------------------------------

namespace {

constexpr const char* kMagicKey = "dve_search_session";
constexpr const char* kMagicValue = "1";
constexpr const char* kCandidateOpen = "[candidate]";
constexpr const char* kPresetBegin = "[dve_search_preset_begin]";
constexpr const char* kPresetEnd = "[dve_search_preset_end]";

bool write_session_text(std::ostream& out, const SearchSession& session) {
    out << "# DVE patch-search session (SYN-016). Unknown fields are skipped by the loader.\n";
    out << kMagicKey << '=' << kMagicValue << '\n';
    out << "target_name=" << sanitize_line(session.targetName) << '\n';
    out << "search_seed=" << session.searchSeed << '\n';
    out << "generations_run=" << session.generationsRun << '\n';
    for (const auto& field : kTargetFields)
        out << "target." << field.name << '=' << to_text(session.target.*(field.member)) << '\n';
    out << "target.mel_bands=";
    for (int i = 0; i < 16; ++i) {
        if (i > 0) out << ',';
        out << to_text(session.target.melBands[i]);
    }
    out << '\n';
    out << "candidate_count=" << session.candidates.size() << '\n';
    for (const auto& candidate : session.candidates) {
        out << kCandidateOpen << '\n';
        out << "label=" << sanitize_line(candidate.label) << '\n';
        out << "distance=" << to_text(candidate.distance) << '\n';
        out << kPresetBegin << '\n';
        out << candidate.preset.serialize();
        // serialize() may or may not end with '\n'; normalize before the delimiter.
        out << '\n' << kPresetEnd << '\n';
    }
    return static_cast<bool>(out);
}

bool read_session_text(std::istream& in, SearchSession& session) {
    SearchSession result;
    bool sawMagic = false;
    bool hasCount = false;
    std::size_t declaredCount = 0;

    bool inCandidate = false;
    bool inPreset = false;
    BrowserCandidate current;
    std::string presetText;

    auto finish_candidate = [&]() -> bool {
        if (!inCandidate || inPreset) return false;  // unterminated block
        std::string error;
        auto preset = SynthPreset::parse(presetText, &error);
        if (!preset) return false;
        current.preset = std::move(*preset);
        result.candidates.push_back(std::move(current));
        current = BrowserCandidate{};
        presetText.clear();
        inCandidate = false;
        return true;
    };

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (inPreset) {
            if (line == kPresetEnd) {
                inPreset = false;
                continue;
            }
            presetText += line;
            presetText += '\n';
            continue;
        }
        if (line.empty() || line[0] == '#') continue;
        if (line == kCandidateOpen) {
            // Close the previous block first (validates it carried a preset).
            if (inCandidate && !finish_candidate()) return false;
            inCandidate = true;
            current = BrowserCandidate{};
            presetText.clear();
            continue;
        }
        if (line == kPresetBegin) {
            if (!inCandidate || inPreset) return false;
            inPreset = true;
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) return false;  // structural garbage
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);

        if (key == kMagicKey) {
            if (value != kMagicValue) return false;
            sawMagic = true;
            continue;
        }
        if (key == "target_name") {
            result.targetName = value;
            continue;
        }
        if (key == "search_seed") {
            if (!parse_uint64(value, result.searchSeed)) return false;
            continue;
        }
        if (key == "generations_run") {
            if (!parse_int(value, result.generationsRun)) return false;
            continue;
        }
        if (key == "candidate_count") {
            std::uint64_t count = 0;
            if (!parse_uint64(value, count)) return false;
            declaredCount = static_cast<std::size_t>(count);
            hasCount = true;
            continue;
        }
        if (key == "target.mel_bands") {
            double bands[16]{};
            std::size_t index = 0;
            std::string rest = value;
            while (index < 16) {
                const auto comma = rest.find(',');
                const std::string piece = (comma == std::string::npos) ? rest : rest.substr(0, comma);
                if (!parse_double(piece, bands[index])) return false;
                ++index;
                if (comma == std::string::npos) break;
                rest = rest.substr(comma + 1);
            }
            if (index != 16 || rest.find(',') != std::string::npos) return false;
            for (int i = 0; i < 16; ++i) result.target.melBands[i] = bands[i];
            continue;
        }
        if (key.rfind("target.", 0) == 0) {
            const std::string fieldName = key.substr(7);
            bool known = false;
            for (const auto& field : kTargetFields) {
                if (fieldName == field.name) {
                    if (!parse_double(value, result.target.*(field.member))) return false;
                    known = true;
                    break;
                }
            }
            (void)known;  // unknown target.* fields are skipped (forward-compatible)
            continue;
        }
        if (inCandidate && key == "label") {
            current.label = value;
            continue;
        }
        if (inCandidate && key == "distance") {
            if (!parse_double(value, current.distance)) return false;
            continue;
        }
        // Unknown top-level field: skipped (forward-compatible).
    }

    if (inPreset) return false;  // truncated inside a preset block
    if (inCandidate && !finish_candidate()) return false;
    if (!sawMagic) return false;
    if (hasCount && declaredCount != result.candidates.size()) return false;

    session = std::move(result);
    return true;
}

}  // namespace

bool save_search_session(const SearchSession& session, const std::string& path) {
    try {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        return write_session_text(out, session);
    } catch (...) {
        return false;
    }
}

bool load_search_session(const std::string& path, SearchSession* session) {
    if (session == nullptr) return false;
    try {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return false;
        SearchSession loaded;
        if (!read_session_text(in, loaded)) return false;
        *session = std::move(loaded);
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace dve::audio

namespace dve::editor {

void PatchSearchBrowserPanel::set_session(const audio::SearchSession& session) {
    session_ = session;
    selected_ = 0;
    page_ = 0;
    status_ = session_.candidates.empty() ? "No candidates in session"
                                          : std::to_string(session_.candidates.size()) + " candidates loaded";
}

void PatchSearchBrowserPanel::set_target_name(std::string name) {
    session_.targetName = std::move(name);
}

void PatchSearchBrowserPanel::resize(int x, int y, int width, int height) noexcept {
    layout_.panel = {x, y, width, height};
    layout_.titleBar = {x, y, width, 30};
    layout_.closeButton = {x + width - 36, y + 4, 28, 22};
    layout_.targetNameField = {x + 10, y + 38, width - 20, 24};
    const int rowTop = y + 70;
    constexpr int rowHeight = 30;
    for (std::size_t i = 0; i < kVisibleCandidates; ++i) {
        const int ry = rowTop + static_cast<int>(i) * rowHeight;
        layout_.candidateRows[i] = {x + 10, ry, width - 20, rowHeight - 4};
    }
    const int listBottom = rowTop + static_cast<int>(kVisibleCandidates) * rowHeight;
    layout_.pagePrevButton = {x + 10, listBottom + 4, 64, 24};
    layout_.pageNextButton = {x + 80, listBottom + 4, 64, 24};
    layout_.pageLabel = {x + 150, listBottom + 4, width - 160, 24};
    layout_.auditionButton = {x + 10, y + height - 62, (width - 30) / 2, 28};
    layout_.promoteButton = {x + 20 + (width - 30) / 2, y + height - 62, (width - 30) / 2, 28};
    layout_.statusField = {x + 10, y + height - 28, width - 20, 22};
}

std::size_t PatchSearchBrowserPanel::page_count() const noexcept {
    const std::size_t pages =
        (session_.candidates.size() + kVisibleCandidates - 1) / kVisibleCandidates;
    return pages == 0 ? 1 : pages;
}

std::size_t PatchSearchBrowserPanel::page_first() const noexcept {
    const std::size_t first = page_ * kVisibleCandidates;
    return first > session_.candidates.size() ? session_.candidates.size() : first;
}

void PatchSearchBrowserPanel::promote_selected(audio::Synthesizer& synth) noexcept {
    try {
        if (session_.candidates.empty()) {
            status_ = "No candidates to promote";
            return;
        }
        const std::size_t index =
            selected_ < session_.candidates.size() ? selected_ : session_.candidates.size() - 1;
        const audio::SynthPreset promoted = audio::promote_candidate(session_.candidates[index]);

        std::string stem = session_.candidates[index].label;
        std::string safe;
        for (char c : stem) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                            c == '_' || c == '-' || c == '.';
            safe += ok ? c : '_';
        }
        if (safe.empty()) safe = "candidate";
        if (safe.size() > 64) safe.resize(64);

        std::error_code ec;
        std::filesystem::create_directories(promoteDirectory_, ec);
        const std::filesystem::path file = promoteDirectory_ / (safe + ".dvesynth");
        std::string error;
        if (!promoted.save(file, &error)) {
            status_ = "Promote failed: " + (error.empty() ? "could not write file" : error);
            return;
        }
        synth.set_preset(promoted);  // live immediately: playable from the keyboard
        status_ = "Promoted: " + file.filename().string();
    } catch (...) {
        status_ = "Promote failed";
    }
}

bool PatchSearchBrowserPanel::pointer_down(int px, int py, audio::Synthesizer& synth) noexcept {
    if (!open_) return false;
    if (!layout_.panel.contains(px, py)) return false;
    try {
        if (layout_.closeButton.contains(px, py)) {
            open_ = false;
            return true;
        }
        const std::size_t first = page_first();
        for (std::size_t i = 0; i < kVisibleCandidates; ++i) {
            if (layout_.candidateRows[i].contains(px, py)) {
                const std::size_t index = first + i;
                if (index < session_.candidates.size()) {
                    selected_ = index;
                    status_ = "Selected: " + session_.candidates[index].label;
                }
                return true;
            }
        }
        if (layout_.pagePrevButton.contains(px, py)) {
            if (page_ > 0) --page_;
            return true;
        }
        if (layout_.pageNextButton.contains(px, py)) {
            if (page_ + 1 < page_count()) ++page_;
            return true;
        }
        if (layout_.auditionButton.contains(px, py)) {
            if (!session_.candidates.empty()) {
                const std::size_t index =
                    selected_ < session_.candidates.size() ? selected_ : session_.candidates.size() - 1;
                if (auditionHook_) auditionHook_(synth, session_.candidates[index].preset);
                status_ = "Auditioning: " + session_.candidates[index].label +
                          " (PHASE6 STUB: live preset; evaluator wires in at merge)";
            } else {
                status_ = "No candidates to audition";
            }
            return true;
        }
        if (layout_.promoteButton.contains(px, py)) {
            promote_selected(synth);
            return true;
        }
        return true;  // click inside the panel is consumed
    } catch (...) {
        return true;
    }
}

bool PatchSearchBrowserPanel::pointer_up(int px, int py, audio::Synthesizer& synth) noexcept {
    (void)synth;
    if (!open_) return false;
    return layout_.panel.contains(px, py);
}

}  // namespace dve::editor
