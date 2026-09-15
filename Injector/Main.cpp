// ────────────────────────────────────────────────────────────────────────────
//  injector / main
//  Argument parsing, instance randomization and the injection driver itself.
//
//  @role     core                 @thread  main or worker
//  @touches  win32, wininet, stl
// ────────────────────────────────────────────────────────────────────────────
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#define NOMINMAX
#include <windows.h>
#include <memory>
#include <limits>
#include <cstdint>
#include <io.h>
#include <fcntl.h>
#include <stdio.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <conio.h>
#include <iomanip>
#include <random>
#include <filesystem>

#include "../API/API.h"
#include "Common.h"
#include "Console.h"
#include "Log.h"
#include "Injection.h"
#include "I18n.h"      // T() / TW() — user-facing strings
using I18n::T;         // T(L"English key") for panel and log text
using I18n::TW;        // when a std::wstring is required
#include "Http.h"      // LoadDllFromLocalFile — manual-map input path
#include "Print.h"

// ─── randomized instance ─────────────────────────────────────────────────────
// !  A fixed image name is a signature. On first start the process copies itself
//    to %TEMP%\PotatoInjectorInstances\<16 random chars>.exe, relaunches from
//    there with --randomized-instance, and the original exits. What the anti-cheat
//    observes is a different path and name on every run.
// ~  one copy plus one CreateProcess, once per session.
namespace {

    constexpr auto kRandomizedInstanceArg = L"--randomized-instance";
    constexpr auto kOriginalDirArg = L"-dll_dir=";

    // !  the buffer must be grown and retried: MAX_PATH is no longer a real bound
    //    on Windows, and a truncated path here would break the copy silently.
    std::wstring GetCurrentExecutablePath() {
        std::wstring path(MAX_PATH, L'\0');
        for (;;) {
            const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (length == 0)
                return {};
            if (length < path.size() - 1) {
                path.resize(length);
                return path;
            }
            path.resize(path.size() * 2);
        }
    }

    // $  16 characters from a 62-symbol alphabet — 95 bits of entropy, far past
    //    what a name-collision or name-pattern check can exploit.
    std::wstring MakeRandomExecutableName() {
        static constexpr wchar_t alphabet[] = L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        std::random_device randomDevice;
        std::mt19937_64 generator(randomDevice());
        std::uniform_int_distribution<size_t> distribution(0, std::size(alphabet) - 2);

        std::wstring name;
        name.reserve(20);
        for (size_t i = 0; i < 16; ++i)
            name.push_back(alphabet[distribution(generator)]);
        name += L".exe";
        return name;
    }

} // namespace

std::wstring FileNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
}

std::vector<std::wstring> ScanDlls(const std::wstring& dir) {
    return FindDllsInDirectory(dir);
}

// ─── argument parsing ────────────────────────────────────────────────────────

InjectConfig ParseInjectArgs(int argc, wchar_t** argv, bool& isRandomizedInstance) {
    InjectConfig cfg;
    isRandomizedInstance = (argc > 1) && (std::wcscmp(argv[1], kRandomizedInstanceArg) == 0);

    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        const size_t pos = arg.find(L'=');
        if (pos == std::wstring::npos)
            continue;

        std::wstring key = arg.substr(0, pos);
        std::wstring value = arg.substr(pos + 1);

        if (key == L"-process") {
            if (!value.empty()) cfg.processName = value;
        }
        else if (key == L"-force_wait_process_start") {
            std::transform(value.begin(), value.end(), value.begin(), ::towlower);
            cfg.forceWait = (value == L"true");
        }
        else if (key == L"-dll_dir") {
            // ?  a path containing spaces arrives quoted by the relaunch line.
            if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"')
                value = value.substr(1, value.size() - 2);
            cfg.dllDir = value;
        }
        else if (key == L"-inject") {
            std::transform(value.begin(), value.end(), value.begin(), ::towlower);
            cfg.manualMap = (value == L"manualmap");
        }
        else if (key == L"-dll") {
            if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"')
                value = value.substr(1, value.size() - 2);
            cfg.dllPath = value;
        }
        else if (key == L"-random_instance") {
            std::transform(value.begin(), value.end(), value.begin(), ::towlower);
            cfg.randomInstance = (value == L"true");
        }
    }
    return cfg;
}

