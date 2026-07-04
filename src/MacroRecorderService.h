#pragma once

#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

enum class MacroEventType
{
    KeyDown,
    KeyUp,
    MouseDown,
    MouseUp,
    MouseMove,
    MouseWheel
};

enum class MacroMouseButton
{
    None,
    Left,
    Right,
    Middle,
    X1,
    X2
};

enum class MacroMouseMode
{
    Relative,
    Absolute,
    WindowRelative
};

enum class MacroRecorderStatus
{
    Idle,
    Armed,
    Recording,
    Stopped
};

enum class MacroPlaybackStatus
{
    Idle,
    Starting,
    Playing,
    Complete,
    Stopped,
    Failed
};

enum class MacroPlaybackBackend
{
    Standard,
    Interception
};

struct MacroHotkey
{
    UINT modifiers = 0;
    UINT virtualKey = 0;
};

struct MacroEvent
{
    long long timeUs = 0;
    MacroEventType type = MacroEventType::KeyDown;
    DWORD virtualKey = 0;
    DWORD scanCode = 0;
    DWORD keyFlags = 0;
    MacroMouseButton mouseButton = MacroMouseButton::None;
    LONG x = 0;
    LONG y = 0;
    LONG dx = 0;
    LONG dy = 0;
    int wheelDelta = 0;
    bool absoluteMove = false;
    bool shift = false;
    bool ctrl = false;
    bool alt = false;
};

struct MacroRecordingMode
{
    MacroMouseMode mouseMode = MacroMouseMode::Relative;
    int captureRateHz = 120;
};

struct MacroDefinition
{
    int version = 1;
    std::wstring id;
    std::wstring name;
    std::wstring description;
    std::wstring createdAt;
    std::wstring updatedAt;
    std::wstring targetWindowTitle;
    std::wstring targetProcessName;
    MacroRecordingMode recordingMode;
    double defaultPlaybackSpeed = 1.0;
    int defaultLoopCount = 1;
    bool defaultLoopUntilStopped = false;
    bool requireTargetFocused = false;
    std::vector<MacroEvent> events;
};

struct MacroRecorderSnapshot
{
    MacroRecorderStatus status = MacroRecorderStatus::Idle;
    std::wstring message;
    std::wstring targetWindowTitle;
    std::wstring targetProcessName;
    long long durationUs = 0;
    size_t eventCount = 0;
};

struct MacroPlaybackOptions
{
    double speed = 1.0;
    int loopCount = 1;
    bool loopUntilStopped = false;
    int startDelaySeconds = 0;
    bool requireTargetFocused = false;
    MacroPlaybackBackend backend = MacroPlaybackBackend::Standard;
};

struct MacroPlaybackSnapshot
{
    MacroPlaybackStatus status = MacroPlaybackStatus::Idle;
    std::wstring message;
    int currentLoop = 0;
    int requestedLoops = 1;
    long long elapsedUs = 0;
    long long totalUs = 0;
};

class MacroTargetWindowService
{
public:
    static void CaptureForegroundTarget(std::wstring& title, std::wstring& processName);
    static bool IsTargetFocused(const MacroDefinition& macro);
    static std::wstring ForegroundWindowLabel();
};

class InterceptionCaptureSession;

class MacroStorageService
{
public:
    std::vector<MacroDefinition> LoadAll(const std::filesystem::path& directory, std::wstring& warning) const;
    bool Save(const std::filesystem::path& directory, const MacroDefinition& macro, std::wstring& errorMessage) const;
    bool Delete(const std::filesystem::path& directory, const MacroDefinition& macro, std::wstring& errorMessage) const;
    bool ExportMacro(const MacroDefinition& macro, const std::filesystem::path& path, std::wstring& errorMessage) const;
    std::optional<MacroDefinition> ImportMacro(const std::filesystem::path& path, std::wstring& errorMessage) const;

    static std::wstring GenerateId();
    static std::wstring FormatLocalIso(std::chrono::system_clock::time_point value);
};

class MacroRecorderService
{
public:
    MacroRecorderService();
    ~MacroRecorderService();
    MacroRecorderService(const MacroRecorderService&) = delete;
    MacroRecorderService& operator=(const MacroRecorderService&) = delete;

    bool StartRecording(
        HINSTANCE instance,
        HWND rawInputWindow,
        const MacroRecordingMode& mode,
        MacroPlaybackBackend inputBackend,
        const std::vector<MacroHotkey>& ignoredHotkeys,
        std::wstring& errorMessage);
    MacroDefinition StopRecording(const std::wstring& requestedName);
    void CancelRecording();
    bool HandleRawInput(LPARAM lParam);
    MacroRecorderSnapshot Snapshot() const;
    bool IsRecording() const;

private:
    friend class InterceptionCaptureSession;

