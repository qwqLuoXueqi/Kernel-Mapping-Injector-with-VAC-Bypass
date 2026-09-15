// ============================================================================
//  LUOXUEQI 注入器 —— 面板 (暗色设计版)
//  自绘 UI: GDI+ 抗锯齿圆角/渐变 + 无边框自绘标题栏 + 侧边导航 + 卡片式内容
//  页面: 注入 / 日志 / 设置 / 关于
//  注入引擎仍走 API/ 与 Common.h 的既有实现(LoadLibrary + hook 绕过 + LdrLoadDll 回退)
// ============================================================================
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM
#include <commdlg.h>
#include <shellapi.h>
#include <richedit.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>

#include "Common.h"
#include "Log.h"
#include "Injection.h"
#include "I18n.h"      // 界面文案(T(L"English key"))
using I18n::T;     // 界面/日志文案: T(L"English key")
using I18n::TW;    // 需要 std::wstring 时

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "psapi.lib")

using namespace Gdiplus;

// ---------------------------------------------------------------------------
// 尺寸 / 调色板
// ---------------------------------------------------------------------------
namespace {

    constexpr float kWinW = 940.0f;      // 客户区
    constexpr float kWinH = 668.0f;
    constexpr float kHeadH = 64.0f;      // 顶栏高(品牌 + 导航 + 状态)
    constexpr float kPad = 20.0f;
    constexpr float kFootH = 78.0f;      // 底部操作栏高
    constexpr float kSideW = 0.0f;       // 旧左侧栏宽度(导航已上移到顶栏, 保留常量兼容算式)
    constexpr float kContentY = kHeadH + 62.0f;   // 内容区(首张卡片)起点
    constexpr float kNavX = 186.0f;      // 顶栏导航起点
    constexpr float kNavW = 86.0f;       // 单个导航项宽(含间隙)

    // 暗色(NL 风: 近黑蓝底 + 蓝青渐变强调色)
    const Color kBgWin  (255, 14, 17, 24);
    const Color kBgSide (255, 17, 20, 28);
    const Color kCard   (255, 22, 26, 36);
    const Color kCardTop(255, 27, 32, 44);
    const Color kEdge   (255, 36, 43, 58);
    const Color kEdgeSoft(255, 28, 34, 46);
    const Color kInput  (255, 24, 29, 40);
    const Color kText   (255, 232, 237, 247);
    const Color kTextDim(255, 146, 155, 172);
    const Color kTextFnt(255, 104, 113, 131);
    const Color kAccent (255, 59, 130, 246);
    const Color kAccent2(255, 34, 211, 238);
    const Color kOk     (255, 74, 222, 128);
    const Color kWarn   (255, 250, 204, 105);
    const Color kErr    (255, 248, 113, 113);

    // 日志行颜色(索引对应 LogLevel)
    const Color kLogColor[5] = {
        Color(255, 190, 198, 214),   // INFO
        Color(255, 122, 226, 154),   // OK
        Color(255, 246, 200, 106),   // WARN
        Color(255, 246, 122, 132),   // ERR
        Color(255, 126, 190, 255)    // STEP
    };

    float g_scale = 1.0f;
    float S(float v) { return v * g_scale; }
    int   SI(float v) { return (int)(v * g_scale + 0.5f); }

    // ---- 页面 / 控件 ID ----
    enum Page { PAGE_HOME = 0, PAGE_LOG, PAGE_SET, PAGE_ABOUT, PAGE_COUNT };

    enum Id {
        ID_NONE = 0,
        ID_NAV0 = 100, ID_NAV1, ID_NAV2, ID_NAV3,
        ID_MIN, ID_CLOSE,
        ID_BROWSE, ID_REFRESH, ID_OPENDIR,
        ID_SEG_LIB, ID_SEG_MM,
        ID_SW_WAIT, ID_SW_RANDOM,
        ID_INJECT, ID_CANCEL, ID_CLEARLOG, ID_EXPORTLOG, ID_OPENINI,
        ID_SELECT, ID_EDIT_HINT, ID_DROP_BASE = 300,
        ID_LANGSEL = 350, ID_LANG_BASE = 400
    };

    struct HitBox { RectF r; int id; };

    // ---- 窗口 / 子控件 ----
    HWND g_hwnd = nullptr;
    HWND g_hProc = nullptr, g_hLog = nullptr;
    HBRUSH g_brInput = nullptr;

    // ---- GDI+ 资源 ----
    ULONG_PTR g_gdiToken = 0;
    FontFamily* g_ff = nullptr;
    Font *g_fBrand = nullptr, *g_fPage = nullptr, *g_fCard = nullptr,
         *g_fBody = nullptr, *g_fSmall = nullptr, *g_fBtn = nullptr;

    // ---- 状态 ----
    int   g_page = PAGE_HOME;
    int   g_hover = ID_NONE;
    int   g_press = ID_NONE;
    std::vector<HitBox> g_hots;

    // 自绘 DLL 下拉
    bool  g_dropOpen = false;
    int   g_dropSel = -1;
    int   g_dropHover = -1;
    float g_dropScroll = 0.0f;
    RectF g_selRect;      // 选择器(收起态)
    RectF g_dropRect;     // 展开的列表
    const float kDropRow = 30.0f;
    const int   kDropMaxRows = 8;

    // 语言选择器(设置页): 与 DLL 下拉同一套画法, 但项固定(自动 + 7 语言)
    bool  g_langOpen = false;
    int   g_langHover = -1;
    RectF g_langRect;
    RectF g_navRect;      // 顶栏导航实测范围(拖动窗口时排除, 依语言宽度变化)
    inline int LangRowCount() { return 1 + (int)I18n::LANG_COUNT; }

    // 语言行的显示文案: 第 0 行=自动, 其余按 I18n 语言名
    inline const wchar_t* LangRowText(int row)
    {
        return (row <= 0) ? T(L"Auto (follow system)") : I18n::LangName(row - 1);
    }

    std::wstring g_dllDir;
    std::vector<std::wstring> g_dlls;
    InjectConfig g_cfg;

    std::thread* g_worker = nullptr;
    std::atomic<bool> g_running{ false };

    // 开关动画(0..1)
    float g_animWait = 1.0f, g_animWaitTo = 1.0f;
    float g_animRand = 1.0f, g_animRandTo = 1.0f;
    float g_animSeg = 0.0f, g_animSegTo = 0.0f;     // 0=LoadLibrary 1=手动映射
    bool  g_animOn = false;

    // 实时状态(定时器刷新)
    struct LiveInfo {
        bool admin = false;
        bool targetUp = false;
        DWORD pid = 0;
        unsigned wsMB = 0;
        int  threads = 0;
    } g_live;

    // 启动日志(面板打不开时看 <DLL目录>\injector.log)
    std::wstring g_logFilePath;

    void InitStartupLog(const std::wstring& dir)
    {
        g_logFilePath = dir;
        if (!g_logFilePath.empty() && g_logFilePath.back() != L'\\' && g_logFilePath.back() != L'/')
            g_logFilePath += L'\\';
        g_logFilePath += L"injector.log";
    }

    // 用 %ls + 宽字符, 避免 UTF-8 窄串被按 ANSI 解释导致乱码
    void StartupLog(const wchar_t* phase, const std::wstring& detail = L"")
    {
        if (g_logFilePath.empty())
            return;
        FILE* f = nullptr;
        if (_wfopen_s(&f, g_logFilePath.c_str(), L"a, ccs=UTF-8") != 0 || !f)
            return;
        SYSTEMTIME st;
        GetLocalTime(&st);
        if (detail.empty())
            fwprintf(f, L"[%02d:%02d:%02d] %ls\n", st.wHour, st.wMinute, st.wSecond, phase);
        else
            fwprintf(f, L"[%02d:%02d:%02d] %ls -> %ls\n", st.wHour, st.wMinute, st.wSecond,
                     phase, detail.c_str());
        fclose(f);
    }

} // namespace

// ---------------------------------------------------------------------------
// 绘制助手
// ---------------------------------------------------------------------------
namespace {

    // GDI+ 的 GraphicsPath 拷贝构造是 protected, 所以用出参方式构造
    void PathRound(GraphicsPath& p, const RectF& r, float rad)
    {
        p.Reset();
        float rr = rad;
        if (rr > r.Width * 0.5f)  rr = r.Width * 0.5f;
        if (rr > r.Height * 0.5f) rr = r.Height * 0.5f;
        if (rr <= 0.01f) {
            p.AddRectangle(r);
            return;
        }
        const float dd = rr * 2.0f;
        p.AddArc(r.X, r.Y, dd, dd, 180.0f, 90.0f);
        p.AddArc(r.GetRight() - dd, r.Y, dd, dd, 270.0f, 90.0f);
        p.AddArc(r.GetRight() - dd, r.GetBottom() - dd, dd, dd, 0.0f, 90.0f);
        p.AddArc(r.X, r.GetBottom() - dd, dd, dd, 90.0f, 90.0f);
        p.CloseFigure();
    }

    void FillRound(Graphics& g, const RectF& r, float rad, const Color& c)
    {
        SolidBrush b(c);
        GraphicsPath p; PathRound(p, r, rad);
        g.FillPath(&b, &p);
    }

    void StrokeRound(Graphics& g, const RectF& r, float rad, const Color& c, float w)
    {
        Pen pen(c, w);
        GraphicsPath p; PathRound(p, r, rad);
        g.DrawPath(&pen, &p);
    }

