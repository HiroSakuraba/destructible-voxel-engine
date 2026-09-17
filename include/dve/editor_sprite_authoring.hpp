#pragma once

#include "dve/sprite_authoring.hpp"
#include "dve/sprite_palette_authoring.hpp"
#include "dve/sprite_production_tools.hpp"
#include "dve/editor_viewport.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <system_error>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dve::editor {

struct SpriteDecodedImage {
    std::filesystem::path sourcePath;
    std::string mimeType;
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::byte> rgba8;

    [[nodiscard]] SpriteSourceImageView view() const noexcept {
        return {width, height, rgba8};
    }
    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

[[nodiscard]] bool decode_sprite_image(
    std::span<const std::byte> encoded,
    std::string_view mimeType,
    SpriteDecodedImage& out,
    std::string* error = nullptr);
[[nodiscard]] bool load_sprite_image(
    const std::filesystem::path& path,
    SpriteDecodedImage& out,
    std::string* error = nullptr);

struct SpriteCanvasViewState {
    float zoom{8.0F};
    SpriteVec2 panPixels{};
    bool showPixelGrid{true};
    bool showTrimBounds{true};
    bool showPivot{true};
    bool onionSkinPrevious{false};
    bool onionSkinNext{false};
    float onionSkinOpacity{0.35F};
};

struct SpriteTimelineState {
    std::string clip;
    std::vector<std::size_t> selectedSequenceIndices;
    std::optional<std::size_t> primarySequenceIndex;
    std::size_t firstVisibleSequenceIndex{};
    float playheadSeconds{};
    bool playing{true};
    bool loopPreview{true};
};

struct SpriteAtlasPackSettings {
    std::uint32_t maximumWidth{1024U};
    std::uint32_t paddingPixels{1U};
    bool powerOfTwo{false};
    bool extrudeEdges{true};
};

struct SpriteAtlasPackResult {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::byte> rgba8;
    std::vector<SpriteRectPixels> frameRects;
};

struct SpriteIndexedAtlasPublishSettings {
    SpritePaletteAsset palette;
    SpritePaletteIndexSettings indexing{};
    std::filesystem::path palettePath;
    // Stored in the cooked .dvesprite. When empty, a path relative to cookedAssetPath is used.
    std::string paletteAssetReference;
};

struct SpriteAtlasPublishSettings {
    SpriteAtlasPackSettings packing{};
    std::filesystem::path atlasPath;
    std::filesystem::path cookedAssetPath;
    std::filesystem::path dependencyManifestPath;
    // Stored in the cooked .dvesprite. When empty, a path relative to cookedAssetPath is used.
    std::string textureAssetReference;
    // When present, packed RGBA pixels are deterministically converted to one-byte indices and
    // the palette is published in the same recoverable transaction.
    std::optional<SpriteIndexedAtlasPublishSettings> indexedPalette;
    bool skipIfUnchanged{true};
};

struct SpriteAtlasPublishResult {
    std::uint64_t dependencyKey{};
    std::uint64_t atlasContentHash{};
    std::uint64_t cookedAssetHash{};
    std::uint64_t paletteContentHash{};
    SpritePaletteIndexDiagnostics paletteDiagnostics{};
    std::uint32_t atlasWidth{};
    std::uint32_t atlasHeight{};
    std::size_t frameCount{};
    bool upToDate{};
};

enum class SpriteSourceWatchState : std::uint8_t {
    Unchanged,
    Reloaded,
    Missing,
    Failed,
};

struct SpriteSourceWatchResult {
    SpriteSourceWatchState state{SpriteSourceWatchState::Unchanged};
    std::filesystem::path sourcePath;
    std::string message;
};

[[nodiscard]] bool repack_sprite_atlas(
    const SpriteAsset& asset,
    const SpriteSourceImageView& source,
    const SpriteAtlasPackSettings& settings,
    SpriteAtlasPackResult& out,
    std::string* error = nullptr);

class SpriteAuthoringWorkspace {
public:
    SpriteAuthoringWorkspace();
    explicit SpriteAuthoringWorkspace(SpriteAsset asset);

    [[nodiscard]] SpriteAuthoringSession& session() noexcept { return session_; }
    [[nodiscard]] const SpriteAuthoringSession& session() const noexcept { return session_; }
    [[nodiscard]] const SpriteDecodedImage& source_image() const noexcept { return sourceImage_; }
    [[nodiscard]] bool has_source_image() const noexcept { return sourceImage_.width != 0U; }
    [[nodiscard]] SpriteCanvasViewState& canvas() noexcept { return canvas_; }
    [[nodiscard]] const SpriteCanvasViewState& canvas() const noexcept { return canvas_; }
    [[nodiscard]] SpriteTimelineState& timeline() noexcept { return timeline_; }
    [[nodiscard]] const SpriteTimelineState& timeline() const noexcept { return timeline_; }

