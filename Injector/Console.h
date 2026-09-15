// ────────────────────────────────────────────────────────────────────────────
//  injector / console
//  Keeps the console path usable when the panel is not the entry point.
//
//  @role     util                 @thread  caller
//  @touches  win32, stl
// ────────────────────────────────────────────────────────────────────────────
#ifndef CONSOLE_H
#define CONSOLE_H

#include <windows.h>
#include <string>
#include <iostream>
#include <thread>
#include <chrono>
#include <io.h>
#include <fcntl.h>
#include "Log.h"

// ?  a redirected stdout has no cursor info; the call then does nothing.
inline void SetConsoleCursorVisibility(bool visible) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hConsole == INVALID_HANDLE_VALUE) {
        return;
    }

    CONSOLE_CURSOR_INFO cursorInfo;
    if (!GetConsoleCursorInfo(hConsole, &cursorInfo)) {
        return;
    }

    cursorInfo.bVisible = visible;
    SetConsoleCursorInfo(hConsole, &cursorInfo);
}

// #  win32: FillConsoleOutputCharacterW over the full buffer — cheaper and
//    flicker-free compared with a cursor walk.
// ~  two buffer-sized fills, once per process start.
inline void ClearConsole() {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hConsole == INVALID_HANDLE_VALUE) {
        return;
    }

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(hConsole, &csbi)) {
        return;
    }

    DWORD dwWritten;
    DWORD dwConsoleSize = csbi.dwSize.X * csbi.dwSize.Y;
    COORD cursorHome = { 0, 0 };

    if (!FillConsoleOutputCharacterW(hConsole, L' ', dwConsoleSize, cursorHome, &dwWritten)) {
        return;
    }

    if (!FillConsoleOutputAttribute(hConsole, csbi.wAttributes, dwConsoleSize, cursorHome, &dwWritten)) {
        return;
    }

    SetConsoleCursorPosition(hConsole, cursorHome);
}

// >  routed through LogLine, so a panel sink captures these too.
inline void PrintMessage(const std::wstring& message) {
    LogLine(LOG_INFO, message);
}

// ~  blocks the calling thread for (length / chars_per_sleep) * delay_ms.
// x  no ANSI escape handling — the console path prints plain text only.
inline void PrintTypewriter(const std::wstring& text, int delay_ms, int chars_per_sleep = 1) {
    if (chars_per_sleep <= 0) chars_per_sleep = 1;

    int chars_printed_in_batch = 0;
    for (wchar_t c : text) {
        std::wcout << c << std::flush;
        chars_printed_in_batch++;

        if (delay_ms > 0 && chars_printed_in_batch >= chars_per_sleep) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            chars_printed_in_batch = 0;
        }
    }
}

#endif
