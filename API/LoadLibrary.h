#ifndef MM_LOADLIBRARY_H
#define MM_LOADLIBRARY_H

#include "Util.h"
#include "NT.h"
#include <string>

#pragma warning(disable: 28251)

namespace ManualMapInjector {

    // ==================== 标准 LoadLibrary 注入（推荐给大型 C++ DLL） ====================
    // 为什么需要它：
    //   手动映射(ManualMapInject)只做基础导入表修复 + 直接调 DllMain，不处理 TLS 回调、
    //   CRT 初始化(_CRT_INIT)、API-set 重定向、延迟导入、SEH 表注册。
    //   对依赖 MSVCP140/VCRUNTIME140 + TLS + SEH 的大型 C++ DLL(如 TempleWare)，
    //   手动映射必然在 DllMain 里 0xC0000005 崩溃。
    //
    //   本函数改用标准 LoadLibraryW：让系统加载器完整处理上述所有初始化。
    //   代价：DLL 会登记进目标进程的 PEB(模块枚举可见)。但这个可以由 DLL 自身
    //   的自隐藏(PEB 断链)抹掉 —— TempleWare 已内置 peb_hide::HideModule()。
    //
    // 原理(x64)：
    //   kernel32.dll 是系统 DLL，全系统共享同一物理页，注入器进程与目标进程的
    //   LoadLibraryW 地址完全一致，直接取本地地址即可，无需读远程模块(避免 VAC 299)。
    //
    // 返回：远程线程执行 LoadLibraryW 后 DLL 的基址；NULL 表示失败。
    // 失败原因通过 outReason(可选) 返回。
    inline HMODULE LoadLibraryInject(DWORD targetPID, const std::wstring& dllPath, std::wstring* outReason = nullptr) {
        if (dllPath.empty()) {
            if (outReason) *outReason = L"DLL 路径为空";
            return NULL;
        }

        LoadNtDll();

        // 打开目标进程（精确权限掩码 + 兜底）
        HANDLE processHandle = NULL;
        OBJECT_ATTRIBUTES objAttr = { sizeof(OBJECT_ATTRIBUTES) };
        CLIENT_ID clientId = { (HANDLE)targetPID, NULL };
        const ACCESS_MASK desiredAccess = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
            PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE |
            PROCESS_DUP_HANDLE | PROCESS_SUSPEND_RESUME;
        NTSTATUS status = NtOpenProcess ? NtOpenProcess(&processHandle, desiredAccess, &objAttr, &clientId) : STATUS_UNSUCCESSFUL;
        if (!NT_SUCCESS(status) || !processHandle) {
            processHandle = OpenProcess(desiredAccess, FALSE, targetPID);
            if (!processHandle) {
                if (outReason) *outReason = L"打开目标进程失败(OpenProcess 错误码 " + std::to_wstring(GetLastError()) + L")";
                return NULL;
            }
        }
        unique_handle uniqueProcessHandle(static_cast<void*>(processHandle), HandleDeleter());
        HANDLE processH = static_cast<HANDLE>(uniqueProcessHandle.get());

        // 取 LoadLibraryW 地址（本地 kernel32 共享基址，去明文）
        HMODULE hKernel32 = GetModuleHandleByHash(HashStringLower("kernel32.dll"));
        if (!hKernel32) {
            if (outReason) *outReason = L"无法定位 kernel32";
            return NULL;
        }
        FARPROC pLoadLibraryW = GetProcAddressByHash(hKernel32, HashStringLower("LoadLibraryW"));
        if (!pLoadLibraryW) {
            if (outReason) *outReason = L"无法找到 LoadLibraryW";
            return NULL;
        }

        // 在目标进程分配内存写 DLL 路径
        const SIZE_T pathBytes = (dllPath.size() + 1) * sizeof(wchar_t);
        void* remotePath = VirtualAllocEx(processH, NULL, pathBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remotePath) {
            if (outReason) *outReason = L"分配路径内存失败(错误码 " + std::to_wstring(GetLastError()) + L")";
            return NULL;
        }
        VirtualFreeDeleter pathDeleter(processH);
        unique_virtual_mem pathWrapper(remotePath, pathDeleter);

        SIZE_T bytesWritten = 0;
        if (!WriteProcessMemory(processH, remotePath, dllPath.c_str(), pathBytes, &bytesWritten) ||
            bytesWritten != pathBytes) {
            if (outReason) *outReason = L"写路径失败(错误码 " + std::to_wstring(GetLastError()) + L")";
            return NULL;
        }

        // 创建远程线程执行 LoadLibraryW。
        // 用 NtCreateThreadEx 而非 CreateRemoteThread：kernel32 的 CreateRemoteThread
        // 是 VAC/VMProtect 必 hook 的目标，改用 ntdll 原生 API 绕过这一层。
        HANDLE threadHandle = NULL;
        OBJECT_ATTRIBUTES threadObjAttr = { sizeof(OBJECT_ATTRIBUTES) };
        status = NtCreateThreadEx ? NtCreateThreadEx(&threadHandle, THREAD_ALL_ACCESS, &threadObjAttr,
            processH, (PVOID)pLoadLibraryW, remotePath, 0, 0, 0, 0, NULL) : STATUS_UNSUCCESSFUL;
        if (!NT_SUCCESS(status) || !threadHandle) {
            DWORD err = RtlNtStatusToDosError ? RtlNtStatusToDosError(status) : GetLastError();
            if (outReason) *outReason = L"NtCreateThreadEx 失败(错误码 " + std::to_wstring(err) + L")";
            return NULL;
        }
        unique_handle uniqueThreadHandle(static_cast<void*>(threadHandle), HandleDeleter());

        // 等待远程线程执行完 LoadLibraryW，拿返回值(= DLL 基址)
        if (NtWaitForSingleObject) {
            NtWaitForSingleObject(static_cast<HANDLE>(uniqueThreadHandle.get()), FALSE, NULL);
        }
        else {
            WaitForSingleObject(static_cast<HANDLE>(uniqueThreadHandle.get()), INFINITE);
        }

        DWORD exitCode = 0;
        GetExitCodeThread(static_cast<HANDLE>(uniqueThreadHandle.get()), &exitCode);

        if (exitCode == 0) {
            if (outReason) *outReason = L"LoadLibraryW 返回 NULL（依赖缺失/路径不存在/被反作弊拦截）";
            return NULL;
        }

        return reinterpret_cast<HMODULE>(exitCode);
    }

