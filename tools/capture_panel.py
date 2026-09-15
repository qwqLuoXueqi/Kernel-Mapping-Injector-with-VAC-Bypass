# -*- coding: utf-8 -*-
"""面板截图工具: 启动 exe -> PrintWindow 客户区 -> 手写 PNG(不需要 Pillow)
   可模拟点击导航/选择器, 逐页出图。"""
import ctypes, struct, zlib, time, subprocess, os, sys
from ctypes import wintypes

u32 = ctypes.WinDLL('user32', use_last_error=True)
g32 = ctypes.WinDLL('gdi32', use_last_error=True)
k32 = ctypes.WinDLL('kernel32', use_last_error=True)

u32.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
u32.FindWindowW.restype = wintypes.HWND
u32.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
u32.GetClientRect.restype = wintypes.BOOL
u32.GetDC.argtypes = [wintypes.HWND]
u32.GetDC.restype = wintypes.HDC
u32.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
u32.PrintWindow.argtypes = [wintypes.HWND, wintypes.HDC, wintypes.UINT]
u32.PrintWindow.restype = wintypes.BOOL
u32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
u32.PostMessageW.restype = wintypes.BOOL
u32.SetForegroundWindow.argtypes = [wintypes.HWND]
g32.CreateCompatibleDC.argtypes = [wintypes.HDC]
g32.CreateCompatibleDC.restype = wintypes.HDC
g32.CreateCompatibleBitmap.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]
g32.CreateCompatibleBitmap.restype = wintypes.HBITMAP
g32.SelectObject.argtypes = [wintypes.HDC, wintypes.HGDIOBJ]
g32.SelectObject.restype = wintypes.HGDIOBJ
g32.GetDIBits.argtypes = [wintypes.HDC, wintypes.HBITMAP, wintypes.UINT, wintypes.UINT,
                          ctypes.c_void_p, ctypes.c_void_p, wintypes.UINT]
g32.GetDIBits.restype = ctypes.c_int
g32.DeleteObject.argtypes = [wintypes.HGDIOBJ]
g32.DeleteDC.argtypes = [wintypes.HDC]

CLS = None


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wintypes.DWORD), ("biWidth", ctypes.c_long),
                ("biHeight", ctypes.c_long), ("biPlanes", wintypes.WORD),
                ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", ctypes.c_long),
                ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", wintypes.DWORD),
                ("biClrImportant", wintypes.DWORD)]


def png_write(path, w, h, bgra):
    raw = bytearray()
    stride = w * 4
    for y in range(h):
        raw.append(0)                      # filter: None
        off = y * stride
        row = bgra[off:off + stride]
        line = bytearray(w * 3)
        for x in range(w):
            j = x * 4
            line[x * 3 + 0] = row[j + 2]   # R  (BGRA -> 交错 RGB!)
            line[x * 3 + 1] = row[j + 1]   # G
            line[x * 3 + 2] = row[j + 0]   # B
        raw.extend(line)

    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)

    out = b'\x89PNG\r\n\x1a\n'
    out += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    out += chunk(b'IDAT', zlib.compress(bytes(raw), 6))
    out += chunk(b'IEND', b'')
    open(path, 'wb').write(out)


def shot(hwnd, path):
    rc = wintypes.RECT()
    u32.GetClientRect(hwnd, ctypes.byref(rc))
    w, h = rc.right, rc.bottom
    hdc = u32.GetDC(hwnd)
    mem = g32.CreateCompatibleDC(hdc)
    bmp = g32.CreateCompatibleBitmap(hdc, w, h)
    old = g32.SelectObject(mem, bmp)
    u32.PrintWindow(hwnd, mem, 1)      # PW_CLIENTONLY
    bi = BITMAPINFOHEADER()
    bi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.biWidth, bi.biHeight = w, -h    # top-down
    bi.biPlanes, bi.biBitCount = 1, 32
    bi.biCompression = 0
    buf = ctypes.create_string_buffer(w * h * 4)
    g32.GetDIBits(mem, bmp, 0, h, ctypes.cast(buf, ctypes.c_void_p), ctypes.byref(bi), 0)
    g32.SelectObject(mem, old)
    g32.DeleteObject(bmp)
    g32.DeleteDC(mem)
    u32.ReleaseDC(hwnd, hdc)
    png_write(path, w, h, buf.raw)
    return w, h


def click(hwnd, x, y):
    lp = (int(y) << 16) | (int(x) & 0xFFFF)
    u32.PostMessageW(hwnd, 0x0200, 0, lp)                 # WM_MOUSEMOVE
    u32.PostMessageW(hwnd, 0x0201, 1, lp)                 # WM_LBUTTONDOWN
    time.sleep(0.05)
    u32.PostMessageW(hwnd, 0x0202, 0, lp)                 # WM_LBUTTONUP
    time.sleep(0.45)


def main():
    outdir = os.path.abspath(os.path.join('x64', 'Release'))
    exe = os.path.join(outdir, 'Manual-Map_x64.exe')
    p = subprocess.Popen([exe, '-random_instance=false'], cwd=outdir)

    hwnd = None
    # EnumWindows 找可见窗口(FindWindow 会命中残留的旧实例)
    WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    u32.EnumWindows.argtypes = [ctypes.c_void_p, wintypes.LPARAM]
    u32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    u32.IsWindowVisible.argtypes = [wintypes.HWND]
    for _ in range(60):
        hits = []
        def cb(h, lp):
            b = ctypes.create_unicode_buffer(256)
            u32.GetWindowTextW(h, b, 256)
            if b.value == 'LUOXUEQI 注入器' and u32.IsWindowVisible(h):
                hits.append(h)
            return True
        u32.EnumWindows(WNDENUMPROC(cb), 0)
        if hits:
            hwnd = hits[-1]
            break
        time.sleep(0.25)
    if not hwnd:
        print('FAIL: window not found')
        p.terminate()
        return
    time.sleep(1.2)

    w, h = shot(hwnd, os.path.join(outdir, 'shot_1_home.png'))
    sc = w / 940.0
    print('client=%dx%d scale=%.2f' % (w, h, sc))

    def C(x, y):
        return int(round(x * sc)), int(round(y * sc))

    # 下拉展开
    click(hwnd, *C(226, 191))
    shot(hwnd, os.path.join(outdir, 'shot_2_dropdown.png'))
    click(hwnd, *C(226, 191))   # 收起

    # 导航: 注入 / 日志 / 设置 / 关于
    navX = [225, 311, 397, 483]
    names = ['home', 'log', 'set', 'about']
    for i, nm in enumerate(names):
        if i:
            click(hwnd, *C(navX[i], 32))
            shot(hwnd, os.path.join(outdir, 'shot_3_%s.png' % nm))
    print('done')

    p.terminate()
    try:
        p.wait(timeout=3)
    except Exception:
        p.kill()


main()
