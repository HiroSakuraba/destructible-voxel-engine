#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <thread>
#include <vector>

struct SDL_Gamepad { SDL_JoystickID id{}; };
struct SDL_AudioStream {
    SDL_AudioSpec spec{};
    SDL_AudioStreamCallback callback{};
    void* userdata{};
    bool resumed{};
    bool recording{};
    std::vector<float> data;
    std::vector<float> inputData;
    std::size_t inputCursor{};
};

struct SDL_Window {
    int logicalW{};
    int logicalH{};
    int pixelW{};
    int pixelH{};
    float scale{1.0F};
    SDL_WindowFlags flags{SDL_WINDOW_INPUT_FOCUS};
    SDL_WindowID id{1};
    std::string title;
};

namespace {
std::string errorText;
std::string clipboard;
std::deque<SDL_Event> events;
SDL_Keymod modState{};
int nextLogicalW = 1280;
int nextLogicalH = 800;
int nextPixelW = 1280;
int nextPixelH = 800;
float nextScale = 1.0F;
std::string dialogPath;
std::string dialogError;
const auto epoch = std::chrono::steady_clock::now();
SDL_AudioStream* activePlaybackStream = nullptr;
SDL_AudioStream* activeRecordingStream = nullptr;

void show_dialog(SDL_DialogFileCallback callback, void* userdata) {
    if (!dialogError.empty()) {
        errorText = dialogError;
        callback(userdata, nullptr, -1);
        return;
    }
    if (dialogPath.empty()) {
        const char* const cancelled[] = {nullptr};
        callback(userdata, cancelled, -1);
        return;
    }
    const char* const chosen[] = {dialogPath.c_str(), nullptr};
    callback(userdata, chosen, 0);
}
}

bool SDL_Init(Uint32) { return true; }
void SDL_QuitSubSystem(Uint32) {}
const char* SDL_GetError() { return errorText.c_str(); }
SDL_Window* SDL_CreateWindow(const char* title, int w, int h, SDL_WindowFlags flags) {
    auto* window = new SDL_Window;
    window->logicalW = w > 0 ? w : nextLogicalW;
    window->logicalH = h > 0 ? h : nextLogicalH;
    window->pixelW = nextPixelW == 1280 ? window->logicalW : nextPixelW;
    window->pixelH = nextPixelH == 800 ? window->logicalH : nextPixelH;
    window->scale = nextScale;
    window->flags = flags | SDL_WINDOW_INPUT_FOCUS;
    window->title = title ? title : "";
    return window;
}
void SDL_DestroyWindow(SDL_Window* window) { delete window; }
bool SDL_ShowWindow(SDL_Window* window) { if (!window) return false; window->flags &= ~SDL_WINDOW_HIDDEN; return true; }
bool SDL_StartTextInput(SDL_Window*) { return true; }
bool SDL_StopTextInput(SDL_Window*) { return true; }
bool SDL_PollEvent(SDL_Event* event) {
    if (!event || events.empty()) return false;
    *event = events.front();
    events.pop_front();
    if (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP) modState = event->key.mod;
    return true;
}
bool SDL_GetWindowSize(SDL_Window* window, int* w, int* h) {
    if (!window) return false; if (w) *w = window->logicalW; if (h) *h = window->logicalH; return true;
}
bool SDL_GetWindowSizeInPixels(SDL_Window* window, int* w, int* h) {
    if (!window) return false; if (w) *w = window->pixelW; if (h) *h = window->pixelH; return true;
}
float SDL_GetWindowDisplayScale(SDL_Window* window) { return window ? window->scale : 0.0F; }
SDL_WindowFlags SDL_GetWindowFlags(SDL_Window* window) { return window ? window->flags : 0; }
SDL_WindowID SDL_GetWindowID(SDL_Window* window) { return window ? window->id : 0; }
bool SDL_SetWindowTitle(SDL_Window* window, const char* title) { if (!window) return false; window->title = title ? title : ""; return true; }
bool SDL_SetClipboardText(const char* text) { clipboard = text ? text : ""; return true; }
char* SDL_GetClipboardText() {
    auto* result = static_cast<char*>(std::malloc(clipboard.size() + 1U));
    if (!result) return nullptr;
    std::memcpy(result, clipboard.c_str(), clipboard.size() + 1U);
    return result;
}
void SDL_free(void* pointer) { std::free(pointer); }
Uint64 SDL_GetTicksNS() { return static_cast<Uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - epoch).count()); }
void SDL_Delay(Uint32 milliseconds) { std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds)); }
const char* SDL_GetKeyName(SDL_Keycode key) {
    static char one[2]{};
    if (key >= 32 && key < 127) { one[0] = static_cast<char>(key); one[1] = 0; return one; }
    return "Unknown";
}
SDL_Keymod SDL_GetModState() { return modState; }
SDL_Gamepad* SDL_OpenGamepad(SDL_JoystickID instance_id) { return new SDL_Gamepad{instance_id}; }
void SDL_CloseGamepad(SDL_Gamepad* gamepad) { delete gamepad; }

