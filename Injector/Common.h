// ────────────────────────────────────────────────────────────────────────────
//  injector / common
//  Filesystem helpers, privilege staging and target discovery for the console path.
//
//  @role     util                 @thread  caller
//  @touches  win32, tlhelp32, psapi
// ────────────────────────────────────────────────────────────────────────────
#ifndef COMMON_H
#define COMMON_H

#include <windows.h>
#include <string>
#include <memory>
#include <vector>
#include <algorithm>
#include <tlhelp32.h>
#include <psapi.h>
#include "Console.h"
#include "Log.h"

// ─── filesystem ──────────────────────────────────────────────────────────────

// x  non-recursive on purpose — the module sits next to the injector by design.
// ?  a directory the process cannot enumerate returns an empty list, which is
//    indistinguishable from a genuinely empty folder.
inline std::vector<std::wstring> FindDllsInDirectory(const std::wstring& dirPath) {
    std::vector<std::wstring> dlls;
    std::wstring searchPath = dirPath;
    if (!searchPath.empty() && searchPath.back() != L'\\' && searchPath.back() != L'/') {
        searchPath += L'\\';
    }
    searchPath += L"*.dll";

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        return dlls;
    }

    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            std::wstring fullPath = dirPath;
            if (!fullPath.empty() && fullPath.back() != L'\\' && fullPath.back() != L'/') {
                fullPath += L'\\';
            }
            fullPath += fd.cFileName;
            dlls.push_back(fullPath);
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
    return dlls;
}

inline std::wstring GetExeDirectory() {
    wchar_t path[MAX_PATH] = { 0 };
    GetModuleFileNameW(NULL, path, MAX_PATH);
    std::wstring full(path);
    size_t pos = full.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        return full.substr(0, pos);
    }
    return L".";
}

// ─── handles ─────────────────────────────────────────────────────────────────