    [[nodiscard]] bool open_asset(
        const std::filesystem::path& assetPath,
        const std::filesystem::path& projectRoot = {},
        std::string* error = nullptr);
    [[nodiscard]] bool create_from_texture(
        const std::filesystem::path& sourcePath,
        std::string assetName,
        std::string textureAsset,
        float pixelsPerWorldUnit,
        std::string* error = nullptr);
    [[nodiscard]] bool reimport_source(bool preserveSelection = true,
                                       std::string* error = nullptr);
    // Saves a valid authoring asset. In-memory atlas previews are converted back to source-space
    // frame rectangles because publishing the packed texture belongs to the cook pipeline.
    [[nodiscard]] bool save_asset(const std::filesystem::path& path,
                                  std::string* error = nullptr) const;
    [[nodiscard]] bool set_source_image(SpriteDecodedImage image,
                                        bool preserveSelection = true,
                                        std::string* error = nullptr);

    [[nodiscard]] bool slice_grid(const SpriteGridSliceSettings& settings,
                                  std::string* error = nullptr);
    [[nodiscard]] bool slice_freeform(std::span<const SpriteSliceSpec> slices,
                                      bool trimTransparent = true,
                                      std::uint8_t alphaThreshold = 0U,
                                      bool skipTransparent = true,
                                      std::string* error = nullptr);

    void zoom_at(float factor, SpriteVec2 canvasPoint) noexcept;
    void pan_by(SpriteVec2 deltaPixels) noexcept;
    void reset_view() noexcept;

    [[nodiscard]] bool select_timeline_frames(std::span<const std::size_t> sequenceIndices,
                                              std::optional<std::size_t> primary = std::nullopt) noexcept;
    [[nodiscard]] bool reorder_timeline_selection(std::size_t destinationIndex,
                                                  std::string* error = nullptr);
    [[nodiscard]] bool set_selected_frame_duration(float seconds,
                                                   std::string* error = nullptr);
    [[nodiscard]] bool set_selected_frame_event(std::string event,
                                                std::string* error = nullptr);
    [[nodiscard]] bool set_selected_frame_pivot(SpriteVec2 pivotPixels,
                                                std::string* error = nullptr);
    [[nodiscard]] bool inset_selected_frame_trim(int pixels,
                                                 std::string* error = nullptr);
    [[nodiscard]] bool set_clip_playback(SpriteLoopMode loopMode, float playbackRate,
                                         std::string* error = nullptr);

    [[nodiscard]] bool repack_atlas(const SpriteAtlasPackSettings& settings,
                                    SpriteAtlasPackResult& out,
                                    std::string* error = nullptr);
    [[nodiscard]] bool publish_packed_atlas(const SpriteAtlasPublishSettings& settings,
                                            SpriteAtlasPublishResult& out,
                                            std::string* error = nullptr) const;

    void set_auto_reimport(bool enabled) noexcept { autoReimport_ = enabled; }
    [[nodiscard]] bool auto_reimport() const noexcept { return autoReimport_; }
    void set_source_watch_interval(float seconds) noexcept;
    [[nodiscard]] float source_watch_interval() const noexcept { return sourceWatchIntervalSeconds_; }
    [[nodiscard]] SpriteSourceWatchResult update_source_watch(float elapsedSeconds);
    [[nodiscard]] SpriteSourceWatchResult poll_source_change(bool force = false);

private:
    void normalize_timeline_selection() noexcept;
    void capture_source_frame_rects();
    [[nodiscard]] bool restore_unpacked_source(std::string* error);
    [[nodiscard]] const SpriteClip* selected_clip() const noexcept;

