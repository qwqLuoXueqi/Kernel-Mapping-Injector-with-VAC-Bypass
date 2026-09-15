// ────────────────────────────────────────────────────────────────────────────
//  injector / log
//  Funnels every stage message to one sink and owns the cooperative stop flag.
//
//  @role     util                 @thread  any
//  @touches  win32, stl
// ────────────────────────────────────────────────────────────────────────────
#pragma once
#include <windows.h>
#include <string>
#include <iostream>
#include <functional>

// >  a sink installed by SetLogSink takes priority; wcout is the fallback only.
// ~  one call per stage, plus one poll per wait-loop iteration.
enum LogLevel {
    LOG_INFO = 0,
    LOG_OK   = 1,
    LOG_WARN = 2,
    LOG_ERR  = 3,
    LOG_STEP = 4
};

using LogSink = std::function<void(int level, const std::wstring& text)>;

inline LogSink& GetLogSink()
{
    static LogSink sink;
    return sink;
}

// !  not thread-safe against a concurrent LogLine; call before the worker starts.
inline void SetLogSink(LogSink sink)
{
    GetLogSink() = std::move(sink);
}

inline void LogLine(int level, const std::wstring& text)
{
    LogSink& sink = GetLogSink();
    if (sink) {
        sink(level, text);
        return;
    }
    std::wcout << L"> " << text << std::endl;
}

inline void LogLine(int level, const wchar_t* text)
{
    LogLine(level, std::wstring(text ? text : L""));
}

// ─── stop flag ───────────────────────────────────────────────────────────────

// ?  only the wait loops poll this; a mid-write cancel is not observed.
// #  win32: volatile long + interlocked-free read is enough here because the
//    value is a latch, never a counter, and no other state depends on it.
// ~  read once per 500 ms per wait loop.
inline volatile long& CancelFlag()
{
    static volatile long flag = 0;
    return flag;
}

inline void RequestCancel() { CancelFlag() = 1; }
inline void ClearCancel()   { CancelFlag() = 0; }
inline bool IsCancelRequested() { return CancelFlag() != 0; }