    // ==================== LdrLoadDll 注入（绕过 LoadLibraryW 的 hook） ====================
    // 背景：CS2 受 VAC/VMProtect 保护，会 hook kernel32 的 LoadLibraryW/LoadLibraryA，
    // 检测到可疑模块加载就返回 NULL。但 ntdll 的 LdrLoadDll 是更底层的加载函数，
    // 通常不被 hook。本函数直接调 LdrLoadDll 绕过 LoadLibraryW。
    //
    // 原理：在远程进程写一个 UNICODE_STRING 结构 + DLL 路径，
    // 用 NtCreateThreadEx 起线程调 LdrLoadDll(NULL, 0, &unicodeString, &moduleHandle)。
    //
    // LdrLoadDll 签名：
    //   NTSTATUS LdrLoadDll(PWCHAR PathToFile, PULONG Flags,
    //                       PUNICODE_STRING ModuleFileName, PHANDLE ModuleHandle);
    // 返回：成功时 ModuleHandle 指向的地址 = DLL 基址；失败返回 NULL。
    inline HMODULE LdrLoadDllInject(DWORD targetPID, const std::wstring& dllPath, std::wstring* outReason = nullptr) {
        if (dllPath.empty()) {
            if (outReason) *outReason = L"DLL 路径为空";
            return NULL;
        }

        LoadNtDll();

        // 打开目标进程
        HANDLE processHandle = NULL;
        OBJECT_ATTRIBUTES objAttr = { sizeof(OBJECT_ATTRIBUTES) };
        CLIENT_ID clientId = { (HANDLE)targetPID, NULL };
        const ACCESS_MASK desiredAccess = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
            PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_DUP_HANDLE;
        NTSTATUS status = NtOpenProcess ? NtOpenProcess(&processHandle, desiredAccess, &objAttr, &clientId) : STATUS_UNSUCCESSFUL;
        if (!NT_SUCCESS(status) || !processHandle) {
            processHandle = OpenProcess(desiredAccess, FALSE, targetPID);
            if (!processHandle) {
                if (outReason) *outReason = L"打开目标进程失败(错误码 " + std::to_wstring(GetLastError()) + L")";
                return NULL;
            }
        }
        unique_handle uniqueProcessHandle(static_cast<void*>(processHandle), HandleDeleter());
        HANDLE processH = static_cast<HANDLE>(uniqueProcessHandle.get());

        // 取 LdrLoadDll 地址（ntdll 全系统共享，本地哈希解析，去明文）
        HMODULE hNtdll = GetModuleHandleByHash(HashStringLower("ntdll.dll"));
        if (!hNtdll) {
            if (outReason) *outReason = L"无法定位 ntdll";
            return NULL;
        }
        FARPROC pLdrLoadDll = GetProcAddressByHash(hNtdll, HashStringLower("LdrLoadDll"));
        if (!pLdrLoadDll) {
            if (outReason) *outReason = L"无法找到 LdrLoadDll";
            return NULL;
        }

        // 在远程进程分配一块内存，写入：
        //   [UNICODE_STRING 结构][DLL 路径宽字符 + 结尾 0][HANDLE 槽]
        const SIZE_T pathBytes = (dllPath.size() + 1) * sizeof(wchar_t);
        const SIZE_T totalBytes = sizeof(UNICODE_STRING) + pathBytes + sizeof(HMODULE);
        void* remoteMem = VirtualAllocEx(processH, NULL, totalBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remoteMem) {
            if (outReason) *outReason = L"分配远程内存失败(错误码 " + std::to_wstring(GetLastError()) + L")";
            return NULL;
        }
        VirtualFreeDeleter memDeleter(processH);
        unique_virtual_mem memWrapper(remoteMem, memDeleter);

        // 计算各字段的远程地址
        auto* pUnicodeStr = reinterpret_cast<UNICODE_STRING*>(remoteMem);
        auto* pPathBuf = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(remoteMem) + sizeof(UNICODE_STRING));
        auto* pModuleHandle = reinterpret_cast<HMODULE*>(reinterpret_cast<BYTE*>(remoteMem) + sizeof(UNICODE_STRING) + pathBytes);