    SpriteAuthoringSession session_;
    // sourceImage_ is the image currently displayed/consumed by the authored atlas layout.
    // importSourceImage_ remains the original texture so deterministic reimport still works after
    // an in-memory atlas repack.
    SpriteDecodedImage sourceImage_;
    SpriteDecodedImage importSourceImage_;
    std::vector<SpriteRectPixels> sourceFrameRects_;
    std::optional<SpriteAtlasPackSettings> activePackSettings_;
    SpriteCanvasViewState canvas_;
    SpriteTimelineState timeline_;
    bool autoReimport_{true};
    float sourceWatchIntervalSeconds_{0.5F};
    float sourceWatchElapsedSeconds_{};
    std::filesystem::file_time_type sourceWriteTime_{};
    std::uintmax_t sourceFileSize_{};
    bool sourceWatchSignatureValid_{};
};


struct SpriteAuthoringPanelFrame {
    UiRect panel{};
    UiRect titleBar{};
    UiRect closeButton{};
    UiRect saveButton{};
    UiRect reimportButton{};
    UiRect gridSliceButton{};
    UiRect freeSliceButton{};
    UiRect repackButton{};
    UiRect playButton{};
    UiRect previousButton{};
    UiRect nextButton{};
    UiRect loopButton{};
    UiRect gridButton{};
    UiRect onionPreviousButton{};
    UiRect onionNextButton{};
    UiRect canvas{};
    UiRect timeline{};
    UiRect inspector{};
    UiRect statusBar{};
    std::vector<UiRect> timelineFrames;
    std::vector<std::size_t> timelineFrameIndices;
    UiRect sliceTabButton{};
    UiRect paletteTabButton{};
    UiRect tracksTabButton{};
    bool paletteMode{};
    bool tracksMode{};
    UiRect cellWidthDown{};
    UiRect cellWidthUp{};
    UiRect cellHeightDown{};
    UiRect cellHeightUp{};
    UiRect durationDown{};
    UiRect durationUp{};
    UiRect pivotXDown{};
    UiRect pivotXUp{};
    UiRect pivotYDown{};
    UiRect pivotYUp{};
    UiRect trimInButton{};
    UiRect trimOutButton{};
    UiRect trimTransparentButton{};
    UiRect skipTransparentButton{};
    UiRect palettePanel{};
    UiRect paletteSaveButton{};
    UiRect palettePreviousBankButton{};
    UiRect paletteNextBankButton{};
    UiRect paletteAddBankButton{};
    UiRect paletteRemoveBankButton{};
    UiRect palettePlayButton{};
    UiRect paletteAddCycleButton{};
    UiRect paletteRemoveCycleButton{};
    UiRect paletteTransparentButton{};
    std::array<UiRect, 4> paletteChannelDown{};
    std::array<UiRect, 4> paletteChannelUp{};
    UiRect paletteCycleFirstDown{};
    UiRect paletteCycleFirstUp{};
    UiRect paletteCycleLastDown{};
    UiRect paletteCycleLastUp{};
    UiRect paletteCycleTicksDown{};
    UiRect paletteCycleTicksUp{};
    UiRect paletteSwatchArea{};
    std::vector<UiRect> paletteSwatches;
    std::vector<std::size_t> paletteSwatchIndices;
    UiRect trackAddHitboxButton{};
    UiRect trackAddHurtboxButton{};
    UiRect trackAddSocketButton{};
    UiRect trackAddPropertyButton{};
    UiRect trackAddSpawnButton{};
    UiRect trackSetRootMotionButton{};
    UiRect trackDeleteButton{};
    UiRect trackDuplicateButton{};
    UiRect trackMirrorButton{};
    UiRect trackCopyRangeButton{};
    UiRect trackPasteRangeButton{};
    UiRect trackRetimeButton{};
    UiRect trackPresetButton{};
    std::vector<UiRect> trackNumericDown;
    std::vector<UiRect> trackNumericUp;
    std::vector<SpriteTrackNumericDescriptor> trackNumericDescriptors;
    std::string trackSearchQuery;
    std::vector<UiRect> trackRows;
    std::vector<SpriteTrackItemKind> trackRowKinds;
    std::vector<SpriteTrackId> trackRowIds;
    std::vector<UiRect> timelineCombatMarkers;
    std::vector<UiRect> timelineSocketMarkers;
    std::vector<UiRect> timelinePropertyMarkers;
    std::vector<UiRect> timelineRootMarkers;
    std::optional<UiRect> freeSlicePreview;
};

// Platform-neutral native sprite authoring panel. It deliberately owns no GPU resources:
// the source image is decoded to RGBA8 on the CPU and the native canvas renders a bounded
// nearest-neighbour preview. The same workspace remains usable by richer SDL/Qt hosts.
class EditorSpriteAuthoringPanel {
public:
    [[nodiscard]] bool open() const noexcept { return open_; }
    void close() noexcept;
    void toggle() noexcept;
    void resize(int width, int height) noexcept;
    void update(float elapsedSeconds) noexcept;

    [[nodiscard]] bool open_texture(const std::filesystem::path& sourcePath,
                                    const std::filesystem::path& assetPath = {},
                                    std::string* error = nullptr);
    [[nodiscard]] bool open_asset(const std::filesystem::path& assetPath,
                                  const std::filesystem::path& projectRoot = {},
                                  std::string* error = nullptr);
    [[nodiscard]] bool save(std::string* error = nullptr);

