// Phase 6 (SYN-016) audio-to-synth search: candidate browser.
//
// Worker D deliverable. Session persistence, candidate ranking/promotion,
// and the editor Search panel.
//
// The browser consumes AudioFeatureVector (audio_features.hpp, owned by the
// Phase 6 coordinator) only as stored target data; it never calls
// extract_audio_features()/feature_distance() — the extractor (Worker A) and
// the offline render evaluator (Worker B) live in other workers' trees.
//
// Session file format (".dvesearch", text, human-inspectable,
// forward-compatible — the loader skips unknown fields):
//
//   # comment lines start with '#'
//   dve_search_session=1
//   target_name=<name>            (newlines stripped on save)
//   search_seed=<uint64>
//   generations_run=<int>
//   target.rms=<double>           (%.17g via std::to_chars; all 14 scalars)
//   ...
//   target.mel_bands=<16 comma-separated doubles>
//   candidate_count=<n>
//   [candidate]
//   label=<label>
//   distance=<double>
//   [dve_search_preset_begin]
//   <SynthPreset::serialize() output, verbatim>
//   [dve_search_preset_end]
//   ... repeated per candidate ...
//
// Loader rules: unknown key=value lines are skipped; every [candidate] block
// must carry a complete preset block that SynthPreset::parse() accepts;
// candidate_count, when present, must match the number of candidate blocks;
// any structural problem (truncation, bad number, unparseable preset) makes
// load_search_session() return false. Distances are written with
// std::to_chars (shortest round-trip) so they reload bit-identically, and
// candidate order is preserved.
//
// Editor note: the Search panel below is self-contained (own layout rects,
// own state). It deliberately does NOT grow any fixed-size header arrays in
// editor_synth.hpp (Phase 4 ABI lesson); the host editor wires it in like
// EditorSynthPanel (open/resize/pointer_down + layout() for rendering).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "dve/audio/audio_features.hpp"
#include "dve/audio/patch_search.hpp"
#include "dve/audio/synthesizer.hpp"
#include "dve/editor_viewport.hpp"

namespace dve::audio {

// A ranked search result: a candidate preset plus its feature distance to the
// search target (lower = closer match).
struct BrowserCandidate {
    SynthPreset preset;
    double distance = 1e9;
    std::string label;
};

// One audio-to-synth search run: the target features plus ranked candidates.
struct SearchSession {
    std::string targetName;
    AudioFeatureVector target{};
    std::vector<BrowserCandidate> candidates;  // sorted best-first
    std::uint64_t searchSeed = 0;
    int generationsRun = 0;
};

// Session persistence; see the file-format comment above.
[[nodiscard]] bool save_search_session(const SearchSession& session, const std::string& path);
[[nodiscard]] bool load_search_session(const std::string& path, SearchSession* session);

// Returns the candidate's preset, validated and ready to save as a normal
// preset. If the candidate's preset fails validation, returns a
// default-constructed preset — documented fallback; this function never
// returns an invalid preset.
[[nodiscard]] SynthPreset promote_candidate(const BrowserCandidate& candidate);

// Best-first, stable (equal distances keep insertion order).
void sort_candidates(SearchSession& session);
// Sorts best-first, then keeps only the first n (no-op when n >= size).
void keep_top_n(SearchSession& session, std::size_t n);

// Bridge from the evolutionary search (Worker C) to the browser: converts
// ranked search results into labeled browser candidates, preserving order.
// `ranked` is expected best-first, as evolutionary_patch_search() returns.
[[nodiscard]] inline std::vector<BrowserCandidate> browser_candidates_from_ranked(
    const std::vector<RankedPatch>& ranked, const std::string& labelPrefix = "candidate") {
    std::vector<BrowserCandidate> out;
    out.reserve(ranked.size());
    for (std::size_t i = 0; i < ranked.size(); ++i) {
        BrowserCandidate c;
        c.preset = ranked[i].preset;
        c.distance = ranked[i].distance;
        c.label = labelPrefix + " " + std::to_string(i + 1);
        out.push_back(std::move(c));
    }
    return out;
}

// Default audition hook: makes the candidate the live preset so it can be
// played and heard immediately through the normal synth path. A different
// audition behavior (e.g. offline-render-then-play via the patch evaluator)
// can be injected with PatchSearchBrowserPanel::set_audition_hook; session
// and browser logic never depend on this function's internals.
inline void audition_preset_live(Synthesizer& synth, const SynthPreset& preset) {
    synth.set_preset(preset);
}

// Back-compat alias for the Phase 6 working name.
inline void stub_audition_preset(Synthesizer& synth, const SynthPreset& preset) {
    audition_preset_live(synth, preset);
}

}  // namespace dve::audio

