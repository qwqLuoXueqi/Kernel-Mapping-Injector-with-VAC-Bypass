// ────────────────────────────────────────────────────────────────────────────
//  injector / http
//  Pulls a module into memory — from disk, or over WinINet when given a URL.
//
//  @role     api                  @thread  caller
//  @touches  win32, wininet, stl
// ────────────────────────────────────────────────────────────────────────────
#ifndef HTTP_H
#define HTTP_H

#include <windows.h>
#include <wininet.h>
#include <string>
#include <memory>
#include <vector>
#include <algorithm>
#include <limits>
#include "Console.h"
#include "Print.h"

// $  linked here rather than in the project file so the URL path stays self-contained.
#pragma comment(lib, "wininet.lib")

// #  win32: InternetCloseHandle is the only valid release; a NULL handle is legal
//    and simply ignored, which is what makes the empty-handle case safe below.
struct InternetHandleDeleter {
    void operator()(HINTERNET handle) const {
        if (handle != NULL) {
            InternetCloseHandle(handle);
        }
    }
};
using unique_internet_handle = std::unique_ptr<void, InternetHandleDeleter>;

// ─── size probe ──────────────────────────────────────────────────────────────

// ?  a server that omits Content-Length yields size 0 and a true return; the
//    caller must treat 0 as unknown, not as an empty file.
// ~  opens and discards a second connection, so a download costs two requests.
inline bool GetHttpFileSize(const std::wstring& url, size_t& fileSize) {
    fileSize = 0;

    unique_internet_handle hInternet(InternetOpenW(L"DLLDownloader", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0));
    if (!hInternet) {
        PrintMessage(L"error: [GetSize] InternetOpenW failed, code " + std::to_wstring(GetLastError()));
        return false;
    }

    // >  INTERNET_FLAG_RELOAD first so a cached body cannot report a stale length.
    DWORD flags = INTERNET_FLAG_RELOAD;
    if (url.find(L"https://") == 0) {
        flags |= INTERNET_FLAG_SECURE;
    }

    unique_internet_handle hUrl(InternetOpenUrlW(hInternet.get(), url.c_str(), NULL, 0, flags, 0));
    if (!hUrl) {
        PrintMessage(L"error: [GetSize] InternetOpenUrlW failed for " + url + L", code " + std::to_wstring(GetLastError()));
        return false;
    }

    DWORD contentLength = 0;
    DWORD length = sizeof(contentLength);

    if (!HttpQueryInfoW(hUrl.get(), HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, &contentLength, &length, NULL)) {
        DWORD lastError = GetLastError();
        if (lastError == ERROR_HTTP_HEADER_NOT_FOUND) {
            PrintMessage(L"warning: [GetSize] no Content-Length header; size stays unknown");
            fileSize = 0;
        }
        else {
            PrintMessage(L"error: [GetSize] HttpQueryInfoW failed to read Content-Length, code " + std::to_wstring(lastError));
            return false;
        }
    }
    else {
        fileSize = static_cast<size_t>(contentLength);
        if (fileSize == 0) {
            PrintMessage(L"warning: [GetSize] Content-Length is 0; the module may be empty");
        }
        else {
            PrintMessage(L"remote size estimate: " + std::to_wstring(fileSize) + L" bytes");
        }
    }

    return true;
}

// ─── local read ──────────────────────────────────────────────────────────────

// ^  supersedes the URL download path; a module shipped next to the injector is
//    the normal case and needs no network at all.
// !  the returned buffer is not wiped here; the caller owns it and must zero it
//    once the module is mapped.
inline std::unique_ptr<BYTE[]> LoadDllFromLocalFile(const std::wstring& filePath, size_t& outSize) {
    outSize = 0;

    // >  FILE_SHARE_READ only: a second writer cannot swap the image mid-read.
    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        PrintMessage(L"error: cannot open module file " + filePath + L", code " + std::to_wstring(GetLastError()));
        return nullptr;
    }
    unique_handle fileHandle(static_cast<void*>(hFile));

    LARGE_INTEGER fileSizeLI;
    if (!GetFileSizeEx(hFile, &fileSizeLI)) {
        PrintMessage(L"error: cannot read module size, code " + std::to_wstring(GetLastError()));
        return nullptr;
    }
    if (fileSizeLI.QuadPart <= 0) {
        PrintMessage(L"error: module file is empty (0 bytes)");
        return nullptr;
    }

    size_t fileSize = static_cast<size_t>(fileSizeLI.QuadPart);
    // $  512 MB is a sanity bound, not a format limit; a legitimate game module
    //    is two orders of magnitude smaller.
    const size_t MAX_DLL_SIZE = 512 * 1024 * 1024;
    if (fileSize > MAX_DLL_SIZE) {
        PrintMessage(L"error: module too large (" + std::to_wstring(fileSize) + L" bytes)");
        return nullptr;
    }

    // x  no fallback to a mapped view — the caller needs a writable copy anyway.
    std::unique_ptr<BYTE[]> buffer(new (std::nothrow) BYTE[fileSize]);
    if (!buffer) {
        PrintMessage(L"error: allocation failed for " + std::to_wstring(fileSize) + L" bytes");
        return nullptr;
    }

    // ~  64 KB chunks: one syscall per chunk, so larger reads pay off until the
    //    buffer stops fitting in cache.
    DWORD bytesRead = 0;
    size_t totalRead = 0;
    BYTE temp[65536];
    while (ReadFile(hFile, temp, sizeof(temp), &bytesRead, NULL) && bytesRead > 0) {
        if (totalRead + bytesRead > fileSize) {
            PrintMessage(L"error: read past the recorded file size");
            return nullptr;
        }
        memcpy(buffer.get() + totalRead, temp, bytesRead);
        totalRead += bytesRead;
    }

    // !  a short read means the file changed under us; a partial image is unusable.
    if (totalRead != fileSize) {
        PrintMessage(L"error: short read — expected " + std::to_wstring(fileSize)
            + L" bytes, got " + std::to_wstring(totalRead));
        return nullptr;
    }

    outSize = totalRead;
    PrintMessage(L"module read into memory: " + std::to_wstring(outSize) + L" bytes");
    return buffer;
}