    [[nodiscard]] SpriteAuthoringWorkspace& workspace() noexcept { return workspace_; }
    [[nodiscard]] const SpriteAuthoringWorkspace& workspace() const noexcept { return workspace_; }
    [[nodiscard]] const std::filesystem::path& asset_path() const noexcept { return assetPath_; }
    [[nodiscard]] const SpriteGridSliceSettings& grid_settings() const noexcept { return gridSettings_; }
    [[nodiscard]] bool free_slice_mode() const noexcept { return freeSliceMode_; }
    [[nodiscard]] bool has_palette_document() const noexcept { return paletteOpen_; }
    [[nodiscard]] bool palette_inspector_mode() const noexcept { return paletteInspectorMode_; }
    [[nodiscard]] bool tracks_inspector_mode() const noexcept { return tracksInspectorMode_; }
    [[nodiscard]] bool palette_texture_is_index_transport() const noexcept {
        return paletteTextureIsIndexTransport_;
    }
    [[nodiscard]] SpritePaletteAuthoringSession& palette_session() noexcept { return paletteSession_; }
    [[nodiscard]] const SpritePaletteAuthoringSession& palette_session() const noexcept { return paletteSession_; }
    [[nodiscard]] const std::string& status() const noexcept { return status_; }
    [[nodiscard]] SpriteAuthoringPanelFrame frame() const;

    [[nodiscard]] bool pointer_move(int x, int y, std::uint32_t modifiers = 0U);
    [[nodiscard]] bool pointer_down(int button, int x, int y, std::uint32_t modifiers = 0U);
    [[nodiscard]] bool pointer_up(int button, int x, int y, std::uint32_t modifiers = 0U);
    [[nodiscard]] bool pointer_wheel(float steps, int x, int y, std::uint32_t modifiers = 0U);
    [[nodiscard]] bool key_down(std::string_view key, bool control, bool shift, bool alt);

private:
    enum class Capture : std::uint8_t { NoCapture, Pan, FreeSlice, TimelineReorder, TrackHandle };
    [[nodiscard]] std::optional<SpriteVec2> canvas_to_source(int x, int y) const noexcept;
    [[nodiscard]] std::optional<SpriteVec2> canvas_to_track_local(int x, int y) const noexcept;
    [[nodiscard]] std::optional<SpriteTrackHandleKind> hit_test_track_handle(int x, int y) const noexcept;
    [[nodiscard]] bool create_free_slice(SpriteVec2 start, SpriteVec2 end, std::string* error);
    [[nodiscard]] bool open_linked_palette(const std::filesystem::path& projectRoot,
                                           std::string* error);
    [[nodiscard]] bool edit_palette_channel(std::size_t channel, int delta,
                                            std::string* error);
    [[nodiscard]] bool edit_selected_cycle(int field, int delta, std::string* error);
    [[nodiscard]] bool nudge_selected_track(int axis, float delta, bool resize, bool rotate,
                                            std::string* error);
    void select_relative_frame(int direction) noexcept;
    void keep_timeline_primary_visible() noexcept;
    void set_status(std::string value) { status_ = std::move(value); }

    bool open_{};
    int width_{1280};
    int height_{800};
    SpriteAuthoringWorkspace workspace_{};
    std::filesystem::path assetPath_{};
    SpriteGridSliceSettings gridSettings_{};
    SpriteAtlasPackSettings packSettings_{};
    SpritePaletteAuthoringSession paletteSession_{};
    bool paletteOpen_{};
    bool paletteInspectorMode_{};
    bool tracksInspectorMode_{};
    bool paletteTextureIsIndexTransport_{};
    std::size_t paletteFirstVisibleColor_{};
    double palettePreviewTickRemainder_{};
    float paletteWatchElapsedSeconds_{};
    bool freeSliceMode_{};
    Capture capture_{Capture::NoCapture};
    int pointerX_{};
    int pointerY_{};
    int pointerDownX_{};
    int pointerDownY_{};
    SpriteVec2 captureSourceStart_{};
    SpriteVec2 panStart_{};
    SpriteVec2 canvasPanStart_{};
    std::optional<std::size_t> timelineDragIndex_{};
    SpriteTrackClipboard trackClipboard_{};
    std::string trackSearchQuery_{};
    bool trackSearchEditing_{};
    std::optional<SpriteTrackHandleKind> trackCaptureHandle_{};
    SpriteVec2 trackCaptureLocalStart_{};
    std::string status_{"Open a PNG, JPEG, or .dvesprite asset"};
};

} // namespace dve::editor
