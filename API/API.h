#ifndef MM_API_H
#define MM_API_H

#include "Config.h"
#include "Util.h"
#include "Shellcode.h"
#include "ManualMap.h"
#include "LoadLibrary.h"
#include "HookBypass.h"

namespace ManualMapInjector {

    bool ManualMapInject(DWORD targetPID, BYTE* dllBuffer, size_t fileSize);
    HMODULE LoadLibraryInject(DWORD targetPID, const std::wstring& dllPath);

}

#endif