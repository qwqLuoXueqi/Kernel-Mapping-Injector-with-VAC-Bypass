#ifndef MM_HOOKBYPASS_H
#define MM_HOOKBYPASS_H

#include "Util.h"
#include "NT.h"
#include <string>
#include <vector>
#include <cstdint>

// ==================== Hook 绕过（BypassUserHooks） ====================
// 思路移植自 kit_attack 内核注入套件的 BypassUserHooks：
//   VAC/VMProtect 会内联 hook kernel32/ntdll/KernelBase 的一批关键 API
//   （LoadLibraryW / CreateRemoteThread / VirtualProtect ...），
//   检测到可疑调用就返回 NULL / 静默失败。
//
//   内核版靠"驱动 IOCTL 写回原始字节"；本注入器是 ring3，退而求其次：
//     1. 用 NtProtectVirtualMemory 改页权限（ntdll 底层，比 VirtualProtect 更难被 hook）
//     2. 用 NtReadVirtualMemory 读目标进程函数头，检测 E9 jmp / FF 25 内联 hook
//     3. 从磁盘 System32 的系统 DLL 文件里读出"干净原始字节"
//     4. 用 NtWriteVirtualMemory 写回干净字节（临时 unhook）
//     5. 注入完成后用保存的"被篡改字节"还原（RestoreUserHooks），做到来无影去无踪
//
// 被绕过的 API 清单（与 kit_attack 一致）：
//   kernel32.dll : LoadLibraryExW / VirtualAlloc / FreeLibrary /
//                  LoadLibraryExA / LoadLibraryA / VirtualAllocEx
//   ntdll.dll    : NtOpenFile / VirtualProtect / CreateProcessW /
//                  CreateProcessA / VirtualProtectEx
//   KernelBase.dll: ResumeThread
//
// 注意：本模块只做"临时 unhook → 注入 → 还原"。写入走 NtWriteVirtualMemory，
// 目标进程若把 ntdll 的这些 API 也 hook 了，仍有被拦的可能，但 VAC 通常只 hook
// kernel32 层，ntdll 的 NtReadVirtualMemory/NtWriteVirtualMemory/NtProtectVirtualMemory
// 是系统调用直接入口，被 hook 的概率极低。

namespace ManualMapInjector {

    // 单条 hook 记录：模块名 + 函数名
    struct HookTarget {
        const wchar_t* moduleName;   // 例如 L"kernel32.dll"
        const char*    functionName; // 例如 "LoadLibraryExW"
    };

    // 宽字符模块名转窄字符（模块名都是纯 ASCII，安全）
    inline const char* NarrowModuleName(const wchar_t* w) {
        static char narrow[64];
        int j = 0;
        for (; w[j] && j < 63; ++j) narrow[j] = (char)w[j];
        narrow[j] = 0;
        return narrow;
    }

    // 需要绕过的 API 清单
    static const HookTarget g_hookTargets[] = {
        // kernel32.dll
        { L"kernel32.dll", "LoadLibraryExW" },
        { L"kernel32.dll", "VirtualAlloc"   },
        { L"kernel32.dll", "FreeLibrary"    },
        { L"kernel32.dll", "LoadLibraryExA" },
        { L"kernel32.dll", "LoadLibraryA"   },
        { L"kernel32.dll", "VirtualAllocEx" },
        // ntdll.dll
        { L"ntdll.dll", "NtOpenFile"        },
        { L"ntdll.dll", "VirtualProtect"    },
        { L"ntdll.dll", "CreateProcessW"    },
        { L"ntdll.dll", "CreateProcessA"    },
        { L"ntdll.dll", "VirtualProtectEx"  },
        // KernelBase.dll
        { L"KernelBase.dll", "ResumeThread" },
    };
    static const size_t g_hookTargetCount = sizeof(g_hookTargets) / sizeof(g_hookTargets[0]);

    // 保存的单条 hook 状态：被 hook 的函数地址 + 被篡改的原始字节（用于还原）
    struct SavedHook {
        uintptr_t address;        // 目标函数地址（远程进程内）
        BYTE      patchedBytes[16]; // 被 hook 时函数头的字节（还原用）
        int       patchedLen;      // 实际保存的字节数
        bool      wasHooked;       // 是否真的检测到 hook
    };

