#include "hint_window_test_shim.h"
#include "../../GlassCore/src/Hooks/delphi/THintWindowDraw.cpp"
#include "../Popup/Tooltip/DelphiHintPaint.h"

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)

namespace
{
    namespace Hint = GlassCore::DelphiHint;
    using HintTest::Event;
    using HintTest::Kind;

    int begins = 0, ends = 0;
    HDC begunWith{};                    // the DC the bridge passed last
    // The two exports of GlassCore, as Exports.cpp forwards them.
    BOOL WINAPI BeginExport(HWND w, UINT message, HDC dc) noexcept
    { ++begins; begunWith = dc; return Hint::BeginPaint(w, message, dc) ? TRUE : FALSE; }
    void WINAPI EndExport() noexcept { ++ends; Hint::EndPaint(); }

    bool Same(const RECT& a, const RECT& b)
    { return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom; }
    [[maybe_unused]] long long Number(const void* p) { return static_cast<long long>(reinterpret_cast<std::intptr_t>(p)); }
    std::vector<Event> Take() { return std::exchange(HintTest::events, {}); }

    // A VCL application with one hint window, and what THintWindow does with
    // the messages that reach it (Controls.pas): WMEraseBkgnd fills the client
    // with the hint colour, NCPaint draws an edge into a window DC, Paint draws
    // the tooltip face and then the caption with one DrawText of the EXE.
    struct Fixture
    {
        TestModule exe, otherModule, coreModule;
        TestWindow hint, form;
        TestDC eraseDc, paintDc, memory, foreign;
        bool unicode;
        int vclBkMode = TRANSPARENT;        // Canvas.Brush.Style = bsClear
        int paintCharset = DEFAULT_CHARSET;
        int ansiCount = -1;
        // What VCL saw of its DrawText.
        int textResult = 0;
        DWORD textError = 0;
        RECT textRect{};
        std::function<void()> duringPaint;  // runs inside Paint, before the caption

        explicit Fixture(bool wide = true) : unicode(wide)
        {
            using namespace HintTest;
            assert(liveDcs.empty() && !exclusive && !shared);
            error = 0; thread = 1; tick += 1000000; disabled = false; tooltipColor = 0x001a1a1a;
            failBlack = failSave = failExclude = failWindowDc = failViewport = failOrg = false;
            failClient = failRect = failOrigin = nullptr; failConversion = false;
            registrationFailures.clear(); beforeExclusive = {};
            alphaResult = S_OK; alphaHeight = 15; alphaThrows = false; alphaMovesRect = true; duringAlpha = {};
            animation = fade = parametersResult = TRUE; parameterCalls = directParametersW = directParametersA = 0;
            dcsTaken = dcsReleased = 0; exclusiveLocks = 0;
            events.clear(); logs.clear();
            shuttingDown = false; begins = ends = 0; begunWith = nullptr;
            if (unicode)
                exe.originals = {{"DrawTextW", reinterpret_cast<void*>(NativeDrawTextW)},
                                 {"SystemParametersInfoW", reinterpret_cast<void*>(NativeParameters)}};
            else
                exe.originals = {{"DrawTextA", reinterpret_cast<void*>(NativeDrawTextA)},
                                 {"SystemParametersInfoA", reinterpret_cast<void*>(NativeParameters)}};
            GlassCore::ModuleHandler::g_entryPointModule = &exe;
            hint.instance = &exe;
            form.cls = L"TForm"; form.instance = &exe; form.screen = RECT{0, 0, 640, 480}; form.frame = 0;
            eraseDc.owner = &hint; paintDc.owner = &hint; foreign.owner = &form;
            core = &coreModule;
            beginExport = reinterpret_cast<FARPROC>(BeginExport);
            endExport = reinterpret_cast<FARPROC>(EndExport);
            windowProcedure = [this](HWND w, UINT message, WPARAM wParam, LPARAM lParam) { return WndProc(w, message, wParam, lParam); };
            Hint::Startup();
        }
        ~Fixture()
        {
            Hint::Shutdown();
            for (auto* module : {&exe, &otherModule, &coreModule})
            {
                for (const auto& [name, ref] : module->refs) { (void)name; assert(ref == 0); }
                assert(module->hooks.empty());
            }
            assert(HintTest::liveDcs.empty() && HintTest::dcsTaken == HintTest::dcsReleased);
            assert(!HintTest::exclusive && !HintTest::shared && Hint::g_sessions.empty());
            assert(!HintTest::beforeExclusive);
            HintTest::windowProcedure = {}; HintTest::duringAlpha = {};
            GlassCore::ModuleHandler::g_entryPointModule = nullptr;
        }
        int Ref(const char* name) const { const auto it = exe.refs.find(name); return it == exe.refs.end() ? 0 : it->second; }

        // Calls the EXE makes through its import table.
        int DrawW(HDC dc, LPCWSTR text, int count, LPRECT rect, UINT flags)
        {
            using Fn = int (WINAPI*)(HDC, LPCWSTR, int, LPRECT, UINT);
            const auto it = exe.hooks.find("DrawTextW");
            return it != exe.hooks.end() ? reinterpret_cast<Fn>(it->second)(dc, text, count, rect, flags)
                : NativeDrawTextW(dc, text, count, rect, flags);
        }
        int DrawA(HDC dc, LPCSTR text, int count, LPRECT rect, UINT flags)
        {
            using Fn = int (WINAPI*)(HDC, LPCSTR, int, LPRECT, UINT);
            const auto it = exe.hooks.find("DrawTextA");
            return it != exe.hooks.end() ? reinterpret_cast<Fn>(it->second)(dc, text, count, rect, flags)
                : NativeDrawTextA(dc, text, count, rect, flags);
        }
        BOOL Parameters(UINT action, PVOID data)
        {
            using Fn = BOOL (WINAPI*)(UINT, UINT, PVOID, UINT);
            const auto it = exe.hooks.find(unicode ? "SystemParametersInfoW" : "SystemParametersInfoA");
            return it != exe.hooks.end() ? reinterpret_cast<Fn>(it->second)(action, 0, data, 0) : NativeParameters(action, 0, data, 0);
        }
        // THintWindow.ActivateHint: SystemParametersInfo(SPI_GETTOOLTIPANIMATION, 0, @Animate, 0).
        bool Animates()
        {
            BOOL animate = 7;
            CHECK(Parameters(SPI_GETTOOLTIPANIMATION, &animate) == TRUE);
            CHECK(animate == TRUE || animate == FALSE);
            return animate != FALSE;
        }