        // 构造 UNICODE_STRING（注意：Length 是字节数，不含结尾 NULL）
        UNICODE_STRING us = { 0 };
        us.Length = static_cast<USHORT>(dllPath.size() * sizeof(wchar_t));
        us.MaximumLength = static_cast<USHORT>(pathBytes);
        us.Buffer = pPathBuf;

        // 写入：UNICODE_STRING 结构 + DLL 路径 + 清零的 HMODULE 槽
        HMODULE zeroHandle = NULL;
        SIZE_T written = 0;
        bool ok = true;
        ok = ok && WriteProcessMemory(processH, pUnicodeStr, &us, sizeof(us), &written) && written == sizeof(us);
        ok = ok && WriteProcessMemory(processH, pPathBuf, dllPath.c_str(), pathBytes, &written) && written == pathBytes;
        ok = ok && WriteProcessMemory(processH, pModuleHandle, &zeroHandle, sizeof(zeroHandle), &written) && written == sizeof(zeroHandle);
        if (!ok) {
            if (outReason) *outReason = L"写远程内存失败(错误码 " + std::to_wstring(GetLastError()) + L")";
            return NULL;
        }

        // 用 NtCreateThreadEx 起远程线程调 LdrLoadDll(NULL, 0, &us, &moduleHandle)
        HANDLE threadHandle = NULL;
        OBJECT_ATTRIBUTES threadObjAttr = { sizeof(OBJECT_ATTRIBUTES) };
        status = NtCreateThreadEx ? NtCreateThreadEx(&threadHandle, THREAD_ALL_ACCESS, &threadObjAttr,
            processH, (PVOID)pLdrLoadDll, pUnicodeStr, 0, 0, 0, 0, NULL) : STATUS_UNSUCCESSFUL;
        if (!NT_SUCCESS(status) || !threadHandle) {
            DWORD err = RtlNtStatusToDosError ? RtlNtStatusToDosError(status) : GetLastError();
            if (outReason) *outReason = L"NtCreateThreadEx 失败(错误码 " + std::to_wstring(err) + L")";
            return NULL;
        }
        unique_handle uniqueThreadHandle(static_cast<void*>(threadHandle), HandleDeleter());

        if (NtWaitForSingleObject) {
            NtWaitForSingleObject(static_cast<HANDLE>(uniqueThreadHandle.get()), FALSE, NULL);
        }
        else {
            WaitForSingleObject(static_cast<HANDLE>(uniqueThreadHandle.get()), INFINITE);
        }

        // 读回 ModuleHandle 槽的值 = DLL 基址
        HMODULE loadedBase = NULL;
        SIZE_T readBytes = 0;
        if (NtReadVirtualMemory && NT_SUCCESS(NtReadVirtualMemory(processH, pModuleHandle, &loadedBase, sizeof(loadedBase), &readBytes)) && readBytes == sizeof(loadedBase)) {
            // 成功读回
        }
        else {
            ReadProcessMemory(processH, pModuleHandle, &loadedBase, sizeof(loadedBase), &readBytes);
        }

        if (loadedBase == NULL) {
            if (outReason) *outReason = L"LdrLoadDll 返回失败（被反作弊拦截或路径问题）";
            return NULL;
        }

        return loadedBase;
    }

}
#endif