    void FillRoundGradV(Graphics& g, const RectF& r, float rad, const Color& top, const Color& bot)
    {
        LinearGradientBrush b(r, top, bot, LinearGradientModeVertical);
        GraphicsPath p; PathRound(p, r, rad);
        g.FillPath(&b, &p);
    }

    void FillRoundGradD(Graphics& g, const RectF& r, float rad, const Color& a, const Color& b)
    {
        LinearGradientBrush br(r, a, b, 0.0f, TRUE);
        GraphicsPath p; PathRound(p, r, rad);
        g.FillPath(&br, &p);
    }

    // 卡片: 投影 + 竖渐变面 + 描边 + 顶部高光
    void Card(Graphics& g, const RectF& r, float rad = 14.0f)
    {
        for (int i = 3; i >= 0; --i) {
            const float d = 2.0f + i * 3.0f;
            RectF s(r.X - d * 0.35f, r.Y + d * 0.45f, r.Width + d * 0.7f, r.Height + d * 0.7f);
            SolidBrush b(Color(10 + (3 - i) * 4, 0, 0, 0));
            GraphicsPath p; PathRound(p, s, rad + d * 0.5f);
            g.FillPath(&b, &p);
        }
        FillRoundGradV(g, r, rad, kCardTop, kCard);
        StrokeRound(g, r, rad, kEdge, S(1.0f));
        Pen hl(Color(22, 255, 255, 255), S(1.0f));
        g.DrawLine(&hl, r.X + rad * 0.8f, r.Y + 1.0f, r.GetRight() - rad * 0.8f, r.Y + 1.0f);
    }

    void Text(Graphics& g, const std::wstring& s, const Font& f, const Color& c, float x, float y)
    {
        SolidBrush b(c);
        g.DrawString(s.c_str(), -1, &f, PointF(x, y), &b);
    }

    // 限宽文本: 超出宽度自动省略号(翻译后长短不一, 防止溢出卡片)
    void TextClipped(Graphics& g, const std::wstring& s, const Font& f, const Color& c,
                     float x, float y, float maxW)
    {
        if (maxW <= S(6.0f))
            return;
        StringFormat fmt;
        fmt.SetFormatFlags(StringFormatFlagsNoWrap);
        fmt.SetTrimming(StringTrimmingEllipsisCharacter);
        SolidBrush b(c);
        g.DrawString(s.c_str(), -1, &f, RectF(x, y, maxW, f.GetHeight(&g) + S(6.0f)), &fmt, &b);
    }

    void TextAlign(Graphics& g, const std::wstring& s, const Font& f, const Color& c,
                   const RectF& box, int align /*0左1中2右*/, bool vcenter = true)
    {
        StringFormat sf;
        sf.SetAlignment(align == 1 ? StringAlignmentCenter : (align == 2 ? StringAlignmentFar : StringAlignmentNear));
        sf.SetLineAlignment(vcenter ? StringAlignmentCenter : StringAlignmentNear);
        sf.SetFormatFlags(StringFormatFlagsNoWrap);
        SolidBrush b(c);
        g.DrawString(s.c_str(), -1, &f, box, &sf, &b);
    }

    void AddHot(const RectF& r, int id) { if (id != ID_NONE) g_hots.push_back({ r, id }); }

    // ---- 矢量小图标 ----
    void IconAim(Graphics& g, float x, float y, float sz, const Color& c)
    {
        Pen p(c, S(1.5f));
        g.DrawEllipse(&p, x, y, sz, sz);
        SolidBrush b(c);
        g.FillEllipse(&b, x + sz * 0.36f, y + sz * 0.36f, sz * 0.28f, sz * 0.28f);
        g.DrawLine(&p, x - S(3.0f), y + sz * 0.5f, x - S(0.5f), y + sz * 0.5f);
        g.DrawLine(&p, x + sz + S(0.5f), y + sz * 0.5f, x + sz + S(3.0f), y + sz * 0.5f);
        g.DrawLine(&p, x + sz * 0.5f, y - S(3.0f), x + sz * 0.5f, y - S(0.5f));
        g.DrawLine(&p, x + sz * 0.5f, y + sz + S(0.5f), x + sz * 0.5f, y + sz + S(3.0f));
    }

    void IconList(Graphics& g, float x, float y, float sz, const Color& c)
    {
        Pen p(c, S(1.4f));
        SolidBrush b(c);
        for (int i = 0; i < 3; ++i) {
            const float yy = y + sz * (0.12f + i * 0.34f);
            g.FillEllipse(&b, x, yy, S(2.6f), S(2.6f));
            g.DrawLine(&p, x + S(6.0f), yy + S(1.3f), x + sz, yy + S(1.3f));
        }
    }

    void IconGear(Graphics& g, float x, float y, float sz, const Color& c)
    {
        Pen p(c, S(1.4f));
        const float cx = x + sz * 0.5f, cy = y + sz * 0.5f;
        g.DrawEllipse(&p, cx - sz * 0.26f, cy - sz * 0.26f, sz * 0.52f, sz * 0.52f);
        for (int i = 0; i < 6; ++i) {
            const float a = (float)i * 1.0472f;
            g.DrawLine(&p, cx + cosf(a) * sz * 0.33f, cy + sinf(a) * sz * 0.33f,
                           cx + cosf(a) * sz * 0.50f, cy + sinf(a) * sz * 0.50f);
        }
    }

    void IconInfo(Graphics& g, float x, float y, float sz, const Color& c)
    {
        Pen p(c, S(1.4f));
        g.DrawEllipse(&p, x, y, sz, sz);
        SolidBrush b(c);
        g.FillEllipse(&b, x + sz * 0.5f - S(1.2f), y + sz * 0.22f, S(2.4f), S(2.4f));
        g.DrawLine(&p, x + sz * 0.5f, y + sz * 0.44f, x + sz * 0.5f, y + sz * 0.74f);
    }

    void IconPlay(Graphics& g, float x, float y, float sz, const Color& c)
    {
        SolidBrush b(c);
        PointF pts[3] = { PointF(x, y), PointF(x + sz, y + sz * 0.5f), PointF(x, y + sz) };
        g.FillPolygon(&b, pts, 3);
    }

    // ---- 自绘控件 ----
    void DrawButton(Graphics& g, const RectF& r, const std::wstring& label, int id,
                    bool primary, bool enabled = true, float withPlayIcon = -1.0f)
    {
        const bool hover = (g_hover == id);
        const bool press = (g_press == id);
        const float rad = S(10.0f);

        if (primary && enabled) {
            const Color a = press ? Color(255, 32, 96, 200) : (hover ? Color(255, 74, 150, 255) : kAccent);
            const Color b = press ? Color(255, 22, 150, 170) : (hover ? Color(255, 45, 220, 245) : kAccent2);
            if (hover) {
                SolidBrush glow(Color(34, 59, 130, 246));
                GraphicsPath gp; PathRound(gp, RectF(r.X - S(2.0f), r.Y - S(2.0f),
                                                  r.Width + S(4.0f), r.Height + S(4.0f)), rad + S(2.0f));
                g.FillPath(&glow, &gp);
            }
            FillRoundGradD(g, r, rad, a, b);
        } else {
            const Color bg = !enabled ? Color(255, 26, 30, 40)
                                      : (press ? Color(255, 30, 36, 50)
                                               : (hover ? Color(255, 38, 45, 60) : Color(255, 30, 35, 48)));
            FillRound(g, r, rad, bg);
            StrokeRound(g, r, rad, hover ? Color(255, 58, 68, 90) : kEdge, S(1.0f));
        }

        const Color tc = !enabled ? kTextFnt : kText;
        if (withPlayIcon >= 0.0f && enabled) {
            const float iconSz = withPlayIcon;
            const float gap = S(9.0f);
            // 粗略估宽: 中文字宽 = 字号, 英文半宽
            RectF bounds;
            SolidBrush mb(tc);
            StringFormat sf;
            sf.SetFormatFlags(StringFormatFlagsMeasureTrailingSpaces);
            g.MeasureString(label.c_str(), -1, g_fBtn, PointF(0, 0), &sf, &bounds);
            const float total = iconSz + gap + bounds.Width;
            const float sx = r.X + (r.Width - total) * 0.5f;
            IconPlay(g, sx, r.Y + r.Height * 0.5f - iconSz * 0.5f, iconSz, tc);
            TextAlign(g, label, *g_fBtn, tc,
                      RectF(sx + iconSz + gap, r.Y, bounds.Width + S(4.0f), r.Height), 1);
        } else {
            TextAlign(g, label, *g_fBtn, tc, r, 1);
        }
        AddHot(r, id);
    }

    void DrawSegmented(Graphics& g, const RectF& r, const std::wstring& l0, const std::wstring& l1,
                       float t, int id0, int id1, const Font* font = nullptr)
    {
        const Font& f = font ? *font : *g_fBody;
        FillRound(g, r, S(10.0f), Color(255, 21, 25, 34));
        StrokeRound(g, r, S(10.0f), kEdgeSoft, S(1.0f));

        const float half = r.Width * 0.5f;
        const float x = r.X + t * (r.Width - half);
        RectF sel(x + S(3.0f), r.Y + S(3.0f), half - S(6.0f), r.Height - S(6.0f));
        FillRoundGradD(g, sel, S(8.0f), Color(255, 48, 108, 214), Color(255, 32, 170, 200));

        TextAlign(g, l0, f, t < 0.5f ? kText : kTextDim, RectF(r.X, r.Y, half, r.Height), 1);
        TextAlign(g, l1, f, t < 0.5f ? kTextDim : kText, RectF(r.X + half, r.Y, half, r.Height), 1);
        AddHot(RectF(r.X, r.Y, half, r.Height), id0);
        AddHot(RectF(r.X + half, r.Y, half, r.Height), id1);
    }