void SDL_ShowOpenFileDialog(SDL_DialogFileCallback callback, void* userdata, SDL_Window*, const SDL_DialogFileFilter*, int, const char*, bool) { show_dialog(callback, userdata); }
void SDL_ShowSaveFileDialog(SDL_DialogFileCallback callback, void* userdata, SDL_Window*, const SDL_DialogFileFilter*, int, const char*) { show_dialog(callback, userdata); }
void SDL_ShowOpenFolderDialog(SDL_DialogFileCallback callback, void* userdata, SDL_Window*, const char*, bool) { show_dialog(callback, userdata); }

void SDLTest_Reset() {
    errorText.clear(); clipboard.clear(); events.clear(); modState = 0;
    nextLogicalW = 1280; nextLogicalH = 800; nextPixelW = 1280; nextPixelH = 800; nextScale = 1.0F;
    dialogPath.clear(); dialogError.clear();
    if (activePlaybackStream) activePlaybackStream->data.clear();
    if (activeRecordingStream) { activeRecordingStream->inputData.clear(); activeRecordingStream->inputCursor = 0U; }
}
void SDLTest_PushEvent(const SDL_Event* event) { if (event) events.push_back(*event); }
void SDLTest_SetWindowMetrics(int logicalW, int logicalH, int pixelW, int pixelH, float scale) {
    nextLogicalW = logicalW; nextLogicalH = logicalH; nextPixelW = pixelW; nextPixelH = pixelH; nextScale = scale;
}
void SDLTest_SetDialogResult(const char* firstPath, const char* error) {
    dialogPath = firstPath ? firstPath : ""; dialogError = error ? error : "";
}

struct SDL_Renderer { SDL_Window* window{}; Uint8 r{}, g{}, b{}, a{255}; };
SDL_Renderer* SDL_CreateRenderer(SDL_Window* window, const char*) { if (!window) return nullptr; return new SDL_Renderer{window}; }
void SDL_DestroyRenderer(SDL_Renderer* renderer) { delete renderer; }
bool SDL_SetRenderDrawColor(SDL_Renderer* renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a) { if (!renderer) return false; renderer->r=r; renderer->g=g; renderer->b=b; renderer->a=a; return true; }
bool SDL_RenderClear(SDL_Renderer* renderer) { return renderer != nullptr; }
bool SDL_RenderPresent(SDL_Renderer* renderer) { return renderer != nullptr; }
bool SDL_RenderFillRect(SDL_Renderer* renderer, const SDL_FRect*) { return renderer != nullptr; }
bool SDL_RenderRect(SDL_Renderer* renderer, const SDL_FRect*) { return renderer != nullptr; }
bool SDL_RenderLine(SDL_Renderer* renderer, float, float, float, float) { return renderer != nullptr; }
bool SDL_RenderDebugText(SDL_Renderer* renderer, float, float, const char*) { return renderer != nullptr; }

