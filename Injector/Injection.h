// ────────────────────────────────────────────────────────────────────────────
//  injector / injection
//  The seam between the panel entry point and the headless console fallback.
//
//  @role     api                  @thread  caller
//  @touches  win32, stl
// ────────────────────────────────────────────────────────────────────────────
#pragma once
#include <windows.h>
#include <string>
#include <vector>

struct InjectConfig {
    std::wstring dllPath;                      // absolute path of the module to load
    std::wstring processName = L"cs2.exe";     // target image name
    bool  manualMap     = false;               // false = LoadLibrary (default), true = manual map
    bool  forceWait     = true;                // true = start the panel first, wait for the game
    bool  randomInstance = true;               // relaunch from a randomized copy in %TEMP%
    std::wstring dllDir;                       // module folder; passed as -dll_dir when relaunched
    int   lang = -1;                           // ui language: -1 = auto, 0..6 = I18n::UiLang
};

enum class InjectStatus {
    Success,
    Failed,
    Cancelled,
    NoProcess,
    BadDll
};

struct InjectOutcome {
    InjectStatus status = InjectStatus::Failed;
    size_t bypassedApis = 0;
    DWORD  pid = 0;
};

// ^  argument set matches the historical console contract; see Main.cpp for parsing.
//    --randomized-instance / -dll_dir=<dir> / -process=<x.exe>
//    -inject=loadlibrary|manualmap / -force_wait_process_start=true|false
InjectConfig ParseInjectArgs(int argc, wchar_t** argv, bool& isRandomizedInstance);

// >  call before any real work; a false return means the caller must exit.
bool HandleRandomizedInstance(const InjectConfig& cfg);

std::vector<std::wstring> ScanDlls(const std::wstring& dir);

// !  safe to call from a worker thread, but only one at a time — the cancel
//    latch it clears is process-wide.
// ~  blocks for as long as the wait-and-load sequence takes; cancellable.
InjectOutcome RunInjection(const InjectConfig& cfg);

// settings live at <dllDir>\injector.ini
void LoadSettings(const std::wstring& dir, InjectConfig& cfg);
void SaveSettings(const InjectConfig& cfg);

// last path component, used as the display name
std::wstring FileNameOf(const std::wstring& path);