    // 开关行: 左侧文字 + 右侧胶囊开关(带过渡动画)
    void DrawSwitchRow(Graphics& g, const RectF& row, const std::wstring& label, const std::wstring& hint,
                       float anim, int id)
    {
        const float sw = S(42.0f), sh = S(22.0f);
        const float kx = row.GetRight() - sw;
        const float ky = row.Y + (row.Height - sh) * 0.5f;

        const float textW = (row.GetRight() - sw - S(12.0f)) - row.X;
        TextClipped(g, label, *g_fBody, kText, row.X, row.Y + S(2.0f), textW);
        if (!hint.empty())
            TextClipped(g, hint, *g_fSmall, kTextFnt, row.X + S(1.0f), row.Y + S(21.0f), textW);

        auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
        const Color off(255, 40, 46, 60);
        const Color c0(255, (BYTE)lerp(off.GetR(), kAccent.GetR(), anim),
                            (BYTE)lerp(off.GetG(), kAccent.GetG(), anim),
                            (BYTE)lerp(off.GetB(), kAccent.GetB(), anim));
        const Color c1(255, (BYTE)lerp(off.GetR(), kAccent2.GetR(), anim),
                            (BYTE)lerp(off.GetG(), kAccent2.GetG(), anim),
                            (BYTE)lerp(off.GetB(), kAccent2.GetB(), anim));
        FillRoundGradD(g, RectF(kx, ky, sw, sh), sh * 0.5f, c0, c1);

        const float kr = sh - S(4.0f);
        const float kx2 = kx + S(2.0f) + anim * (sw - kr - S(4.0f));
        SolidBrush knob(Color(255, 255, 255, 255));
        g.FillEllipse(&knob, kx2, ky + S(2.0f), kr, kr);

        AddHot(row, id);
    }

} // namespace

// ---------------------------------------------------------------------------
// 日志(工作线程 -> UI 线程)
// ---------------------------------------------------------------------------
namespace {

    void GuiLogSink(int level, const std::wstring& text)
    {
        auto* payload = new std::pair<int, std::wstring>(level, text);
        if (!PostMessageW(g_hwnd, WM_APP + 1, 0, reinterpret_cast<LPARAM>(payload)))
            delete payload;
    }

    void AppendLogLine(int level, const std::wstring& text)
    {
        if (!g_hLog)
            return;
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t stamp[32];
        swprintf_s(stamp, L"%02d:%02d:%02d   ", st.wHour, st.wMinute, st.wSecond);

        CHARRANGE cr{ -1, -1 };
        SendMessageW(g_hLog, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&cr));

        CHARFORMAT2W cf{};
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_COLOR | CFM_FACE | CFM_SIZE;
        cf.crTextColor = kLogColor[(level >= 0 && level <= 4) ? level : 0].ToCOLORREF();
        wcscpy_s(cf.szFaceName, L"Consolas");
        cf.yHeight = (LONG)(S(13.0f) * 20.0f);
        SendMessageW(g_hLog, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));

        std::wstring line = std::wstring(stamp) + text + L"\r\n";
        SendMessageW(g_hLog, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
        SendMessageW(g_hLog, EM_SCROLLCARET, 0, 0);
    }

} // namespace

// ---------------------------------------------------------------------------
// 界面状态 / 子控件
// ---------------------------------------------------------------------------
namespace {

    std::wstring CurrentComboPath()
    {
        if (g_dropSel >= 0 && g_dropSel < (int)g_dlls.size())
            return g_dlls[(size_t)g_dropSel];
        return {};
    }

    void FillDllCombo(const std::wstring& preferName)
    {
        g_dlls = ScanDlls(g_dllDir);
        g_dropSel = g_dlls.empty() ? -1 : 0;
        for (size_t i = 0; i < g_dlls.size(); ++i) {
            if (!preferName.empty() &&
                _wcsicmp(::FileNameOf(g_dlls[i]).c_str(), preferName.c_str()) == 0)
                g_dropSel = (int)i;
        }
        g_dropScroll = 0.0f;
        g_dropOpen = false;
        g_cfg.dllPath = CurrentComboPath();
    }

    InjectConfig CollectConfig()
    {
        InjectConfig cfg = g_cfg;
        cfg.dllPath = CurrentComboPath();
        if (cfg.dllPath.empty() && !g_dlls.empty())
            cfg.dllPath = g_dlls[0];

        wchar_t buf[260] = {};
        GetWindowTextW(g_hProc, buf, _countof(buf));
        if (buf[0])
            cfg.processName = buf;

        cfg.manualMap = (g_animSegTo > 0.5f);
        cfg.forceWait = (g_animWaitTo > 0.5f);
        cfg.randomInstance = (g_animRandTo > 0.5f);
        cfg.dllDir = g_dllDir;
        return cfg;
    }

    void SetBusy(bool busy)
    {
        g_running = busy;
        if (g_hwnd)
            InvalidateRect(g_hwnd, nullptr, FALSE);
    }

    void RequestAnim()
    {
        if (g_animOn)
            return;
        g_animOn = true;
        SetTimer(g_hwnd, 2, 15, nullptr);
    }

    void UpdateAnim()
    {
        auto step = [](float& v, float to) -> bool {
            const float d = to - v;
            if (fabsf(d) < 0.02f) { v = to; return true; }
            v += d * 0.28f;
            return false;
        };
        bool done = true;
        if (!step(g_animWait, g_animWaitTo)) done = false;
        if (!step(g_animRand, g_animRandTo)) done = false;
        if (!step(g_animSeg, g_animSegTo))   done = false;
        InvalidateRect(g_hwnd, nullptr, FALSE);
        if (done) {
            KillTimer(g_hwnd, 2);
            g_animOn = false;
        }
    }

    bool IsElevatedNow()
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            return false;
        TOKEN_ELEVATION el{};
        DWORD sz = sizeof(el);
        const bool ok = GetTokenInformation(token, TokenElevation, &el, sizeof(el), &sz) &&
                        el.TokenIsElevated != 0;
        CloseHandle(token);
        return ok;
    }

    void RefreshLive()
    {
        g_live.admin = IsElevatedNow();
        g_live.targetUp = false;
        g_live.pid = 0;
        g_live.wsMB = 0;
        g_live.threads = 0;

        const std::wstring target = g_cfg.processName.empty() ? std::wstring(L"cs2.exe") : g_cfg.processName;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return;
        PROCESSENTRY32W pe{ sizeof(pe) };
        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, target.c_str()) != 0)
                    continue;
                if (pe.cntThreads == 0)      // 僵尸空壳
                    continue;
                g_live.targetUp = true;
                g_live.pid = pe.th32ProcessID;
                g_live.threads = (int)pe.cntThreads;
                break;
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);

        if (g_live.pid) {
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, g_live.pid);
            if (h) {
                PROCESS_MEMORY_COUNTERS pmc{};
                if (GetProcessMemoryInfo(h, &pmc, sizeof(pmc)))
                    g_live.wsMB = (unsigned)(pmc.WorkingSetSize / (1024 * 1024));
                CloseHandle(h);
            }
        }
    }

    void LayoutChildren()
    {
        const bool home = (g_page == PAGE_HOME);
        const bool logPage = (g_page == PAGE_LOG);

        const float mainX = kPad;
        const float y1 = kContentY;

        // 进程名编辑框 / 日志框(其余全部自绘)
        // 注意: 未缩放常量相加后再整体缩放, 否则非 100% DPI 会错位
        MoveWindow(g_hProc,
                   SI(mainX + 152.0f), SI(y1 + 204.0f), SI(172.0f), SI(30.0f), TRUE);
        ShowWindow(g_hProc, home ? SW_SHOW : SW_HIDE);

        if (logPage) {
            // 与 DrawLogPage 的卡片/内缩一致: 卡内缩 16, 标题区 52, 底部留 16
            MoveWindow(g_hLog, SI(mainX + 16.0f), SI(y1 + 52.0f),
                       SI(kWinW - mainX - kPad - 32.0f),
                       SI(kWinH - kFootH - y1 - 14.0f - 68.0f), TRUE);
            ShowWindow(g_hLog, SW_SHOW);
        } else {
            ShowWindow(g_hLog, SW_HIDE);
        }
    }

} // namespace

// ---------------------------------------------------------------------------
// 各页面绘制
// ---------------------------------------------------------------------------
namespace {

