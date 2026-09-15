// ────────────────────────────────────────────────────────────────────────────
//  manual-map / config
//  Picks the PE structures that match the architecture being compiled for.
//
//  @role     api                  @thread  compile-time only
//  @touches  win32
// ────────────────────────────────────────────────────────────────────────────
#ifndef MM_CONFIG_H
#define MM_CONFIG_H

#include <windows.h>
#include <cstdint>

namespace ManualMapInjector {

#if defined(_WIN64)
    using IMAGE_NT_HEADERS_CURRENT = IMAGE_NT_HEADERS64;
    using PIMAGE_NT_HEADERS_CURRENT = PIMAGE_NT_HEADERS64;
    using IMAGE_NT_OPTIONAL_HDR_CURRENT = IMAGE_OPTIONAL_HEADER64;
    using TULONGLONG = ULONGLONG;
    const WORD TARGET_MACHINE = IMAGE_FILE_MACHINE_AMD64;
#define IMAGE_REL_BASED_SELF_ARCH IMAGE_REL_BASED_DIR64
#define TULONGLONG_FORMAT "0x%llX"
#define IS_ALIGNED(ptr) (true)  // x64 alignment is enforced by the hardware
#else
    using IMAGE_NT_HEADERS_CURRENT = IMAGE_NT_HEADERS32;
    using PIMAGE_NT_HEADERS_CURRENT = PIMAGE_NT_HEADERS32;
    using IMAGE_NT_OPTIONAL_HDR_CURRENT = IMAGE_OPTIONAL_HEADER32;
    using TULONGLONG = DWORD;
    const WORD TARGET_MACHINE = IMAGE_FILE_MACHINE_I386;
#define IMAGE_REL_BASED_SELF_ARCH IMAGE_REL_BASED_HIGHLOW
#define TULONGLONG_FORMAT "0x%X"
#define IS_ALIGNED(ptr) (((uintptr_t)(ptr) & 0x3) == 0)  // 4-byte minimum on x86
#endif

    // ─── bounds check ────────────────────────────────────────────────────────
    // !  every RVA read out of the mapped image goes through this first — the
    //    image is untrusted input and a gap in the section table is not a
    //    reason to trust the offset.
#define IS_VALID_RVA(base, rva, size, imageSize) \
    ((rva) < (imageSize) && ((rva) + (size)) <= (imageSize) && (size) > 0)

}
#endif
