// ────────────────────────────────────────────────────────────────────────────
//  injector / print
//  Console-only cosmetica: the startup card and a wget-shaped progress bar.
//
//  @role     util                 @thread  caller
//  @touches  win32, stl
// ────────────────────────────────────────────────────────────────────────────
#ifndef PRINT_H
#define PRINT_H

#include <windows.h>
#include <string>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <sstream>
#include "Console.h"

// x  no figlet-style ASCII banner — the panel is the real entry point, and the
//    console path exists only for a headless build.
inline void DisplayBanner() {
    PrintMessage(L"LUOXUEQI — ring-3 injector");
    PrintMessage(L"native C++ · x64");
    PrintMessage(L"");
}

// ─── progress bar ────────────────────────────────────────────────────────────

// !  Update() writes with a bare carriage return; any other writer on stdout
//    between two Update calls will interleave into the bar.
class WgetStyleProgressBar {
private:
    size_t total_size;
    size_t downloaded;
    std::chrono::steady_clock::time_point start_time;
    int last_progress_percent;
    int console_width;

    std::wstring FormatSize(size_t bytes) {
        const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB" };
        int unit_index = 0;
        double size = static_cast<double>(bytes);

        while (size >= 1024.0 && unit_index < 3) {
            size /= 1024.0;
            unit_index++;
        }

        std::wstringstream ss;
        ss << std::fixed << std::setprecision(1) << size << L" " << units[unit_index];
        return ss.str();
    }

    std::wstring FormatTime(double seconds) {
        int hours = static_cast<int>(seconds) / 3600;
        int minutes = (static_cast<int>(seconds) % 3600) / 60;
        int secs = static_cast<int>(seconds) % 60;

        std::wstringstream ss;
        if (hours > 0) {
            ss << hours << L":" << std::setw(2) << std::setfill(L'0') << minutes
                << L":" << std::setw(2) << std::setfill(L'0') << secs;
        }
        else {
            ss << minutes << L":" << std::setw(2) << std::setfill(L'0') << secs;
        }
        return ss.str();
    }

    // $  80 is the historic console default, used when the real width is unavailable.
    void GetConsoleWidth() {
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
            console_width = csbi.dwSize.X;
        }
        else {
            console_width = 80;
        }
    }

public:
    WgetStyleProgressBar(size_t total) : total_size(total), downloaded(0),
        last_progress_percent(-1), console_width(80) {
        start_time = std::chrono::steady_clock::now();
        GetConsoleWidth();
    }

    // ?  total_size == 0 means the server sent no Content-Length; the bar then
    //    degrades to a running byte count and reports no ETA.
    void Update(size_t new_downloaded) {
        downloaded = new_downloaded;

        if (total_size == 0) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
            double speed = (elapsed > 0) ? static_cast<double>(downloaded) / elapsed : 0;

            std::wcout << L"\r" << FormatSize(downloaded) << L" ["
                << std::setw(5) << std::setprecision(1) << std::fixed
                << speed << L"B/s]";
            std::wcout.flush();
            return;
        }

        // ~  early-out unless the integer percentage moved; called once per 8 KB chunk.
        int percent = static_cast<int>((static_cast<double>(downloaded) / total_size) * 100);
        if (percent == last_progress_percent && percent < 100) {
            return;
        }
        last_progress_percent = percent;

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
        double speed = (elapsed > 0) ? static_cast<double>(downloaded) / elapsed : 0;

        double eta_seconds = (speed > 0) ? (total_size - downloaded) / speed : 0;

        // $  20 is the narrowest bar worth drawing; 50 columns are reserved for
        //    the counters printed to the right of the bar.
        int bar_width = std::max(20, console_width - 50);
        int completed_width = static_cast<int>((static_cast<double>(bar_width) * percent) / 100);

        std::wstringstream progress_bar;
        progress_bar << L"[";
        for (int i = 0; i < bar_width; i++) {
            if (i < completed_width) {
                progress_bar << L"=";
            }
            else if (i == completed_width) {
                progress_bar << L">";
            }
            else {
                progress_bar << L" ";
            }
        }
        progress_bar << L"]";

        std::wcout << L"\r" << std::setw(3) << percent << L"% " << progress_bar.str()
            << L" " << FormatSize(downloaded) << L" " << FormatSize(static_cast<size_t>(speed)) << L"/s"
            << L" eta " << FormatTime(eta_seconds);
        std::wcout.flush();
    }

    // >  forces the final 100% frame before the terminating newline.
    void Finish() {
        if (total_size > 0) {
            Update(total_size);
        }
        std::wcout << std::endl;
    }
};

#endif
