/*
 * duckstation_bridge.cpp -- Embed DuckStation as a golden oracle.
 *
 * Provides the psx_oracle_backend_t implementation for DuckStation.
 * Runs DuckStation in a worker thread; main thread signals one-frame
 * advances via condition variable.
 *
 * All Host:: namespace stubs are included here (adapted from
 * duckstation/src/duckstation-regtest/regtest_host.cpp).
 *
 * Compiled only when ENABLE_DUCKSTATION_ORACLE is defined.
 */

#ifdef ENABLE_DUCKSTATION_ORACLE

#include "psx_oracle_backend.h"

/* Standard library (must come before DuckStation headers that use them). */
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

/* DuckStation core headers. */
#include "core/achievements.h"
#include "core/bus.h"
#include "core/controller.h"
#include "core/core_private.h"
#include "core/cpu_core.h"
#include "core/fullscreenui.h"
#include "core/fullscreenui_widgets.h"
#include "core/game_list.h"
#include "core/gpu.h"
#include "core/gpu_backend.h"
#include "core/host.h"
#include "core/spu.h"
#include "core/system.h"
#include "core/system_private.h"
#include "core/video_presenter.h"
#include "core/video_thread.h"

#include "scmversion/scmversion.h"

#include "util/gpu_device.h"
#include "util/imgui_manager.h"
#include "util/input_manager.h"
#include "util/translation.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"
#include "common/task_queue.h"
#include "common/threading.h"

#include "fmt/format.h"

LOG_CHANNEL(Host);

/* ============================================================
 * Frame sync state
 * ============================================================ */

static std::mutex s_frame_mutex;
static std::condition_variable s_frame_cv;
static bool s_frame_advance_requested = false;
static bool s_frame_done = false;
static bool s_shutdown_requested = false;
static uint16_t s_pending_pad = 0xFFFF;
static uint32_t s_oracle_frame_count = 0;

static bool s_ds_loaded = false;
static std::thread s_ds_thread;
static std::string s_bios_path_copy;

/* Per-frame RAM snapshot for delta tracking. */
static uint8_t s_ram_before[2 * 1024 * 1024];

/* ============================================================
 * Host:: thread events (from regtest_host.cpp)
 * ============================================================ */

static std::mutex s_events_mutex;
static std::condition_variable s_events_cv;
static std::deque<std::pair<std::function<void()>, bool>> s_events_queue;
static uint32_t s_blocking_events = 0;
static TaskQueue s_async_tasks;

static void ProcessEvents() {
    std::unique_lock lock(s_events_mutex);
    while (!s_events_queue.empty()) {
        auto ev = std::move(s_events_queue.front());
        s_events_queue.pop_front();
        lock.unlock();
        ev.first();
        lock.lock();
        if (ev.second) {
            s_blocking_events--;
            s_events_cv.notify_one();
        }
    }
}

/* ============================================================
 * Pad input injection
 * ============================================================ */

static void InjectPadState(uint16_t buttons) {
    Controller* ctrl = System::GetController(0);
    if (!ctrl) return;
    /* PS1 pad buttons are active-low. DuckStation SetBindState takes
     * 0.0 = released, 1.0 = pressed. Map each bit. */
    /* Digital controller bind indices (from digital_controller.h):
     *   0=Select, 1=L3, 2=R3, 3=Start, 4=Up, 5=Right, 6=Down, 7=Left,
     *   8=L2, 9=R2, 10=L1, 11=R1, 12=Triangle, 13=Circle, 14=Cross, 15=Square */
    for (int i = 0; i < 16; i++) {
        float val = ((buttons >> i) & 1) ? 0.0f : 1.0f;
        ctrl->SetBindState(static_cast<u32>(i), val);
    }
}

/* ============================================================
 * DuckStation worker thread
 * ============================================================ */