        void Paint(HWND w, HDC dc)
        {
            RECT r = w->Client();
            if (w->themed) HintTest::events.push_back(Event(Kind::Face, dc, r));
            if (duringPaint) duringPaint();
            r.left += 2; r.top += 2;
            dc->charset = paintCharset;
            SetTextColor(dc, w->fontColor); SetBkMode(dc, vclBkMode);
            textRect = r;
            SetLastError(0x55);
            textResult = unicode ? DrawW(dc, w->caption.c_str(), -1, &textRect, w->textFlags)
                : DrawA(dc, w->ansiCaption.c_str(), ansiCount, &textRect, w->textFlags);
            textError = GetLastError();
        }
        void Erase(HWND w, HDC dc)
        {
            Event e(Kind::Fill, dc, w->Client());
            e.color = w->color;
            HintTest::events.push_back(e);
        }
        void Edge(HWND w, HDC dc)
        { HintTest::events.push_back(Event(Kind::Edge, dc, RECT{0, 0, w->screen.right - w->screen.left, w->screen.bottom - w->screen.top})); }
        LRESULT WndProc(HWND w, UINT message, WPARAM wParam, LPARAM)
        {
            switch (message)
            {
            case WM_ERASEBKGND:
                Erase(w, reinterpret_cast<HDC>(wParam));
                return 1;
            case WM_NCPAINT:
            {
                const HDC dc = GetWindowDC(w);
                Edge(w, dc);
                ReleaseDC(w, dc);
                return 0;
            }
            case WM_PAINT:
                if (wParam) { Paint(w, reinterpret_cast<HDC>(wParam)); return 0; }
                // BeginPaint: a DC of the window, and the erase the update region carries.
                paintDc = TestDC{}; paintDc.owner = w;
                if (std::exchange(w->erasePending, false)) Send(w, WM_ERASEBKGND, reinterpret_cast<WPARAM>(&paintDc));
                Paint(w, &paintDc);
                return 0;
            case WM_PRINT:
                // THintWindow.WMPrint: PaintTo and NCPaint, inside VCL, past the subclass.
                Erase(w, reinterpret_cast<HDC>(wParam));
                Paint(w, reinterpret_cast<HDC>(wParam));
                Edge(w, reinterpret_cast<HDC>(wParam));
                return 0;
            default:
                return 0;
            }
        }
        // What the tooltip subclass does with a message of a VCL hint window.
        LRESULT Send(HWND w, UINT message, WPARAM wParam = 0, LPARAM lParam = 0)
        {
            LRESULT result{};
            if ((message == WM_ERASEBKGND || message == WM_NCPAINT || message == WM_PAINT)
                && WndCompat::TryHandleDelphiHintPaint(w, message, wParam, lParam, result))
                return result;
            return DefSubclassProc(w, message, wParam, lParam);
        }
        // The caption was drawn by VCL's own DrawText, with the arguments VCL gave it.
        bool NativeCaption(const Event& e, HDC dc) const
        {
            return e.kind == (unicode ? Kind::TextW : Kind::TextA) && e.dc == dc && e.entryError == 0x55
                && e.rectPointer == &textRect && e.flags == hint.textFlags && e.color == hint.fontColor
                && textResult == (unicode ? 41 : 42) && textError == 0xc102
                && (unicode ? e.text == hint.caption && e.count == -1 : e.bytes == hint.ansiCaption && e.count == ansiCount);
        }
    };
    const RECT kClient{0, 0, 113, 19}, kWindow{0, 0, 115, 21}, kCaption{2, 2, 113, 19};

    // The hint of a control is shown: VCL asks about the animation, shows the
    // window, Windows asks for the frame and erases, and WM_PAINT follows.
    void Shown()
    {
        Fixture f;
        Hint::OnAttached(&f.hint);
        CHECK(f.Ref("DrawTextW") == 1 && f.Ref("SystemParametersInfoW") == 1 && f.exe.hooks.size() == 2);
        CHECK(Hint::g_hint == &f.hint && Hint::g_sessions.empty());
        // Tooltips are animated, but not this one: VCL will not call AnimateWindow.
        SetLastError(0x77);
        CHECK(!f.Animates() && HintTest::parameterCalls == 1 && GetLastError() == 0xc103);
        CHECK(Take().empty() && begins == 0);

        // The frame: the base around the client, into a window DC that is given back.
        CHECK(f.Send(&f.hint, WM_NCPAINT, 1) == 0);
        auto e = Take();
        CHECK(e.size() == 1 && e[0].kind == Kind::Black && Same(e[0].rect, kWindow));
        CHECK(e[0].excluded.size() == 1 && Same(e[0].excluded[0], RECT{1, 1, 114, 20}));
        CHECK(HintTest::dcsTaken == 1 && HintTest::dcsReleased == 1 && begins == 1 && ends == 1);
        // The erase: the base over the client, in the DC the message brought.
        CHECK(f.Send(&f.hint, WM_ERASEBKGND, reinterpret_cast<WPARAM>(&f.eraseDc)) == 1);
        e = Take();
        CHECK(e.size() == 1 && e[0].kind == Kind::Black && e[0].dc == &f.eraseDc && Same(e[0].rect, kClient) && e[0].excluded.empty());
        CHECK(begins == 2 && ends == 2 && Hint::g_sessions.empty());

        // WM_PAINT: VCL lays the tooltip face; under the caption the client is
        // cleared, and the caption is drawn with alpha in the tooltip colour.
        f.vclBkMode = OPAQUE;       // whatever VCL left in the DC is there again afterwards
        CHECK(f.Send(&f.hint, WM_PAINT) == 0);
        e = Take();
        CHECK(e.size() == 3 && e[0].kind == Kind::Face && e[1].kind == Kind::Black && e[2].kind == Kind::Alpha);
        CHECK(e[1].dc == &f.paintDc && Same(e[1].rect, kClient));
        CHECK(e[2].dc == &f.paintDc && e[2].text == L"Open a file" && e[2].count == -1 && Same(e[2].rect, kCaption));
        CHECK(e[2].flags == (DT_NOPREFIX | DT_WORDBREAK) && e[2].color == 0x001a1a1a && e[2].bkMode == TRANSPARENT);
        // VCL got the height of the text, its rectangle, its DC and its last error back as they were.
        CHECK(f.textResult == 15 && f.textError == 0x55 && Same(f.textRect, kCaption) && e[2].rectPointer != &f.textRect);
        CHECK(f.paintDc.text == f.hint.fontColor && f.paintDc.bkMode == OPAQUE && f.paintDc.saved.empty());
        CHECK(begins == 3 && ends == 3 && Hint::g_sessions.empty());

        // The window is hidden and shown again for the next control: the same
        // window, the same answers, nothing taken twice.
        f.hint.caption = L"Save"; f.hint.erasePending = true; f.vclBkMode = TRANSPARENT;
        CHECK(!f.Animates());
        CHECK(f.Send(&f.hint, WM_PAINT) == 0);
        e = Take();
        // The erase BeginPaint sent, the face, the clear under the caption, the caption.
        CHECK(e.size() == 4 && e[0].kind == Kind::Black && e[0].dc == &f.paintDc && e[1].kind == Kind::Face);
        CHECK(e[2].kind == Kind::Black && e[3].kind == Kind::Alpha && e[3].text == L"Save");
        CHECK(f.Ref("DrawTextW") == 1 && f.Ref("SystemParametersInfoW") == 1);
        CHECK(HintTest::Count(Kind::Fill) == 0 && HintTest::Count(Kind::Edge) == 0);
    }

