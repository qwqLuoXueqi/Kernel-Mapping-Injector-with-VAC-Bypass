#ifndef MM_MANUALMAP_H
#define MM_MANUALMAP_H
#include "API.h"
#include <cstdio>
#include "Util.h"
#include "NT.h"
#include <thread>
#include <chrono>
#include <atomic>

#pragma warning(disable: 28251)  // 忽略 NTSTATUS 警告

namespace ManualMapInjector {
    // 等待进程稳定（不读模块列表，因为 CS2 受 VAC 保护，读模块会返回 ERROR_PARTIAL_COPY=299）
    inline bool WaitForProcessStability(HANDLE processH, DWORD timeoutMs = 1000) {
        wprintf(L"提示：等待目标进程稳定...\n");

        // 用 NtQueryInformationProcess 判断进程是否还在运行（读基本信息，不读模块列表）
        LoadNtDll();
        bool alive = false;
        for (int check = 0; check < 10; ++check) {
            PROCESS_BASIC_INFORMATION pbi = { 0 };
            ULONG returnLength = 0;
            NTSTATUS st = NtQueryInformationProcess ? NtQueryInformationProcess(processH, ProcessBasicInformation, &pbi, sizeof(pbi), &returnLength) : STATUS_UNSUCCESSFUL;
            if (NT_SUCCESS(st)) {
                alive = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs / 10));
        }