    // 用哈希在指定模块里找函数 RVA（返回相对模块基址的偏移；找不到返回 0）
    inline uintptr_t GetFunctionRvaByName(HMODULE hModule, const char* name) {
        if (!hModule) return 0;
        __try {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hModule);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
                reinterpret_cast<const BYTE*>(hModule) + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
            const auto& expDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            if (expDir.VirtualAddress == 0 || expDir.Size == 0) return 0;
            const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
                reinterpret_cast<const BYTE*>(hModule) + expDir.VirtualAddress);
            const auto* names = reinterpret_cast<const DWORD*>(
                reinterpret_cast<const BYTE*>(hModule) + exports->AddressOfNames);
            const auto* ordinals = reinterpret_cast<const WORD*>(
                reinterpret_cast<const BYTE*>(hModule) + exports->AddressOfNameOrdinals);
            const auto* functions = reinterpret_cast<const DWORD*>(
                reinterpret_cast<const BYTE*>(hModule) + exports->AddressOfFunctions);
            for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
                const char* n = reinterpret_cast<const char*>(
                    reinterpret_cast<const BYTE*>(hModule) + names[i]);
                if (n && _stricmp(n, name) == 0) {
                    return functions[ordinals[i]];
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
        return 0;
    }

    // 从磁盘 System32 的系统 DLL 文件读出"干净原始字节"。
    // 原理：系统 DLL 在磁盘上的 .text 节字节 = 未加载前的原始字节（不含运行时 hook）。
    // 返回函数在磁盘文件中的偏移，调用者用它去读干净字节。
    inline bool ReadCleanBytesFromDisk(const wchar_t* moduleName, const char* functionName,
                                       BYTE outCleanBytes[16], int* outLen) {
        if (outLen) *outLen = 0;
        wchar_t sysPath[MAX_PATH] = { 0 };
        if (!GetSystemDirectoryW(sysPath, MAX_PATH)) return false;
        std::wstring dllPath = std::wstring(sysPath) + L"\\" + moduleName;

        // 读整个 DLL 文件到内存（系统 DLL 通常 1~10MB，一次性读入可接受）
        HANDLE hFile = CreateFileW(dllPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) return false;
        DWORD fileSize = GetFileSize(hFile, NULL);
        if (fileSize == INVALID_FILE_SIZE || fileSize == 0) { CloseHandle(hFile); return false; }

        std::vector<BYTE> buf(fileSize);
        DWORD read = 0;
        BOOL ok = ReadFile(hFile, buf.data(), fileSize, &read, NULL);
        CloseHandle(hFile);
        if (!ok || read != fileSize) return false;

        // 解析磁盘文件里的 PE，找函数 RVA
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(buf.data());
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(buf.data() + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

        const auto& expDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (expDir.VirtualAddress == 0 || expDir.Size == 0) return false;

        // 转 RVA -> 文件偏移
        auto rvaToFile = [&](DWORD rva) -> DWORD {
            if (rva < nt->OptionalHeader.SizeOfHeaders) return rva;
            const auto* sec = IMAGE_FIRST_SECTION(nt);
            for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
                DWORD start = sec->VirtualAddress;
                DWORD rawEnd = sec->Misc.VirtualSize > sec->SizeOfRawData ? sec->Misc.VirtualSize : sec->SizeOfRawData;
                DWORD end = start + rawEnd;
                if (rva >= start && rva < end) {
                    return rva - start + sec->PointerToRawData;
                }
            }
            return 0;
        };

        const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
            buf.data() + rvaToFile(expDir.VirtualAddress));
        const auto* names = reinterpret_cast<const DWORD*>(
            buf.data() + rvaToFile(exports->AddressOfNames));
        const auto* ordinals = reinterpret_cast<const WORD*>(
            buf.data() + rvaToFile(exports->AddressOfNameOrdinals));
        const auto* functions = reinterpret_cast<const DWORD*>(
            buf.data() + rvaToFile(exports->AddressOfFunctions));