    // An ANSI Delphi: DrawTextA and SystemParametersInfoA. The caption is
    // converted with the code page of the font's charset, as GDI would.
    void Ansi()
    {
        {
            Fixture f(false);
            Hint::OnAttached(&f.hint);
            CHECK(f.Ref("DrawTextA") == 1 && f.Ref("SystemParametersInfoA") == 1 && f.Ref("DrawTextW") == 0);
            CHECK(!f.Animates());
            f.hint.ansiCaption = "\xCE\xF2\xEA\xF0\xFB\xF2\xFC"; f.paintCharset = RUSSIAN_CHARSET;
            CHECK(f.Send(&f.hint, WM_PAINT) == 0);
            auto e = Take();
            CHECK(e.size() == 3 && e[1].kind == Kind::Black && e[2].kind == Kind::Alpha);
            CHECK(e[2].text == L"\x041E\x0442\x043A\x0440\x044B\x0442\x044C" && e[2].count == 7 && Same(e[2].rect, kCaption));
            CHECK(f.textResult == 15 && f.textError == 0x55 && Same(f.textRect, kCaption));
            // A count of bytes: no terminator is taken off.
            f.ansiCount = 3;
            CHECK(f.Send(&f.hint, WM_PAINT) == 0);
            e = Take();
            CHECK(e.size() == 3 && e[2].kind == Kind::Alpha && e[2].text == L"\x041E\x0442\x043A" && e[2].count == 3);
            // The default charset: the code page of the system.
            f.ansiCount = -1; f.hint.ansiCaption = "Open"; f.paintCharset = DEFAULT_CHARSET;
            CHECK(f.Send(&f.hint, WM_PAINT) == 0);
            e = Take();
            CHECK(e.size() == 3 && e[2].kind == Kind::Alpha && e[2].text == L"Open" && e[2].count == 4);
        }
        // What cannot be converted, or is nothing, is VCL's own call - with its own arguments.
        for (int which = 0; which < 6; ++which)
        {
            Fixture f(false);
            Hint::OnAttached(&f.hint);
            switch (which)
            {
            case 0: f.paintCharset = SYMBOL_CHARSET; break;         // glyphs of a symbol font are not text
            case 1: f.paintCharset = 77; break;                     // a charset nobody knows
            case 2: f.hint.ansiCaption.clear(); break;
            case 3: f.ansiCount = 0; break;
            case 4: f.ansiCount = -2; break;                        // not a count DrawText knows
            case 5: HintTest::failConversion = true; break;         // no memory for the converted caption
            }
            CHECK(f.Send(&f.hint, WM_PAINT) == 0);
            const auto e = Take();
            CHECK(e.size() == 2 && e[0].kind == Kind::Face && f.NativeCaption(e[1], &f.paintDc));
            CHECK(Hint::g_sessions.empty());
        }
    }