static void DsWorkerThread() {
    Threading::SetNameOfCurrentThread("DS Oracle");

    Error error;

    /* Early hardware checks + process startup (must come first). */
    if (!System::PerformEarlyHardwareChecks(&error)) {
        std::fprintf(stderr, "DS oracle: PerformEarlyHardwareChecks failed: %s\n",
                     error.GetDescription().c_str());
        return;
    }
    if (!System::ProcessStartup(&error)) {
        std::fprintf(stderr, "DS oracle: ProcessStartup failed: %s\n",
                     error.GetDescription().c_str());
        return;
    }

    /* Set up folders and base settings. */
    if (!Core::SetCriticalFolders("resources", &error)) {
        std::fprintf(stderr, "DS oracle: SetCriticalFolders failed: %s\n",
                     error.GetDescription().c_str());
        return;
    }
    if (!Core::InitializeBaseSettingsLayer({}, &error)) {
        std::fprintf(stderr, "DS oracle: InitializeBaseSettingsLayer failed\n");
        return;
    }

    /* Configure headless settings. */
    {
        const auto lock = Core::GetSettingsLock();
        SettingsInterface& si = *Core::GetBaseSettingsLayer();
        si.SetStringValue("GPU", "Renderer", Settings::GetRendererName(GPURenderer::Software));
        si.SetBoolValue("GPU", "DisableShaderCache", true);
        si.SetStringValue("CPU", "ExecutionMode",
                          Settings::GetCPUExecutionModeName(CPUExecutionMode::Interpreter));
        si.SetStringValue("Pad1", "Type",
                          Controller::GetControllerInfo(ControllerType::DigitalController).name);
        si.SetStringValue("Pad2", "Type",
                          Controller::GetControllerInfo(ControllerType::None).name);
        si.SetStringValue("MemoryCards", "Card1Type",
                          Settings::GetMemoryCardTypeName(MemoryCardType::NonPersistent));
        si.SetStringValue("MemoryCards", "Card2Type",
                          Settings::GetMemoryCardTypeName(MemoryCardType::None));
        si.SetStringValue("ControllerPorts", "MultitapMode",
                          Settings::GetMultitapModeName(MultitapMode::Disabled));
        si.SetStringValue("Audio", "Backend", AudioStream::GetBackendName(AudioBackend::Null));
        si.SetBoolValue("Logging", "LogToConsole", false);
        si.SetBoolValue("Logging", "LogToFile", false);
        si.SetStringValue("Logging", "LogLevel", Settings::GetLogLevelName(Log::Level::Warning));
        si.SetBoolValue("Main", "ApplyGameSettings", false);
        si.SetBoolValue("BIOS", "PatchFastBoot", false); /* FULL boot for lock-step */
        si.SetFloatValue("Main", "EmulationSpeed", 0.0f);

        /* Disable all input sources — we inject programmatically. */
        for (u32 i = 0; i < static_cast<u32>(InputSourceType::Count); i++)
            si.SetBoolValue("InputSources",
                            InputManager::InputSourceToString(static_cast<InputSourceType>(i)),
                            false);
    }

    if (!System::CoreThreadInitialize(&error)) {
        std::fprintf(stderr, "DS oracle: CoreThreadInitialize failed: %s\n",
                     error.GetDescription().c_str());
        return;
    }

    /* Boot in BIOS-only mode (empty filename). */
    SystemBootParameters params;
    params.override_fast_boot = false;
    if (!System::BootSystem(std::move(params), &error)) {
        std::fprintf(stderr, "DS oracle: BootSystem failed: %s\n",
                     error.GetDescription().c_str());
        System::CoreThreadShutdown();
        return;
    }

    s_ds_loaded = true;

    /* Signal that init is done. */
    {
        std::lock_guard lock(s_frame_mutex);
        s_frame_done = true;
    }
    s_frame_cv.notify_one();

    /* Run the DuckStation main loop. System::Execute() calls
     * Host::PumpMessagesOnCoreThread() at each VBlank, where we
     * park the thread until the main thread requests the next frame. */
    System::Execute();

    /* Cleanup. */
    System::ShutdownSystem(false);
    System::CoreThreadShutdown();
    s_ds_loaded = false;
}

/* ============================================================
 * Host:: namespace implementations (stubs from regtest_host.cpp)
 * ============================================================ */

void Host::ReportFatalError(std::string_view title, std::string_view message) {
    std::fprintf(stderr, "DS FATAL: %.*s\n", (int)message.size(), message.data());
}