    static LRESULT CALLBACK KeyboardHookProc(int code, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK MouseHookProc(int code, WPARAM wParam, LPARAM lParam);
    LRESULT HandleKeyboard(int code, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMouse(int code, WPARAM wParam, LPARAM lParam);
    void AddEvent(MacroEvent event);
    long long ElapsedUs() const;
    long long ElapsedUsFromMessageTime(DWORD messageTime) const;
    bool ShouldIgnoreKeyboardEvent(DWORD virtualKey, WPARAM message) const;
    bool ShouldCaptureMove(long long nowUs, const POINT& point, LONG& dx, LONG& dy);
    bool ShouldCaptureAbsoluteMove(long long nowUs, const POINT& point, bool force);
    void AddAbsoluteMouseMove(long long nowUs, const POINT& point, bool force);
    bool RegisterRawMouseInput(HWND hwnd);
    bool RegisterRawKeyboardInput(HWND hwnd);
    void UnregisterRawMouseInput();
    void UnregisterRawKeyboardInput();
    void AddRelativeMouseMove(long long nowUs, LONG dx, LONG dy);
    void FlushPendingRelativeMouseMove(long long timeUs);
    void RecordInterceptionKeyboard(unsigned short code, unsigned short state, unsigned int information, long long timeUs);
    void RecordInterceptionMouse(unsigned short state, unsigned short flags, short rolling, int x, int y, unsigned int information, long long timeUs);
    void AddEventLocked(MacroEvent event);
    void AddAbsoluteMouseMoveLocked(long long nowUs, const POINT& point, bool force);
    bool ShouldCaptureAbsoluteMoveLocked(long long nowUs, const POINT& point, bool force);

    mutable std::mutex mutex_;
    HHOOK keyboardHook_ = nullptr;
    HHOOK mouseHook_ = nullptr;
    HWND rawInputWindow_ = nullptr;
    MacroRecorderStatus status_ = MacroRecorderStatus::Idle;
    MacroRecordingMode mode_;
    std::vector<MacroHotkey> ignoredHotkeys_;
    std::chrono::steady_clock::time_point startTime_;
    std::chrono::steady_clock::time_point lastEventTime_;
    DWORD startTickMs_ = 0;
    std::vector<MacroEvent> events_;
    std::wstring targetWindowTitle_;
    std::wstring targetProcessName_;
    POINT lastMousePoint_ {};
    long long lastMouseMoveUs_ = 0;
    long long lastAbsoluteMouseMoveUs_ = 0;
    long long pendingMouseStartUs_ = 0;
    LONG pendingMouseDx_ = 0;
    LONG pendingMouseDy_ = 0;
    std::map<DWORD, bool> rawKeyDown_;
    bool haveLastMousePoint_ = false;
    bool haveLastAbsoluteMousePoint_ = false;
    POINT lastAbsoluteMousePoint_ {};
    bool rawMouseRegistered_ = false;
    bool rawKeyboardRegistered_ = false;
    bool interceptionRecording_ = false;
    std::unique_ptr<InterceptionCaptureSession> interceptionCaptureSession_;
};

class MacroPlaybackService
{
public:
    MacroPlaybackService();
    ~MacroPlaybackService();
    MacroPlaybackService(const MacroPlaybackService&) = delete;
    MacroPlaybackService& operator=(const MacroPlaybackService&) = delete;

    bool Start(const MacroDefinition& macro, const MacroPlaybackOptions& options, std::wstring& errorMessage);
    void Stop();
    bool IsPlaying() const;
    MacroPlaybackSnapshot Snapshot() const;
    bool IsInterceptionAvailable(std::wstring& errorMessage) const;

private:
    void PlaybackLoop(MacroDefinition macro, MacroPlaybackOptions options);
    bool BuildMacroInput(const MacroEvent& event, MacroMouseMode mode, INPUT& input);
    bool SendMacroEvent(const MacroEvent& event, MacroMouseMode mode, void* driverBackend);
    void ReleaseHeldInputs(const std::map<DWORD, MacroEvent>& heldKeys, const std::vector<MacroMouseButton>& heldMouseButtons);
    bool WaitInterruptible(std::chrono::steady_clock::duration duration);
    bool WaitUntilInterruptible(std::chrono::steady_clock::time_point targetTime);

    mutable std::mutex mutex_;
    std::thread playbackThread_;
    std::atomic_bool stopRequested_ = true;
    MacroPlaybackSnapshot snapshot_;
};

class MacroHotkeyService
{
public:
    ~MacroHotkeyService();
    MacroHotkeyService(const MacroHotkeyService&) = delete;
    MacroHotkeyService& operator=(const MacroHotkeyService&) = delete;
    MacroHotkeyService() = default;

    bool Register(HWND hwnd, int id, const MacroHotkey& hotkey, std::wstring& errorMessage);
    void Unregister(HWND hwnd, int id);
    void UnregisterAll(HWND hwnd);

    static std::wstring HotkeyLabel(const MacroHotkey& hotkey);
    static bool IsEmpty(const MacroHotkey& hotkey);

private:
    std::vector<int> registeredIds_;
};