    // What stays VCL's: a window that is not such a hint, a DC that is not the
    // window's own, a call that is not a caption, a message that is not one of
    // the three.
    void Gates()
    {
        // The window.
        for (int which = 0; which < 9; ++which)
        {
            Fixture f;
            Hint::OnAttached(&f.hint);
            switch (which)
            {
            case 0: f.hint.cls = L"THintWindowEx"; break;
            case 1: f.hint.cls = L"tooltips_class32"; break;
            case 2: f.hint.thread = 2; break;
            case 3: HintTest::thread = 2; break;
            case 4: f.hint.instance = &f.otherModule; break;        // VCL in a package
            case 5: f.hint.ex |= WS_EX_LAYOUTRTL; break;
            case 6: HintTest::disabled = true; break;
            case 7: GlassCore::ModuleHandler::g_entryPointModule = &f.otherModule; break;
            case 8: Hint::g_shutdown.store(true); break;
            }
            CHECK(f.Send(&f.hint, WM_NCPAINT, 1) == 0);
            CHECK(f.Send(&f.hint, WM_ERASEBKGND, reinterpret_cast<WPARAM>(&f.eraseDc)) == 1);
            CHECK(f.Send(&f.hint, WM_PAINT) == 0);
            const auto e = Take();
            CHECK(e.size() == 4 && e[0].kind == Kind::Edge && e[1].kind == Kind::Fill && e[1].dc == &f.eraseDc);
            CHECK(e[2].kind == Kind::Face && f.NativeCaption(e[3], &f.paintDc));
            CHECK(begins == 3 && ends == 0 && Hint::g_sessions.empty());
            // ...and its hint is animated as the system says.
            CHECK(f.Animates());
            HintTest::thread = 1; Hint::g_shutdown.store(false);
            GlassCore::ModuleHandler::g_entryPointModule = &f.exe;
        }
        // A class name is compared whole, whatever its case.
        {
            Fixture f; f.hint.cls = L"thintwindow";
            CHECK(f.Send(&f.hint, WM_PAINT) == 0);
            const auto e = Take();
            CHECK(e.size() == 3 && e[2].kind == Kind::Alpha);
        }
        // The erase: only into a DC of this window, in device units.
        for (int which = 0; which < 8; ++which)
        {
            Fixture f; HDC dc = &f.eraseDc;
            switch (which)
            {
            case 0: dc = &f.memory; break;                          // a print
            case 1: dc = &f.foreign; break;
            case 2: dc = nullptr; break;
            case 3: f.eraseDc.map = MM_ANISOTROPIC; break;
            case 4: f.eraseDc.viewport = POINT{0, 1}; break;
            case 5: f.eraseDc.window = POINT{1, 0}; break;
            case 6: HintTest::failViewport = true; break;
            case 7: HintTest::failOrg = true; break;
            }
            CHECK(f.Send(&f.hint, WM_ERASEBKGND, reinterpret_cast<WPARAM>(dc)) == 1);
            const auto e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::Fill && e[0].dc == dc && e[0].color == 0x00e1ffff);
            CHECK(begins == 1 && ends == 0 && Hint::g_sessions.empty());
        }
        {
            Fixture f; f.eraseDc.viewport = POINT{1, 0};
            CHECK(f.Send(&f.hint, WM_ERASEBKGND, reinterpret_cast<WPARAM>(&f.eraseDc)) == 1);
            CHECK(HintTest::Count(Kind::Fill) == 1 && HintTest::Count(Kind::Black) == 0);
            f.eraseDc.viewport = POINT{}; f.eraseDc.window = POINT{0, 1}; Take();
            CHECK(f.Send(&f.hint, WM_ERASEBKGND, reinterpret_cast<WPARAM>(&f.eraseDc)) == 1);
            CHECK(HintTest::Count(Kind::Fill) == 1 && HintTest::Count(Kind::Black) == 0);
            // A client that is nothing, or cannot be asked for.
            f.eraseDc.window = POINT{}; f.hint.screen = RECT{300, 200, 302, 221}; Take();
            CHECK(f.Send(&f.hint, WM_ERASEBKGND, reinterpret_cast<WPARAM>(&f.eraseDc)) == 1);
            CHECK(HintTest::Count(Kind::Fill) == 1 && HintTest::Count(Kind::Black) == 0);
            f.hint.screen = RECT{300, 200, 415, 202}; Take();
            CHECK(f.Send(&f.hint, WM_ERASEBKGND, reinterpret_cast<WPARAM>(&f.eraseDc)) == 1);
            CHECK(HintTest::Count(Kind::Fill) == 1 && HintTest::Count(Kind::Black) == 0);
            f.hint.screen = RECT{300, 200, 415, 221}; HintTest::failClient = &f.hint; Take();
            CHECK(f.Send(&f.hint, WM_ERASEBKGND, reinterpret_cast<WPARAM>(&f.eraseDc)) == 1);
            CHECK(HintTest::Count(Kind::Fill) == 1 && HintTest::Count(Kind::Black) == 0);
        }
        // A print: WM_PAINT with a DC, WM_PRINT (the picture AnimateWindow plays), WM_PRINTCLIENT.
        {
            Fixture f;
            Hint::OnAttached(&f.hint);
            CHECK(f.Send(&f.hint, WM_PAINT, reinterpret_cast<WPARAM>(&f.memory)) == 0);
            auto e = Take();
            CHECK(e.size() == 2 && e[0].kind == Kind::Face && f.NativeCaption(e[1], &f.memory) && begins == 1 && ends == 0);
            // Even into a DC of the window itself.
            CHECK(f.Send(&f.hint, WM_PAINT, reinterpret_cast<WPARAM>(&f.eraseDc)) == 0);
            e = Take();
            CHECK(e.size() == 2 && f.NativeCaption(e[1], &f.eraseDc) && Hint::g_sessions.empty());
            CHECK(f.Send(&f.hint, WM_PRINT, reinterpret_cast<WPARAM>(&f.memory)) == 0);
            e = Take();
            CHECK(e.size() == 4 && e[0].kind == Kind::Fill && e[1].kind == Kind::Face && f.NativeCaption(e[2], &f.memory)
                && e[3].kind == Kind::Edge);
            CHECK(f.Send(&f.hint, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(&f.memory)) == 0);
            CHECK(Take().empty() && begins == 2 && ends == 0);
            // The unit itself is asked about three messages only.
            for (UINT message : {WM_PRINT, WM_PRINTCLIENT, WM_SETTEXT, UINT{0}})
                CHECK(!Hint::BeginPaint(&f.hint, message, nullptr) && !Hint::BeginPaint(&f.hint, message, &f.eraseDc));
            CHECK(Hint::g_sessions.empty() && Take().empty());
        }
        // The caption: only DrawText into the window's own DC during its WM_PAINT.
        {
            Fixture f;
            Hint::OnAttached(&f.hint);
            RECT r = kCaption;
            // Outside a paint the import is hooked, and every call is the application's.
            SetLastError(0x55);
            CHECK(f.DrawW(&f.eraseDc, L"x", 1, &r, 0) == 41 && GetLastError() == 0xc102);
            auto e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::TextW && e[0].entryError == 0x55 && e[0].rectPointer == &r);
            // During the paint: a buffer, another window, no DC.
            f.duringPaint = [&] {
                for (HDC dc : {&f.memory, &f.foreign, static_cast<HDC>(nullptr)})
                { SetLastError(0x55); CHECK(f.DrawW(dc, L"x", 1, &r, 0) == 41 && GetLastError() == 0xc102); }
                // A measurement, a text the call may rewrite, no text, no rectangle, nothing to draw.
                CHECK(f.DrawW(&f.paintDc, L"x", 1, &r, DT_CALCRECT) == 41);
                CHECK(f.DrawW(&f.paintDc, L"x", 1, &r, DT_MODIFYSTRING) == 41);
                CHECK(f.DrawW(&f.paintDc, nullptr, 1, &r, 0) == 41);
                CHECK(f.DrawW(&f.paintDc, L"x", 1, nullptr, 0) == 41);
                CHECK(f.DrawW(&f.paintDc, L"x", 0, &r, 0) == 41);
                CHECK(f.DrawW(&f.paintDc, L"x", -2, &r, 0) == 41);
                RECT none{5, 5, 5, 9}; CHECK(f.DrawW(&f.paintDc, L"x", 1, &none, 0) == 41);
                RECT flat{5, 5, 9, 5}; CHECK(f.DrawW(&f.paintDc, L"x", 1, &flat, 0) == 41);
                // A mapping of another kind.
                f.paintDc.map = MM_ANISOTROPIC; CHECK(f.DrawW(&f.paintDc, L"x", 1, &r, 0) == 41); f.paintDc.map = MM_TEXT;
                f.paintDc.viewport = POINT{3, 0}; CHECK(f.DrawW(&f.paintDc, L"x", 1, &r, 0) == 41); f.paintDc.viewport = POINT{};
                f.paintDc.window = POINT{0, 3}; CHECK(f.DrawW(&f.paintDc, L"x", 1, &r, 0) == 41); f.paintDc.window = POINT{};
                // The client cannot be asked for, the DC cannot be saved.
                HintTest::failClient = &f.hint; CHECK(f.DrawW(&f.paintDc, L"x", 1, &r, 0) == 41); HintTest::failClient = nullptr;
                HintTest::failSave = true; CHECK(f.DrawW(&f.paintDc, L"x", 1, &r, 0) == 41); HintTest::failSave = false;
                CHECK(HintTest::Count(Kind::TextW) == 16 && HintTest::Count(Kind::Black) == 0 && HintTest::Count(Kind::Alpha) == 0);
                CHECK(!Hint::g_sessions.back().cleared && Hint::g_sessions.back().texts == 0);
            };
            CHECK(f.Send(&f.hint, WM_PAINT) == 0);
            e = Take();
            // Then the caption itself, as ever.
            CHECK(e.size() == 19 && e[0].kind == Kind::Face && e[17].kind == Kind::Black && e[18].kind == Kind::Alpha);
            // DT_NOCLIP is not the compositor's; the other flags are.
            f.duringPaint = {}; f.hint.textFlags = DT_NOPREFIX | DT_WORDBREAK | DT_NOCLIP | DT_RTLREADING;
            CHECK(f.Send(&f.hint, WM_PAINT) == 0);
            e = Take();
            CHECK(e.size() == 3 && e[2].kind == Kind::Alpha && e[2].flags == (DT_NOPREFIX | DT_WORDBREAK | DT_RTLREADING));
        }
        // An erase or a frame that is being answered runs no code of VCL: a text
        // call that arrives then is not a caption. Nor is one of a paint below.
        {
            Fixture f;
            Hint::OnAttached(&f.hint);
            RECT r = kCaption;
            for (UINT message : {WM_ERASEBKGND, WM_NCPAINT})
            {
                CHECK(Hint::BeginPaint(&f.hint, message, &f.eraseDc)); Take();
                CHECK(f.DrawW(&f.eraseDc, L"x", 1, &r, 0) == 41);
                CHECK(HintTest::Count(Kind::TextW) == 1 && HintTest::Count(Kind::Alpha) == 0); Take();
                Hint::EndPaint();
            }
            CHECK(Hint::BeginPaint(&f.hint, WM_PAINT, nullptr));
            CHECK(Hint::BeginPaint(&f.hint, WM_ERASEBKGND, &f.eraseDc)); Take();
            CHECK(f.DrawW(&f.eraseDc, L"x", 1, &r, 0) == 41 && HintTest::Count(Kind::Alpha) == 0); Take();
            Hint::EndPaint();
            // A shutdown that another thread began while this paint is open: nothing more is drawn here.
            Hint::g_shutdown.store(true);
            CHECK(f.DrawW(&f.eraseDc, L"x", 1, &r, 0) == 41 && HintTest::Count(Kind::Alpha) == 0); Take();
            Hint::g_shutdown.store(false);
            CHECK(f.DrawW(&f.eraseDc, L"x", 1, &r, 0) == 15 && HintTest::Count(Kind::Alpha) == 1);
            Hint::EndPaint();
            CHECK(Hint::g_sessions.empty());
        }
    }

    // The frame VCL draws around the window.
    void Frame()
    {
        // Any width of it; a window without one has all of itself excluded.
        for (LONG frame : {LONG{1}, LONG{3}, LONG{0}})
        {
            Fixture f; f.hint.frame = frame;
            CHECK(f.Send(&f.hint, WM_NCPAINT, 1) == 0);
            const auto e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::Black && e[0].dc != &f.eraseDc && Same(e[0].rect, kWindow));
            CHECK(e[0].excluded.size() == 1 && Same(e[0].excluded[0], RECT{frame, frame, 115 - frame, 21 - frame}));
            CHECK(HintTest::dcsTaken == 1 && HintTest::dcsReleased == 1 && ends == 1);
        }
        // The update region the message carries is not a DC: none is passed on.
        {
            Fixture f;
            begunWith = &f.foreign;
            CHECK(f.Send(&f.hint, WM_NCPAINT, reinterpret_cast<WPARAM>(&f.memory)) == 0);
            const auto e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::Black && e[0].dc != &f.memory && begins == 1 && !begunWith);
            // The erase and a print bring one.
            CHECK(f.Send(&f.hint, WM_ERASEBKGND, reinterpret_cast<WPARAM>(&f.eraseDc)) == 1 && begunWith == &f.eraseDc);
            CHECK(f.Send(&f.hint, WM_PAINT, reinterpret_cast<WPARAM>(&f.memory)) == 0 && begunWith == &f.memory);
            CHECK(f.Send(&f.hint, WM_PAINT) == 0 && !begunWith && begins == 4);
            Take();
        }
        // Whatever refuses, the frame is VCL's - and the DC is given back as it was taken.
        for (int which = 0; which < 9; ++which)
        {
            Fixture f; bool taken = true;
            switch (which)
            {
            case 0: HintTest::failRect = &f.hint; taken = false; break;
            case 1: HintTest::failClient = &f.hint; taken = false; break;
            case 2: HintTest::failOrigin = &f.hint; taken = false; break;
            case 3: HintTest::failWindowDc = true; taken = false; break;
            case 4: HintTest::failSave = true; break;
            case 5: HintTest::failExclude = true; break;
            case 6: HintTest::failBlack = true; break;
            case 7: f.hint.screen = RECT{300, 200, 300, 221}; taken = false; break;
            case 8: f.hint.screen = RECT{300, 200, 415, 200}; taken = false; break;
            }
            const int before = HintTest::dcsTaken;
            if (which == 3)
            {
                // VCL cannot draw its edge either; the message is still its own.
                CHECK(!Hint::BeginPaint(&f.hint, WM_NCPAINT, nullptr));
                CHECK(Take().empty() && Hint::g_sessions.empty());
                continue;
            }
            CHECK(f.Send(&f.hint, WM_NCPAINT, 1) == 0);
            const auto e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::Edge && begins == 1 && ends == 0 && Hint::g_sessions.empty());
            // One DC for VCL's edge, and one more where the unit got as far as taking it.
            CHECK(HintTest::dcsTaken - before == (taken ? 2 : 1) && HintTest::dcsTaken == HintTest::dcsReleased);
        }
    }

    // The answer about the animation of tooltips.
    void Animation()
    {
        for (bool unicode : {true, false})
        {
            Fixture f(unicode);
            // Nothing is hooked before a hint window is attached.
            CHECK(f.Animates() && f.exe.hooks.empty());
            Hint::OnAttached(&f.hint);
            CHECK(!f.Animates());
            // Only that question.
            BOOL fade = 7;
            CHECK(f.Parameters(SPI_GETTOOLTIPFADE, &fade) == TRUE && fade == TRUE);
            BOOL menu = 7;
            CHECK(f.Parameters(SPI_GETMENUANIMATION, &menu) == TRUE && menu == 7);
            // Only a call that succeeded, with somewhere to answer.
            HintTest::parametersResult = FALSE;
            BOOL animate = 7;
            CHECK(f.Parameters(SPI_GETTOOLTIPANIMATION, &animate) == FALSE && animate == 7 && GetLastError() == 0xc103);
            HintTest::parametersResult = TRUE;
            CHECK(f.Parameters(SPI_GETTOOLTIPANIMATION, nullptr) == TRUE);
            // Tooltips are not animated at all: nothing to change.
            HintTest::animation = FALSE; CHECK(!f.Animates()); HintTest::animation = TRUE;
            // The caller's last error is what the system left.
            SetLastError(0x77); CHECK(!f.Animates() && GetLastError() == 0xc103);
            // The window is gone, is not a hint any more, or Glass tooltips are off.
            f.hint.valid = false; CHECK(f.Animates()); f.hint.valid = true;
            f.hint.cls = L"TForm"; CHECK(f.Animates()); f.hint.cls = L"THintWindow";
            HintTest::disabled = true; CHECK(f.Animates()); HintTest::disabled = false;
            // Another thread has no hint window of its own.
            Hint::g_hint = nullptr; CHECK(f.Animates());
            // The window that was attached last is the one asked about.
            TestWindow second; second.instance = &f.exe;
            Hint::OnAttached(&second); CHECK(Hint::g_hint == &second && !f.Animates());
            second.valid = false; CHECK(f.Animates());
            Hint::OnAttached(&f.hint); CHECK(Hint::g_hint == &f.hint && !f.Animates());
            // A window that is not such a hint does not replace it.
            Hint::OnAttached(&f.form); Hint::OnAttached(nullptr); CHECK(Hint::g_hint == &f.hint);
            CHECK(Take().empty());
        }
        // The EXE has no DrawText to draw a caption through: nothing is answered at all.
        for (bool unicode : {true, false})
        {
            Fixture f(unicode); f.exe.originals.erase(unicode ? "DrawTextW" : "DrawTextA");
            Hint::OnAttached(&f.hint);
            CHECK(f.exe.hooks.empty() && Hint::g_armed.load() && !Hint::g_text.load());
            CHECK(f.Animates());
            CHECK(f.Send(&f.hint, WM_ERASEBKGND, reinterpret_cast<WPARAM>(&f.eraseDc)) == 1);
            CHECK(f.Send(&f.hint, WM_NCPAINT, 1) == 0);
            CHECK(HintTest::Count(Kind::Fill) == 1 && HintTest::Count(Kind::Edge) == 1 && HintTest::Count(Kind::Black) == 0);
            CHECK(ends == 0);
        }
        // No such question in the EXE: the hint is painted here all the same.
        {
            Fixture f; f.exe.originals.erase("SystemParametersInfoW");
            Hint::OnAttached(&f.hint);
            CHECK(f.exe.hooks.size() == 1 && f.Animates());
            CHECK(f.Send(&f.hint, WM_PAINT) == 0);
            CHECK(HintTest::Count(Kind::Alpha) == 1);
        }
    }

    // The imports: taken once when the first hint window is attached, or when
    // the first message of one arrives; held until Shutdown.
    void Lifecycle()
    {
        {
            Fixture f;
            CHECK(f.exe.hooks.empty() && !Hint::g_armed.load() && Hint::g_dispatcher.cached == 1);
            // A window that is not a hint takes nothing.
            Hint::OnAttached(&f.form); Hint::OnAttached(nullptr);
            CHECK(f.exe.hooks.empty() && !Hint::g_armed.load() && !Hint::g_hint);
            Hint::OnAttached(&f.hint);
            const int locks = HintTest::exclusiveLocks;
            for (int i = 0; i < 5; ++i)
            {
                Hint::OnAttached(&f.hint);
                CHECK(f.Send(&f.hint, WM_NCPAINT, 1) == 0);
                CHECK(f.Send(&f.hint, WM_ERASEBKGND, reinterpret_cast<WPARAM>(&f.eraseDc)) == 1);
                CHECK(f.Send(&f.hint, WM_PAINT) == 0);
                CHECK(!f.Animates());
            }
            CHECK(f.Ref("DrawTextW") == 1 && f.Ref("SystemParametersInfoW") == 1);
            // Once armed, nothing takes the exclusive lock again.
            CHECK(HintTest::exclusiveLocks == locks && HintTest::Count(Kind::Alpha) == 5);
            Take();
            // Shutdown gives everything back; what arrives afterwards is VCL's.
            Hint::Shutdown();
            CHECK(f.exe.hooks.empty() && f.Ref("DrawTextW") == 0 && f.Ref("SystemParametersInfoW") == 0);
            CHECK(!Hint::g_armed.load() && !Hint::g_text.load() && !Hint::g_hint);
            const int afterShutdown = HintTest::exclusiveLocks;
            Hint::OnAttached(&f.hint);
            CHECK(!Hint::g_hint && HintTest::exclusiveLocks == afterShutdown);
            CHECK(f.Send(&f.hint, WM_PAINT) == 0);
            auto e = Take();
            CHECK(e.size() == 2 && f.NativeCaption(e[1], &f.paintDc) && f.exe.hooks.empty() && f.Animates());
            Hint::Shutdown(); Hint::Shutdown();
            // A restart arms again, and a second Startup is nothing.
            Hint::Startup(); Hint::Startup();
            CHECK(Hint::g_dispatcher.cached == 2 && f.exe.hooks.empty());
            // The window was attached before the restart: its first message arms.
            CHECK(f.Send(&f.hint, WM_PAINT) == 0);
            e = Take();
            CHECK(e.size() == 3 && e[2].kind == Kind::Alpha && f.Ref("DrawTextW") == 1);
            // It is this thread's hint only once it is attached again.
            CHECK(f.Animates());
            Hint::OnAttached(&f.hint); CHECK(!f.Animates());
        }
        // An EXE that imports both kinds has both taken.
        {
            Fixture f;
            f.exe.originals["DrawTextA"] = reinterpret_cast<void*>(NativeDrawTextA);
            f.exe.originals["SystemParametersInfoA"] = reinterpret_cast<void*>(NativeParameters);
            Hint::OnAttached(&f.hint);
            CHECK(f.exe.hooks.size() == 4 && f.Ref("DrawTextA") == 1 && f.Ref("SystemParametersInfoA") == 1);
        }
        // A slot that cannot be taken: the other kind of text is enough; without any, nothing.
        {
            Fixture f;
            f.exe.originals["DrawTextA"] = reinterpret_cast<void*>(NativeDrawTextA);
            HintTest::registrationFailures[&f.exe].insert("DrawTextW");
            Hint::OnAttached(&f.hint);
            CHECK(Hint::g_text.load() && f.Ref("DrawTextW") == 0 && f.Ref("DrawTextA") == 1 && !f.Animates());
        }
        {
            Fixture f;
            HintTest::registrationFailures[&f.exe].insert("DrawTextW");
            Hint::OnAttached(&f.hint);
            CHECK(Hint::g_armed.load() && !Hint::g_text.load() && f.exe.hooks.empty() && f.Animates());
            // Asked once: a later attach does not try again.
            HintTest::registrationFailures.clear();
            Hint::OnAttached(&f.hint);
            CHECK(f.exe.hooks.empty());
        }
        // No EXE is known yet: not armed, and asked again next time.
        {
            Fixture f; GlassCore::ModuleHandler::g_entryPointModule = nullptr;
            Hint::OnAttached(&f.hint);
            CHECK(!Hint::g_armed.load() && f.exe.hooks.empty());
            GlassCore::ModuleHandler::g_entryPointModule = &f.exe;
            Hint::OnAttached(&f.hint);
            CHECK(Hint::g_armed.load() && f.Ref("DrawTextW") == 1);
        }
        // The EXE became known only after Startup: its table is the one taken from.
        {
            Fixture f;
            Hint::Shutdown();
            GlassCore::ModuleHandler::g_entryPointModule = nullptr;
            const int cached = Hint::g_dispatcher.cached;
            Hint::Startup();
            CHECK(Hint::g_dispatcher.cached == cached && !Hint::g_shutdown.load());
            f.otherModule.originals = {{"DrawTextW", reinterpret_cast<void*>(NativeDrawTextW)},
                                       {"SystemParametersInfoW", reinterpret_cast<void*>(NativeParameters)}};
            GlassCore::ModuleHandler::g_entryPointModule = &f.otherModule;
            f.hint.instance = &f.otherModule;
            Hint::OnAttached(&f.hint);
            CHECK(f.otherModule.hooks.size() == 2 && f.exe.hooks.empty() && Hint::g_text.load());
        }
        // A paint is open when Shutdown comes on its thread: the session is dropped, its end is nothing.
        {
            Fixture f;
            CHECK(Hint::BeginPaint(&f.hint, WM_PAINT, nullptr) && Hint::g_sessions.size() == 1);
            Hint::Shutdown();
            CHECK(Hint::g_sessions.empty());
            HintTest::logs.clear();
            Hint::EndPaint();
            CHECK(Hint::g_sessions.empty() && HintTest::logs.empty());
        }
        // Two threads meet at the first hint. The other one armed while this one waited for the lock:
        // nothing is taken twice.
        {
            Fixture f;
            HintTest::beforeExclusive = [&] { Hint::OnAttached(&f.hint); CHECK(Hint::g_armed.load() && f.Ref("DrawTextW") == 1); };
            Hint::OnAttached(&f.hint);
            CHECK(!HintTest::beforeExclusive && f.Ref("DrawTextW") == 1 && f.Ref("SystemParametersInfoW") == 1);
            CHECK(Hint::g_text.load() && !f.Animates());
        }
        // The other one shut the unit down while this one waited: nothing is taken after a shutdown.
        {
            Fixture f;
            HintTest::beforeExclusive = [&] { Hint::Shutdown(); };
            Hint::OnAttached(&f.hint);
            CHECK(!HintTest::beforeExclusive && f.exe.hooks.empty() && !Hint::g_armed.load() && !Hint::g_text.load());
            // ...and a message that asked is VCL's.
            Hint::Startup();
            CHECK(!Hint::g_shutdown.load());
            HintTest::beforeExclusive = [&] { Hint::Shutdown(); };
            CHECK(f.Send(&f.hint, WM_PAINT) == 0);
            const auto e = Take();
            CHECK(e.size() == 2 && f.NativeCaption(e[1], &f.paintDc) && f.exe.hooks.empty() && ends == 0);
        }
        // Sessions pair by order; an end without a begin is nothing.
        {
            Fixture f;
            Hint::EndPaint();
            CHECK(Hint::BeginPaint(&f.hint, WM_PAINT, nullptr) && Hint::g_sessions.size() == 1);
            TestWindow second; second.instance = &f.exe;
            CHECK(Hint::BeginPaint(&second, WM_PAINT, nullptr) && Hint::g_sessions.size() == 2);
            CHECK(Hint::g_sessions.back().hwnd == &second && Hint::g_sessions.front().hwnd == &f.hint);
            // The caption of the window on top only.
            RECT r = kCaption; TestDC secondDc; secondDc.owner = &second;
            CHECK(f.DrawW(&f.eraseDc, L"x", 1, &r, 0) == 41 && f.DrawW(&secondDc, L"x", 1, &r, 0) == 15);
            CHECK(Hint::g_sessions.back().texts == 1 && Hint::g_sessions.back().cleared && !Hint::g_sessions.front().cleared);
            Hint::EndPaint();
            CHECK(f.DrawW(&f.eraseDc, L"x", 1, &r, 0) == 15 && Hint::g_sessions.back().texts == 1);
            Hint::EndPaint(); Hint::EndPaint();
            CHECK(Hint::g_sessions.empty());
            Take();
        }
    }

    // The compositor or GDI refuses.
    void Failures()
    {
        // The erase cannot be laid: VCL erases.
        {
            Fixture f; HintTest::failBlack = true;
            CHECK(f.Send(&f.hint, WM_ERASEBKGND, reinterpret_cast<WPARAM>(&f.eraseDc)) == 1);
            const auto e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::Fill && ends == 0);
        }
        // The compositor fails or throws: the caption is VCL's own call, once, with its arguments;
        // the DC is as VCL left it.
        for (int which = 0; which < 4; ++which)
        {
            Fixture f(which < 2);       // a wide caption and an ANSI one
            HintTest::alphaResult = E_FAIL; HintTest::alphaThrows = (which & 1) != 0; f.vclBkMode = OPAQUE;
            CHECK(f.Send(&f.hint, WM_PAINT) == 0);
            const auto e = Take();
            CHECK(e.size() == 4 && e[1].kind == Kind::Black && e[2].kind == Kind::Alpha && f.NativeCaption(e[3], &f.paintDc));
            CHECK(e[3].bkMode == OPAQUE && f.paintDc.saved.empty() && !Hint::g_inside);
            CHECK(Same(f.textRect, kCaption) && ends == 1);
        }
        // The clear fails: the caption is still composed, and the next one tries the clear again.
        {
            Fixture f; RECT r = kCaption;
            CHECK(Hint::BeginPaint(&f.hint, WM_PAINT, nullptr));
            HintTest::failBlack = true;
            CHECK(f.DrawW(&f.eraseDc, L"x", 1, &r, 0) == 15);
            CHECK(!Hint::g_sessions.back().cleared && Hint::g_sessions.back().texts == 1);
            HintTest::failBlack = false;
            CHECK(f.DrawW(&f.eraseDc, L"y", 1, &r, 0) == 15);
            auto e = Take();
            CHECK(e.size() == 3 && e[0].kind == Kind::Alpha && e[1].kind == Kind::Black && e[2].kind == Kind::Alpha);
            // Once cleared, not again in this paint.
            CHECK(f.DrawW(&f.eraseDc, L"z", 1, &r, 0) == 15);
            e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::Alpha && Hint::g_sessions.back().texts == 3);
            Hint::EndPaint();
        }
        // The compositor draws text of its own through the same import: that call is not a caption.
        {
            Fixture f; RECT inner{4, 4, 40, 12};
            HintTest::duringAlpha = [&] {
                HintTest::duringAlpha = {};
                CHECK(Hint::g_inside);
                SetLastError(0x66);
                CHECK(f.DrawW(&f.paintDc, L"inner", 5, &inner, 0) == 41);
            };
            CHECK(f.Send(&f.hint, WM_PAINT) == 0);
            const auto e = Take();
            CHECK(e.size() == 4 && e[2].kind == Kind::Alpha && e[3].kind == Kind::TextW && e[3].text == L"inner");
            CHECK(e[3].entryError == 0x66 && f.textResult == 15 && !Hint::g_inside);
        }
        // No original to forward to (the hook outlived its dispatcher): the function itself, by name.
        {
            Fixture f; RECT r = kCaption;
            Hint::OnAttached(&f.hint);
            const auto hookW = f.exe.hooks.at("DrawTextW"), hookP = f.exe.hooks.at("SystemParametersInfoW");
            Hint::Shutdown();
            using TextFn = int (WINAPI*)(HDC, LPCWSTR, int, LPRECT, UINT);
            using ParamFn = BOOL (WINAPI*)(UINT, UINT, PVOID, UINT);
            CHECK(reinterpret_cast<TextFn>(hookW)(&f.eraseDc, L"x", 1, &r, 0) == 43);
            BOOL animate = 7;
            CHECK(reinterpret_cast<ParamFn>(hookP)(SPI_GETTOOLTIPANIMATION, 0, &animate, 0) == TRUE && animate == TRUE);
            CHECK(HintTest::directParametersW == 1 && HintTest::directParametersA == 0);
            const auto e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::DirectTextW);
        }
        {
            Fixture f(false); RECT r = kCaption;
            Hint::OnAttached(&f.hint);
            const auto hookA = f.exe.hooks.at("DrawTextA"), hookP = f.exe.hooks.at("SystemParametersInfoA");
            Hint::Shutdown();
            using TextFn = int (WINAPI*)(HDC, LPCSTR, int, LPRECT, UINT);
            using ParamFn = BOOL (WINAPI*)(UINT, UINT, PVOID, UINT);
            CHECK(reinterpret_cast<TextFn>(hookA)(&f.eraseDc, "x", 1, &r, 0) == 44);
            BOOL animate = 7;
            CHECK(reinterpret_cast<ParamFn>(hookP)(SPI_GETTOOLTIPANIMATION, 0, &animate, 0) == TRUE && animate == TRUE);
            CHECK(HintTest::directParametersW == 0 && HintTest::directParametersA == 1);
            const auto e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::DirectTextA);
        }
    }

    // The bridge of WndCompat: optional, paired, and it passes on what it was given.
    void Bridge()
    {
        LRESULT result = 99;
        // No GlassCore, an export missing, WndCompat shutting down: the message is VCL's.
        for (int which = 0; which < 4; ++which)
        {
            Fixture f;
            switch (which)
            {
            case 0: HintTest::core = nullptr; break;
            case 1: HintTest::beginExport = nullptr; break;
            case 2: HintTest::endExport = nullptr; break;
            case 3: HintTest::shuttingDown = true; break;
            }
            for (UINT message : {WM_NCPAINT, WM_ERASEBKGND, WM_PAINT})
            {
                result = 99;
                CHECK(!WndCompat::TryHandleDelphiHintPaint(&f.hint, message, reinterpret_cast<WPARAM>(&f.eraseDc), 0, result));
                CHECK(result == 99);
            }
            CHECK(begins == 0 && ends == 0 && Take().empty() && Hint::g_sessions.empty());
        }
        // Only the three messages reach GlassCore.
        {
            Fixture f;
            for (UINT message : {WM_PRINT, WM_PRINTCLIENT, WM_SETTEXT})
                CHECK(!WndCompat::TryHandleDelphiHintPaint(&f.hint, message, 0, 0, result));
            CHECK(begins == 0);
        }
        // What it answers, and that every begin has its end - also when VCL's paint sends more messages.
        {
            Fixture f;
            result = 99;
            CHECK(WndCompat::TryHandleDelphiHintPaint(&f.hint, WM_ERASEBKGND, reinterpret_cast<WPARAM>(&f.eraseDc), 0, result) && result == 1);
            CHECK(WndCompat::TryHandleDelphiHintPaint(&f.hint, WM_NCPAINT, 1, 0, result) && result == 0);
            CHECK(begins == 2 && ends == 2);
            f.hint.erasePending = true;
            f.duringPaint = [&] {
                // A frame repainted from inside the paint: its own begin and end, inside the paint's.
                CHECK(Hint::g_sessions.size() == 1 && ends == 3);       // the erase BeginPaint sent is over
                CHECK(f.Send(&f.hint, WM_NCPAINT, 1) == 0);
                CHECK(Hint::g_sessions.size() == 1 && ends == 4);
            };
            result = 99;
            CHECK(WndCompat::TryHandleDelphiHintPaint(&f.hint, WM_PAINT, 0, 0, result) && result == 0);
            CHECK(begins == 5 && ends == 5 && Hint::g_sessions.empty());
            const auto e = Take();
            CHECK(HintTest::Count(Kind::Fill) == 0 && HintTest::Count(Kind::Edge) == 0);
            CHECK(e.back().kind == Kind::Alpha && f.textResult == 15);
        }
        // GlassCore declines: no end, and the message goes on.
        {
            Fixture f; f.hint.cls = L"TForm";
            CHECK(!WndCompat::TryHandleDelphiHintPaint(&f.hint, WM_PAINT, 0, 0, result));
            CHECK(begins == 1 && ends == 0 && Take().empty());
        }
    }

    // What a Debug build writes; a release build writes nothing.
    void Diagnostics()
    {
        {
            Fixture f;
            Hint::OnAttached(&f.hint);
            CHECK(f.Send(&f.hint, WM_NCPAINT, 1) == 0);
            CHECK(f.Send(&f.hint, WM_ERASEBKGND, reinterpret_cast<WPARAM>(&f.eraseDc)) == 1);
            CHECK(f.Send(&f.hint, WM_PAINT) == 0);
#if defined(_DEBUG)
            CHECK(HintTest::logs.size() == 2);
            CHECK(HintTest::logs[0].format == L"[GlassCore][DelphiHint] armed exe=0x%p textW=%d textA=%d parameters=%d");
            CHECK((HintTest::logs[0].numbers == std::vector<long long>{Number(&f.exe), 1, 0, 1}));
            CHECK(HintTest::logs[1].format == L"[GlassCore][DelphiHint] paint hwnd=0x%p cleared=%d texts=%u");
            CHECK((HintTest::logs[1].numbers == std::vector<long long>{Number(&f.hint), 1, 1}));
            // A paint VCL drew nothing in.
            HintTest::logs.clear();
            CHECK(Hint::BeginPaint(&f.hint, WM_PAINT, nullptr)); Hint::EndPaint();
            CHECK(HintTest::logs.size() == 1 && (HintTest::logs[0].numbers == std::vector<long long>{Number(&f.hint), 0, 0}));
            // Eight a second.
            HintTest::tick += 1000; HintTest::logs.clear();
            for (int i = 0; i < 12; ++i) { CHECK(Hint::BeginPaint(&f.hint, WM_PAINT, nullptr)); Hint::EndPaint(); }
            CHECK(HintTest::logs.size() == 8);
            HintTest::tick += 999;
            CHECK(Hint::BeginPaint(&f.hint, WM_PAINT, nullptr)); Hint::EndPaint();
            CHECK(HintTest::logs.size() == 8);
            HintTest::tick += 1;
            CHECK(Hint::BeginPaint(&f.hint, WM_PAINT, nullptr)); Hint::EndPaint();
            CHECK(HintTest::logs.size() == 9);
#else
            CHECK(HintTest::logs.empty());
#endif
            Take();
        }
        // The line about the imports says which were taken.
        for (int which = 0; which < 3; ++which)
        {
            Fixture other(which != 0);      // 0: an ANSI EXE; 1: no question about the animation; 2: no DrawText at all
            if (which == 1) other.exe.originals.erase("SystemParametersInfoW");
            if (which == 2) other.exe.originals.erase("DrawTextW");
            Hint::OnAttached(&other.hint);
#if defined(_DEBUG)
            const std::vector<long long> expected = which == 0 ? std::vector<long long>{Number(&other.exe), 0, 1, 1}
                : which == 1 ? std::vector<long long>{Number(&other.exe), 1, 0, 0}
                : std::vector<long long>{Number(&other.exe), 0, 0, 0};
            CHECK(HintTest::logs.size() == 1 && HintTest::logs[0].numbers == expected);
#else
            CHECK(HintTest::logs.empty());
#endif
        }
    }
}

int main(int argc, char** argv)
{
    try
    {
        if (argc != 2) throw std::runtime_error("one case required");
        const std::string name = argv[1];
        if (name == "shown") Shown();
        else if (name == "ansi") Ansi();
        else if (name == "gates") Gates();
        else if (name == "frame") Frame();
        else if (name == "animation") Animation();
        else if (name == "lifecycle") Lifecycle();
        else if (name == "failures") Failures();
        else if (name == "bridge") Bridge();
        else if (name == "diagnostics") Diagnostics();
        else throw std::runtime_error("unknown case");
        std::cout << "PASS " << name << '\n';
    }
    catch (const std::exception& e) { std::cerr << argv[1] << ": " << e.what() << '\n'; return 1; }
}