// ─── randomized instance ─────────────────────────────────────────────────────

// >  must run before any work that depends on the module directory.
// x  no retry beyond the copy attempts below — a failure falls through to the
//    normal panel start, which is a working configuration on its own.
bool HandleRandomizedInstance(const InjectConfig& cfg) {
    if (!cfg.randomInstance)
        return false;

    const auto currentExecutable = GetCurrentExecutablePath();
    if (currentExecutable.empty())
        return false;

    std::error_code error;
    const auto instanceDirectory = std::filesystem::temp_directory_path(error) / L"PotatoInjectorInstances";
    if (error)
        return false;

    std::filesystem::create_directories(instanceDirectory, error);
    if (error)
        return false;

    // ~  best-effort sweep; a still-running copy is locked and skipped.
    for (std::filesystem::directory_iterator iterator(instanceDirectory, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (iterator->is_regular_file(error) && iterator->path().extension() == L".exe") {
            std::error_code removeError;
            std::filesystem::remove(iterator->path(), removeError);
        }
    }

    // $  8 attempts against a 95-bit name space; a collision is not the real risk,
    //    a transient share violation during the sweep is.
    std::filesystem::path randomizedExecutable;
    for (int attempt = 0; attempt < 8; ++attempt) {
        randomizedExecutable = instanceDirectory / MakeRandomExecutableName();
        if (CopyFileW(currentExecutable.c_str(), randomizedExecutable.c_str(), TRUE))
            break;
        randomizedExecutable.clear();
    }
    if (randomizedExecutable.empty())
        return false;

    // !  the copy lives under %TEMP% and cannot see the user's module folder.
    //    Passing the original directory through is what keeps -dll_dir working.
    std::wstring originalDir = cfg.dllDir;
    if (originalDir.empty()) {
        originalDir = currentExecutable;
        const size_t slash = originalDir.find_last_of(L"\\/");
        originalDir = (slash != std::wstring::npos) ? originalDir.substr(0, slash) : L".";
    }

    std::wstring commandLine = L"\"" + randomizedExecutable.wstring() + L"\" " + kRandomizedInstanceArg
        + L" " + kOriginalDirArg + L"\"" + originalDir + L"\"";
    if (cfg.manualMap)
        commandLine += L" -inject=manualmap";

    STARTUPINFOW startupInfo{ sizeof(startupInfo) };
    PROCESS_INFORMATION processInfo{};

    const BOOL created = CreateProcessW(
        randomizedExecutable.c_str(),
        commandLine.data(),
        nullptr, nullptr, FALSE, 0, nullptr, nullptr,
        &startupInfo, &processInfo);
    if (!created) {
        std::filesystem::remove(randomizedExecutable, error);
        return false;
    }
    // !  both handles are closed here; the child is not waited on, and a true
    //    return tells the caller to exit immediately.
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return true;
}

// ─── settings ────────────────────────────────────────────────────────────────

namespace {
    // >  resolved against the module directory, so the ini travels with the tool.
    std::wstring SettingsPath(const InjectConfig& cfg) {
        std::wstring dir = cfg.dllDir;
        if (dir.empty())
            dir = GetExeDirectory();
        if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/')
            dir += L'\\';
        return dir + L"injector.ini";
    }
}

void LoadSettings(const std::wstring& dir, InjectConfig& cfg) {
    InjectConfig probe;
    probe.dllDir = dir;
    const std::wstring ini = SettingsPath(probe);

    wchar_t buf[MAX_PATH * 2] = {};

    // !  the ini stores a bare file name, not a path — the tool folder may have
    //    been moved since the value was written, so it is re-anchored here.
    GetPrivateProfileStringW(L"injector", L"dll", L"", buf, _countof(buf), ini.c_str());
    if (buf[0]) {
        cfg.dllPath = dir + L"\\" + buf;
    }

    GetPrivateProfileStringW(L"injector", L"process", L"cs2.exe", buf, _countof(buf), ini.c_str());
    if (buf[0]) cfg.processName = buf;

    cfg.manualMap      = GetPrivateProfileIntW(L"injector", L"manualmap", 0, ini.c_str()) != 0;
    cfg.forceWait      = GetPrivateProfileIntW(L"injector", L"forcewait", 1, ini.c_str()) != 0;
    cfg.randomInstance = GetPrivateProfileIntW(L"injector", L"randominstance", 1, ini.c_str()) != 0;

    // !  an out-of-range language index would index past the translation table.
    cfg.lang = GetPrivateProfileIntW(L"ui", L"lang", -1, ini.c_str());
    if (cfg.lang < -1 || cfg.lang >= (int)I18n::LANG_COUNT)
        cfg.lang = -1;
}

void SaveSettings(const InjectConfig& cfg) {
    const std::wstring ini = SettingsPath(cfg);

    WritePrivateProfileStringW(L"injector", L"dll", FileNameOf(cfg.dllPath).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"injector", L"process", cfg.processName.c_str(), ini.c_str());
    WritePrivateProfileStringW(L"injector", L"manualmap", cfg.manualMap ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"injector", L"forcewait", cfg.forceWait ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"injector", L"randominstance", cfg.randomInstance ? L"1" : L"0", ini.c_str());

    wchar_t langBuf[16] = {};
    swprintf_s(langBuf, L"%d", cfg.lang);
    WritePrivateProfileStringW(L"ui", L"lang", langBuf, ini.c_str());
}

// ─── injection driver ────────────────────────────────────────────────────────

// ?  the return value reports the outcome; every failure also writes a line to
//    the active sink, and the two are expected to agree.
InjectOutcome RunInjection(const InjectConfig& cfg) {
    InjectOutcome out;
    ClearCancel();

    if (cfg.dllPath.empty()) {
        LogLine(LOG_ERR, T(L"No DLL selected."));
        out.status = InjectStatus::BadDll;
        return out;
    }
    {
        const DWORD attr = GetFileAttributesW(cfg.dllPath.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            LogLine(LOG_ERR, T(L"DLL file not found: ") + cfg.dllPath);
            out.status = InjectStatus::BadDll;
            return out;
        }
    }

    LogLine(LOG_STEP, T(L"Target process: ") + cfg.processName);
    LogLine(LOG_STEP, T(L"Injection method: ") + std::wstring(cfg.manualMap ? T(L"Manual map (this DLL will crash, not recommended)") : T(L"LoadLibrary (recommended)")));
    LogLine(LOG_STEP, T(L"Target DLL: ") + FileNameOf(cfg.dllPath));

    // >  privilege first: the process open below needs it.
    EnableDebugPrivilege();

    const DWORD targetPID = FindTargetProcess(cfg.processName, cfg.forceWait);
    if (targetPID == 0) {
        if (IsCancelRequested()) {
            out.status = InjectStatus::Cancelled;
        } else {
            LogLine(LOG_ERR, T(L"Target process not found: ") + cfg.processName);
            out.status = InjectStatus::NoProcess;
        }
        return out;
    }
    out.pid = targetPID;

    bool injectionSuccess = false;
    std::vector<ManualMapInjector::SavedHook> savedHooks;

    if (cfg.manualMap) {
        // >  manual mapping consumes a buffer, so the image is read into memory first.
        size_t dllSize = 0;
        std::unique_ptr<BYTE[]> dllBuffer = LoadDllFromLocalFile(cfg.dllPath, dllSize);
        if (!dllBuffer || dllSize == 0) {
            LogLine(LOG_ERR, T(L"Failed to read DLL or file is empty; manual map aborted."));
            out.status = InjectStatus::BadDll;
            return out;
        }
        LogLine(LOG_STEP, T(L"Manual map injection started..."));
        injectionSuccess = ManualMapInjector::ManualMapInject(targetPID, dllBuffer.get(), dllSize);
        // !  the plaintext image must not remain in this process after the map.
        SecureZeroMemory(dllBuffer.get(), dllSize);
        LogLine(LOG_INFO, T(L"DLL memory buffer wiped."));
    }
    else {
        // ─── LoadLibrary path ────────────────────────────────────────────────
        // ^  BypassUserHooks, taken from kit_attack. Twelve APIs that an
        //    anti-cheat or an unpacker commonly inline-hooks — LoadLibraryW among
        //    them — are restored to their original bytes before the injection and
        //    put back afterwards.
        // #  win32: the restore uses ntdll-level read/write, so a hook planted in
        //    kernel32 is not what the restore itself calls.
        // x  no attempt is made to detect which of the twelve is actually hooked;
        //    the restore is unconditional and idempotent.
        HANDLE bypassProcess = OpenProcess(
            PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION,
            FALSE, targetPID);
        if (bypassProcess) {
            savedHooks = ManualMapInjector::BypassUserHooks(bypassProcess);
            LogLine(LOG_INFO, T(L"Restored ") + std::to_wstring(savedHooks.size()) + T(L" hooked API(s) (put back after injecting)"));
        }

        LogLine(LOG_STEP, T(L"Injecting..."));
        std::wstring reason;
        HMODULE injectedBase = ManualMapInjector::LoadLibraryInject(targetPID, cfg.dllPath, &reason);
        if (injectedBase == NULL) {
            // >  fallback only after the documented path fails, never before —
            //    LdrLoadDll skips the kernel32 bookkeeping that some modules expect.
            if (!reason.empty())
                LogLine(LOG_WARN, T(L"LoadLibraryW failed: ") + reason + T(L", retrying with LdrLoadDll..."));
            else
                LogLine(LOG_WARN, T(L"LoadLibraryW failed, retrying with LdrLoadDll..."));
            std::wstring reason2;
            injectedBase = ManualMapInjector::LdrLoadDllInject(targetPID, cfg.dllPath, &reason2);
            if (injectedBase == NULL && !reason2.empty())
                LogLine(LOG_ERR, T(L"LdrLoadDll failed: ") + reason2);
        }

        // !  the restore runs on both outcomes. Leaving a game process with
        //    patched kernel32 bytes is worse than a failed injection.
        if (bypassProcess && !savedHooks.empty()) {
            ManualMapInjector::RestoreUserHooks(bypassProcess, savedHooks);
            LogLine(LOG_INFO, T(L"Hooks restored."));
        }
        if (bypassProcess)
            CloseHandle(bypassProcess);

        injectionSuccess = (injectedBase != NULL);
    }

    out.bypassedApis = savedHooks.size();
    if (injectionSuccess) {
        LogLine(LOG_OK, T(L"Injected successfully! Bypassed ") + std::to_wstring(savedHooks.size()) + T(L" API(s)   (press INSERT in game for the menu)"));
        out.status = InjectStatus::Success;
    } else {
        LogLine(LOG_ERR, T(L"Injection failed."));
        out.status = InjectStatus::Failed;
    }
    return out;
}

// ─── headless entry point ────────────────────────────────────────────────────

// x  the panel in Gui.cpp is the real entry point. This one is kept for a build
//    switched to the console subsystem, where it drives the same pipeline.
// ~  blocks until the injection resolves, or indefinitely under forceWait.
int wmain(int argc, wchar_t* argv[]) {
    bool isRandomizedInstance = false;
    InjectConfig cfg = ParseInjectArgs(argc, argv, isRandomizedInstance);

    // >  the relaunched copy must not relaunch again; the flag is its only guard.
    if (!isRandomizedInstance) {
        if (HandleRandomizedInstance(cfg))
            return 0;
    }

    if (GetConsoleWindow()) {
        _setmode(_fileno(stdout), _O_U16TEXT);
        _setmode(_fileno(stderr), _O_U16TEXT);
        SetConsoleTitleW(L"Injector");
        SetConsoleCursorVisibility(false);
        ClearConsole();
        DisplayBanner();
    }

    const std::wstring exeDir = cfg.dllDir.empty() ? GetExeDirectory() : cfg.dllDir;
    const std::vector<std::wstring> dlls = ScanDlls(exeDir);
    if (dlls.empty()) {
        LogLine(LOG_ERR, T(L"No .dll found under: ") + exeDir);
        if (GetConsoleWindow()) _getwch();
        return 1;
    }
    // x  no picker in this path by design — the panel owns the selection UI.
    cfg.dllPath = dlls.front();
    if (dlls.size() > 1)
        LogLine(LOG_WARN, T(L"Multiple DLLs found, console mode picks the first: ") + FileNameOf(cfg.dllPath));

    const InjectOutcome outcome = RunInjection(cfg);
    if (GetConsoleWindow()) {
        LogLine(LOG_INFO, T(L"Press any key to exit..."));
        _getwch();
    }
    return outcome.status == InjectStatus::Success ? 0 : 1;
}