    // 顶栏: 品牌 + 横向导航(原左侧栏整体上移)
    void DrawTopBar(Graphics& g, float W)
    {
        FillRound(g, RectF(0, 0, W, kHeadH), 0.0f, kBgSide);
        Pen sep(kEdgeSoft, S(1.0f));
        g.DrawLine(&sep, 0.0f, kHeadH - 0.5f, W, kHeadH - 0.5f);

        // 品牌
        const RectF logo(S(20.0f), S(14.0f), S(36.0f), S(36.0f));
        FillRoundGradD(g, logo, S(10.0f), kAccent, kAccent2);
        if (g_ff) {
            Font fb(g_ff, S(18.0f), FontStyleBold, UnitPixel);
            TextAlign(g, L"L", fb, Color(255, 255, 255, 248), logo, 1);
        }
        Text(g, L"LUOXUEQI", *g_fBrand, kText, S(64.0f), S(13.0f));
        Text(g, T(L"Injector · Panel"), *g_fSmall, kTextFnt, S(65.0f), S(35.0f));

        // 横向导航
        struct Nav { const wchar_t* label; int id; int page; };
        const Nav nav[4] = {
            { T(L"Inject"),  ID_NAV0, PAGE_HOME  },
            { T(L"Log"),  ID_NAV1, PAGE_LOG   },
            { T(L"Settings"),  ID_NAV2, PAGE_SET   },
            { T(L"About"),  ID_NAV3, PAGE_ABOUT },
        };
        // 依语言自适应项宽(日/俄/德文明显比中文长, 固定宽度会挤成一团)
        const float ny = (kHeadH - S(38.0f)) * 0.5f;
        float navX = S(kNavX);
        for (int i = 0; i < 4; ++i) {
            RectF meas;
            g.MeasureString(T(nav[i].label), -1, g_fBody, PointF(0.0f, 0.0f), &meas);
            const float itemW = S(36.0f) + meas.Width + S(18.0f);
            const RectF r(navX, ny, itemW, S(38.0f));
            navX += itemW + S(6.0f);
            if (i == 0)      g_navRect.X = r.X;
            if (i == 3)      g_navRect.Width = r.GetRight() - g_navRect.X;
            const bool active = (g_page == nav[i].page);
            const bool hover = (g_hover == nav[i].id);

            if (active) {
                FillRound(g, r, S(10.0f), Color(255, 30, 38, 54));
                StrokeRound(g, r, S(10.0f), Color(56, 59, 130, 246), S(1.0f));
                // 底部指示条
                const float iw = r.Width * 0.44f;
                FillRound(g, RectF(r.X + (r.Width - iw) * 0.5f, r.GetBottom() - S(2.0f), iw, S(2.5f)),
                          S(1.25f), kAccent2);
            } else if (hover) {
                FillRound(g, r, S(10.0f), Color(255, 26, 31, 43));
            }

            const Color ic = active ? kAccent2 : kTextFnt;
            const Color tc = active ? kText : kTextDim;
            const float ix = r.X + S(14.0f), iy = r.Y + (r.Height - S(14.0f)) * 0.5f;
            switch (i) {
            case 0: IconAim(g, ix, iy, S(14.0f), ic); break;
            case 1: IconList(g, ix, iy, S(14.0f), ic); break;
            case 2: IconGear(g, ix, iy, S(14.0f), ic); break;
            default: IconInfo(g, ix, iy, S(14.0f), ic); break;
            }
            TextClipped(g, T(nav[i].label), *g_fBody, tc, r.X + S(36.0f), r.Y + S(9.0f),
                        r.Width - S(44.0f));
            AddHot(r, nav[i].id);
        }
    }

    void DrawHeader(Graphics& g, float W)
    {
        const wchar_t* titles[PAGE_COUNT] = { T(L"Inject"), T(L"Run log"), T(L"Settings"), T(L"About") };
        const wchar_t* subs[PAGE_COUNT] = {
            T(L"Pick a DLL and set the options, then inject"),
            T(L"Every step is written here"),
            T(L"Default options & file locations"),
            T(L"Version · hotkeys · notes"),
        };
        Text(g, titles[g_page], *g_fPage, kText, S(kPad), kHeadH + S(14.0f));
        Text(g, subs[g_page], *g_fSmall, kTextFnt, S(kPad + 1.0f), kHeadH + S(40.0f));

        // 状态胶囊
        const std::wstring s = g_running ? T(L"Injecting") : T(L"Idle");
        const Color c = g_running ? kAccent2 : kTextDim;
        const float pw = S(100.0f), ph = S(26.0f);
        const RectF pill(W - kPad - S(84.0f) - pw, S(19.0f), pw, ph);
        FillRound(g, pill, ph * 0.5f, Color(255, 21, 25, 34));
        StrokeRound(g, pill, ph * 0.5f, kEdgeSoft, S(1.0f));
        SolidBrush dot(c);
        g.FillEllipse(&dot, pill.X + S(12.0f), pill.Y + ph * 0.5f - S(3.5f), S(7.0f), S(7.0f));
        Text(g, s, *g_fSmall, c, pill.X + S(26.0f), pill.Y + S(6.0f));

        // 最小化 / 关闭
        const float bx = W - kPad - S(72.0f);
        const RectF rMin(bx, S(18.0f), S(28.0f), S(28.0f));
        const RectF rClose(bx + S(34.0f), S(18.0f), S(28.0f), S(28.0f));
        if (g_hover == ID_MIN)   FillRound(g, rMin, S(8.0f), Color(255, 30, 36, 48));
        if (g_hover == ID_CLOSE) FillRound(g, rClose, S(8.0f), Color(210, 220, 70, 80));
        {
            const Color lc = (g_hover == ID_MIN) ? kText : kTextDim;
            Pen p1(lc, S(1.6f));
            g.DrawLine(&p1, rMin.X + S(8.0f), rMin.Y + S(14.0f), rMin.X + S(20.0f), rMin.Y + S(14.0f));
            const Color xc = (g_hover == ID_CLOSE) ? Color(255, 255, 255, 255) : kTextDim;
            Pen p2(xc, S(1.6f));
            g.DrawLine(&p2, rClose.X + S(9.0f), rClose.Y + S(9.0f), rClose.X + S(19.0f), rClose.Y + S(19.0f));
            g.DrawLine(&p2, rClose.X + S(19.0f), rClose.Y + S(9.0f), rClose.X + S(9.0f), rClose.Y + S(19.0f));
        }
        AddHot(rMin, ID_MIN);
        AddHot(rClose, ID_CLOSE);
    }

    // 底部操作栏: 左侧常驻状态(原侧栏底部状态卡) + 右侧操作按钮(原注入页按钮行)
    void DrawFooter(Graphics& g, float H, float W)
    {
        const float y0 = H - S(kFootH);
        FillRound(g, RectF(0, y0, W, S(kFootH)), 0.0f, kBgSide);
        Pen sep(kEdgeSoft, S(1.0f));
        g.DrawLine(&sep, 0.0f, y0 + 0.5f, W, y0 + 0.5f);

        // 左: 状态
        {
            const float lx = S(kPad);
            float ly = y0 + S(13.0f);
            SolidBrush d1(g_live.admin ? kOk : kErr);
            g.FillEllipse(&d1, lx, ly + S(4.0f), S(7.0f), S(7.0f));
            Text(g, g_live.admin ? T(L"Admin rights: enabled") : T(L"Not running as administrator"), *g_fSmall,
                 g_live.admin ? kOk : kErr, lx + S(13.0f), ly);
            ly += S(20.0f);
            SolidBrush d2(g_live.targetUp ? kOk : kTextFnt);
            g.FillEllipse(&d2, lx, ly + S(4.0f), S(7.0f), S(7.0f));
            const std::wstring t = (g_cfg.processName.empty() ? std::wstring(L"cs2.exe") : g_cfg.processName) +
                                   (g_live.targetUp ? T(L" running") : T(L" not running"));
            Text(g, t, *g_fSmall, g_live.targetUp ? kText : kTextDim, lx + S(13.0f), ly);
            ly += S(20.0f);
            if (g_live.targetUp && g_live.wsMB) {
                wchar_t buf[64];
                swprintf_s(buf, L"PID %u   %u MB", g_live.pid, g_live.wsMB);
                Text(g, buf, *g_fSmall, kTextFnt, lx + S(13.0f), ly);
            } else {
                Text(g, T(L"Waiting to start..."), *g_fSmall, kTextFnt, lx + S(13.0f), ly);
            }
        }

        // 右: 操作按钮(右对齐)
        const float bh = S(46.0f);
        const float by = y0 + (S(kFootH) - bh) * 0.5f;
        float bx = W - S(kPad);
        bx -= S(124.0f); DrawButton(g, RectF(bx, by, S(124.0f), bh), T(L"Clear log"), ID_CLEARLOG, false);
        bx -= S(12.0f);
        bx -= S(110.0f); DrawButton(g, RectF(bx, by, S(110.0f), bh), T(L"Cancel"), ID_CANCEL, false, g_running);
        bx -= S(12.0f);
        bx -= S(252.0f); DrawButton(g, RectF(bx, by, S(252.0f), bh), T(L"Start Inject"), ID_INJECT, true, !g_running, S(12.0f));
    }