void Host::ReportErrorAsync(std::string_view title, std::string_view message) {
    if (!message.empty())
        std::fprintf(stderr, "DS ERROR: %.*s\n", (int)message.size(), message.data());
}

void Host::ReportStatusMessage(std::string_view message) {
    /* quiet */
}

void Host::ConfirmMessageAsync(std::string_view, std::string_view, ConfirmMessageAsyncCallback cb,
                               std::string_view, std::string_view) {
    cb(true);
}

void Host::ReportDebuggerEvent(CPU::DebuggerEvent, std::string_view) {}

std::span<const std::pair<const char*, const char*>> Host::GetAvailableLanguageList() { return {}; }
const char* Host::GetLanguageName(std::string_view) { return ""; }
bool Host::ChangeLanguage(const char*) { return false; }

s32 Host::Internal::GetTranslatedStringImpl(std::string_view, std::string_view msg,
                                            std::string_view, char* tbuf, size_t tbuf_space) {
    if (msg.size() > tbuf_space) return -1;
    if (msg.empty()) return 0;
    std::memcpy(tbuf, msg.data(), msg.size());
    return static_cast<s32>(msg.size());
}

std::string Host::TranslatePluralToString(const char*, const char* msg, const char*, int count) {
    std::string ret(msg);
    TinyString cs = TinyString::from_format("{}", count);
    for (;;) {
        auto pos = ret.find("%n");
        if (pos == std::string::npos) break;
        ret.replace(pos, 2, cs.view());
    }
    return ret;
}

SmallString Host::TranslatePluralToSmallString(const char*, const char* msg, const char*, int count) {
    SmallString ret(msg);
    ret.replace("%n", TinyString::from_format("{}", count));
    return ret;
}

void Host::LoadSettings(const SettingsInterface&, std::unique_lock<std::mutex>&) {}
void Host::CheckForSettingsChanges(const Settings&) {}
void Host::CommitBaseSettingChanges() {}

bool Host::ResourceFileExists(std::string_view filename, bool) {
    return FileSystem::FileExists(Path::Combine(EmuFolders::Resources, filename).c_str());
}

std::optional<DynamicHeapArray<u8>> Host::ReadResourceFile(std::string_view filename, bool, Error* error) {
    return FileSystem::ReadBinaryFile(Path::Combine(EmuFolders::Resources, filename).c_str(), error);
}

std::optional<std::string> Host::ReadResourceFileToString(std::string_view filename, bool, Error* error) {
    return FileSystem::ReadFileToString(Path::Combine(EmuFolders::Resources, filename).c_str(), error);
}

std::optional<std::time_t> Host::GetResourceFileTimestamp(std::string_view filename, bool) {
    FILESYSTEM_STAT_DATA sd;
    if (!FileSystem::StatFile(Path::Combine(EmuFolders::Resources, filename).c_str(), &sd))
        return std::nullopt;
    return sd.ModificationTime;
}

void Host::OnSystemStarting() {}
void Host::OnSystemStarted() {}
void Host::OnSystemStopping() {}
void Host::OnSystemDestroyed() {}
void Host::OnSystemPaused() {}
void Host::OnSystemResumed() {}
void Host::OnSystemAbnormalShutdown(std::string_view) {}
void Host::OnVideoThreadRunIdleChanged(bool) {}

bool Host::SetScreensaverInhibit(bool, Error* error) {
    Error::SetStringView(error, "Not supported");
    return false;
}

void Host::OnPerformanceCountersUpdated(const GPUBackend*) {}
void Host::OnSystemGameChanged(const std::string&, const std::string&, const std::string&, GameHash) {}
void Host::OnSystemUndoStateAvailabilityChanged(bool, u64) {}
void Host::OnMediaCaptureStarted() {}
void Host::OnMediaCaptureStopped() {}

/* ---- Frame sync: this is called by DuckStation at each VBlank ---- */
void Host::PumpMessagesOnCoreThread() {
    ProcessEvents();

    s_oracle_frame_count++;

    /* Signal frame done and wait for next advance request. */
    {
        std::unique_lock lock(s_frame_mutex);
        s_frame_done = true;
        s_frame_cv.notify_one();

        /* Wait until main thread requests next frame or shutdown. */
        s_frame_cv.wait(lock, [] {
            return s_frame_advance_requested || s_shutdown_requested;
        });

        if (s_shutdown_requested) {
            System::ShutdownSystem(false);
            return;
        }

        s_frame_advance_requested = false;
    }

    /* Apply pending pad state. */
    InjectPadState(s_pending_pad);
}