// ─── streaming download ──────────────────────────────────────────────────────

// ?  growth is capped at MAX_BUFFER_SIZE; a body past that is rejected rather
//    than truncated, since a truncated image would map and then misbehave.
inline std::unique_ptr<BYTE[]> DownloadDLLToMemory(const std::wstring& url, size_t& outSize) {
    outSize = 0;
    size_t fileSize = 0;

    bool sizeKnown = GetHttpFileSize(url, fileSize);

    unique_internet_handle hInternet(InternetOpenW(L"DLLDownloader", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0));
    if (!hInternet) {
        PrintMessage(L"error: InternetOpenW failed, code " + std::to_wstring(GetLastError()));
        return nullptr;
    }

    DWORD flags = INTERNET_FLAG_RELOAD;
    if (url.find(L"https://") == 0) {
        flags |= INTERNET_FLAG_SECURE;
    }

    unique_internet_handle hUrl(InternetOpenUrlW(hInternet.get(), url.c_str(), NULL, 0, flags, 0));
    if (!hUrl) {
        PrintMessage(L"error: InternetOpenUrlW failed for " + url + L", code " + std::to_wstring(GetLastError()));
        return nullptr;
    }

    WgetStyleProgressBar progressBar(fileSize);
    PrintMessage(L"downloading from " + url);

    // $  +10% headroom when the length is known, so a marginally larger body
    //    does not trigger a reallocation; 10 MB is the guess when it is not.
    size_t bufferSize = sizeKnown && fileSize > 0 ?
        fileSize + std::max<size_t>(fileSize / 10, 1024 * 1024) :
        10 * 1024 * 1024;

    if (bufferSize < 4096) bufferSize = 4096;

    std::unique_ptr<BYTE[]> buffer(new (std::nothrow) BYTE[bufferSize]);
    if (!buffer) {
        PrintMessage(L"error: allocation failed for the download buffer (" + std::to_wstring(bufferSize) + L" bytes)");
        return nullptr;
    }

    // ~  8 KB per InternetReadFile call, matching the WinINet internal chunk size.
    size_t totalRead = 0;
    BYTE temp[8192];
    DWORD bytesRead;

    while (InternetReadFile(hUrl.get(), temp, sizeof(temp), &bytesRead) && bytesRead > 0) {
        if (totalRead + bytesRead > bufferSize) {
            size_t newBufferSize = bufferSize * 2;

            // $  256 MB ceiling for the same reason as MAX_DLL_SIZE above.
            const size_t MAX_BUFFER_SIZE = 256 * 1024 * 1024;
            if (newBufferSize > MAX_BUFFER_SIZE) {
                if (totalRead + bytesRead > MAX_BUFFER_SIZE) {
                    PrintMessage(L"error: body exceeds the " + std::to_wstring(MAX_BUFFER_SIZE) + L" byte ceiling");
                    return nullptr;
                }
                newBufferSize = MAX_BUFFER_SIZE;
            }

            std::unique_ptr<BYTE[]> newBuffer(new (std::nothrow) BYTE[newBufferSize]);
            if (!newBuffer) {
                PrintMessage(L"error: buffer growth failed, download aborted");
                return nullptr;
            }

            std::copy(buffer.get(), buffer.get() + totalRead, newBuffer.get());
            buffer = std::move(newBuffer);
            bufferSize = newBufferSize;

            PrintMessage(L"warning: buffer grew to " + std::to_wstring(bufferSize) + L" bytes");
        }

        std::copy(temp, temp + bytesRead, buffer.get() + totalRead);
        totalRead += bytesRead;

        progressBar.Update(totalRead);
    }

    // ?  InternetReadFile returns FALSE for a clean end of stream as well as for a
    //    fault, so the distinguishing signal is "nothing read" plus a live error.
    DWORD lastInternetError = GetLastError();
    if (totalRead == 0 && lastInternetError != ERROR_SUCCESS) {
        PrintMessage(L"error: download produced no data, InternetReadFile code " + std::to_wstring(lastInternetError));
        return nullptr;
    }
    else if (totalRead == 0) {
        PrintMessage(L"warning: downloaded body is empty (0 bytes)");
        return nullptr;
    }

    progressBar.Finish();

    // !  the staging buffer over-allocates; this exact-size copy is what the
    //    mapper receives, so no trailing slack is ever mapped.
    std::unique_ptr<BYTE[]> finalBuffer(new (std::nothrow) BYTE[totalRead]);
    if (!finalBuffer) {
        PrintMessage(L"error: final allocation failed for " + std::to_wstring(totalRead) + L" bytes");
        return nullptr;
    }

    std::copy(buffer.get(), buffer.get() + totalRead, finalBuffer.get());
    outSize = totalRead;

    PrintMessage(L"module downloaded into memory: " + std::to_wstring(outSize) + L" bytes");
    return finalBuffer;
}

#endif