    void DrawHome(Graphics& g)
    {
        const float x = kSideW + kPad;
        const float w = kWinW - x - kPad;
        float y = kContentY;

        // 卡片 1: DLL
        {
            const RectF card(x, y, w, S(130.0f));
            Card(g, card);
            Text(g, T(L"Target DLL"), *g_fCard, kText, x + S(20.0f), y + S(14.0f));
            Text(g, T(L"Auto-lists .dll files in this folder; or pick one from elsewhere"), *g_fSmall, kTextFnt,
                 x + S(20.0f), y + S(37.0f));

            const float by = y + S(50.0f);
            const float bh = S(30.0f);

            // 自绘选择器(收起态): 框 + 当前 DLL 名 + 右侧箭头
            const RectF sel(x + S(20.0f), by, S(372.0f), bh);
            g_selRect = sel;
            FillRound(g, sel, S(9.0f), kInput);
            StrokeRound(g, sel, S(9.0f),
                        g_dropOpen ? Color(120, 59, 130, 246)
                                   : (g_hover == ID_SELECT ? Color(255, 62, 72, 94) : kEdgeSoft), S(1.0f));
            {
                std::wstring cur = (g_dropSel >= 0 && g_dropSel < (int)g_dlls.size())
                                 ? ::FileNameOf(g_dlls[(size_t)g_dropSel])
                                 : std::wstring(T(L"(No .dll in this folder — click Browse to pick one)"));
                Text(g, cur, *g_fBody, g_dropSel >= 0 ? kText : kTextFnt,
                     sel.X + S(12.0f), sel.Y + S(6.0f));
                // 箭头
                const float ax = sel.GetRight() - S(22.0f), ay = sel.Y + bh * 0.5f - S(3.0f);
                Pen p(kTextDim, S(1.6f));
                g.DrawLine(&p, ax, ay, ax + S(5.0f), ay + S(5.0f));
                g.DrawLine(&p, ax + S(5.0f), ay + S(5.0f), ax + S(10.0f), ay);
            }
            AddHot(sel, ID_SELECT);

            float bx = sel.GetRight() + S(12.0f);
            DrawButton(g, RectF(bx, by, S(76.0f), bh), T(L"Browse..."), ID_BROWSE, false);   bx += S(84.0f);
            DrawButton(g, RectF(bx, by, S(64.0f), bh), T(L"Refresh"), ID_REFRESH, false);   bx += S(72.0f);
            DrawButton(g, RectF(bx, by, S(100.0f), bh), T(L"Open folder"), ID_OPENDIR, false);

            Text(g, T(L"Folder: ") + g_dllDir, *g_fSmall, kTextFnt, x + S(20.0f), y + S(95.0f));
        }
        y += S(130.0f) + S(14.0f);

        // 卡片 2: 参数
        {
            const RectF card(x, y, w, S(168.0f));
            Card(g, card);
            Text(g, T(L"Inject options"), *g_fCard, kText, x + S(20.0f), y + S(14.0f));

            Text(g, T(L"Target process"), *g_fBody, kTextDim, x + S(20.0f), y + S(62.0f));
            {   // 编辑框外框(与 LayoutChildren 里的 EDIT 对齐: 绝对坐标 + 内缩 2px)
                const RectF ef(S(kSideW + kPad + 150.0f), S(kContentY + 202.0f),
                               S(176.0f), S(34.0f));
                FillRound(g, ef, S(9.0f), kInput);
                StrokeRound(g, ef, S(9.0f),
                            (g_hover == ID_EDIT_HINT) ? Color(255, 62, 72, 94) : kEdgeSoft, S(1.0f));
            }

            Text(g, T(L"Injection method"), *g_fBody, kTextDim, x + S(20.0f), y + S(114.0f));
            DrawSegmented(g, RectF(x + S(140.0f), y + S(106.0f), S(310.0f), S(36.0f)),
                          T(L"LoadLibrary (recommended)"), T(L"Manual map"), g_animSeg, ID_SEG_LIB, ID_SEG_MM);

            DrawSwitchRow(g, RectF(x + w - S(326.0f), y + S(56.0f), S(306.0f), S(38.0f)),
                          T(L"Wait for game"), T(L"Open the panel first, then start the game"), g_animWait, ID_SW_WAIT);
            DrawSwitchRow(g, RectF(x + w - S(326.0f), y + S(108.0f), S(306.0f), S(38.0f)),
                          T(L"Random instance"), T(L"Copy to a random name and run"), g_animRand, ID_SW_RANDOM);
        }
        y += S(168.0f) + S(14.0f);

        // 卡片 3: 状态
        {
            const RectF card(x, y, w, S(96.0f));
            Card(g, card);
            Text(g, T(L"Current status"), *g_fCard, kText, x + S(20.0f), y + S(14.0f));

            const bool up = g_live.targetUp;
            const std::wstring proc = g_cfg.processName.empty() ? std::wstring(L"cs2.exe") : g_cfg.processName;
            SolidBrush dot(g_running ? kAccent2 : (up ? kOk : kTextFnt));
            g.FillEllipse(&dot, x + S(20.0f), y + S(48.0f), S(9.0f), S(9.0f));

            const std::wstring big = g_running ? T(L"Injecting, please wait...")
                                              : (up ? (proc + T(L" ready to inject"))
                                                    : (proc + T(L" not running yet")));
            Text(g, big, *g_fBody, g_running ? kAccent2 : (up ? kOk : kText), x + S(38.0f), y + S(44.0f));

            const std::wstring sub = up
                ? (T(L"Working set ") + std::to_wstring(g_live.wsMB) + T(L" MB     Threads ") +
                   std::to_wstring(g_live.threads) + T(L"    Ready threshold 600 MB"))
                : T(L"Click Start Inject first, then launch the game; or turn off 'Wait for game' to inject right away");
            Text(g, sub, *g_fSmall, kTextFnt, x + S(38.0f), y + S(68.0f));
        }
        // 操作按钮已移到底部操作栏(见 DrawFooter)
    }

    void DrawLogPage(Graphics& g)
    {
        const float x = kSideW + kPad;
        const float w = kWinW - x - kPad;
        const float y = kContentY;
        const RectF card(x, y, w, kWinH - kFootH - y - S(14.0f));
        Card(g, card);
        Text(g, T(L"Run log"), *g_fCard, kText, x + S(20.0f), y + S(14.0f));
        Text(g, T(L"Every step is written here, and also to injector.log next to the program"), *g_fSmall, kTextFnt,
             x + S(20.0f), y + S(36.0f));

        // 小按钮放在卡片标题行右侧(原底部的按钮行已并入底部操作栏)
        const float bh = S(30.0f), by = y + S(14.0f);
        DrawButton(g, RectF(card.GetRight() - S(20.0f) - S(104.0f), by, S(104.0f), bh),
                   T(L"Open folder"), ID_OPENDIR, false);
        DrawButton(g, RectF(card.GetRight() - S(20.0f) - S(104.0f) - S(12.0f) - S(148.0f), by,
                            S(148.0f), bh), T(L"Export to file"), ID_EXPORTLOG, false);
    }

    void DrawSettingsPage(Graphics& g)
    {
        const float x = kSideW + kPad;
        const float w = kWinW - x - kPad;
        float y = kContentY;

        {
            const RectF card(x, y, w, S(214.0f));
            Card(g, card);
            Text(g, T(L"Defaults"), *g_fCard, kText, x + S(20.0f), y + S(14.0f));
            Text(g, T(L"Stored in injector.ini and restored next time"), *g_fSmall, kTextFnt,
                 x + S(20.0f), y + S(36.0f));

            DrawSwitchRow(g, RectF(x + S(20.0f), y + S(64.0f), S(300.0f), S(38.0f)),
                          T(L"Wait for game"), T(L"Open the panel first, then start the game"), g_animWait, ID_SW_WAIT);
            DrawSwitchRow(g, RectF(x + S(20.0f), y + S(112.0f), S(300.0f), S(38.0f)),
                          T(L"Random instance"), T(L"Copy to a random name and run"), g_animRand, ID_SW_RANDOM);

            Text(g, T(L"Injection method"), *g_fBody, kTextDim, x + w - S(312.0f), y + S(72.0f));
            DrawSegmented(g, RectF(x + w - S(312.0f), y + S(98.0f), S(280.0f), S(36.0f)),
                          L"LoadLibrary", T(L"Manual map"), g_animSeg, ID_SEG_LIB, ID_SEG_MM);
            TextClipped(g, T(L"Manual map crashes DLLs with TLS/CRT — not recommended"), *g_fSmall,
                        kTextFnt, x + w - S(312.0f), y + S(142.0f), S(292.0f));

            // 语言: 自动(检测 CS2/Steam/系统语言) 或 手动指定
            Text(g, T(L"Language"), *g_fBody, kTextDim, x + S(20.0f), y + S(178.0f));
            {
                const RectF ls(x + S(120.0f), y + S(168.0f), S(226.0f), S(34.0f));
                g_langRect = ls;
                FillRound(g, ls, S(9.0f), kInput);
                StrokeRound(g, ls, S(9.0f),
                            g_langOpen ? Color(120, 59, 130, 246)
                                       : (g_hover == ID_LANGSEL ? Color(255, 62, 72, 94) : kEdgeSoft), S(1.0f));
                Text(g, (g_cfg.lang < 0) ? T(L"Auto (follow system)") : I18n::LangName(g_cfg.lang),
                     *g_fBody, kText, ls.X + S(12.0f), ls.Y + S(7.0f));
                const float ax = ls.GetRight() - S(22.0f), ay = ls.Y + S(14.0f);
                Pen p(kTextDim, S(1.6f));
                g.DrawLine(&p, ax, ay, ax + S(5.0f), ay + S(5.0f));
                g.DrawLine(&p, ax + S(5.0f), ay + S(5.0f), ax + S(10.0f), ay);
                AddHot(ls, ID_LANGSEL);
                // 自动模式下把实际检测结果显示出来
                Text(g, I18n::LangName(I18n::g_lang), *g_fSmall, kTextFnt,
                     ls.GetRight() + S(12.0f), y + S(177.0f));
            }
        }
        y += S(214.0f) + S(14.0f);

        {
            const RectF card(x, y, w, S(116.0f));
            Card(g, card);
            Text(g, T(L"Files"), *g_fCard, kText, x + S(20.0f), y + S(14.0f));
            Text(g, T(L"Folder: ") + g_dllDir, *g_fSmall, kTextFnt, x + S(20.0f), y + S(40.0f));
            Text(g, T(L"Settings injector.ini        Log injector.log"), *g_fSmall, kTextFnt,
                 x + S(20.0f), y + S(60.0f));
            DrawButton(g, RectF(x + S(20.0f), y + S(78.0f), S(110.0f), S(30.0f)), T(L"Open folder"), ID_OPENDIR, false);
            DrawButton(g, RectF(x + S(142.0f), y + S(78.0f), S(120.0f), S(30.0f)), T(L"Open ini"), ID_OPENINI, false);
        }
        y += S(116.0f) + S(14.0f);

        {
            const RectF card(x, y, w, S(96.0f));
            Card(g, card);
            Text(g, T(L"Current status"), *g_fCard, kText, x + S(20.0f), y + S(14.0f));
            Text(g, g_live.admin ? T(L"Admin rights: enabled (ready to inject)")
                                 : T(L"Admin rights: disabled — right-click and 'Run as administrator'"),
                 *g_fSmall, g_live.admin ? kOk : kErr, x + S(20.0f), y + S(44.0f));
            Text(g, (g_cfg.processName.empty() ? std::wstring(L"cs2.exe") : g_cfg.processName) +
                    (g_live.targetUp ? T(L": running") : T(L": not running")),
                 *g_fSmall, kTextDim, x + S(20.0f), y + S(66.0f));
        }
    }

