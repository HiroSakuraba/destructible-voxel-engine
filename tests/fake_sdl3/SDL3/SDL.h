#pragma once

#include <cstddef>
#include <cstdint>

#define SDLCALL
using Uint8 = std::uint8_t;
using Uint16 = std::uint16_t;
using Uint32 = std::uint32_t;
using Sint16 = std::int16_t;
using Sint32 = std::int32_t;
using Uint64 = std::uint64_t;
using SDL_Keycode = std::int32_t;
using SDL_Keymod = std::uint32_t;
using SDL_WindowFlags = std::uint64_t;
using SDL_WindowID = std::uint32_t;
using SDL_JoystickID = std::int32_t;

struct SDL_Window;
struct SDL_Gamepad;

constexpr Uint32 SDL_INIT_VIDEO = 1U << 0U;
constexpr Uint32 SDL_INIT_GAMEPAD = 1U << 1U;
constexpr Uint32 SDL_INIT_AUDIO = 1U << 2U;
constexpr SDL_WindowFlags SDL_WINDOW_RESIZABLE = 1ULL << 0U;
constexpr SDL_WindowFlags SDL_WINDOW_HIGH_PIXEL_DENSITY = 1ULL << 1U;
constexpr SDL_WindowFlags SDL_WINDOW_HIDDEN = 1ULL << 2U;
constexpr SDL_WindowFlags SDL_WINDOW_INPUT_FOCUS = 1ULL << 3U;
constexpr SDL_WindowFlags SDL_WINDOW_MINIMIZED = 1ULL << 4U;

constexpr SDL_Keymod SDL_KMOD_NONE = 0;
constexpr SDL_Keymod SDL_KMOD_SHIFT = 1U << 0U;
constexpr SDL_Keymod SDL_KMOD_CTRL = 1U << 1U;
constexpr SDL_Keymod SDL_KMOD_ALT = 1U << 2U;
constexpr SDL_Keymod SDL_KMOD_GUI = 1U << 3U;
constexpr SDL_Keymod SDL_KMOD_CAPS = 1U << 4U;
constexpr SDL_Keymod SDL_KMOD_NUM = 1U << 5U;

constexpr Uint8 SDL_BUTTON_LEFT = 1;
constexpr Uint8 SDL_BUTTON_MIDDLE = 2;
constexpr Uint8 SDL_BUTTON_RIGHT = 3;
constexpr Uint8 SDL_BUTTON_X1 = 4;
constexpr Uint8 SDL_BUTTON_X2 = 5;

constexpr SDL_Keycode SDLK_ESCAPE = 27;
constexpr SDL_Keycode SDLK_TAB = 9;
constexpr SDL_Keycode SDLK_RETURN = 13;
constexpr SDL_Keycode SDLK_KP_ENTER = 1001;
constexpr SDL_Keycode SDLK_BACKSPACE = 8;
constexpr SDL_Keycode SDLK_DELETE = 127;
constexpr SDL_Keycode SDLK_SPACE = 32;
constexpr SDL_Keycode SDLK_LEFT = 1002;
constexpr SDL_Keycode SDLK_RIGHT = 1003;
constexpr SDL_Keycode SDLK_UP = 1004;
constexpr SDL_Keycode SDLK_DOWN = 1005;
constexpr SDL_Keycode SDLK_F1 = 1011;
constexpr SDL_Keycode SDLK_F2 = 1012;
constexpr SDL_Keycode SDLK_F5 = 1015;
constexpr SDL_Keycode SDLK_F6 = 1016;
constexpr SDL_Keycode SDLK_EQUALS = '=';
constexpr SDL_Keycode SDLK_PLUS = '+';
constexpr SDL_Keycode SDLK_MINUS = '-';
constexpr SDL_Keycode SDLK_LEFTBRACKET = '[';
constexpr SDL_Keycode SDLK_RIGHTBRACKET = ']';

constexpr Uint32 SDL_EVENT_QUIT = 0x100;
constexpr Uint32 SDL_EVENT_WINDOW_CLOSE_REQUESTED = 0x201;
constexpr Uint32 SDL_EVENT_WINDOW_RESIZED = 0x202;
constexpr Uint32 SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED = 0x203;
constexpr Uint32 SDL_EVENT_WINDOW_FOCUS_GAINED = 0x204;
constexpr Uint32 SDL_EVENT_WINDOW_FOCUS_LOST = 0x205;
constexpr Uint32 SDL_EVENT_KEY_DOWN = 0x300;
constexpr Uint32 SDL_EVENT_KEY_UP = 0x301;
constexpr Uint32 SDL_EVENT_TEXT_EDITING = 0x302;
constexpr Uint32 SDL_EVENT_TEXT_INPUT = 0x303;
constexpr Uint32 SDL_EVENT_MOUSE_MOTION = 0x400;
constexpr Uint32 SDL_EVENT_MOUSE_BUTTON_DOWN = 0x401;
constexpr Uint32 SDL_EVENT_MOUSE_BUTTON_UP = 0x402;
constexpr Uint32 SDL_EVENT_MOUSE_WHEEL = 0x403;
constexpr Uint32 SDL_EVENT_DROP_FILE = 0x500;
constexpr Uint32 SDL_EVENT_GAMEPAD_AXIS_MOTION = 0x650;
constexpr Uint32 SDL_EVENT_GAMEPAD_BUTTON_DOWN = 0x651;
constexpr Uint32 SDL_EVENT_GAMEPAD_BUTTON_UP = 0x652;
constexpr Uint32 SDL_EVENT_GAMEPAD_ADDED = 0x653;
constexpr Uint32 SDL_EVENT_GAMEPAD_REMOVED = 0x654;

