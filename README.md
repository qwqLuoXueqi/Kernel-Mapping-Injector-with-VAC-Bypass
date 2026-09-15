The Kernel-Mapping-Injector-with-VAC-Bypass is now open source on GitHub. The code is open for everyone to review. I was inspired to develop this injector after seeing a similar one on GitHub. The core logic is borrowed from it. I only worked on the i18n and GUI. I am deeply ashamed—I don't have the ability to develop a better one myself.

Actually, the GUI was made using AI-generated artwork.

支持 x64/x86，基于 C++20 实现的手动映射 DLL 注入工具，支持从指定的 URL 下载 DLL 文件并将其注入到目标进程中
Supports x64/x86. A manual-mapping DLL injection tool implemented in C++20, supporting downloading a DLL file from a specified URL and injecting it into the target process.

Improved the injection process to optimize performance overhead

Fixed early injection and random crash issues on x86 architecture

Fully migrated to the NT API

More intuitive documentation

More complete API

Usage (Command-line launch arguments)
Specify the target process and DLL using the following command-line arguments:

bash
Manual-Map_x64.exe -process=<process_name.exe> -dll=<DLL URL> [-force_wait_process_start=<true|false>]
Examples:

bash
Manual-Map_x64.exe -process=cs2.exe -dll=https://example.com/cs2.dll -force_wait_process_start=true
Manual-Map_x86.exe -process=csgo.exe -dll=https://example.com/csgo.dll -force_wait_process_start=false