    void DrawAboutPage(Graphics& g)
    {
        const float x = kSideW + kPad;
        const float w = kWinW - x - kPad;
        const float y = kContentY;

        const RectF card(x, y, w, S(268.0f));
        Card(g, card);
        const RectF logo(x + S(24.0f), y + S(24.0f), S(46.0f), S(46.0f));
        FillRoundGradD(g, logo, S(13.0f), kAccent, kAccent2);
        if (g_ff) {
            Font fb(g_ff, S(23.0f), FontStyleBold, UnitPixel);
            TextAlign(g, L"L", fb, Color(255, 255, 255, 248), logo, 1);
        }
        Text(g, T(L"LUOXUEQI Injector"), *g_fPage, kText, x + S(86.0f), y + S(24.0f));
        Text(g, T(L"Panel build · 2026-09-14"), *g_fSmall, kTextFnt, x + S(87.0f), y + S(50.0f));

        const wchar_t* lines[] = {
            T(L"Method: NtCreateThreadEx calls LoadLibraryW, falls back to LdrLoadDll"),
            T(L"Hook bypass: restores 12 hooked APIs before injecting, then puts them back"),
            T(L"Ready check: working set >= 600MB and stable over two samples"),
            L"",
            T(L"Hotkeys: INSERT opens the menu in game / F4 unloads the DLL"),
            T(L"Troubleshooting: injector.log next to the program records every step"),
        };
        float ly = y + S(92.0f);
        for (const wchar_t* l : lines) {
            if (l[0])
                Text(g, l, *g_fBody, kTextDim, x + S(24.0f), ly);
            ly += S(25.0f);
        }
    }

    // 展开的 DLL 列表(画在最上层)
    void DrawDropdown(Graphics& g)
    {
        // ---- 语言列表(设置页) ----
        if (g_langOpen) {
            const int total = LangRowCount();
            const float rh = S(kDropRow);
            const RectF list(g_langRect.X, g_langRect.GetBottom() + S(6.0f), g_langRect.Width,
                             rh * (float)total + S(10.0f));
            g_dropRect = list;

            FillRound(g, RectF(list.X + S(2.0f), list.Y + S(4.0f), list.Width, list.Height), S(12.0f),
                      Color(80, 0, 0, 0));
            FillRound(g, list, S(12.0f), Color(255, 24, 28, 38));
            StrokeRound(g, list, S(12.0f), Color(120, 59, 130, 246), S(1.0f));

            for (int i = 0; i < total; ++i) {
                const int value = i - 1;                  // -1 = 自动
                const RectF r(list.X + S(5.0f), list.Y + S(5.0f) + i * rh, list.Width - S(10.0f), rh - S(2.0f));
                const bool sel = (value == g_cfg.lang);
                const bool hov = (i == g_langHover);
                if (sel)      FillRound(g, r, S(8.0f), Color(255, 32, 44, 62));
                else if (hov) FillRound(g, r, S(8.0f), Color(255, 27, 33, 45));

                TextAlign(g, LangRowText(i), *g_fBody,
                          sel ? kAccent2 : (hov ? kText : Color(255, 205, 213, 228)),
                          RectF(r.X + S(10.0f), r.Y, r.Width - S(20.0f), r.Height), 0);
                AddHot(r, ID_LANG_BASE + i);
            }
            return;
        }

        if (!g_dropOpen)
            return;

        const int total = (int)g_dlls.size();
        const int rows = (total == 0) ? 1 : (total < kDropMaxRows ? total : kDropMaxRows);
        const float rh = S(kDropRow);
        const RectF list(g_selRect.X, g_selRect.GetBottom() + S(6.0f), g_selRect.Width,
                         rh * (float)rows + S(10.0f));
        g_dropRect = list;

        FillRound(g, RectF(list.X + S(2.0f), list.Y + S(4.0f), list.Width, list.Height), S(12.0f),
                  Color(80, 0, 0, 0));
        FillRound(g, list, S(12.0f), Color(255, 24, 28, 38));
        StrokeRound(g, list, S(12.0f), Color(120, 59, 130, 246), S(1.0f));

        if (total == 0) {   // 本目录没有 DLL: 给一行提示, 免得只是空条
            TextAlign(g, T(L"(No .dll in this folder — click Browse to pick one)"), *g_fBody, kTextFnt,
                      RectF(list.X + S(12.0f), list.Y + S(5.0f), list.Width - S(24.0f), rh), 0);
            return;
        }

        for (int i = 0; i < rows; ++i) {
            const int idx = i + (int)g_dropScroll;
            if (idx >= total)
                break;
            const RectF r(list.X + S(5.0f), list.Y + S(5.0f) + i * rh, list.Width - S(10.0f), rh - S(2.0f));
            const bool sel = (idx == g_dropSel);
            const bool hov = (idx == g_dropHover);
            if (sel)      FillRound(g, r, S(8.0f), Color(255, 32, 44, 62));
            else if (hov) FillRound(g, r, S(8.0f), Color(255, 27, 33, 45));

            TextAlign(g, ::FileNameOf(g_dlls[(size_t)idx]), *g_fBody,
                      sel ? kAccent2 : (hov ? kText : Color(255, 205, 213, 228)),
                      RectF(r.X + S(10.0f), r.Y, r.Width - S(20.0f), r.Height), 0);
            AddHot(r, ID_DROP_BASE + idx);
        }

        if (total > kDropMaxRows) {   // 滚动提示
            Text(g, T(L"Scroll with the wheel"), *g_fSmall, kTextFnt,
                 list.X + S(10.0f), list.GetBottom() - S(16.0f));
        }
    }

    void PaintAll(HWND hwnd)
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rc;
        GetClientRect(hwnd, &rc);

        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);

        {
            Graphics g(mem);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
            g.SetPixelOffsetMode(PixelOffsetModeHalf);

            SolidBrush bg(kBgWin);
            g.FillRectangle(&bg, 0.0f, 0.0f, (REAL)rc.right, (REAL)rc.bottom);

            g_hots.clear();
            DrawTopBar(g, (float)rc.right);     // 顶栏: 品牌 + 导航
            DrawHeader(g, (float)rc.right);     // 页标题 + 状态胶囊 + 窗口按钮

            switch (g_page) {
            case PAGE_HOME: DrawHome(g); break;
            case PAGE_LOG:  DrawLogPage(g); break;
            case PAGE_SET:  DrawSettingsPage(g); break;
            default:        DrawAboutPage(g); break;
            }

            DrawFooter(g, (float)rc.bottom, (float)rc.right);   // 底栏: 状态 + 操作
            DrawDropdown(g);     // 覆盖层: 必须最后画
        }

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
    }

} // namespace

// ---------------------------------------------------------------------------
// 交互
// ---------------------------------------------------------------------------
namespace {

    int HitTest(float x, float y)
    {
        // 倒序: 后绘制的覆盖层(下拉列表)优先命中
        for (size_t i = g_hots.size(); i-- > 0; ) {
            const HitBox& h = g_hots[i];
            if (x >= h.r.X && x <= h.r.GetRight() && y >= h.r.Y && y <= h.r.GetBottom())
                return h.id;
        }
        return ID_NONE;
    }

    void StartInjection()
    {
        if (g_running)
            return;
        InjectConfig cfg = CollectConfig();
        if (cfg.dllPath.empty()) {
            AppendLogLine(LOG_ERR, T(L"No DLL selected — cannot inject."));
            MessageBoxW(g_hwnd, T(L"Please pick a DLL to inject first."), T(L"LUOXUEQI Injector"), MB_ICONWARNING | MB_OK);
            return;
        }
        g_cfg = cfg;
        SaveSettings(cfg);          // 记住这次选择

        SetBusy(true);
        ClearCancel();
        g_page = PAGE_LOG;          // 自动切到日志页看进度
        LayoutChildren();
        AppendLogLine(LOG_STEP, T(L"Injection task started"));

        g_worker = new std::thread([cfg]() {
            InjectOutcome oc = RunInjection(cfg);
            auto* payload = new InjectOutcome(oc);
            if (!PostMessageW(g_hwnd, WM_APP + 2, 0, reinterpret_cast<LPARAM>(payload)))
                delete payload;
        });
    }

    void StopWorker()
    {
        RequestCancel();
        if (g_worker) {
            if (g_worker->joinable())
                g_worker->join();
            delete g_worker;
            g_worker = nullptr;
        }
    }