enum SDL_GamepadButton {
    SDL_GAMEPAD_BUTTON_INVALID = -1, SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
    SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_BACK,
    SDL_GAMEPAD_BUTTON_GUIDE, SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_LEFT_STICK,
    SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_DPAD_UP,
    SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
    SDL_GAMEPAD_BUTTON_MISC1, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1,
    SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2,
    SDL_GAMEPAD_BUTTON_LEFT_PADDLE2, SDL_GAMEPAD_BUTTON_TOUCHPAD, SDL_GAMEPAD_BUTTON_MISC2,
    SDL_GAMEPAD_BUTTON_MISC3, SDL_GAMEPAD_BUTTON_MISC4, SDL_GAMEPAD_BUTTON_MISC5,
    SDL_GAMEPAD_BUTTON_MISC6, SDL_GAMEPAD_BUTTON_COUNT
};
enum SDL_GamepadAxis {
    SDL_GAMEPAD_AXIS_INVALID = -1, SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY,
    SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY, SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
    SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_GAMEPAD_AXIS_COUNT
};

struct SDL_CommonEvent { Uint32 type{}; Uint32 reserved{}; Uint64 timestamp{}; };
struct SDL_KeyboardEvent {
    Uint32 type{}; Uint32 reserved{}; Uint64 timestamp{}; SDL_WindowID windowID{};
    SDL_Keycode key{}; SDL_Keymod mod{}; bool down{}; bool repeat{};
};
struct SDL_TextEditingEvent {
    Uint32 type{}; Uint32 reserved{}; Uint64 timestamp{}; SDL_WindowID windowID{};
    const char* text{}; Sint32 start{-1}; Sint32 length{-1};
};
struct SDL_TextInputEvent {
    Uint32 type{}; Uint32 reserved{}; Uint64 timestamp{}; SDL_WindowID windowID{};
    const char* text{};
};
struct SDL_MouseMotionEvent {
    Uint32 type{}; Uint32 reserved{}; Uint64 timestamp{}; SDL_WindowID windowID{};
    float x{}; float y{};
};
struct SDL_MouseButtonEvent {
    Uint32 type{}; Uint32 reserved{}; Uint64 timestamp{}; SDL_WindowID windowID{};
    Uint8 button{}; bool down{}; float x{}; float y{};
};
struct SDL_MouseWheelEvent {
    Uint32 type{}; Uint32 reserved{}; Uint64 timestamp{}; SDL_WindowID windowID{};
    float x{}; float y{};
};
struct SDL_WindowEvent {
    Uint32 type{}; Uint32 reserved{}; Uint64 timestamp{}; SDL_WindowID windowID{};
    int data1{}; int data2{};
};
struct SDL_GamepadDeviceEvent {
    Uint32 type{}; Uint32 reserved{}; Uint64 timestamp{}; SDL_JoystickID which{};
};
struct SDL_GamepadButtonEvent {
    Uint32 type{}; Uint32 reserved{}; Uint64 timestamp{}; SDL_JoystickID which{};
    Uint8 button{}; bool down{}; Uint8 padding1{}; Uint8 padding2{};
};
struct SDL_GamepadAxisEvent {
    Uint32 type{}; Uint32 reserved{}; Uint64 timestamp{}; SDL_JoystickID which{};
    Uint8 axis{}; Uint8 padding1{}; Uint8 padding2{}; Uint8 padding3{};
    Sint16 value{}; Uint16 padding4{};
};
struct SDL_DropEvent {
    Uint32 type{}; Uint32 reserved{}; Uint64 timestamp{}; SDL_WindowID windowID{};
    float x{}; float y{}; const char* data{};
};
union SDL_Event {
    Uint32 type;
    SDL_CommonEvent common;
    SDL_KeyboardEvent key;
    SDL_TextEditingEvent edit;
    SDL_TextInputEvent text;
    SDL_MouseMotionEvent motion;
    SDL_MouseButtonEvent button;
    SDL_MouseWheelEvent wheel;
    SDL_WindowEvent window;
    SDL_GamepadDeviceEvent gdevice;
    SDL_GamepadButtonEvent gbutton;
    SDL_GamepadAxisEvent gaxis;
    SDL_DropEvent drop;
};