SDL_AudioStream* SDL_OpenAudioDeviceStream(SDL_AudioDeviceID device, const SDL_AudioSpec* spec,
                                           SDL_AudioStreamCallback callback, void* userdata) {
    if (!spec || spec->format != SDL_AUDIO_F32 || (spec->channels != 1 && spec->channels != 2) || spec->freq <= 0) {
        errorText = "invalid audio spec"; return nullptr;
    }
    auto* stream = new SDL_AudioStream;
    stream->spec = *spec;
    stream->callback = callback;
    stream->userdata = userdata;
    stream->recording = device == SDL_AUDIO_DEVICE_DEFAULT_RECORDING || device == 101U || device == 102U;
    if (stream->recording) activeRecordingStream = stream;
    else activePlaybackStream = stream;
    return stream;
}
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream* stream) {
    if (!stream) return false;
    stream->resumed = true;
    if (!stream->recording && stream->callback) stream->callback(stream->userdata, stream, 4096, 4096);
    return true;
}
bool SDL_PutAudioStreamData(SDL_AudioStream* stream, const void* buf, int len) {
    if (!stream || !buf || stream->recording || len < 0 || len % static_cast<int>(sizeof(float)) != 0) return false;
    const auto* samples = static_cast<const float*>(buf);
    stream->data.insert(stream->data.end(), samples, samples + len / static_cast<int>(sizeof(float))); return true;
}
int SDL_GetAudioStreamData(SDL_AudioStream* stream, void* buf, int len) {
    if (!stream || !buf || !stream->recording || len < 0 || len % static_cast<int>(sizeof(float)) != 0) return -1;
    const std::size_t requested = static_cast<std::size_t>(len) / sizeof(float);
    const std::size_t available = stream->inputData.size() - stream->inputCursor;
    const std::size_t count = std::min(requested, available);
    if (count > 0U) {
        std::memcpy(buf, stream->inputData.data() + stream->inputCursor, count * sizeof(float));
        stream->inputCursor += count;
        if (stream->inputCursor == stream->inputData.size()) {
            stream->inputData.clear(); stream->inputCursor = 0U;
        }
    }
    return static_cast<int>(count * sizeof(float));
}
int SDL_GetAudioStreamAvailable(SDL_AudioStream* stream) {
    if (!stream || !stream->recording) return -1;
    return static_cast<int>((stream->inputData.size() - stream->inputCursor) * sizeof(float));
}
SDL_AudioDeviceID* SDL_GetAudioRecordingDevices(int* count) {
    if (count) *count = 2;
    auto* devices = static_cast<SDL_AudioDeviceID*>(std::malloc(2U * sizeof(SDL_AudioDeviceID)));
    if (!devices) { if (count) *count = 0; return nullptr; }
    devices[0] = 101U; devices[1] = 102U; return devices;
}
const char* SDL_GetAudioDeviceName(SDL_AudioDeviceID device) {
    if (device == SDL_AUDIO_DEVICE_DEFAULT_RECORDING) return "Fake default microphone";
    if (device == 101U) return "Fake studio microphone";
    if (device == 102U) return "Fake line input";
    if (device == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK) return "Fake speakers";
    return "Fake audio device";
}
bool SDL_GetAudioDeviceFormat(SDL_AudioDeviceID device, SDL_AudioSpec* spec, int* sampleFrames) {
    if (!spec) return false;
    spec->format = SDL_AUDIO_F32;
    spec->channels = device == 102U ? 1 : 2;
    spec->freq = device == 102U ? 44100 : 48000;
    if (sampleFrames) *sampleFrames = 256;
    return true;
}
void SDL_DestroyAudioStream(SDL_AudioStream* stream) {
    if (activePlaybackStream == stream) activePlaybackStream = nullptr;
    if (activeRecordingStream == stream) activeRecordingStream = nullptr;
    delete stream;
}
void SDLTest_RequestAudio(int additionalBytes) {
    if (activePlaybackStream && activePlaybackStream->resumed && activePlaybackStream->callback)
        activePlaybackStream->callback(activePlaybackStream->userdata, activePlaybackStream, additionalBytes, additionalBytes);
}
void SDLTest_PushRecordingData(const float* samples, std::size_t sampleCount) {
    if (!activeRecordingStream || !activeRecordingStream->resumed || !samples || sampleCount == 0U) return;
    activeRecordingStream->inputData.insert(activeRecordingStream->inputData.end(), samples, samples + sampleCount);
    if (activeRecordingStream->callback) {
        const int bytes = static_cast<int>(sampleCount * sizeof(float));
        activeRecordingStream->callback(activeRecordingStream->userdata, activeRecordingStream, bytes, bytes);
    }
}
const float* SDLTest_AudioData(std::size_t* sampleCount) {
    if (sampleCount) *sampleCount = activePlaybackStream ? activePlaybackStream->data.size() : 0U;
    return activePlaybackStream && !activePlaybackStream->data.empty() ? activePlaybackStream->data.data() : nullptr;
}