    void Activate(int id)
    {
        switch (id) {
        case ID_NAV0: g_page = PAGE_HOME;  LayoutChildren(); InvalidateRect(g_hwnd, nullptr, FALSE); return;
        case ID_NAV1: g_page = PAGE_LOG;   LayoutChildren(); InvalidateRect(g_hwnd, nullptr, FALSE); return;
        case ID_NAV2: g_page = PAGE_SET;   LayoutChildren(); InvalidateRect(g_hwnd, nullptr, FALSE); return;
        case ID_NAV3: g_page = PAGE_ABOUT; LayoutChildren(); InvalidateRect(g_hwnd, nullptr, FALSE); return;
        case ID_MIN:   ShowWindow(g_hwnd, SW_MINIMIZE); return;
        case ID_CLOSE: PostMessageW(g_hwnd, WM_CLOSE, 0, 0); return;

        case ID_SEG_LIB:
            if (g_animSegTo != 0.0f) { g_animSegTo = 0.0f; RequestAnim(); }
            return;
        case ID_SEG_MM:
            if (g_animSegTo != 1.0f) { g_animSegTo = 1.0f; RequestAnim(); }
            return;
        case ID_SW_WAIT:
            g_animWaitTo = (g_animWaitTo > 0.5f) ? 0.0f : 1.0f;
            RequestAnim();
            return;
        case ID_SW_RANDOM:
            g_animRandTo = (g_animRandTo > 0.5f) ? 0.0f : 1.0f;
            RequestAnim();
            return;

        case ID_SELECT:
            g_dropOpen = !g_dropOpen;
            g_dropHover = -1;
            g_dropScroll = 0.0f;
            InvalidateRect(g_hwnd, nullptr, FALSE);
            return;
        case ID_INJECT: StartInjection(); return;
        case ID_CANCEL:
            RequestCancel();
            AppendLogLine(LOG_WARN, T(L"Cancel requested (the wait loop exits on the next poll)"));
            return;
        case ID_CLEARLOG:
            SetWindowTextW(g_hLog, L"");
            return;
        case ID_EXPORTLOG:
        {
            const std::wstring out = g_dllDir + L"\\injector_log_export.txt";
            wchar_t buf[1024 * 8] = {};
            GetWindowTextW(g_hLog, buf, _countof(buf));
            FILE* f = nullptr;
            if (_wfopen_s(&f, out.c_str(), L"w, ccs=UTF-8") == 0 && f) {
                fputws(buf, f);
                fclose(f);
                AppendLogLine(LOG_OK, T(L"Log exported: ") + out);
            }
            return;
        }
        case ID_OPENINI:
            ShellExecuteW(nullptr, L"open", L"notepad.exe",
                          (L"\"" + g_dllDir + L"\\injector.ini\"").c_str(), nullptr, SW_SHOWNORMAL);
            return;
        case ID_OPENDIR:
            ShellExecuteW(nullptr, L"open", g_dllDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return;
        case ID_REFRESH:
            FillDllCombo(::FileNameOf(CurrentComboPath()));
            AppendLogLine(LOG_INFO, T(L"Refreshed, found ") + std::to_wstring(g_dlls.size()) + T(L" DLL file(s)"));
            InvalidateRect(g_hwnd, nullptr, FALSE);
            return;
        case ID_BROWSE:
        {
            wchar_t file[MAX_PATH] = {};
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = g_hwnd;
            ofn.lpstrFilter = T(L"DLL files (*.dll)");
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrTitle = T(L"Select the DLL to inject");
            ofn.lpstrInitialDir = g_dllDir.c_str();
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&ofn)) {
                const std::wstring picked(file);
                bool found = false;
                for (size_t i = 0; i < g_dlls.size(); ++i) {
                    if (_wcsicmp(g_dlls[i].c_str(), picked.c_str()) == 0) {
                        g_dropSel = (int)i;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    g_dlls.push_back(picked);
                    g_dropSel = (int)g_dlls.size() - 1;
                }
                g_cfg.dllPath = picked;
                AppendLogLine(LOG_OK, T(L"Selected: ") + ::FileNameOf(picked));
            }
            return;
        }
        default:
            return;
        }
    }

} // namespace