bool SDL_Init(Uint32 flags);
void SDL_QuitSubSystem(Uint32 flags);
const char* SDL_GetError();
SDL_Window* SDL_CreateWindow(const char* title, int w, int h, SDL_WindowFlags flags);
void SDL_DestroyWindow(SDL_Window* window);
bool SDL_ShowWindow(SDL_Window* window);
bool SDL_StartTextInput(SDL_Window* window);
bool SDL_StopTextInput(SDL_Window* window);
bool SDL_PollEvent(SDL_Event* event);
bool SDL_GetWindowSize(SDL_Window* window, int* w, int* h);
bool SDL_GetWindowSizeInPixels(SDL_Window* window, int* w, int* h);
float SDL_GetWindowDisplayScale(SDL_Window* window);
SDL_WindowFlags SDL_GetWindowFlags(SDL_Window* window);
SDL_WindowID SDL_GetWindowID(SDL_Window* window);
bool SDL_SetWindowTitle(SDL_Window* window, const char* title);
bool SDL_SetClipboardText(const char* text);
char* SDL_GetClipboardText();
void SDL_free(void* pointer);
Uint64 SDL_GetTicksNS();
void SDL_Delay(Uint32 milliseconds);
const char* SDL_GetKeyName(SDL_Keycode key);
SDL_Keymod SDL_GetModState();
SDL_Gamepad* SDL_OpenGamepad(SDL_JoystickID instance_id);
void SDL_CloseGamepad(SDL_Gamepad* gamepad);

void SDLTest_Reset();
void SDLTest_PushEvent(const SDL_Event* event);
void SDLTest_SetWindowMetrics(int logicalW, int logicalH, int pixelW, int pixelH, float scale);
void SDLTest_SetDialogResult(const char* firstPath, const char* error);

struct SDL_Renderer;
struct SDL_FRect { float x{}; float y{}; float w{}; float h{}; };
constexpr Uint8 SDL_ALPHA_OPAQUE = 255;
constexpr int SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE = 8;
SDL_Renderer* SDL_CreateRenderer(SDL_Window* window, const char* name);
void SDL_DestroyRenderer(SDL_Renderer* renderer);
bool SDL_SetRenderDrawColor(SDL_Renderer* renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a);
bool SDL_RenderClear(SDL_Renderer* renderer);
bool SDL_RenderPresent(SDL_Renderer* renderer);
bool SDL_RenderFillRect(SDL_Renderer* renderer, const SDL_FRect* rect);
bool SDL_RenderRect(SDL_Renderer* renderer, const SDL_FRect* rect);
bool SDL_RenderLine(SDL_Renderer* renderer, float x1, float y1, float x2, float y2);
bool SDL_RenderDebugText(SDL_Renderer* renderer, float x, float y, const char* str);

struct SDL_AudioStream;
using SDL_AudioDeviceID = Uint32;
using SDL_AudioFormat = Uint32;
struct SDL_AudioSpec { SDL_AudioFormat format{}; int channels{}; int freq{}; };
using SDL_AudioStreamCallback = void (SDLCALL *)(void* userdata, SDL_AudioStream* stream, int additional_amount, int total_amount);
constexpr SDL_AudioFormat SDL_AUDIO_F32 = 0x8120U;
constexpr SDL_AudioDeviceID SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK = 0xFFFFFFFFU;
constexpr SDL_AudioDeviceID SDL_AUDIO_DEVICE_DEFAULT_RECORDING = 0xFFFFFFFEU;
SDL_AudioStream* SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec* spec,
                                           SDL_AudioStreamCallback callback, void* userdata);
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream* stream);
bool SDL_PutAudioStreamData(SDL_AudioStream* stream, const void* buf, int len);
int SDL_GetAudioStreamData(SDL_AudioStream* stream, void* buf, int len);
int SDL_GetAudioStreamAvailable(SDL_AudioStream* stream);
SDL_AudioDeviceID* SDL_GetAudioRecordingDevices(int* count);
const char* SDL_GetAudioDeviceName(SDL_AudioDeviceID devid);
bool SDL_GetAudioDeviceFormat(SDL_AudioDeviceID devid, SDL_AudioSpec* spec, int* sample_frames);
void SDL_DestroyAudioStream(SDL_AudioStream* stream);
void SDLTest_RequestAudio(int additionalBytes);
void SDLTest_PushRecordingData(const float* samples, std::size_t sampleCount);
const float* SDLTest_AudioData(std::size_t* sampleCount);
