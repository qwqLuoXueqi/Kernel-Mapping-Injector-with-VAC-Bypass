#pragma once
#ifndef MM_UTIL_H
#define MM_UTIL_H
#include "Config.h"
#include "NT.h"  // 引入 NT API
#include <tlhelp32.h>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <windows.h>
#include <thread>
#include <chrono>

namespace ManualMapInjector {

    struct HandleDeleter {
        void operator()(void* ptr) const {
            if (ptr) {
                HANDLE handle = static_cast<HANDLE>(ptr);
                if (handle != NULL && handle != INVALID_HANDLE_VALUE) {
                    LoadNtDll();
                    if (NtClose) {
                        NtClose(handle);
                    }
                    else {
                        CloseHandle(handle);
                    }
                }
            }
        }
    };
    using unique_handle = std::unique_ptr<void, HandleDeleter>;

    struct VirtualFreeDeleter {
        HANDLE hProcess;
        VirtualFreeDeleter(HANDLE process) : hProcess(process) {}
        void operator()(void* memory) const {
            if (memory) {
                LoadNtDll();
                if (NtFreeVirtualMemory) {
                    SIZE_T size = 0;
                    NtFreeVirtualMemory(hProcess, &memory, &size, MEM_RELEASE);
                }
                else {
                    VirtualFreeEx(hProcess, memory, 0, MEM_RELEASE);
                }
            }
        }
    };
    using unique_virtual_mem = std::unique_ptr<void, VirtualFreeDeleter>;