// ---------------------------------------------------------------------------
// 窗口过程
// ---------------------------------------------------------------------------
namespace {

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg) {
        case WM_CREATE:
        {
            g_hwnd = hwnd;
            g_brInput = CreateSolidBrush(RGB(24, 29, 40));

            g_ff = new FontFamily(L"Microsoft YaHei UI");
            g_fBrand = new Font(g_ff, S(16.0f), FontStyleBold, UnitPixel);
            g_fPage  = new Font(g_ff, S(19.0f), FontStyleBold, UnitPixel);
            g_fCard  = new Font(g_ff, S(15.0f), FontStyleBold, UnitPixel);
            g_fBody  = new Font(g_ff, S(14.0f), FontStyleRegular, UnitPixel);
            g_fSmall = new Font(g_ff, S(12.0f), FontStyleRegular, UnitPixel);
            g_fBtn   = new Font(g_ff, S(14.0f), FontStyleBold, UnitPixel);

            // DLL 下拉改为完全自绘(见 DrawDropdown), 不用原生 COMBOBOX

            g_hProc = CreateWindowExW(0, L"EDIT", L"cs2.exe",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                      0, 0, 10, 10, hwnd, (HMENU)2, nullptr, nullptr);

            // 日志(Rich Edit, 暗底彩字)
            g_hLog = CreateWindowExW(0, L"RICHEDIT50W", L"",
                                     WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                     0, 0, 10, 10, hwnd, (HMENU)3, nullptr, nullptr);
            SendMessageW(g_hLog, EM_SETBKGNDCOLOR, 0, RGB(15, 18, 24));
            SendMessageW(g_hLog, EM_SETREADONLY, TRUE, 0);
            SendMessageW(g_hLog, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                         MAKELPARAM(SI(10.0f), SI(10.0f)));

            LoadSettings(g_dllDir, g_cfg);
            g_dllDir = g_cfg.dllDir.empty() ? GetExeDirectory() : g_cfg.dllDir;
            g_cfg.dllDir = g_dllDir;

            // 界面语言: 先用 ini 里的手选值, 没设过(-1)就按 CS2 语言 -> Steam -> 系统语言 检测
            I18n::Init(g_cfg.lang);

            g_animWait = g_animWaitTo = g_cfg.forceWait ? 1.0f : 0.0f;
            g_animRand = g_animRandTo = g_cfg.randomInstance ? 1.0f : 0.0f;
            g_animSeg  = g_animSegTo  = g_cfg.manualMap ? 1.0f : 0.0f;

            SetWindowTextW(g_hProc, g_cfg.processName.c_str());
            FillDllCombo(g_cfg.dllPath.empty() ? L"" : ::FileNameOf(g_cfg.dllPath));

            RefreshLive();
            SetTimer(hwnd, 1, 1000, nullptr);
            LayoutChildren();

            SetLogSink(GuiLogSink);
            AppendLogLine(LOG_STEP, T(L"LUOXUEQI Injector · Panel"));
            AppendLogLine(LOG_INFO, T(L"Program folder: ") + g_dllDir);
            AppendLogLine(LOG_INFO, T(L"Found ") + std::to_wstring(g_dlls.size()) + T(L" DLL file(s)"));
            AppendLogLine(LOG_INFO, T(L"Usage: pick a DLL -> Start Inject -> launch CS2 (or turn off 'Wait for game')"));
            return 0;
        }

        case WM_APP + 1:
        {
            auto* p = reinterpret_cast<std::pair<int, std::wstring>*>(lParam);
            if (p) {
                AppendLogLine(p->first, p->second);
                delete p;
            }
            return 0;
        }

        case WM_APP + 2:
        {
            auto* oc = reinterpret_cast<InjectOutcome*>(lParam);
            if (oc) {
                switch (oc->status) {
                case InjectStatus::Success:   AppendLogLine(LOG_OK,  T(L"Task finished: success")); break;
                case InjectStatus::Cancelled: AppendLogLine(LOG_WARN, T(L"Task finished: cancelled")); break;
                case InjectStatus::NoProcess: AppendLogLine(LOG_ERR, T(L"Task finished: target process not found")); break;
                case InjectStatus::BadDll:    AppendLogLine(LOG_ERR, T(L"Task finished: invalid DLL")); break;
                default:                      AppendLogLine(LOG_ERR, T(L"Task finished: inject failed")); break;
                }
                delete oc;
            }
            SetBusy(false);
            if (g_worker) {
                if (g_worker->joinable())
                    g_worker->join();
                delete g_worker;
                g_worker = nullptr;
            }
            RefreshLive();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_TIMER:
            if (wParam == 1) {
                RefreshLive();
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (wParam == 2) {
                UpdateAnim();
            }
            return 0;

        case WM_MOUSEWHEEL:
        {
            if (g_dropOpen && (int)g_dlls.size() > kDropMaxRows) {
                const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                g_dropScroll -= (delta > 0 ? 1.0f : -1.0f);
                const float maxScroll = (float)((int)g_dlls.size() - kDropMaxRows);
                if (g_dropScroll < 0.0f) g_dropScroll = 0.0f;
                if (g_dropScroll > maxScroll) g_dropScroll = maxScroll;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;
        }

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE && (g_dropOpen || g_langOpen)) {
                g_dropOpen = false;
                g_langOpen = false;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;

        case WM_MOUSEMOVE:
        {
            const float x = (float)GET_X_LPARAM(lParam);
            const float y = (float)GET_Y_LPARAM(lParam);
            if (g_dropOpen) {   // 列表内悬停高亮
                const int h2 = HitTest(x, y);
                const int newHover = (h2 >= ID_DROP_BASE) ? (h2 - ID_DROP_BASE) : -1;
                if (newHover != g_dropHover) {
                    g_dropHover = newHover;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            if (g_langOpen) {   // 语言列表内悬停高亮
                const int h3 = HitTest(x, y);
                const int newHover = (h3 >= ID_LANG_BASE) ? (h3 - ID_LANG_BASE) : -1;
                if (newHover != g_langHover) {
                    g_langHover = newHover;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            const int h = HitTest(x, y);
            if (h != g_hover) {
                g_hover = h;
                SetCursor(LoadCursorW(nullptr, h != ID_NONE ? IDC_HAND : IDC_ARROW));
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONDOWN:
        {
            const int id = HitTest((float)GET_X_LPARAM(lParam), (float)GET_Y_LPARAM(lParam));

            if (g_langOpen) {                       // 语言列表展开时
                if (id >= ID_LANG_BASE && id < ID_LANG_BASE + LangRowCount()) {
                    g_cfg.lang = (id - ID_LANG_BASE) - 1;    // -1 = 自动
                    g_langOpen = false;
                    I18n::Init(g_cfg.lang);
                    SaveSettings(g_cfg);
                    AppendLogLine(LOG_OK, std::wstring(T(L"Language")) + L": " +
                                          I18n::LangName(I18n::g_lang));
                    LayoutChildren();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                g_langOpen = false;                 // 点别处: 先收起, 再继续
                InvalidateRect(hwnd, nullptr, FALSE);
            }

            if (id == ID_LANGSEL) {                 // 语言选择器: 切换展开
                g_langOpen = !g_langOpen;
                g_dropOpen = false;
                g_langHover = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            if (g_dropOpen) {
                if (id >= ID_DROP_BASE) {           // 点选项 = 选中并收起
                    g_dropSel = id - ID_DROP_BASE;
                    g_dropOpen = false;
                    g_cfg.dllPath = CurrentComboPath();
                    AppendLogLine(LOG_OK, T(L"Selected: ") + ::FileNameOf(g_cfg.dllPath));
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                g_dropOpen = false;                 // 点别处: 先收起, 再继续处理这次点击
                InvalidateRect(hwnd, nullptr, FALSE);
            }

            if (id == ID_SELECT) {                  // 选择器: 切换展开
                g_dropOpen = !g_dropOpen;
                g_langOpen = false;
                g_dropHover = -1;
                g_dropScroll = 0.0f;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            g_press = id;
            if (id != ID_NONE) {
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONUP:
        {
            ReleaseCapture();
            const int up = HitTest((float)GET_X_LPARAM(lParam), (float)GET_Y_LPARAM(lParam));
            const int pressed = g_press;
            g_press = ID_NONE;
            if (pressed != ID_NONE && pressed == up)
                Activate(pressed);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_NCHITTEST:
        {
            POINT p{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &p);
            // 顶栏可拖动(导航项 / 状态胶囊 / 窗口按钮区除外)
            if (p.y < SI(kHeadH) && p.x < SI(kWinW - 130.0f)) {
                const float fx = (float)p.x / (g_scale > 0.0f ? g_scale : 1.0f);
                const bool onNav = (fx >= g_navRect.X - S(6.0f)) && (fx <= g_navRect.GetRight() + S(6.0f));
                if (!onNav)
                    return HTCAPTION;
            }
            break;
        }

        case WM_DRAWITEM:
        {
            auto* di = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (di && di->CtlType == ODT_COMBOBOX) {
                Graphics g(di->hDC);
                g.SetSmoothingMode(SmoothingModeAntiAlias);
                g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
                const RectF r((REAL)di->rcItem.left, (REAL)di->rcItem.top,
                              (REAL)(di->rcItem.right - di->rcItem.left),
                              (REAL)(di->rcItem.bottom - di->rcItem.top));
                const bool sel = (di->itemState & ODS_SELECTED) != 0;
                SolidBrush bg(sel ? Color(255, 32, 40, 56) : Color(255, 24, 29, 40));
                g.FillRectangle(&bg, r);
                wchar_t text[300] = {};
                SendMessageW(di->hwndItem, CB_GETLBTEXT, (WPARAM)di->itemID, (LPARAM)text);
                TextAlign(g, text, *g_fBody, sel ? kAccent2 : kText,
                          RectF(r.X + S(10.0f), r.Y, r.Width - S(16.0f), r.Height), 0);
                return TRUE;
            }
            break;
        }

        case WM_MEASUREITEM:
        {
            auto* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            if (mi && mi->CtlType == ODT_COMBOBOX) {
                mi->itemHeight = (UINT)SI(26.0f);
                return TRUE;
            }
            break;
        }

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORLISTBOX:
        {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, RGB(232, 237, 247));
            SetBkColor(hdc, RGB(24, 29, 40));
            return (LRESULT)g_brInput;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            PaintAll(hwnd);
            return 0;

        case WM_CLOSE:
            StopWorker();
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            StopWorker();
            KillTimer(hwnd, 1);
            KillTimer(hwnd, 2);
            SetLogSink(nullptr);
            delete g_fBrand; g_fBrand = nullptr;
            delete g_fPage;  g_fPage = nullptr;
            delete g_fCard;  g_fCard = nullptr;
            delete g_fBody;  g_fBody = nullptr;
            delete g_fSmall; g_fSmall = nullptr;
            delete g_fBtn;   g_fBtn = nullptr;
            delete g_ff;     g_ff = nullptr;
            if (g_brInput) { DeleteObject(g_brInput); g_brInput = nullptr; }
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

} // namespace

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    {
        using SetDpiFn = BOOL(WINAPI*)(void*);
        if (HMODULE u = GetModuleHandleW(L"user32.dll")) {
            auto fn = reinterpret_cast<SetDpiFn>(GetProcAddress(u, "SetProcessDpiAwarenessContext"));
            if (fn) fn(reinterpret_cast<void*>(-4));   // PER_MONITOR_AWARE_V2
        }
    }
    {
        HDC screen = GetDC(nullptr);
        if (screen) {
            g_scale = (float)GetDeviceCaps(screen, LOGPIXELSY) / 96.0f;
            ReleaseDC(nullptr, screen);
            if (g_scale < 1.0f) g_scale = 1.0f;
            if (g_scale > 2.0f) g_scale = 2.0f;
        }
    }

    {
        GdiplusStartupInput in;
        if (GdiplusStartup(&g_gdiToken, &in, nullptr) != Ok) {
            MessageBoxW(nullptr, T(L"GDI+ initialization failed."), T(L"LUOXUEQI Injector"), MB_ICONERROR | MB_OK);
            return 1;
        }
    }

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool isRandomizedInstance = false;
    InjectConfig cfg = ParseInjectArgs(argc, argv, isRandomizedInstance);

    const std::wstring dir = cfg.dllDir.empty() ? GetExeDirectory() : cfg.dllDir;
    LoadSettings(dir, cfg);
    g_dllDir = dir;
    g_cfg = cfg;
    g_cfg.dllDir = dir;

    InitStartupLog(dir);
    StartupLog(L"==== 面板启动 ====");
    StartupLog(L"参数", L"随机实例=" + std::to_wstring((int)isRandomizedInstance) +
                       L" 目录=" + dir + L" 进程=" + cfg.processName +
                       L" 方式=" + (cfg.manualMap ? L"manualmap" : L"loadlibrary") +
                       L" 随机实例开关=" + std::to_wstring((int)cfg.randomInstance));

    if (!isRandomizedInstance && cfg.randomInstance) {
        if (HandleRandomizedInstance(cfg)) {
            StartupLog(L"已复制为随机实例副本并重启, 本进程退出");
            if (argv) LocalFree(argv);
            GdiplusShutdown(g_gdiToken);
            return 0;
        }
        StartupLog(L"随机实例复制失败, 就地启动面板");
    }
    if (argv) LocalFree(argv);

    if (!LoadLibraryW(L"Msftedit.dll")) {
        StartupLog(L"Msftedit.dll 加载失败, 面板无法启动");
        MessageBoxW(nullptr, T(L"Failed to load Msftedit.dll, cannot start the panel."), T(L"LUOXUEQI Injector"),
                    MB_ICONERROR | MB_OK);
        GdiplusShutdown(g_gdiToken);
        return 1;
    }
    StartupLog(L"Msftedit.dll 就绪");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"TWInjectorPanelWnd";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc)) {
        StartupLog(L"RegisterClassExW 失败", std::to_wstring(GetLastError()));
        GdiplusShutdown(g_gdiToken);
        return 1;
    }

    RECT rc{ 0, 0, SI(kWinW), SI(kWinH) };
    const DWORD kStyle = WS_POPUP | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    AdjustWindowRectEx(&rc, kStyle, FALSE, 0);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, T(L"LUOXUEQI Injector"), kStyle,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                rc.right - rc.left, rc.bottom - rc.top,
                                nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) {
        StartupLog(L"CreateWindowExW 失败", std::to_wstring(GetLastError()));
        GdiplusShutdown(g_gdiToken);
        return 1;
    }

    {   // 深色标题栏 + Win11 圆角
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, 20 /*IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
        int pref = 2;   // DWMWCP_ROUND
        DwmSetWindowAttribute(hwnd, 33 /*WINDOW_CORNER_PREFERENCE*/, &pref, sizeof(pref));
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    StartupLog(L"面板已创建, 进入消息循环");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    StartupLog(L"消息循环结束, 退出", std::to_wstring((int)msg.wParam));
    GdiplusShutdown(g_gdiToken);
    return (int)msg.wParam;
}