namespace dve::editor {

// Self-contained Search panel for the synth editor (Phase 6 candidate
// browser UI). Follows the EditorSynthPanel interaction patterns (UiRect
// layout, open/resize/pointer_down, button-driven, no text entry widgets):
// target-name display field, candidate list showing label + distance,
// Audition button (routes through the audition hook, defaulting to
// audio::audition_preset_live), Promote button
// (audio::promote_candidate -> saved as .dvesynth and made the live preset).
class PatchSearchBrowserPanel {
public:
    static constexpr std::size_t kVisibleCandidates = 8;

    struct Layout {
        UiRect panel{};
        UiRect titleBar{};
        UiRect closeButton{};
        UiRect targetNameField{};
        std::array<UiRect, kVisibleCandidates> candidateRows{};
        UiRect pagePrevButton{};
        UiRect pageNextButton{};
        UiRect pageLabel{};
        UiRect auditionButton{};
        UiRect promoteButton{};
        UiRect statusField{};
    };

    [[nodiscard]] bool open() const noexcept { return open_; }
    void set_open(bool openValue) noexcept { open_ = openValue; }
    void toggle() noexcept { open_ = !open_; }

    void set_session(const audio::SearchSession& session);
    [[nodiscard]] const audio::SearchSession& session() const noexcept { return session_; }
    void set_target_name(std::string name);

    // Audition hook: invoked as hook(synth, selectedPreset) when the Audition
    // button is clicked. Defaults to audio::audition_preset_live.
    void set_audition_hook(std::function<void(audio::Synthesizer&, const audio::SynthPreset&)> hook) {
        auditionHook_ = std::move(hook);
    }

    void set_promote_directory(std::filesystem::path directory) { promoteDirectory_ = std::move(directory); }
    [[nodiscard]] std::string_view status() const noexcept { return status_; }
    [[nodiscard]] std::size_t selected_candidate() const noexcept { return selected_; }
    [[nodiscard]] const Layout& layout() const noexcept { return layout_; }
    // Visible slice of candidates for the current page (for renderers).
    [[nodiscard]] std::size_t visible_first() const noexcept { return page_first(); }
    [[nodiscard]] std::size_t visible_count() const noexcept {
        const std::size_t n = session_.candidates.size();
        const std::size_t first = page_first();
        return first >= n ? 0 : std::min(kVisibleCandidates, n - first);
    }
    [[nodiscard]] std::string page_text() const {
        return std::to_string(page_ + 1) + "/" + std::to_string(page_count());
    }

    // Positions the panel at (x, y) with the given size; recomputes rows.
    void resize(int x, int y, int width, int height) noexcept;
    // Returns true when the click was inside the panel (consumed).
    bool pointer_down(int px, int py, audio::Synthesizer& synth) noexcept;
    bool pointer_up(int px, int py, audio::Synthesizer& synth) noexcept;

private:
    void promote_selected(audio::Synthesizer& synth) noexcept;
    [[nodiscard]] std::size_t page_count() const noexcept;
    [[nodiscard]] std::size_t page_first() const noexcept;

    Layout layout_{};
    bool open_{false};
    audio::SearchSession session_{};
    std::size_t selected_{0};
    std::size_t page_{0};
    std::function<void(audio::Synthesizer&, const audio::SynthPreset&)> auditionHook_{
        audio::audition_preset_live};
    std::filesystem::path promoteDirectory_{"assets/audio/presets/search"};
    std::string status_{"No search session loaded"};
};

}  // namespace dve::editor