        if (alive) {
            wprintf(L"成功：进程稳定。\n");
            return true;
        }
        wprintf(L"警告：无法确认进程状态，但仍尝试注入。\n");
        return true;  // 不再因读不到模块而中止注入
    }

    // ==================== 完整 PE 镜像校验（防畸形 DLL 打崩注入器） ====================
    // 移植自 Potato-Injector 的 validatePeImage, 映射前逐项校验。
    // 任何越界/非法字段都拒绝映射, 避免 malformed PE 让映射器自身越界读写。
    inline bool RangeWithin(size_t offset, size_t length, size_t total) {
        return offset <= total && length <= total - offset;
    }

    inline bool ValidatePeImage(const BYTE* buffer, size_t fileSize, std::wstring& reason) {
        if (!buffer || fileSize < sizeof(IMAGE_DOS_HEADER)) {
            reason = L"缓冲区小于 DOS 头";
            return false;
        }
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(buffer);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) {
            reason = L"无效 DOS 头";
            return false;
        }

        const size_t ntOffset = static_cast<size_t>(dos->e_lfanew);
        if (!RangeWithin(ntOffset, sizeof(IMAGE_NT_HEADERS64), fileSize)) {
            reason = L"NT 头超出文件";
            return false;
        }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(buffer + ntOffset);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != TARGET_MACHINE) {
            reason = L"不是有效 x64 PE";
            return false;
        }
        if (nt->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            reason = L"无效 x64 可选头";
            return false;
        }
        if (nt->FileHeader.NumberOfSections == 0 || nt->FileHeader.NumberOfSections > 96) {
            reason = L"无效节区数量";
            return false;
        }
        if (nt->OptionalHeader.SizeOfImage == 0 ||
            nt->OptionalHeader.SizeOfHeaders > nt->OptionalHeader.SizeOfImage ||
            nt->OptionalHeader.SizeOfHeaders > fileSize) {
            reason = L"无效镜像/头大小";
            return false;
        }

        const size_t sectionTableOffset = ntOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + nt->FileHeader.SizeOfOptionalHeader;
        const size_t sectionTableSize = static_cast<size_t>(nt->FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
        if (!RangeWithin(sectionTableOffset, sectionTableSize, fileSize)) {
            reason = L"节区表超出文件";
            return false;
        }

        const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(buffer + sectionTableOffset);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            const auto& section = sections[i];
            if (section.SizeOfRawData != 0 &&
                !RangeWithin(section.PointerToRawData, section.SizeOfRawData, fileSize)) {
                reason = L"节区原始数据超出文件";
                return false;
            }
            const uint64_t sectionEnd = static_cast<uint64_t>(section.VirtualAddress) +
                (section.Misc.VirtualSize > section.SizeOfRawData ? section.Misc.VirtualSize : section.SizeOfRawData);
            if (sectionEnd > nt->OptionalHeader.SizeOfImage) {
                reason = L"节区虚拟范围超出 SizeOfImage";
                return false;
            }
        }

        if (nt->OptionalHeader.AddressOfEntryPoint != 0 &&
            nt->OptionalHeader.AddressOfEntryPoint >= nt->OptionalHeader.SizeOfImage) {
            reason = L"入口点超出镜像";
            return false;
        }

        // 校验导入目录（若存在）
        if (nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT) {
            const auto& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            if (imports.VirtualAddress != 0 && imports.Size != 0 &&
                imports.VirtualAddress >= nt->OptionalHeader.SizeOfImage) {
                reason = L"导入目录超出镜像";
                return false;
            }
        }

        // 校验重定位目录（若存在）
        if (nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_BASERELOC) {
            const auto& relocs = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
            if (relocs.VirtualAddress != 0 && relocs.Size != 0 &&
                relocs.VirtualAddress >= nt->OptionalHeader.SizeOfImage) {
                reason = L"重定位目录超出镜像";
                return false;
            }
        }
        return true;
    }

    inline bool ManualMapInject(DWORD targetPID, BYTE* dllBuffer, size_t fileSize) {
        wprintf(L"正在将 DLL 注入到 PID: %lu\n", targetPID);
        std::wstring validationReason;
        if (!ValidatePeImage(dllBuffer, fileSize, validationReason)) {
            wprintf(L"错误：PE 镜像校验失败：%s\n", validationReason.c_str());
            return false;
        }
        PIMAGE_DOS_HEADER pDosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(dllBuffer);
        PIMAGE_NT_HEADERS_CURRENT pNtHeaders = reinterpret_cast<PIMAGE_NT_HEADERS_CURRENT>(dllBuffer + pDosHeader->e_lfanew);
        wprintf(L"PE 镜像校验通过。\n");

        LoadNtDll();
        HANDLE processHandle = NULL;
        OBJECT_ATTRIBUTES objAttr = { sizeof(OBJECT_ATTRIBUTES) };
        CLIENT_ID clientId = { (HANDLE)targetPID, NULL };
        // 用精确的权限掩码（CS2 的 VAC 会过滤 PROCESS_ALL_ACCESS，只给受限句柄）
        const ACCESS_MASK desiredAccess = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
            PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE |
            PROCESS_DUP_HANDLE | PROCESS_SUSPEND_RESUME;
        NTSTATUS status = NtOpenProcess ? NtOpenProcess(&processHandle, desiredAccess, &objAttr, &clientId) : STATUS_UNSUCCESSFUL;
        if (!NT_SUCCESS(status) || !processHandle) {
            DWORD error = RtlNtStatusToDosError ? RtlNtStatusToDosError(status) : GetLastError();
            wprintf(L"错误：NtOpenProcess 失败。错误代码：%lu\n", error);
            // 兜底：退回 OpenProcess
            processHandle = OpenProcess(desiredAccess, FALSE, targetPID);
            if (!processHandle) {
                wprintf(L"错误：OpenProcess 也失败。错误代码：%lu\n", GetLastError());
                return false;
            }
        }
        unique_handle uniqueProcessHandle(static_cast<void*>(processHandle), HandleDeleter());
        wprintf(L"目标进程已打开。\n");
        HANDLE processH = static_cast<HANDLE>(uniqueProcessHandle.get());

        if (!WaitForProcessStability(processH)) {
            return false;
        }

        // ==================== 目标进程存活监控线程 ====================
        // 映射过程中若目标进程退出(崩溃/被关), 后续所有 NtWriteVirtualMemory 会失败,
        // 但更糟的是可能写到已释放的地址。用后台线程监控, 一旦目标退出立即中止。
        // 注: 纯内部辅助无需复杂回滚, 只需检测到退出就尽快返回。
        std::atomic_bool targetExited{ false };
        std::thread monitorThread([&]() {
            while (!targetExited.load(std::memory_order_acquire)) {
                DWORD exitCode = 0;
                if (GetExitCodeProcess(processH, &exitCode) && exitCode != STILL_ACTIVE) {
                    targetExited.store(true, std::memory_order_release);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        });

        // ==================== 分配远程内存 ====================
        // 先以 PAGE_READWRITE 分配（避免直接分配 RWX 页被 VAC 标记），写完镜像后再改可执行
        void* allocatedBase = reinterpret_cast<void*>(pNtHeaders->OptionalHeader.ImageBase);
        SIZE_T imageSize = pNtHeaders->OptionalHeader.SizeOfImage;
        SIZE_T regionSize = imageSize;
        ULONG allocProtect = PAGE_READWRITE;
        status = NtAllocateVirtualMemory ? NtAllocateVirtualMemory(processH, &allocatedBase, 0, &regionSize, MEM_COMMIT | MEM_RESERVE, allocProtect) : STATUS_UNSUCCESSFUL;
        if (!NT_SUCCESS(status)) {
            DWORD preferredAllocError = RtlNtStatusToDosError ? RtlNtStatusToDosError(status) : GetLastError();
            wprintf(L"警告：首选基址分配失败于 " TULONGLONG_FORMAT "。错误代码：%lu。尝试在任意位置分配...\n",
                (TULONGLONG)pNtHeaders->OptionalHeader.ImageBase, preferredAllocError);
            allocatedBase = NULL;
            regionSize = imageSize;
            status = NtAllocateVirtualMemory ? NtAllocateVirtualMemory(processH, &allocatedBase, 0, &regionSize, MEM_COMMIT | MEM_RESERVE, allocProtect) : STATUS_UNSUCCESSFUL;
            if (!NT_SUCCESS(status)) {
                DWORD error = RtlNtStatusToDosError ? RtlNtStatusToDosError(status) : GetLastError();
                wprintf(L"错误：NtAllocateVirtualMemory 失败。错误代码：%lu\n", error);
                // 兜底：退回 VirtualAllocEx
                allocatedBase = VirtualAllocEx(processH, NULL, imageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (!allocatedBase) {
                    wprintf(L"错误：VirtualAllocEx 也失败。错误代码：%lu\n", GetLastError());
                    return false;
                }
                wprintf(L"通过 VirtualAllocEx 兜底分配成功。\n");
            }
        }
        VirtualFreeDeleter deleter(processH);
        unique_virtual_mem allocatedBaseWrapper(allocatedBase, deleter);
        wprintf(L"为 DLL 镜像分配内存于： " TULONGLONG_FORMAT "\n", (TULONGLONG)allocatedBase);

        // ==================== 在本地构建完整镜像（包含重定位） ====================
        BYTE* localImage = new BYTE[imageSize]();  // 初始化为零

        // 复制 PE 头
        DWORD sizeOfHeaders = pNtHeaders->OptionalHeader.SizeOfHeaders;
        if (sizeOfHeaders > fileSize) {
            wprintf(L"错误：SizeOfHeaders 大于缓冲区大小。\n");
            delete[] localImage;
            return false;
        }
        memcpy(localImage, dllBuffer, sizeOfHeaders);

        // 复制节数据
        PIMAGE_SECTION_HEADER pSectionHeader = IMAGE_FIRST_SECTION(pNtHeaders);
        for (WORD i = 0; i < pNtHeaders->FileHeader.NumberOfSections; ++i, ++pSectionHeader) {
            if (pSectionHeader->PointerToRawData != 0 &&
                (pSectionHeader->PointerToRawData > fileSize ||
                    pSectionHeader->PointerToRawData + pSectionHeader->SizeOfRawData > fileSize)) {
                wprintf(L"错误：节 %d 原始数据超出范围。\n", i);
                delete[] localImage;
                return false;
            }
            if (static_cast<TULONGLONG>(pSectionHeader->VirtualAddress) + pSectionHeader->Misc.VirtualSize > imageSize) {
                wprintf(L"错误：节 %d 虚拟地址超出分配内存范围。\n", i);
                delete[] localImage;
                return false;
            }
            if (pSectionHeader->SizeOfRawData > 0) {
                void* sectionTargetAddress = localImage + pSectionHeader->VirtualAddress;
                LPVOID sectionSourceAddress = dllBuffer + pSectionHeader->PointerToRawData;
                memcpy(sectionTargetAddress, sectionSourceAddress, pSectionHeader->SizeOfRawData);
            }
        }
        wprintf(L"本地镜像构建完成。\n");

        // ==================== 在本地处理重定位 ====================
        TULONGLONG delta = (TULONGLONG)allocatedBase - pNtHeaders->OptionalHeader.ImageBase;
        if (delta != 0) {
            wprintf(L"需要重定位。偏移量：" TULONGLONG_FORMAT "\n", delta);
            IMAGE_DATA_DIRECTORY relocDir = pNtHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
            if (relocDir.VirtualAddress == 0 || relocDir.Size == 0) {
                wprintf(L"警告：需要重定位但缺少重定位表。\n");
            }
            else {
                DWORD relocOffset = RvaToFileOffset(pNtHeaders, relocDir.VirtualAddress, fileSize);
                if (relocOffset == 0 || relocOffset + relocDir.Size > fileSize) {
                    wprintf(L"错误：重定位目录超出范围。\n");
                    delete[] localImage;
                    return false;
                }
                PIMAGE_BASE_RELOCATION pRelocBlock = (PIMAGE_BASE_RELOCATION)(dllBuffer + relocOffset);
                LPBYTE relocTableEnd = (LPBYTE)pRelocBlock + relocDir.Size;

                unsigned long relocationSuccessCount = 0;
                unsigned long relocationSkippedCount = 0;
                unsigned long relocationFailedCount = 0;
                unsigned long long relocationCounter = 0;

                while ((LPBYTE)pRelocBlock < relocTableEnd && pRelocBlock->SizeOfBlock > 0) {
                    if ((LPBYTE)pRelocBlock + pRelocBlock->SizeOfBlock > relocTableEnd) {
                        wprintf(L"错误：无效的重定位块大小。\n");
                        delete[] localImage;
                        return false;
                    }
                    DWORD count = (pRelocBlock->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
                    PWORD pRelocEntry = (PWORD)((LPBYTE)pRelocBlock + sizeof(IMAGE_BASE_RELOCATION));

                    for (DWORD i = 0; i < count; ++i, ++pRelocEntry) {
                        WORD type = (*pRelocEntry >> 12);
                        WORD offset = (*pRelocEntry & 0xFFF);
                        relocationCounter++;

                        if (type == IMAGE_REL_BASED_ABSOLUTE) { // 0
                            relocationSkippedCount++;
                            continue;
                        }

                        DWORD rva = pRelocBlock->VirtualAddress + offset;
                        void* patchAddrLocal = localImage + rva;

                        if ((LPBYTE)patchAddrLocal < localImage || (LPBYTE)patchAddrLocal >= localImage + imageSize) {
                            // 重定位失败日志 - 使用指定格式
                            wprintf(L"[%llu] Relocation=0x%llX | Address=-- → -- | Offset=0x%llX | RVA=0x%lX\n",
                                relocationCounter,
                                (unsigned long long)type,
                                (unsigned long long)offset,
                                rva);
                            relocationFailedCount++;
                            continue;
                        }

#if defined(_WIN64)
                        // x64 注入 (只处理 DIR64) - 保持原有操作
                        if (type == IMAGE_REL_BASED_DIR64) { // 10
                            TULONGLONG* patchAddr = reinterpret_cast<TULONGLONG*>(patchAddrLocal);
                            TULONGLONG originalValue = *patchAddr;
                            TULONGLONG newValue = originalValue + delta;
                            *patchAddr = newValue;

                            // 重定位成功日志 - 使用指定格式
                            wprintf(L"[%llu] Relocation=0x%llX | Address=0x%llX → 0x%llX | Offset=0x%llX | RVA=0x%lX\n",
                                relocationCounter,
                                (unsigned long long)patchAddrLocal,
                                originalValue, newValue,
                                (unsigned long long)offset,
                                rva);
                            relocationSuccessCount++;
                        }
                        else {
                            // 重定位失败日志（不支持的类型）
                            wprintf(L"[%llu] Relocation=0x%llX | Address=-- → -- | Offset=0x%llX | RVA=0x%lX\n",
                                relocationCounter,
                                (unsigned long long)type,
                                (unsigned long long)offset,
                                rva);
                            relocationFailedCount++;
                        }
#else
                        // x86 注入 (处理 HIGHLOW 和 HIGHADJ) - 保持原有操作
                        if (type == IMAGE_REL_BASED_HIGHLOW) { // 3
                            DWORD* patchAddr = reinterpret_cast<DWORD*>(patchAddrLocal);
                            DWORD originalValue = *patchAddr;
                            DWORD newValue = originalValue + static_cast<DWORD>(delta);
                            *patchAddr = newValue;

                            // 重定位成功日志 - 使用指定格式
                            wprintf(L"[%llu] Relocation=0x%llX | Address=0x%llX → 0x%llX | Offset=0x%llX | RVA=0x%lX\n",
                                relocationCounter,
                                (unsigned long long)patchAddrLocal,
                                (unsigned long long)originalValue, (unsigned long long)newValue,
                                (unsigned long long)offset,
                                rva);
                            relocationSuccessCount++;
                        }
                        else if (type == IMAGE_REL_BASED_HIGH) { // 1
                            WORD* patchAddr = reinterpret_cast<WORD*>(patchAddrLocal);
                            WORD originalValue = *patchAddr;
                            WORD newValue = originalValue + HIWORD(static_cast<DWORD>(delta));
                            *patchAddr = newValue;

                            // 重定位成功日志 - 使用指定格式
                            wprintf(L"[%llu] Relocation=0x%llX | Address=0x%llX → 0x%llX | Offset=0x%llX | RVA=0x%lX\n",
                                relocationCounter,
                                (unsigned long long)patchAddrLocal,
                                (unsigned long long)originalValue, (unsigned long long)newValue,
                                (unsigned long long)offset,
                                rva);
                            relocationSuccessCount++;
                        }
                        else if (type == IMAGE_REL_BASED_LOW) { // 2
                            WORD* patchAddr = reinterpret_cast<WORD*>(patchAddrLocal);
                            WORD originalValue = *patchAddr;
                            WORD newValue = originalValue + LOWORD(static_cast<DWORD>(delta));
                            *patchAddr = newValue;

                            // 重定位成功日志 - 使用指定格式
                            wprintf(L"[%llu] Relocation=0x%llX | Address=0x%llX → 0x%llX | Offset=0x%llX | RVA=0x%lX\n",
                                relocationCounter,
                                (unsigned long long)patchAddrLocal,
                                (unsigned long long)originalValue, (unsigned long long)newValue,
                                (unsigned long long)offset,
                                rva);
                            relocationSuccessCount++;
                        }
                        else if (type == IMAGE_REL_BASED_HIGHADJ) { // 4
                            if (i + 1 >= count) {
                                // 重定位失败日志（HIGHADJ 缺少调整值）
                                wprintf(L"[%llu] Relocation=0x%llX | Address=-- → -- | Offset=0x%llX | RVA=0x%lX\n",
                                    relocationCounter,
                                    (unsigned long long)type,
                                    (unsigned long long)offset,
                                    rva);
                                relocationFailedCount++;
                                continue;
                            }
                            ++i; // 消耗下一个条目
                            ++pRelocEntry;
                            SHORT adjustment = static_cast<SHORT>(*pRelocEntry); // 这是调整值

                            WORD* highPartAddr = reinterpret_cast<WORD*>(patchAddrLocal);
                            WORD originalValue = *highPartAddr;

                            // 计算完整的 32 位地址
                            DWORD originalAddr = (originalValue << 16) + adjustment;
                            DWORD newAddr = originalAddr + static_cast<DWORD>(delta);

                            // 写回新的 16 位高位，必须 +0x8000 来处理有符号的低位
                            WORD newValue = static_cast<WORD>((newAddr + 0x8000) >> 16);
                            *highPartAddr = newValue;

                            // 重定位成功日志 - 使用指定格式
                            wprintf(L"[%llu] Relocation=0x%llX | Address=0x%llX → 0x%llX | Offset=0x%llX | RVA=0x%lX\n",
                                relocationCounter,
                                (unsigned long long)patchAddrLocal,
                                (unsigned long long)originalValue, (unsigned long long)newValue,
                                (unsigned long long)offset,
                                rva);
                            relocationSuccessCount++;
                        }
                        else {
                            // 重定位失败日志（不支持的类型）
                            wprintf(L"[%llu] Relocation=0x%llX | Address=-- → -- | Offset=0x%llX | RVA=0x%lX\n",
                                relocationCounter,
                                (unsigned long long)type,
                                (unsigned long long)offset,
                                rva);
                            relocationFailedCount++;
                        }
#endif
                    }
                    pRelocBlock = (PIMAGE_BASE_RELOCATION)((LPBYTE)pRelocBlock + pRelocBlock->SizeOfBlock);
                }
                wprintf(L"重定位已处理。成功：%lu，失败：%lu，跳过：%lu\n",
                    relocationSuccessCount, relocationFailedCount, relocationSkippedCount);
            }
        }
        else {
            wprintf(L"无需重定位。\n");
        }

        // ==================== 一次性写入完整镜像 ====================
        SIZE_T bytesWritten;
        status = NtWriteVirtualMemory ? NtWriteVirtualMemory(processH, allocatedBase, localImage, imageSize, &bytesWritten) : STATUS_UNSUCCESSFUL;
        if (!NT_SUCCESS(status) || bytesWritten != imageSize) {
            DWORD error = RtlNtStatusToDosError ? RtlNtStatusToDosError(status) : GetLastError();
            wprintf(L"错误：NtWriteVirtualMemory（完整镜像）失败。错误代码：%lu\n", error);
            delete[] localImage;
            return false;
        }
        wprintf(L"完整镜像已写入目标进程。\n");

        // ==================== 将镜像内存页改为可执行 ====================
        {
            void* protectBase = allocatedBase;
            SIZE_T protectSize = imageSize;
            ULONG oldProtect = 0;
            NTSTATUS protStatus = NtProtectVirtualMemory ? NtProtectVirtualMemory(processH, &protectBase, &protectSize, PAGE_EXECUTE_READ, &oldProtect) : STATUS_UNSUCCESSFUL;
            if (!NT_SUCCESS(protStatus)) {
                wprintf(L"警告：NtProtectVirtualMemory 改可执行失败（错误码 %lu），尝试 VirtualProtectEx...\n",
                    (unsigned long)(RtlNtStatusToDosError ? RtlNtStatusToDosError(protStatus) : GetLastError()));
                DWORD old = 0;
                if (!VirtualProtectEx(processH, allocatedBase, imageSize, PAGE_EXECUTE_READ, &old)) {
                    wprintf(L"警告：VirtualProtectEx 也失败（错误码 %lu），继续尝试注入。\n", (unsigned long)GetLastError());
                }
                else {
                    wprintf(L"通过 VirtualProtectEx 改可执行成功。\n");
                }
            }
            else {
                wprintf(L"镜像内存已改为可执行（PAGE_EXECUTE_READ）。\n");
            }
        }

        // 清理本地镜像
        delete[] localImage;

        // ==================== 准备 shellcode 数据 ====================
        // 关键修复：不再读远程进程的模块基址（CS2 受 VAC 保护，Toolhelp/PEB 都会返回
        // ERROR_PARTIAL_COPY=299）。x64 下 kernel32.dll 是系统 DLL，全系统共享同一物理页，
        // 注入器进程与目标进程的 kernel32 基址完全一致，直接取本地地址即可。
        // 去明文特征：用哈希解析取 kernel32 基址和 LoadLibraryA/GetProcAddress 地址,
        // 二进制里不留下 "kernel32.dll" / "LoadLibraryA" / "GetProcAddress" 明文字符串。
        HMODULE hKernel32 = GetModuleHandleByHash(HashStringLower("kernel32.dll"));
        if (!hKernel32) {
            wprintf(L"错误：无法定位 kernel32（PEB 遍历失败）。\n");
            return false;
        }
        FARPROC pLoadLibraryA_Remote = GetProcAddressByHash(hKernel32, HashStringLower("LoadLibraryA"));
        FARPROC pGetProcAddress_Remote = GetProcAddressByHash(hKernel32, HashStringLower("GetProcAddress"));
        if (!pLoadLibraryA_Remote || !pGetProcAddress_Remote) {
            wprintf(L"错误：无法找到 LoadLibraryA 或 GetProcAddress。\n");
            return false;
        }

        // 验证导入表目录
        IMAGE_DATA_DIRECTORY importDir = pNtHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (importDir.VirtualAddress == 0 || importDir.Size == 0) {
            wprintf(L"警告：DLL 没有导入表。\n");
        }

        void* shellcodeDataMem = NULL;
        SIZE_T shellcodeDataSize = sizeof(ShellcodeData);
        status = NtAllocateVirtualMemory ? NtAllocateVirtualMemory(processH, &shellcodeDataMem, 0, &shellcodeDataSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) : STATUS_UNSUCCESSFUL;
        if (!NT_SUCCESS(status)) {
            DWORD error = RtlNtStatusToDosError ? RtlNtStatusToDosError(status) : GetLastError();
            wprintf(L"错误：NtAllocateVirtualMemory（ShellcodeData）失败。错误代码：%lu\n", error);
            return false;
        }
        unique_virtual_mem shellcodeDataWrapper(shellcodeDataMem, deleter);

        // ==================== 关键修复：确保 ShellcodeData 结构正确 ====================
        ShellcodeData data;
        memset(&data, 0, sizeof(ShellcodeData));  // 清零初始化

        data.InjectedDllBase = allocatedBase;
        data.pLoadLibraryA = (LoadLibraryA_t)pLoadLibraryA_Remote;
        data.pGetProcAddress = (GetProcAddress_t)pGetProcAddress_Remote;
        data.ImportDirRVA = importDir.VirtualAddress;
        data.ImportDirSize = importDir.Size;
        data.TlsDirRVA = pNtHeaders->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_TLS
            ? pNtHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress : 0;

        // 添加调试信息
        wprintf(L"ShellcodeData 信息:\n");
        wprintf(L"  InjectedDllBase: " TULONGLONG_FORMAT "\n", (TULONGLONG)data.InjectedDllBase);
        wprintf(L"  pLoadLibraryA: " TULONGLONG_FORMAT "\n", (TULONGLONG)data.pLoadLibraryA);
        wprintf(L"  pGetProcAddress: " TULONGLONG_FORMAT "\n", (TULONGLONG)data.pGetProcAddress);
        wprintf(L"  ImportDirRVA: 0x%08X\n", data.ImportDirRVA);
        wprintf(L"  ImportDirSize: 0x%08X\n", data.ImportDirSize);
        wprintf(L"  TlsDirRVA: 0x%08X\n", data.TlsDirRVA);

        status = NtWriteVirtualMemory ? NtWriteVirtualMemory(processH, shellcodeDataMem, &data, sizeof(ShellcodeData), &bytesWritten) : STATUS_UNSUCCESSFUL;
        if (!NT_SUCCESS(status) || bytesWritten != sizeof(ShellcodeData)) {
            DWORD error = RtlNtStatusToDosError ? RtlNtStatusToDosError(status) : GetLastError();
            wprintf(L"错误：NtWriteVirtualMemory（ShellcodeData）失败。错误代码：%lu\n", error);
            return false;
        }

        // ==================== 写入 shellcode ====================
        SIZE_T shellcodeSize = GetFunctionSize(Shellcode);
        wprintf(L"Shellcode 大小: %zu 字节\n", shellcodeSize);

        // 验证 shellcode 大小
        if (shellcodeSize == 0 || shellcodeSize > 4096) {
            wprintf(L"错误：无效的 shellcode 大小。\n");
            return false;
        }

        void* shellcodeMem = NULL;
        SIZE_T shellcodeAllocSize = shellcodeSize;
        status = NtAllocateVirtualMemory ? NtAllocateVirtualMemory(processH, &shellcodeMem, 0, &shellcodeAllocSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) : STATUS_UNSUCCESSFUL;
        if (!NT_SUCCESS(status)) {
            DWORD error = RtlNtStatusToDosError ? RtlNtStatusToDosError(status) : GetLastError();
            wprintf(L"错误：NtAllocateVirtualMemory（shellcode）失败。错误代码：%lu\n", error);
            shellcodeMem = VirtualAllocEx(processH, NULL, shellcodeSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!shellcodeMem) {
                wprintf(L"错误：VirtualAllocEx（shellcode）也失败。错误代码：%lu\n", GetLastError());
                return false;
            }
        }
        unique_virtual_mem shellcodeWrapper(shellcodeMem, deleter);

        status = NtWriteVirtualMemory ? NtWriteVirtualMemory(processH, shellcodeMem, (LPVOID)Shellcode, shellcodeSize, &bytesWritten) : STATUS_UNSUCCESSFUL;
        if (!NT_SUCCESS(status) || bytesWritten != shellcodeSize) {
            DWORD error = RtlNtStatusToDosError ? RtlNtStatusToDosError(status) : GetLastError();
            wprintf(L"错误：NtWriteVirtualMemory（shellcode）失败。错误代码：%lu\n", error);
            return false;
        }

        // shellcode 写入后改为可执行
        {
            void* scProtectBase = shellcodeMem;
            SIZE_T scProtectSize = shellcodeSize;
            ULONG scOldProtect = 0;
            if (!(NtProtectVirtualMemory && NT_SUCCESS(NtProtectVirtualMemory(processH, &scProtectBase, &scProtectSize, PAGE_EXECUTE_READ, &scOldProtect)))) {
                DWORD scOld = 0;
                VirtualProtectEx(processH, shellcodeMem, shellcodeSize, PAGE_EXECUTE_READ, &scOld);
            }
        }

        // ==================== 清理和创建线程 ====================
        SecureZeroMemory(dllBuffer, fileSize);
        wprintf(L"原始 DLL 缓冲区已从内存中擦除。\n");

        // 创建远程线程前, 最终确认目标进程仍存活
        if (targetExited.load(std::memory_order_acquire)) {
            wprintf(L"错误：目标进程在映射过程中已退出, 中止注入。\n");
            monitorThread.join();
            return false;
        }

        // 添加短暂延迟确保内存稳定
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        HANDLE threadHandle = NULL;
        OBJECT_ATTRIBUTES threadObjAttr = { sizeof(OBJECT_ATTRIBUTES) };
        status = NtCreateThreadEx ? NtCreateThreadEx(&threadHandle, THREAD_ALL_ACCESS, &threadObjAttr, processH, (PVOID)shellcodeMem, shellcodeDataMem, 0, 0, 0, 0, NULL) : STATUS_UNSUCCESSFUL;
        if (!NT_SUCCESS(status) || !threadHandle) {
            DWORD error = RtlNtStatusToDosError ? RtlNtStatusToDosError(status) : GetLastError();
            wprintf(L"错误：NtCreateThreadEx 失败。错误代码：%lu\n", error);
            monitorThread.join();
            return false;
        }
        unique_handle uniqueThreadHandle(static_cast<void*>(threadHandle), HandleDeleter());

        // 远程线程已创建, 监控使命完成, 停止并回收监控线程
        targetExited.store(true, std::memory_order_release);  // 复用此标志让监控线程退出循环
        monitorThread.join();

        status = NtWaitForSingleObject ? NtWaitForSingleObject(static_cast<HANDLE>(uniqueThreadHandle.get()), FALSE, NULL) : STATUS_UNSUCCESSFUL;
        if (!NT_SUCCESS(status)) {
            DWORD error = RtlNtStatusToDosError ? RtlNtStatusToDosError(status) : GetLastError();
            wprintf(L"警告：NtWaitForSingleObject 失败。错误代码：%lu\n", error);
        }

        DWORD exitCode = 0;
        GetExitCodeThread(static_cast<HANDLE>(uniqueThreadHandle.get()), &exitCode);

        // 输出详细的退出代码信息
        if (exitCode == 0xC00001A5) {
            wprintf(L"远程线程完成，退出代码：0xC00001A5 (检测到无效的异常处理程序例程)\n");
        }
        else if (exitCode == 0xC0000005) {
            wprintf(L"远程线程完成，退出代码：0xC0000005 (访问冲突)\n");
            wprintf(L"崩溃分析：可能的原因：\n");
            wprintf(L"  1. 导入表解析失败\n");
            wprintf(L"  2. DLL 依赖项缺失\n");
            wprintf(L"  3. Shellcode 参数错误\n");
            wprintf(L"  4. 重定位不完整\n");
        }
        else {
            wprintf(L"远程线程完成，退出代码：0x%08X (%lu)\n", exitCode, exitCode);
        }

        // 详细的错误代码解析
        if (exitCode == 0xC0000005) {
            wprintf(L"崩溃：访问违规 - 检查 DLL 依赖或内存页。\n");
        }
        else if (exitCode == (DWORD)-2) {
            wprintf(L"错误：Shellcode 失败 - 无效 DOS 签名（重定位后恢复）。\n");
        }
        else if (exitCode == 0) {
            wprintf(L"警告：DllMain 返回 FALSE。\n");
        }
        else if (exitCode < 100) {
            if (exitCode == (DWORD)-1) wprintf(L"错误：Shellcode 失败 - 无效参数。\n");
            else if (exitCode == (DWORD)-3) wprintf(L"错误：Shellcode 失败 - 无效 NT 签名。\n");
            else if (exitCode == (DWORD)-4) wprintf(L"错误：Shellcode 失败 - 非 DLL 文件。\n");
            else if (exitCode == (DWORD)-5) wprintf(L"错误：Shellcode 失败 - 加载依赖 DLL 失败。\n");
            else if (exitCode == (DWORD)-6) wprintf(L"错误：Shellcode 失败 - 获取函数地址失败。\n");
            else if (exitCode == (DWORD)-7) wprintf(L"错误：Shellcode 失败 - RVA 越界。\n");
            else if (exitCode == (DWORD)-8) wprintf(L"错误：Shellcode 失败 - DllMain 异常。\n");
            else wprintf(L"注意：Shellcode 返回 %lu。\n", exitCode);
        }

        allocatedBaseWrapper.release();
        shellcodeDataWrapper.release();
        shellcodeWrapper.release();
        wprintf(L"注入完成。\n");
        return exitCode == 1;  // 成功时 shellcode 应该返回 1
    }
}
#endif