    inline std::string WstringToUtf8(const std::wstring& wstr) {
        if (wstr.empty()) return "";
        int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, NULL, 0, NULL, NULL);
        std::string str(size, 0);
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], size, NULL, NULL);
        str.resize(size - 1);
        return str;
    }

    // 注: 原 GetModuleBaseInTargetProcess(读远程 PEB/Toolhelp 取模块基址)已废弃删除。
    // 原因同 FindRemoteProcAddress —— CS2 受 VAC 保护读不到远程模块列表(299),
    // 且已用本地哈希解析替代, 见下方 GetModuleHandleByHash / GetProcAddressByHash。


    // 注: FindRemoteProcAddress / GetModuleBaseInTargetProcess 已废弃删除。
    // 历史原因: 这两个函数通过 Toolhelp/PEB 读取"远程进程"的模块基址, 但 CS2 受 VAC
    // 保护, 这两条路都会返回 ERROR_PARTIAL_COPY=299, 根本读不到。且它们内部调用
    // LoadLibraryA/GetProcAddress 会留下明文字符串特征。已改用:
    //   - 本地 PEB 哈希取模块基址 (GetModuleHandleByHash, 见下方)
    //   - 本地导出表哈希取函数地址 (GetProcAddressByHash, 见下方)
    // x64 下 kernel32.dll 是系统 DLL, 注入器与目标进程基址一致, 无需读远程模块。

    // ==================== 哈希解析（去除明文函数名字符串特征） ====================
    // 用 DJ2 哈希遍历模块导出表, 匹配预计算的哈希值, 避免在二进制里留下
    // "LoadLibraryA" / "GetProcAddress" / "kernel32.dll" 等明文字符串(静态扫描特征)。

    // DJ2 哈希: 对字符串小写化后逐字符计算, 结果存为 32 位无符号。
    inline constexpr DWORD HashStringLower(const char* str, DWORD hash = 5381) {
        unsigned char c;
        while ((c = static_cast<unsigned char>(*str++)) != 0) {
            if (c >= 'A' && c <= 'Z') c += 32;  // 小写化
            hash = ((hash << 5) + hash) + c;
        }
        return hash;
    }

    // 用哈希在指定模块的导出表里查找函数地址（不依赖 GetProcAddress 明文）。
    inline FARPROC GetProcAddressByHash(HMODULE hModule, DWORD targetHash) {
        if (!hModule)
            return nullptr;

        __try {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hModule);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE)
                return nullptr;
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
                reinterpret_cast<const BYTE*>(hModule) + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE)
                return nullptr;

            const auto& expDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            if (expDir.VirtualAddress == 0 || expDir.Size == 0)
                return nullptr;

            const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
                reinterpret_cast<const BYTE*>(hModule) + expDir.VirtualAddress);

            const auto* names = reinterpret_cast<const DWORD*>(
                reinterpret_cast<const BYTE*>(hModule) + exports->AddressOfNames);
            const auto* ordinals = reinterpret_cast<const WORD*>(
                reinterpret_cast<const BYTE*>(hModule) + exports->AddressOfNameOrdinals);
            const auto* functions = reinterpret_cast<const DWORD*>(
                reinterpret_cast<const BYTE*>(hModule) + exports->AddressOfFunctions);

            for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
                const char* name = reinterpret_cast<const char*>(
                    reinterpret_cast<const BYTE*>(hModule) + names[i]);
                if (HashStringLower(name) == targetHash) {
                    const WORD ordinalIndex = ordinals[i];
                    const DWORD funcRva = functions[ordinalIndex];
                    // 转发导出(forwarder)落在导出目录范围内, 这里只处理直接导出
                    return reinterpret_cast<FARPROC>(
                        reinterpret_cast<BYTE*>(hModule) + funcRva);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
        return nullptr;
    }

    // 完整版本地 PEB/LDR 结构（专用于哈希取模块基址, 与 NT.h 里为"远程读取"
    // 而精简的 PEB/LDR_DATA_TABLE_ENTRY 分开命名, 避免字段布局冲突）。
    struct LocalUnicodeString {
        USHORT Length;
        USHORT MaximumLength;
        PWSTR  Buffer;
    };
    struct LocalPebLdrData {
        BYTE      Reserved1[8];
        PVOID     Reserved2[3];
        LIST_ENTRY InMemoryOrderModuleList;
    };
    struct LocalLdrDataTableEntry {
        LIST_ENTRY        InLoadOrderLinks;
        LIST_ENTRY        InMemoryOrderLinks;
        LIST_ENTRY        InInitializationOrderLinks;
        PVOID             DllBase;
        PVOID             EntryPoint;
        ULONG             SizeOfImage;
        LocalUnicodeString FullDllName;
        LocalUnicodeString BaseDllName;
        ULONG             Flags;
        SHORT             LoadCount;
        SHORT             TlsIndex;
        LIST_ENTRY        HashLinks;
        ULONG             TimeDateStamp;
    };
    struct LocalPeb {
        BYTE          Reserved1[2];
        BYTE          BeingDebugged;
        BYTE          Reserved2[1];
        PVOID         Reserved3[2];
        LocalPebLdrData* Ldr;
        PVOID         ProcessParameters;
    };

    // 用哈希取模块基址（kernel32.dll 一定已加载, 直接遍历 PEB 本地链表匹配哈希）。
    inline HMODULE GetModuleHandleByHash(DWORD targetHash) {
        __try {
            LocalPeb* peb = nullptr;
#ifdef _WIN64
            peb = reinterpret_cast<LocalPeb*>(__readgsqword(0x60));
#else
            peb = reinterpret_cast<LocalPeb*>(__readfsdword(0x30));
#endif
            if (!peb || !peb->Ldr)
                return nullptr;

            const LIST_ENTRY* head = &peb->Ldr->InMemoryOrderModuleList;
            const LIST_ENTRY* cur = head->Flink;
            for (int i = 0; cur && cur != head && i < 4096; ++i) {
                const auto* mod = CONTAINING_RECORD(cur, LocalLdrDataTableEntry, InMemoryOrderLinks);
                if (mod->BaseDllName.Length > 0 && mod->BaseDllName.Buffer) {
                    char narrow[256] = { 0 };
                    const int count = mod->BaseDllName.Length / sizeof(wchar_t);
                    for (int j = 0; j < count && j < 255; ++j) {
                        const wchar_t wc = mod->BaseDllName.Buffer[j];
                        narrow[j] = (wc >= 0 && wc < 128) ? static_cast<char>(wc) : '?';
                    }
                    if (HashStringLower(narrow) == targetHash)
                        return reinterpret_cast<HMODULE>(mod->DllBase);
                }
                cur = cur->Flink;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
        return nullptr;
    }

    inline DWORD RvaToFileOffset(PIMAGE_NT_HEADERS_CURRENT pNtHeaders, DWORD rva, size_t fileSize) {
        if (rva < pNtHeaders->OptionalHeader.SizeOfHeaders) return rva;
        PIMAGE_SECTION_HEADER pSectionHeader = IMAGE_FIRST_SECTION(pNtHeaders);
        for (WORD i = 0; i < pNtHeaders->FileHeader.NumberOfSections; ++i, ++pSectionHeader) {
            if (rva >= pSectionHeader->VirtualAddress && rva < pSectionHeader->VirtualAddress + pSectionHeader->Misc.VirtualSize) {
                if (rva - pSectionHeader->VirtualAddress < pSectionHeader->SizeOfRawData) {
                    DWORD offset = rva - pSectionHeader->VirtualAddress + pSectionHeader->PointerToRawData;
                    if (offset < static_cast<DWORD>(fileSize)) return offset;
                }
                return 0;
            }
        }
        return 0;
    }

#pragma optimize("", off)
    template<typename T>
    size_t GetFunctionSize(T* function) {
        uint8_t* ptr = (uint8_t*)function;
        while (true) {
#if defined(_WIN64)
            if (ptr[0] == 0xC3 || ptr[0] == 0xC2)
#else
            if (ptr[0] == 0xC3 || ptr[0] == 0xC2 || ptr[0] == 0xC9 || ptr[0] == 0xCA)
#endif
            {
                bool isFunctionEnd = true;
                for (int i = 1; i < 8; i++) {
                    if (ptr[i] == 0xCC) break;
                    if (ptr[i] != 0x00 && ptr[i] != 0x90) {
                        isFunctionEnd = false;
                        break;
                    }
                }
                if (isFunctionEnd) {
                    return (ptr - (uint8_t*)function) + 1;
                }
            }
            ptr++;
        }
    }
#pragma optimize("", on)
}
#endif