// #  win32: both NULL and INVALID_HANDLE_VALUE are treated as "nothing to close",
//    because CreateToolhelp32Snapshot returns the latter on failure while
//    OpenProcess returns the former.
struct HandleDeleter {
    void operator()(HANDLE handle) const {
        if (handle != NULL && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};

using unique_handle = std::unique_ptr<void, HandleDeleter>;

struct ProcessInfo {
    DWORD pid = 0;
    std::wstring name;
    DWORD threadCount = 0;
};

struct CommandLineArgs {
    std::wstring processName;
    std::wstring dllUrl;
    bool forceWaitProcessStart = false;
};

// ─── command line ────────────────────────────────────────────────────────────

// !  applied to every argument before parsing, so a value can never carry a
//    shell metacharacter into a downstream CreateProcess call.
// $  512 is a length bound on the whole argument, not on the name.
inline std::wstring SanitizeInput(const std::wstring& input) {
    std::wstring sanitized = input;

    sanitized.erase(std::remove_if(sanitized.begin(), sanitized.end(), [](wchar_t c) {
        return c == L'"' || c == L';' || c == L'|' || c == L'&' || c == L'<' || c == L'>' || c == L'\'';
        }), sanitized.end());

    const size_t MAX_LEN = 512;
    if (sanitized.length() > MAX_LEN) {
        sanitized = sanitized.substr(0, MAX_LEN);
    }
    return sanitized;
}

// ^  legacy console contract. Main.cpp parses a wider argument set; this one is
//    kept because a headless build still routes through wmain.
inline bool ParseCommandLine(int argc, wchar_t* argv[], CommandLineArgs& args) {
    if (argc < 3) {
        PrintMessage(L"error: missing required arguments");
        PrintMessage(L"usage: " + std::wstring(argv[0])
            + L" -process=<name.exe> -dll=<path or url> [-force_wait_process_start=true|false]");
        return false;
    }

    bool hasProcess = false;
    bool hasDll = false;

    for (int i = 1; i < argc; ++i) {
        std::wstring arg = SanitizeInput(argv[i]);
        if (arg.empty()) {
            continue;
        }

        size_t pos = arg.find(L'=');
        if (pos == std::wstring::npos || pos == 0 || pos == arg.length() - 1) {
            PrintMessage(L"error: malformed argument, or empty value: " + arg);
            return false;
        }

        std::wstring key = arg.substr(0, pos);
        std::wstring value = arg.substr(pos + 1);

        if (key == L"-process") {
            args.processName = value;
            hasProcess = true;
        }
        else if (key == L"-dll") {
            args.dllUrl = value;
            hasDll = true;
        }
        else if (key == L"-force_wait_process_start") {
            std::transform(value.begin(), value.end(), value.begin(), ::towlower);
            if (value == L"true") {
                args.forceWaitProcessStart = true;
            }
            else if (value == L"false") {
                args.forceWaitProcessStart = false;
            }
            else {
                PrintMessage(L"error: -force_wait_process_start expects 'true' or 'false', got: " + value);
                return false;
            }
        }
        else {
            PrintMessage(L"warning: unknown argument ignored: " + key);
        }
    }

    if (!hasProcess) {
        PrintMessage(L"error: -process is required");
        return false;
    }
    if (!hasDll) {
        PrintMessage(L"error: -dll is required");
        return false;
    }

    if (args.processName.length() < 4 || _wcsicmp(args.processName.substr(args.processName.length() - 4).c_str(), L".exe") != 0) {
        PrintMessage(L"error: -process must end in .exe: " + args.processName);
        return false;
    }

    // ?  a URL need not end in .dll, so this stays a warning rather than a rejection.
    if (args.dllUrl.length() < 4 || _wcsicmp(args.dllUrl.substr(args.dllUrl.length() - 4).c_str(), L".dll") != 0) {
        PrintMessage(L"warning: " + args.dllUrl + L" does not end in .dll; continuing anyway");
    }

    return true;
}

// ─── privilege ───────────────────────────────────────────────────────────────

// #  win32: SE_DEBUG_NAME on the current token — required for OpenProcess with
//    PROCESS_VM_WRITE against a process owned by another integrity level.
// x  AdjustTokenPrivileges is not checked for ERROR_NOT_ALL_ASSIGNED; a token
//    that cannot hold the privilege still proceeds, and the open call will fail
//    later with a clearer error than this one could give.
inline bool EnableDebugPrivilege() {
    HANDLE hToken;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        return false;
    }
    unique_handle tokenHandle(hToken);

    LUID luid;
    if (!LookupPrivilegeValueW(NULL, SE_DEBUG_NAME, &luid)) {
        return false;
    }

    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    AdjustTokenPrivileges(tokenHandle.get(), FALSE, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL);
    return true;
}

// ─── process enumeration ─────────────────────────────────────────────────────

// ~  snapshot of every process, once per poll; the wait loop calls this at 2 Hz.
inline std::vector<ProcessInfo> ListProcesses() {
    std::vector<ProcessInfo> processes;

    unique_handle snapshotHandle(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshotHandle.get() == INVALID_HANDLE_VALUE) {
        PrintMessage(L"error: CreateToolhelp32Snapshot failed, code " + std::to_wstring(GetLastError()));
        return processes;
    }

    PROCESSENTRY32 pe32 = { sizeof(PROCESSENTRY32) };
    if (Process32First(snapshotHandle.get(), &pe32)) {
        do {
            if (pe32.th32ProcessID == 0) continue;
            ProcessInfo info;
            info.pid = pe32.th32ProcessID;
            info.name = pe32.szExeFile;
            info.threadCount = pe32.cntThreads;
            processes.push_back(info);
        } while (Process32Next(snapshotHandle.get(), &pe32));
    }
    else {
        // ?  ERROR_NO_MORE_FILES on the very first call means an empty snapshot,
        //    which is not worth reporting.
        DWORD lastError = GetLastError();
        if (lastError != ERROR_NO_MORE_FILES) {
            PrintMessage(L"error: process enumeration failed, code " + std::to_wstring(lastError));
        }
    }
    return processes;
}

// ─── target discovery ────────────────────────────────────────────────────────

// !  a thread count of 0 identifies a zombie remainder: the process object still
//    exists but ExitProcess never completed. Injecting into it stalls the loader
//    forever, and a live game process always holds dozens of threads.
// ~  polling path: one snapshot per 500 ms, bounded by maxReadyChecks.
inline DWORD FindTargetProcess(const std::wstring& processName, bool forceWait) {
    DWORD pid = 0;

    auto isZombieProcess = [](const ProcessInfo& p) -> bool {
        return p.threadCount == 0;
    };

    auto findHealthyPid = [&](std::vector<ProcessInfo>& processes) -> DWORD {
        for (const auto& proc : processes) {
            if (_wcsicmp(proc.name.c_str(), processName.c_str()) == 0) {
                if (isZombieProcess(proc)) {
                    continue;
                }
                return proc.pid;
            }
        }
        return 0;
    };

    if (forceWait) {
        // >  name match first; the readiness gate below is a second, stricter pass.
        while (pid == 0) {
            if (IsCancelRequested()) {
                LogLine(LOG_WARN, L"cancelled while waiting for the target process");
                return 0;
            }
            auto processes = ListProcesses();
            pid = findHealthyPid(processes);
            if (pid == 0) {
                // ~  2 Hz is the slowest rate that still feels immediate to a user.
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
        LogLine(LOG_OK, L"found " + processName + L"  pid=" + std::to_wstring(pid));

        // ─── readiness gate ──────────────────────────────────────────────────
        // !  the image name alone is not enough. cs2.exe appears first as a ~40 MB
        //    launcher and only later becomes the multi-gigabyte game process;
        //    injecting during the launcher phase fails.
        // $  600 MB is the observed floor once the main menu is up, and 100 MB of
        //    drift between two samples means the working set is still climbing.
        // x  window class is not usable as the signal — the launcher belongs to a
        //    different Source generation and never shares the game's class name.
        // ~  up to 480 checks at 500 ms = 240 s, covering a cold start plus time
        //    spent on the server browser.
        LogLine(LOG_INFO, L"waiting for the game to become ready (working set >= 600 MB, stable)");
        {
            const int maxReadyChecks = 480;
            const SIZE_T wsThreshold = 600;   // MB
            SIZE_T lastWsMB = 0;
            int stableCount = 0;
            bool ready = false;
            for (int check = 0; check < maxReadyChecks && !ready; ++check) {
                if (IsCancelRequested()) {
                    LogLine(LOG_WARN, L"cancelled while waiting for the game to become ready");
                    return 0;
                }

                // #  win32: PROCESS_QUERY_LIMITED_INFORMATION is the narrowest right
                //    that still permits GetProcessMemoryInfo, and it does not expose
                //    the module list.
                SIZE_T wsBytes = 0;
                HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (hProc) {
                    PROCESS_MEMORY_COUNTERS pmc = { 0 };
                    if (GetProcessMemoryInfo(hProc, &pmc, sizeof(pmc))) {
                        wsBytes = pmc.WorkingSetSize;
                    }
                    CloseHandle(hProc);
                }
                const SIZE_T wsMB = wsBytes / (1024 * 1024);

                if (wsMB >= wsThreshold) {
                    if (lastWsMB > 0 && (wsMB > lastWsMB ? (wsMB - lastWsMB) : (lastWsMB - wsMB)) < 100) {
                        ++stableCount;
                    }
                    else {
                        stableCount = 0;
                    }
                }
                else {
                    stableCount = 0;
                }
                lastWsMB = wsMB;

                // >  two consecutive stable samples, not one — a single flat reading
                //    can land inside a load spike.
                if (wsMB >= wsThreshold && stableCount >= 2) {
                    LogLine(LOG_OK, L"game is ready");
                    ready = true;
                }
                else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
            }
            if (!ready) {
                // x  a timeout still attempts the injection; the readiness gate is a
                //    heuristic, and refusing to try would be worse than trying late.
                PrintMessage(L"readiness gate timed out after 240 s; attempting the injection anyway");
            }
        }
    }
    else {
        auto processes = ListProcesses();
        pid = findHealthyPid(processes);
        if (pid == 0) {
            PrintMessage(L"error: target process not found: " + processName);
        }
        else {
            PrintMessage(L"target process found: " + processName);
        }
    }

    return pid;
}

#endif