        for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
            const char* n = reinterpret_cast<const char*>(buf.data() + rvaToFile(names[i]));
            if (n && _stricmp(n, functionName) == 0) {
                DWORD funcRva = functions[ordinals[i]];
                DWORD fileOff = rvaToFile(funcRva);
                if (fileOff == 0 || fileOff + 16 > fileSize) return false;
                memcpy(outCleanBytes, buf.data() + fileOff, 16);
                if (outLen) *outLen = 16;
                return true;
            }
        }
        return false;
    }

    // 检测函数头是否被 inline hook（E9 rel32 jmp 或 FF 25 [rip+rel32] jmp）
    inline bool IsInlineHooked(const BYTE* funcBytes, const BYTE* cleanBytes) {
        // 先看是否以 E9 (near jmp) 或 FF 25 (jmp [mem]) 开头
        if (funcBytes[0] == 0xE9 || funcBytes[0] == 0xEB) return true;
        if (funcBytes[0] == 0xFF && funcBytes[1] == 0x25) return true;
        // 更稳妥：与磁盘干净字节逐字节对比，前 N 字节不一致即认为被 hook
        return memcmp(funcBytes, cleanBytes, 16) != 0;
    }

    // 临时绕过目标进程里被 hook 的 API。
    // 返回保存的 hook 状态（供 RestoreUserHooks 还原）。返回 vector 大小 = 实际处理条数。
    inline std::vector<SavedHook> BypassUserHooks(HANDLE processH) {
        std::vector<SavedHook> saved;
        LoadNtDll();

        for (size_t i = 0; i < g_hookTargetCount; ++i) {
            const HookTarget& t = g_hookTargets[i];
            SavedHook rec = { 0 };
            rec.patchedLen = 0;
            rec.wasHooked = false;

            // 取目标进程内该函数的当前地址（本地同基址，直接哈希解析）
            HMODULE hLocal = GetModuleHandleByHash(HashStringLower(NarrowModuleName(t.moduleName)));
            if (!hLocal) continue;

            uintptr_t funcRva = GetFunctionRvaByName(hLocal, t.functionName);
            if (!funcRva) continue;
            uintptr_t remoteAddr = reinterpret_cast<uintptr_t>(hLocal) + funcRva;
            rec.address = remoteAddr;

            // 读目标进程函数头 16 字节
            BYTE funcBytes[16] = { 0 };
            SIZE_T readBytes = 0;
            BOOL readOk = false;
            if (NtReadVirtualMemory) {
                readOk = NT_SUCCESS(NtReadVirtualMemory(processH, (PVOID)remoteAddr,
                    funcBytes, sizeof(funcBytes), &readBytes)) && readBytes == sizeof(funcBytes);
            }
            if (!readOk) {
                readOk = ReadProcessMemory(processH, (PVOID)remoteAddr,
                    funcBytes, sizeof(funcBytes), &readBytes) && readBytes == sizeof(funcBytes);
            }
            if (!readOk) continue;

            // 从磁盘读干净字节
            BYTE cleanBytes[16] = { 0 };
            int cleanLen = 0;
            if (!ReadCleanBytesFromDisk(t.moduleName, t.functionName, cleanBytes, &cleanLen))
                continue;

            // 判断是否被 hook
            if (!IsInlineHooked(funcBytes, cleanBytes)) {
                continue; // 未被 hook，跳过
            }

            // 被 hook：保存被篡改字节，写回干净字节（临时 unhook）
            memcpy(rec.patchedBytes, funcBytes, 16);
            rec.patchedLen = 16;
            rec.wasHooked = true;

            // 改页权限为可写执行（NtProtectVirtualMemory 底层）
            SIZE_T regionSize = 16;
            PVOID baseAddr = (PVOID)remoteAddr;
            ULONG oldProtect = 0;
            if (NtProtectVirtualMemory) {
                NtProtectVirtualMemory(processH, &baseAddr, &regionSize, PAGE_EXECUTE_READWRITE, &oldProtect);
            }
            else {
                VirtualProtectEx(processH, (PVOID)remoteAddr, 16, PAGE_EXECUTE_READWRITE, &oldProtect);
            }

            // 写回干净字节
            SIZE_T written = 0;
            BOOL writeOk = false;
            if (NtWriteVirtualMemory) {
                writeOk = NT_SUCCESS(NtWriteVirtualMemory(processH, (PVOID)remoteAddr,
                    cleanBytes, 16, &written)) && written == 16;
            }
            if (!writeOk) {
                writeOk = WriteProcessMemory(processH, (PVOID)remoteAddr,
                    cleanBytes, 16, &written) && written == 16;
            }

            // 还原页权限（尽力而为）
            if (NtProtectVirtualMemory) {
                baseAddr = (PVOID)remoteAddr;
                regionSize = 16;
                NtProtectVirtualMemory(processH, &baseAddr, &regionSize, oldProtect, &oldProtect);
            }

            if (writeOk) {
                saved.push_back(rec);
            }
        }
        return saved;
    }

    // 还原之前绕过的 hook（把被篡改字节写回去）
    inline void RestoreUserHooks(HANDLE processH, std::vector<SavedHook>& saved) {
        LoadNtDll();
        for (const auto& rec : saved) {
            if (!rec.wasHooked || rec.patchedLen == 0) continue;

            SIZE_T regionSize = 16;
            PVOID baseAddr = (PVOID)rec.address;
            ULONG oldProtect = 0;
            if (NtProtectVirtualMemory) {
                NtProtectVirtualMemory(processH, &baseAddr, &regionSize, PAGE_EXECUTE_READWRITE, &oldProtect);
            }

            SIZE_T written = 0;
            if (NtWriteVirtualMemory) {
                NtWriteVirtualMemory(processH, (PVOID)rec.address, (PVOID)rec.patchedBytes, 16, &written);
            }
            else {
                WriteProcessMemory(processH, (PVOID)rec.address, rec.patchedBytes, 16, &written);
            }

            if (NtProtectVirtualMemory) {
                baseAddr = (PVOID)rec.address;
                regionSize = 16;
                NtProtectVirtualMemory(processH, &baseAddr, &regionSize, oldProtect, &oldProtect);
            }
        }
        saved.clear();
    }

}

#endif