void Host::RunOnCoreThread(std::function<void()> func, bool block) {
    std::unique_lock lock(s_events_mutex);
    s_events_queue.emplace_back(std::move(func), block);
    s_blocking_events += block ? 1 : 0;
    if (block)
        s_events_cv.wait(lock, [] { return s_blocking_events == 0; });
}

void Host::RunOnUIThread(std::function<void()> func, bool block) {
    RunOnCoreThread(std::move(func), block);
}

void Host::QueueAsyncTask(std::function<void()> func) {
    s_async_tasks.SubmitTask(std::move(func));
}

void Host::WaitForAllAsyncTasks() {
    s_async_tasks.WaitForAll();
}

void Host::RequestResizeHostDisplay(s32, s32) {}
void Host::SetDefaultSettings(SettingsInterface&) {}
void Host::OnSettingsResetToDefault(bool, bool, bool) {}
void Host::RequestExitApplication(bool) {}
void Host::RequestExitBigPicture() {}
void Host::RequestSystemShutdown(bool, bool, bool) {}

std::optional<WindowInfo> Host::AcquireRenderWindow(RenderAPI, bool, bool, Error*) {
    return WindowInfo();
}

WindowInfoType Host::GetRenderWindowInfoType() {
    return WindowInfoType::Surfaceless;
}

void Host::ReleaseRenderWindow() {}
bool Host::CanChangeFullscreenMode(bool) { return false; }
void Host::BeginTextInput() {}
void Host::EndTextInput() {}

bool Host::CreateAuxiliaryRenderWindow(s32, s32, u32, u32, std::string_view, std::string_view,
                                       AuxiliaryRenderWindowUserData, AuxiliaryRenderWindowHandle*,
                                       WindowInfo*, Error*) {
    return false;
}

void Host::DestroyAuxiliaryRenderWindow(AuxiliaryRenderWindowHandle, s32*, s32*, u32*, u32*) {}

void Host::FrameDoneOnVideoThread(GPUBackend*, u32) {
    /* No frame dumping in oracle mode. */
}

void Host::OpenURL(std::string_view) {}
std::string Host::GetClipboardText() { return {}; }
bool Host::CopyTextToClipboard(std::string_view) { return false; }

std::string Host::FormatNumber(NumberFormatType, s64 value) {
    return fmt::format("{}", value);
}

std::string Host::FormatNumber(NumberFormatType, double value) {
    return fmt::format("{}", value);
}

void Host::SetMouseMode(bool, bool) {}
void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason) {}
void Host::OnAchievementsLoginSuccess(const char*, u32, u32, u32) {}
void Host::OnAchievementsActiveChanged(bool) {}
void Host::OnAchievementsHardcoreModeChanged(bool) {}

void Host::OnRAIntegrationMenuChanged() {}

const char* Host::GetDefaultFullscreenUITheme() { return ""; }
void Host::AddFixedInputBindings(const SettingsInterface&) {}
void Host::OnInputDeviceConnected(InputBindingKey, std::string_view, std::string_view) {}
void Host::OnInputDeviceDisconnected(InputBindingKey, std::string_view) {}
std::optional<WindowInfo> Host::GetTopLevelWindowInfo() { return std::nullopt; }
void Host::RefreshGameListAsync(bool) {}
void Host::CancelGameListRefresh() {}
void Host::OnGameListEntriesChanged(std::span<const u32>) {}

/* ============================================================
 * Backend function implementations
 * ============================================================ */

static int ds_init(const char* bios_path) {
    std::fprintf(stderr, "DS bridge: ds_init called\n"); std::fflush(stderr);
    (void)bios_path; /* BIOS is located by DuckStation's settings/search paths */
    s_bios_path_copy = bios_path ? bios_path : "";
    s_oracle_frame_count = 0;
    s_frame_advance_requested = false;
    s_frame_done = false;
    s_shutdown_requested = false;
    s_ds_loaded = false;

    /* Launch DuckStation in a worker thread. */
    s_ds_thread = std::thread(DsWorkerThread);

    /* Wait for init completion (signaled by first s_frame_done). */
    {
        std::unique_lock lock(s_frame_mutex);
        s_frame_cv.wait(lock, [] { return s_frame_done; });
        s_frame_done = false;
    }

    return s_ds_loaded ? 0 : -1;
}

static void ds_shutdown(void) {
    if (!s_ds_loaded && !s_ds_thread.joinable()) return;

    {
        std::lock_guard lock(s_frame_mutex);
        s_shutdown_requested = true;
        s_frame_advance_requested = true; /* unblock PumpMessages */
    }
    s_frame_cv.notify_one();

    if (s_ds_thread.joinable())
        s_ds_thread.join();

    s_ds_loaded = false;
}

static int ds_is_loaded(void) {
    return s_ds_loaded ? 1 : 0;
}

static void ds_run_frame(uint16_t pad1_buttons) {
    if (!s_ds_loaded) return;

    /* Snapshot RAM before frame for delta tracking. */
    if (Bus::g_ram)
        std::memcpy(s_ram_before, Bus::g_ram, Bus::RAM_2MB_SIZE);

    /* Signal worker to advance one frame. */
    {
        std::lock_guard lock(s_frame_mutex);
        s_pending_pad = pad1_buttons;
        s_frame_advance_requested = true;
    }
    s_frame_cv.notify_one();

    /* Wait for frame completion. */
    {
        std::unique_lock lock(s_frame_mutex);
        s_frame_cv.wait(lock, [] { return s_frame_done; });
        s_frame_done = false;
    }
}

static uint32_t ds_get_frame_count(void) {
    return s_oracle_frame_count;
}

static void ds_get_ram(uint8_t* out_2mb) {
    if (Bus::g_ram)
        std::memcpy(out_2mb, Bus::g_ram, Bus::RAM_2MB_SIZE);
    else
        std::memset(out_2mb, 0, 2 * 1024 * 1024);
}

static uint8_t ds_read_byte(uint32_t phys) {
    u8 val = 0;
    CPU::SafeReadMemoryByte(phys, &val);
    return val;
}

static uint32_t ds_read_word(uint32_t phys) {
    u32 val = 0;
    CPU::SafeReadMemoryWord(phys, &val);
    return val;
}

static void ds_get_cpu_regs(PsxCpuRegs* out) {
    std::memset(out, 0, sizeof(*out));
    if (!s_ds_loaded) return;
    for (int i = 0; i < 32; i++)
        out->gpr[i] = CPU::g_state.regs.r[i];
    out->pc = CPU::g_state.pc;
    out->hi = CPU::g_state.regs.hi;
    out->lo = CPU::g_state.regs.lo;
    out->cop0_sr = CPU::g_state.cop0_regs.sr.bits;
    out->cop0_cause = CPU::g_state.cop0_regs.cause.bits;
    out->cop0_epc = CPU::g_state.cop0_regs.EPC;
}

/* ============================================================
 * Backend instance + convenience wrappers
 * ============================================================ */

static const psx_oracle_backend_t s_duckstation_backend = {
    "duckstation",
    ds_init,
    ds_shutdown,
    ds_is_loaded,
    ds_run_frame,
    ds_get_frame_count,
    ds_get_ram,
    ds_read_byte,
    ds_read_word,
    ds_get_cpu_regs,
};

const psx_oracle_backend_t* g_psx_oracle = nullptr;

extern "C" int psx_oracle_init(const char* bios_path) {
    /* TEMP: skip DS init to isolate crash */
    (void)bios_path;
    std::fprintf(stderr, "DS oracle: init SKIPPED (debug test)\n");
    return -1;
}

extern "C" void psx_oracle_shutdown(void) {
    if (g_psx_oracle) {
        g_psx_oracle->shutdown();
        g_psx_oracle = nullptr;
    }
}

extern "C" void psx_oracle_run_frame(uint16_t pad1_buttons) {
    if (g_psx_oracle && g_psx_oracle->is_loaded())
        g_psx_oracle->run_frame(pad1_buttons);
}

#endif /* ENABLE_DUCKSTATION_ORACLE */
