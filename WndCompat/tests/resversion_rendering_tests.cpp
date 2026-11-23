#include "resversion_test_shim.h"
#include "../../GlassCore/src/Hooks/delphi/TResVersionEditorDraw.cpp"

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)

namespace
{
    namespace Rv = GlassCore::DelphiResVersionEditor;
    using ResTest::Event;
    using ResTest::Kind;
    void* const kWide = reinterpret_cast<void*>(&ExtTextOutW); // what the renderer must draw with

    void ResetFaults()
    {
        using namespace ResTest;
        nativeTextResult = TRUE; nativeFillResult = 1; nativeLeavesError = false;
        renderResult = S_OK; throwRenderer = failBlack = failBrush = failFill = failObject = false;
        failTranslate = failConvert = partialConvert = throwConvert = false;
        duringRender = {}; duringNative = {};
        windowColor = 0xffffff; faceColor = 0xf0f0f0;
    }
    std::vector<Event> Take() { return std::exchange(ResTest::events, {}); }
    bool Same(const RECT& a, const RECT& b)
    { return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom; }
    long long Number(const void* p) { return static_cast<long long>(reinterpret_cast<std::intptr_t>(p)); }
    std::vector<ResTest::Log> Logs(const wchar_t* part)
    {
        std::vector<ResTest::Log> found;
        for (const auto& log : ResTest::logs)
            if (log.format.find(part) != std::wstring::npos) found.push_back(log);
        return found;
    }
    // The reason is the last string of a `text` or `fill` line.
    std::wstring LastReason(const wchar_t* part)
    {
        const auto lines = Logs(part);
        CHECK(!lines.empty() && !lines.back().strings.empty());
        return lines.back().strings.back();
    }

    struct Fixture
    {
        TestModule exe, otherModule;
        TestWindow editor, editor2, other;
        TestDC paintDc, canvasDc, paintDc2, memoryDc, foreignDc;
        TestFont font{7};
        LOGBRUSH selected{BS_SOLID, RGB(9, 8, 7)};
        LOGBRUSH window{BS_SOLID, 0xffffff}, face{BS_SOLID, 0xf0f0f0}, black{BS_SOLID, 0};
        LOGBRUSH highlight{BS_SOLID, RGB(0, 120, 215)}, hatch{BS_HATCHED, 0xffffff};
        // One cell of the editor, as TCustomGrid paints it: FillRect, then TextRect.
        const RECT cell{65, 20, 265, 38};
        const char* value = "3.70.0.1709";

        Fixture()
        {
            using namespace ResTest;
            ResetFaults();
            extra.clear(); instances.clear(); objectTypes.clear(); events.clear(); codepage = 0; conversions = 0;
            static ULONGLONG epoch = 0;
            ResTest::clock = epoch += 1000000; // a fresh rate-limit window for every fixture
            logs.clear();
            Test::full = true; Test::stackOwners.clear(); Test::topWindow = Test::host = nullptr;
            exe.originals["ExtTextOutA"] = reinterpret_cast<void*>(&ResNativeExtTextOutA);
            exe.originals["FillRect"] = reinterpret_cast<void*>(&ResNativeFillRect);
            GlassCore::ModuleHandler::g_entryPointModule = &exe;
            GlassCore::PaintDispatcher::resProviders.clear();
            editor.cls = editor2.cls = L"TResVersionEditor"; other.cls = L"TPanel";
            paintDc.owner = canvasDc.owner = &editor; paintDc2.owner = &editor2; foreignDc.owner = &other;
            for (TestDC* dc : {&paintDc, &canvasDc, &paintDc2, &memoryDc, &foreignDc})
            {
                dc->brush = &selected; dc->font = &font; dc->text = RGB(0, 0, 128);
                dc->background = 0xffffff; dc->bkMode = OPAQUE; // VCL: Brush.Color = clWindow
            }
            // What BeginPaint returned for each window; other DCs are tracked by the tests.
            Test::stackOwners[&paintDc] = &editor; Test::stackOwners[&paintDc2] = &editor2;
            Rv::Startup();
            CHECK(GlassCore::PaintDispatcher::resProviders.size() == 1 && Idle());
        }
        ~Fixture()
        {
            ResetFaults();
            Rv::Shutdown();
            assert(exe.hooks.empty() && Rv::g_sessions.empty() && !Rv::g_inside);
        }
        int Refs(const char* name) { return exe.refs[name]; }
        bool Idle() { return exe.hooks.empty() && !Refs("ExtTextOutA") && !Refs("FillRect") && Rv::g_sessions.empty() && !Rv::g_inside; }
        bool Hooked() { return exe.hooks.size() == 2 && Refs("ExtTextOutA") == Refs("FillRect") && Refs("FillRect") > 0; }

        auto& Provider()
        {
            CHECK(GlassCore::PaintDispatcher::resProviders.size() == 1);
            return GlassCore::PaintDispatcher::resProviders.front();
        }
        void Begin(TestWindow& w, HMODULE module = nullptr)
        { Test::topWindow = &w; Provider().onBegin(&w, module ? module : &exe); }
        void End(TestWindow& w) { Provider().onEnd(&w, &exe); }
        // The EXE's calls through its import table. Every call opens a new
        // rate-limit window, so that each one is logged.
        BOOL Text(HDC dc, int x, int y, UINT options, const RECT* rect, LPCSTR text, UINT count, const INT* dx = nullptr)
        {
            ResTest::clock += 1000; SetLastError(0x29);
            return ExeExtTextOutA(exe, dc, x, y, options, rect, text, count, dx);
        }
        int Fill(HDC dc, const RECT* rect, HBRUSH brush)
        {
            ResTest::clock += 1000; SetLastError(0x29);
            return ExeFillRect(exe, dc, rect, brush);
        }
        BOOL Cell(HDC dc = nullptr)
        { return Text(dc ? dc : &paintDc, 67, 22, ETO_OPAQUE | ETO_CLIPPED, &cell, value, 11); }

        // The caller's DC is as it was: nothing selected, saved or switched.
        void Unchanged(TestDC& dc, COLORREF background = 0xffffff, int mode = OPAQUE)
        {
            CHECK(dc.brush == &selected && dc.font == &font && dc.text == RGB(0, 0, 128));
            CHECK(dc.background == background && dc.bkMode == mode && dc.saved.empty());
            CHECK(!Rv::g_inside);
        }
        // Exactly one call of the true function with the caller's own arguments.
        void NativeText(HDC dc, int x, int y, UINT options, const RECT* rect, LPCSTR text, int count, const INT* dx = nullptr)
        {
            const auto e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::NativeText && e[0].dc == dc && e[0].x == x && e[0].y == y);
            CHECK(e[0].options == options && e[0].rectPointer == rect && e[0].ansi == text && e[0].count == count);
            CHECK(e[0].dxPointer == dx && e[0].entryError == 0x29 && GetLastError() == 0x51);
        }
        void NativeFill(HDC dc, const RECT* rect, HBRUSH brush)
        {
            const auto e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::NativeFill && e[0].dc == dc && e[0].rectPointer == rect);
            CHECK(e[0].brush == brush && e[0].entryError == 0x29 && GetLastError() == 0x52);
        }
    };

    // A cell of the grid: VCL fills it with the control's colour and writes the
    // text clipped to it; a selected cell has the highlight colour behind it.
    void Cells()
    {
        Fixture f;
        f.Begin(f.editor);
        CHECK(f.Hooked() && f.Refs("FillRect") == 1);
        // Canvas.FillRect(cell) with the brush of clWindow: cleared, not white.
        CHECK(f.Fill(&f.paintDc, &f.cell, &f.window) == 1);
        auto e = Take();
        CHECK(e.size() == 1 && e[0].kind == Kind::Black && e[0].dc == &f.paintDc && Same(e[0].rect, f.cell));
        CHECK(GetLastError() == 0x29 && LastReason(L"] fill ") == L"none");
        f.Unchanged(f.paintDc);
        // Canvas.TextRect: background first, then the glyphs at the caller's
        // position with the caller's clipping; the opaque fill is not asked of
        // the renderer again.
        CHECK(f.Cell() == TRUE);
        e = Take();
        CHECK(e.size() == 2 && e[0].kind == Kind::Black && e[0].dc == &f.paintDc && Same(e[0].rect, f.cell));
        CHECK(e[1].kind == Kind::Glyphs && e[1].dc == &f.paintDc && e[1].x == 67 && e[1].y == 22);
        CHECK(e[1].options == ETO_CLIPPED && e[1].rectPointer == &f.cell && e[1].text == L"3.70.0.1709");
        CHECK(!e[1].dxPointer && e[1].original == kWide);
        CHECK(e[1].state.text == RGB(0, 0, 128) && e[1].state.font == &f.font);
        CHECK(GetLastError() == 0x29 && LastReason(L"] text ") == L"none");
        f.Unchanged(f.paintDc);
        // Root may already have turned the DC's background black: the same.
        f.paintDc.background = 0;
        CHECK(f.Cell() == TRUE);
        e = Take();
        CHECK(e.size() == 2 && e[0].kind == Kind::Black && e[1].kind == Kind::Glyphs);
        // The fixed column: COLOR_BTNFACE.
        f.paintDc.background = 0xf0f0f0;
        CHECK(f.Fill(&f.paintDc, &f.cell, &f.face) == 1 && f.Cell() == TRUE);
        e = Take();
        CHECK(e.size() == 3 && e[0].kind == Kind::Black && e[1].kind == Kind::Black && e[2].kind == Kind::Glyphs);
        // A selected cell: its fill is the application's, and the text call
        // repaints the cell fully opaque before the glyphs.
        f.paintDc.background = RGB(0, 120, 215);
        CHECK(f.Fill(&f.paintDc, &f.cell, &f.highlight) == 1);
        f.NativeFill(&f.paintDc, &f.cell, &f.highlight);
        CHECK(f.Cell() == TRUE);
        e = Take();
        CHECK(e.size() == 2 && e[0].kind == Kind::Fill && e[0].color == RGB(0, 120, 215) && e[0].alpha == 255);
        CHECK(Same(e[0].rect, f.cell) && e[1].kind == Kind::Glyphs && e[1].options == ETO_CLIPPED);
        f.Unchanged(f.paintDc, RGB(0, 120, 215));
        f.paintDc.background = 0xffffff;
        // Canvas.TextOut: no rectangle, no options.
        CHECK(f.Text(&f.paintDc, 4, 2, 0, nullptr, "FileVersion", 11) == TRUE);
        e = Take();
        CHECK(e.size() == 1 && e[0].kind == Kind::Glyphs && e[0].x == 4 && e[0].y == 2 && e[0].options == 0);
        CHECK(!e[0].rectPointer && e[0].text == L"FileVersion");
        // Clipped without a background (Brush.Style = bsClear).
        CHECK(f.Text(&f.paintDc, 67, 22, ETO_CLIPPED, &f.cell, f.value, 11) == TRUE);
        e = Take();
        CHECK(e.size() == 1 && e[0].kind == Kind::Glyphs && e[0].options == ETO_CLIPPED && e[0].rectPointer == &f.cell);
        // An empty cell is only its background.
        CHECK(f.Text(&f.paintDc, 67, 22, ETO_OPAQUE | ETO_CLIPPED, &f.cell, "", 0) == TRUE);
        e = Take();
        CHECK(e.size() == 1 && e[0].kind == Kind::Black && Same(e[0].rect, f.cell));
        CHECK(f.Text(&f.paintDc, 67, 22, ETO_OPAQUE, &f.cell, nullptr, 0) == TRUE);
        CHECK(Take().size() == 1);
        // Explicit advances are the caller's.
        const INT dx[3]{9, 9, 9};
        CHECK(f.Text(&f.paintDc, 1, 1, 0, nullptr, "abc", 3, dx) == TRUE);
        e = Take();
        CHECK(e.size() == 1 && e[0].dxPointer == dx && e[0].dx == (std::vector<INT>{9, 9, 9}));
        f.End(f.editor);
        CHECK(f.Idle());
        // Outside the paint session the imports are the EXE's own again.
        CHECK(f.Cell() == TRUE);
        e = Take();
        CHECK(e.size() == 1 && e[0].kind == Kind::NativeText);
        CHECK(f.Fill(&f.paintDc, &f.cell, &f.window) == 1);
        e = Take();
        CHECK(e.size() == 1 && e[0].kind == Kind::NativeFill);
    }

    // Only a solid brush of the default window or button-face colour is cleared.
    void Fills()
    {
        Fixture f;
        f.Begin(f.editor);
        const RECT area{0, 0, 300, 200};
        const auto native = [&](HBRUSH brush, const wchar_t* reason) {
            CHECK(f.Fill(&f.paintDc, &area, brush) == 1);
            f.NativeFill(&f.paintDc, &area, brush);
            CHECK(LastReason(L"] fill ") == reason);
            f.Unchanged(f.paintDc);
        };
        const auto cleared = [&](HBRUSH brush) {
            CHECK(f.Fill(&f.paintDc, &area, brush) == 1);
            const auto e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::Black && Same(e[0].rect, area) && GetLastError() == 0x29);
        };
        cleared(&f.window); cleared(&f.face);
        native(&f.black, L"colour");        // black stays black either way: left to the application
        native(&f.highlight, L"colour");
        native(&f.hatch, L"brush");
        native(nullptr, L"brush");
        ResTest::failObject = true; native(&f.window, L"brush"); ResTest::failObject = false;
        // The current system colours, not constants.
        ResTest::windowColor = 0x202020; ResTest::faceColor = 0x303030;
        native(&f.window, L"colour"); native(&f.face, L"colour");
        LOGBRUSH dark{BS_SOLID, 0x202020}, darker{BS_SOLID, 0x303030};
        cleared(&dark); cleared(&darker);
        ResTest::windowColor = 0xffffff; ResTest::faceColor = 0xf0f0f0;
        // Rectangles that are nothing or cannot be cleared.
        CHECK(f.Fill(&f.paintDc, nullptr, &f.window) == 1);
        f.NativeFill(&f.paintDc, nullptr, &f.window);
        CHECK(LastReason(L"] fill ") == L"rect");
        for (const RECT& r : {RECT{10, 10, 10, 40}, RECT{10, 10, 40, 10}, RECT{40, 10, 10, 40},
            RECT{INT_MIN, 0, INT_MAX, 10}, RECT{0, INT_MIN, 10, INT_MAX}})
        {
            CHECK(f.Fill(&f.paintDc, &r, &f.window) == 1);
            f.NativeFill(&f.paintDc, &r, &f.window);
            CHECK(LastReason(L"] fill ") == L"rect");
        }
        // The clear failed: the application's fill runs, once.
        ResTest::failBlack = true; native(&f.window, L"clear"); ResTest::failBlack = false;
        // What the original returns and leaves is what the EXE gets.
        ResTest::nativeFillResult = 0;
        CHECK(f.Fill(&f.paintDc, &area, &f.highlight) == 0 && GetLastError() == 0x52); Take();
        ResTest::nativeFillResult = 1; ResTest::nativeLeavesError = true;
        CHECK(f.Fill(&f.paintDc, &area, &f.highlight) == 1 && GetLastError() == 0x29); Take();
        f.End(f.editor);
        CHECK(f.Idle());
    }

    // Calls this module cannot place exactly are one native call, with the reason logged.
    void TextGates()
    {
        Fixture f;
        f.Begin(f.editor);
        const auto native = [&](UINT options, const RECT* rect, LPCSTR text, UINT count, const wchar_t* reason, const INT* dx = nullptr) {
            CHECK(f.Text(&f.paintDc, 67, 22, options, rect, text, count, dx) == TRUE);
            f.NativeText(&f.paintDc, 67, 22, options, rect, text, static_cast<int>(count), dx);
            CHECK(LastReason(L"] text ") == reason);
            f.Unchanged(f.paintDc);
        };
        native(ETO_GLYPH_INDEX | ETO_CLIPPED, &f.cell, f.value, 11, L"options");
        native(ETO_PDY, nullptr, f.value, 11, L"options");
        native(ETO_OPAQUE, nullptr, f.value, 11, L"rect");
        native(ETO_CLIPPED, nullptr, f.value, 11, L"rect");
        native(0, nullptr, "", 0, L"empty");
        native(ETO_CLIPPED, &f.cell, nullptr, 0, L"empty");
        native(0, nullptr, nullptr, 3, L"conversion");
        for (UINT align : {TA_RIGHT, TA_CENTER, TA_BOTTOM, TA_BASELINE, TA_UPDATECP})
        {
            ResTest::extra[&f.paintDc].align = align;
            native(ETO_OPAQUE | ETO_CLIPPED, &f.cell, f.value, 11, L"align");
            // ...but a background alone does not depend on it.
            CHECK(f.Text(&f.paintDc, 67, 22, ETO_OPAQUE, &f.cell, "", 0) == TRUE);
            const auto e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::Black);
        }
        ResTest::extra[&f.paintDc].align = TA_LEFT | TA_TOP | TA_NOUPDATECP;
        // Character spacing is not carried to the renderer's buffer; explicit advances are.
        ResTest::extra[&f.paintDc].spacing = 2;
        native(0, nullptr, f.value, 11, L"spacing");
        const INT dx[3]{9, 9, 9};
        CHECK(f.Text(&f.paintDc, 1, 1, 0, nullptr, "abc", 3, dx) == TRUE);
        {
            const auto e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::Glyphs && e[0].dxPointer == dx);
        }
        ResTest::extra[&f.paintDc].spacing = 0;
        // Conversion.
        ResTest::extra[&f.paintDc].charset = SYMBOL_CHARSET; native(0, nullptr, f.value, 11, L"conversion");
        ResTest::extra[&f.paintDc].charset = 77; native(0, nullptr, f.value, 11, L"conversion"); // no such charset
        ResTest::extra[&f.paintDc].charset = -1; native(0, nullptr, f.value, 11, L"conversion");
        ResTest::extra[&f.paintDc].charset = DEFAULT_CHARSET;
        ResTest::failConvert = true; native(0, nullptr, f.value, 11, L"conversion"); ResTest::failConvert = false;
        ResTest::partialConvert = true; native(0, nullptr, f.value, 11, L"conversion"); ResTest::partialConvert = false;
        ResTest::throwConvert = true; native(0, nullptr, f.value, 11, L"conversion"); ResTest::throwConvert = false;
        ResTest::extra[&f.paintDc].charset = RUSSIAN_CHARSET; ResTest::failTranslate = true;
        native(0, nullptr, f.value, 11, L"conversion");
        ResTest::failTranslate = false; ResTest::extra[&f.paintDc].charset = DEFAULT_CHARSET;
        // The background could not be drawn: nothing of the call is kept.
        ResTest::failBlack = true;
        native(ETO_OPAQUE | ETO_CLIPPED, &f.cell, f.value, 11, L"background");
        ResTest::failBlack = false;
        f.paintDc.background = RGB(0, 120, 215);
        ResTest::failBrush = true;
        CHECK(f.Cell() == TRUE);
        auto e = Take();
        CHECK(e.size() == 1 && e[0].kind == Kind::NativeText && LastReason(L"] text ") == L"background");
        ResTest::failBrush = false; ResTest::failFill = true;
        CHECK(f.Cell() == TRUE);
        e = Take();
        CHECK(e.size() == 1 && e[0].kind == Kind::NativeText && LastReason(L"] text ") == L"background");
        ResTest::failFill = false; f.paintDc.background = 0xffffff;
        // An overflowing background rectangle is a failure; an empty one is nothing to fill.
        const RECT huge{INT_MIN, 0, INT_MAX, 10};
        CHECK(f.Text(&f.paintDc, 67, 22, ETO_OPAQUE | ETO_CLIPPED, &huge, f.value, 11) == TRUE);
        e = Take();
        CHECK(e.size() == 1 && e[0].kind == Kind::NativeText && LastReason(L"] text ") == L"background");
        const RECT none{10, 10, 10, 30};
        CHECK(f.Text(&f.paintDc, 67, 22, ETO_OPAQUE | ETO_CLIPPED, &none, f.value, 11) == TRUE);
        e = Take();
        CHECK(e.size() == 1 && e[0].kind == Kind::Glyphs && e[0].rectPointer == &none);
        // The renderer failed or threw after the background: the original repaints the call.
        ResTest::renderResult = E_FAIL;
        CHECK(f.Cell() == TRUE);
        e = Take();
        CHECK(e.size() == 3 && e[0].kind == Kind::Black && e[1].kind == Kind::Glyphs && e[2].kind == Kind::NativeText);
        CHECK(e[2].options == (ETO_OPAQUE | ETO_CLIPPED) && e[2].ansi == f.value && LastReason(L"] text ") == L"renderer");
        ResTest::renderResult = S_OK; ResTest::throwRenderer = true;
        CHECK(f.Cell() == TRUE);
        e = Take();
        CHECK(e.size() == 3 && e[2].kind == Kind::NativeText && LastReason(L"] text ") == L"renderer" && !Rv::g_inside);
        ResTest::throwRenderer = false;
        // What the original returns and leaves is what the EXE gets.
        ResTest::nativeTextResult = FALSE;
        CHECK(f.Text(&f.paintDc, 67, 22, ETO_PDY, nullptr, f.value, 11) == FALSE && GetLastError() == 0x51); Take();
        ResTest::nativeTextResult = TRUE; ResTest::nativeLeavesError = true;
        CHECK(f.Text(&f.paintDc, 67, 22, ETO_PDY, nullptr, f.value, 11) == TRUE && GetLastError() == 0x29); Take();
        f.End(f.editor);
        CHECK(f.Idle());
    }

    // ANSI bytes are decoded with the code page of the selected font's charset.
    void Ansi()
    {
        Fixture f;
        f.Begin(f.editor);
        CHECK(f.Cell() == TRUE && ResTest::codepage == static_cast<UINT>(CP_ACP) && ResTest::conversions == 2);
        Take();
        ResTest::extra[&f.paintDc].charset = RUSSIAN_CHARSET;
        const char russian[]{char(0xc0), char(0xc1), char(0xc2)};
        CHECK(f.Text(&f.paintDc, 1, 1, 0, nullptr, russian, 3) == TRUE && ResTest::codepage == 1251);
        auto e = Take();
        CHECK(e.size() == 1 && e[0].text == L"\u0410\u0411\u0412");
        // A double-byte character: fewer characters than bytes. Without advances it
        // is drawn; advances given per byte do not map onto it.
        ResTest::extra[&f.paintDc].charset = SHIFTJIS_CHARSET;
        const char kana[]{char(0x82), char(0xa0), 'a'};
        CHECK(f.Text(&f.paintDc, 1, 1, 0, nullptr, kana, 3) == TRUE && ResTest::codepage == 932);
        e = Take();
        CHECK(e.size() == 1 && e[0].text == L"\u3042a" && e[0].count == 2);
        const INT dx[3]{9, 9, 9};
        CHECK(f.Text(&f.paintDc, 1, 1, 0, nullptr, kana, 3, dx) == TRUE);
        f.NativeText(&f.paintDc, 1, 1, 0, nullptr, kana, 3, dx);
        CHECK(LastReason(L"] text ") == L"dx");
        // The count is the caller's: bytes beyond it are not read.
        ResTest::extra[&f.paintDc].charset = DEFAULT_CHARSET;
        CHECK(f.Text(&f.paintDc, 1, 1, 0, nullptr, "FileVersion", 4) == TRUE);
        e = Take();
        CHECK(e.size() == 1 && e[0].text == L"File");
        f.End(f.editor);
    }

    // The exact class, painted by the EXE that owns the window, in Full mode with glass.
    void Scope()
    {
        Fixture f;
        const auto leased = [&](bool expected) {
            CHECK(Rv::g_sessions.size() == 1 && Rv::g_sessions.back().leased == expected);
            CHECK(expected ? f.Hooked() : f.exe.hooks.empty());
            CHECK(f.Cell() == TRUE);
            const auto e = Take();
            CHECK(e.size() == (expected ? 2u : 1u) && e.back().kind == (expected ? Kind::Glyphs : Kind::NativeText));
            CHECK(f.Fill(&f.paintDc, &f.cell, &f.window) == 1);
            CHECK(Take().back().kind == (expected ? Kind::Black : Kind::NativeFill));
            f.End(f.editor);
            CHECK(f.Idle());
        };
        f.Begin(f.editor); leased(true);
        f.editor.cls = L"tresversioneditor"; f.Begin(f.editor); leased(true);
        // Not this class: not even recorded.
        for (LPCWSTR cls : {L"TResVersionEditor2", L"TResVersion", L"ResVersionEditor", L"THexDump", L"TStringGrid", L"TPanel"})
        {
            f.editor.cls = cls; f.Begin(f.editor);
            CHECK(Rv::g_sessions.empty() && f.exe.hooks.empty());
            CHECK(f.Cell() == TRUE && Take().back().kind == Kind::NativeText);
            f.End(f.editor);
        }
        f.editor.cls = L"TResVersionEditor";
        // Recorded, but without the imports.
        f.Begin(f.editor, &f.otherModule); leased(false);           // BeginPaint called by another module
        ResTest::instances[&f.editor] = &f.otherModule;
        f.Begin(f.editor); leased(false);                           // the window belongs to another module
        ResTest::instances.clear();
        Test::full = false; f.Begin(f.editor); leased(false); Test::full = true;
        f.editor.backdrop = false; f.Begin(f.editor); leased(false); f.editor.backdrop = true;
        GlassCore::ModuleHandler::g_entryPointModule = nullptr;
        Test::topWindow = &f.editor; f.Provider().onBegin(&f.editor, nullptr);
        CHECK(Rv::g_sessions.size() == 1 && !Rv::g_sessions.back().leased);
        f.End(f.editor);
        GlassCore::ModuleHandler::g_entryPointModule = &f.exe;

        // Mode and glass are checked again when the EXE draws.
        f.Begin(f.editor);
        Test::full = false;
        CHECK(f.Cell() == TRUE && Take().back().kind == Kind::NativeText && LastReason(L"] text ") == L"mode");
        CHECK(f.Fill(&f.paintDc, &f.cell, &f.window) == 1 && Take().back().kind == Kind::NativeFill);
        CHECK(LastReason(L"] fill ") == L"mode");
        Test::full = true; f.editor.backdrop = false;
        CHECK(f.Cell() == TRUE && Take().back().kind == Kind::NativeText);
        f.editor.backdrop = true; f.editor.valid = false;
        CHECK(f.Cell() == TRUE && Take().back().kind == Kind::NativeText);
        f.editor.valid = true;
        CHECK(f.Cell() == TRUE && Take().back().kind == Kind::Glyphs);
        f.End(f.editor);

        // The newest session of this thread decides: one that could not take the
        // imports is a native barrier over the outer one, also for its DC.
        f.Begin(f.editor);
        f.Begin(f.editor2, &f.otherModule);
        CHECK(Rv::g_sessions.size() == 2 && f.Refs("FillRect") == 1);
        CHECK(f.Cell() == TRUE && Take().back().kind == Kind::NativeText);
        CHECK(f.Fill(&f.paintDc, &f.cell, &f.window) == 1 && Take().back().kind == Kind::NativeFill);
        // ...and for its own: the imports are hooked, but not for this session.
        ResTest::logs.clear();
        CHECK(f.Cell(&f.paintDc2) == TRUE && Take().back().kind == Kind::NativeText);
        CHECK(f.Fill(&f.paintDc2, &f.cell, &f.window) == 1 && Take().back().kind == Kind::NativeFill);
        CHECK(ResTest::logs.empty());
        f.End(f.editor2);
        CHECK(f.Cell() == TRUE && Take().back().kind == Kind::Glyphs);
        // Two leased sessions: each draws on its own DC only while it is the newest.
        f.Begin(f.editor2);
        CHECK(f.Refs("ExtTextOutA") == 2 && f.Refs("FillRect") == 2);
        CHECK(f.Cell(&f.paintDc2) == TRUE && Take().back().kind == Kind::Glyphs);
        CHECK(f.Cell(&f.paintDc) == TRUE && Take().back().kind == Kind::NativeText);
        f.End(f.editor2);
        CHECK(f.Refs("ExtTextOutA") == 1 && f.Hooked());
        CHECK(f.Cell(&f.paintDc) == TRUE && Take().back().kind == Kind::Glyphs);
        f.End(f.editor);
        CHECK(f.Idle());
        // An End without a Begin, and of another window, changes nothing.
        f.End(f.editor); f.Begin(f.editor); f.End(f.other);
        CHECK(Rv::g_sessions.size() == 1 && f.Hooked());
        f.End(f.editor);
        CHECK(f.Idle());
    }

    // Only the window's own DC, held by its open session, with no other window
    // session above it, on the thread that paints.
    void Isolation()
    {
        Fixture f;
        f.Begin(f.editor);
        const auto native = [&](HDC dc, const wchar_t* reason) {
            CHECK(f.Text(dc, 67, 22, ETO_OPAQUE | ETO_CLIPPED, &f.cell, f.value, 11) == TRUE);
            auto e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::NativeText && e[0].dc == dc && LastReason(L"] text ") == reason);
            CHECK(f.Fill(dc, &f.cell, &f.window) == 1);
            e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::NativeFill && e[0].dc == dc && LastReason(L"] fill ") == reason);
        };
        native(&f.foreignDc, L"dc");                         // another window's DC
        Test::stackOwners[&f.foreignDc] = &f.editor; native(&f.foreignDc, L"dc"); Test::stackOwners.erase(&f.foreignDc);
        native(&f.canvasDc, L"dc");                          // this window's, but no open session holds it
        native(&f.memoryDc, L"dc");                          // a buffer nobody saw this window create
        native(nullptr, L"dc");
        Test::stackOwners[&f.memoryDc] = &f.other; native(&f.memoryDc, L"dc");
        // A buffer the dispatcher saw this window create while painting, and a
        // DC its canvas took with GetDC during the paint.
        Test::stackOwners[&f.memoryDc] = &f.editor;
        CHECK(f.Cell(&f.memoryDc) == TRUE && Take().back().kind == Kind::Glyphs);
        CHECK(f.Fill(&f.memoryDc, &f.cell, &f.window) == 1 && Take().back().kind == Kind::Black);
        Test::stackOwners[&f.canvasDc] = &f.editor;
        CHECK(f.Cell(&f.canvasDc) == TRUE && Take().back().kind == Kind::Glyphs);
        // VCL keeps the canvas DC of another control until the message ends: a
        // session of another window opened above ours does not take our DC away.
        for (HWND top : {static_cast<HWND>(&f.other), static_cast<HWND>(nullptr)})
        {
            Test::topWindow = Test::host = top;
            CHECK(f.Cell() == TRUE && Take().back().kind == Kind::Glyphs);
            CHECK(f.Fill(&f.paintDc, &f.cell, &f.window) == 1 && Take().back().kind == Kind::Black);
            CHECK(f.Cell(&f.memoryDc) == TRUE && Take().back().kind == Kind::Glyphs);
        }
        Test::topWindow = &f.editor; Test::host = nullptr;
        // The record of a buffer is what counts, not that it is a buffer; and a
        // window DC is never taken for one.
        Test::stackOwners.erase(&f.memoryDc); native(&f.memoryDc, L"dc");
        Test::stackOwners[&f.memoryDc] = &f.editor;
        // Neither a window's DC nor a buffer (a printer, a metafile), whatever was recorded for it.
        ResTest::objectTypes[&f.memoryDc] = 3; native(&f.memoryDc, L"dc");
        ResTest::objectTypes.clear();
        CHECK(f.Cell(&f.memoryDc) == TRUE && Take().back().kind == Kind::Glyphs);
        Test::stackOwners.erase(&f.canvasDc); native(&f.canvasDc, L"dc");
        // A mapped DC.
        ResTest::extra[&f.paintDc].map = MM_ANISOTROPIC; native(&f.paintDc, L"map");
        ResTest::extra[&f.paintDc].map = MM_TEXT;
        // The import table is the process's; the sessions are this thread's.
        std::thread([&] {
            CHECK(Rv::g_sessions.empty());
            CHECK(ExeExtTextOutA(f.exe, &f.paintDc, 67, 22, ETO_OPAQUE | ETO_CLIPPED, &f.cell, f.value, 11) == TRUE);
            CHECK(ExeFillRect(f.exe, &f.paintDc, &f.cell, &f.window) == 1);
            CHECK(ResTest::events.size() == 2 && ResTest::events[0].kind == Kind::NativeText && ResTest::events[1].kind == Kind::NativeFill);
            CHECK(ResTest::logs.empty());
        }).join();
        // What the EXE draws from inside the renderer or the original is not asked again.
        ResTest::duringRender = [&] {
            ResTest::duringRender = {};
            CHECK(Rv::g_inside);
            CHECK(ExeExtTextOutA(f.exe, &f.paintDc, 1, 1, 0, nullptr, "x", 1) == TRUE);
            CHECK(ExeFillRect(f.exe, &f.paintDc, &f.cell, &f.window) == 1);
        };
        CHECK(f.Cell() == TRUE);
        auto e = Take();
        CHECK(e.size() == 4 && e[0].kind == Kind::Black && e[1].kind == Kind::Glyphs);
        CHECK(e[2].kind == Kind::NativeText && e[3].kind == Kind::NativeFill && !Rv::g_inside);
        ResTest::duringNative = [&] {
            ResTest::duringNative = {};
            CHECK(Rv::g_inside && ExeFillRect(f.exe, &f.paintDc, &f.cell, &f.window) == 1);
        };
        CHECK(f.Fill(&f.paintDc, &f.cell, &f.highlight) == 1);
        e = Take();
        CHECK(e.size() == 2 && e[0].kind == Kind::NativeFill && e[1].kind == Kind::NativeFill && !Rv::g_inside);
        f.End(f.editor);
        CHECK(f.Idle());
    }

    // The two imports are taken together or not at all, counted per session.
    void Leases()
    {
        Fixture f;
        // The EXE does not import one of them.
        for (const char* missing : {"ExtTextOutA", "FillRect"})
        {
            const auto saved = f.exe.originals;
            f.exe.originals.erase(missing);
            f.Begin(f.editor);
            CHECK(Rv::g_sessions.size() == 1 && !Rv::g_sessions.back().leased);
            CHECK(f.exe.hooks.empty() && !f.Refs("ExtTextOutA") && !f.Refs("FillRect"));
            f.End(f.editor);
            f.exe.originals = saved;
            CHECK(f.Idle());
        }
        f.Begin(f.editor);
        CHECK(f.Hooked() && f.Refs("ExtTextOutA") == 1);
        f.Begin(f.editor2);
        CHECK(f.Hooked() && f.Refs("ExtTextOutA") == 2);
        f.End(f.editor);                // out of order: by window, not by position
        CHECK(f.Hooked() && f.Refs("ExtTextOutA") == 1 && Rv::g_sessions.size() == 1 && Rv::g_sessions[0].hwnd == &f.editor2);
        f.End(f.editor2);
        CHECK(f.Idle());

        // Shutdown drops every reference; a session that outlives it releases nothing.
        f.Begin(f.editor);
        std::thread other([&] {
            Test::topWindow = &f.editor2; f.Provider().onBegin(&f.editor2, &f.exe);
            CHECK(Rv::g_sessions.size() == 1 && Rv::g_sessions.back().leased && f.Refs("FillRect") == 2);
            Rv::Shutdown();
            CHECK(f.exe.hooks.empty() && !f.Refs("FillRect") && Rv::g_sessions.empty());
        });
        other.join();
        CHECK(Rv::g_sessions.size() == 1 && Rv::g_sessions.back().leased); // this thread's record is still here
        Test::topWindow = &f.editor;
        CHECK(f.Cell() == TRUE && Take().back().kind == Kind::NativeText);  // the import is the EXE's own again
        // A restart while that session is still open: its lease is void, and
        // closing it does not take a reference from the new generation.
        Rv::Startup();
        CHECK(GlassCore::PaintDispatcher::resProviders.size() == 2);
        GlassCore::PaintDispatcher::resProviders.erase(GlassCore::PaintDispatcher::resProviders.begin());
        std::thread fresh([&] {
            Test::topWindow = &f.editor2; f.Provider().onBegin(&f.editor2, &f.exe);
            CHECK(Rv::g_sessions.back().leased && f.Refs("FillRect") == 1);
        });
        fresh.join();
        Test::topWindow = &f.editor;
        CHECK(f.Hooked());
        CHECK(f.Cell() == TRUE && Take().back().kind == Kind::NativeText);  // hooked by the other thread, not for this stale session
        CHECK(f.Fill(&f.paintDc, &f.cell, &f.window) == 1 && Take().back().kind == Kind::NativeFill);
        f.End(f.editor);
        CHECK(f.Hooked() && f.Refs("FillRect") == 1 && Rv::g_sessions.empty());
        // Before Startup and after Shutdown nothing is recorded.
        Rv::Shutdown();
        CHECK(f.exe.hooks.empty());
        f.Begin(f.editor);
        CHECK(Rv::g_sessions.empty() && f.exe.hooks.empty());
        f.End(f.editor);
        Rv::Startup();
        GlassCore::PaintDispatcher::resProviders.erase(GlassCore::PaintDispatcher::resProviders.begin());
        // Startup twice registers once.
        Rv::Startup();
        CHECK(GlassCore::PaintDispatcher::resProviders.size() == 1);
        // A hook entered without its original (the slot was just released): the API by name, once.
        using TextFn = BOOL (*)(HDC, int, int, UINT, const RECT*, LPCSTR, UINT, const INT*);
        using FillFn = int (*)(HDC, const RECT*, HBRUSH);
        f.Begin(f.editor);
        const auto text = reinterpret_cast<TextFn>(f.exe.hooks.at("ExtTextOutA"));
        const auto fill = reinterpret_cast<FillFn>(f.exe.hooks.at("FillRect"));
        f.End(f.editor);
        CHECK(f.Idle());
        SetLastError(0x29);
        CHECK(text(&f.paintDc, 67, 22, ETO_OPAQUE | ETO_CLIPPED, &f.cell, f.value, 11, nullptr) == TRUE);
        CHECK(fill(&f.paintDc, &f.cell, &f.window) == 1);
        const auto e = Take();
        CHECK(e.size() == 2 && e[0].kind == Kind::DirectText && e[0].entryError == 0x29 && e[0].ansi == f.value);
        CHECK(e[1].kind == Kind::DirectFill && e[1].brush == &f.window);
    }

    // Metadata only, 16 lines a second per channel; the text of a cell is never logged.
    void Diagnostics()
    {
        Fixture f;
        f.Begin(f.editor);
        auto begin = Logs(L"] Begin ");
        CHECK(begin.size() == 1 && begin[0].format
            == L"[GlassCore][DelphiResVersionEditor] Begin hwnd=0x%p caller=0x%p eligible=%d leased=%d depth=%zu");
        CHECK(begin[0].numbers == (std::vector<long long>{Number(&f.editor), Number(&f.exe), 1, 1, 1}));
        const char* secret = "secret-cell-text";
        CHECK(f.Text(&f.paintDc, 67, 22, ETO_OPAQUE | ETO_CLIPPED, &f.cell, secret, 16) == TRUE);
        auto text = Logs(L"] text ");
        CHECK(text.size() == 1 && text[0].format.find(L"[GlassCore][DelphiResVersionEditor] text hwnd=0x%p dc=0x%p dcWnd=0x%p dcType=%lu pos=[%d,%d] options=0x%X rect=%d[%ld,%ld,%ld,%ld] ansiCount=%u wideCount=%zu cp=%u align=0x%X bkMode=%d bk=0x%06X color=0x%06X drawn=%d reason=%s result=%d") == 0);
        CHECK(text[0].strings == std::vector<std::wstring>{L"none"});
        CHECK(text[0].numbers == (std::vector<long long>{Number(&f.editor), Number(&f.paintDc), Number(&f.editor), 3,
            67, 22, ETO_OPAQUE | ETO_CLIPPED, 1, 65, 20, 265, 38, 16, 16, CP_ACP, 0, OPAQUE, 0xffffff, RGB(0, 0, 128), 1, 1}));
        CHECK(f.Fill(&f.paintDc, &f.cell, &f.highlight) == 1);
        auto fill = Logs(L"] fill ");
        CHECK(fill.size() == 1 && fill[0].format
            == L"[GlassCore][DelphiResVersionEditor] fill hwnd=0x%p dc=0x%p dcWnd=0x%p rect=%d[%ld,%ld,%ld,%ld] rgb=0x%06X cleared=%d reason=%s");
        CHECK(fill[0].strings == std::vector<std::wstring>{L"colour"});
        CHECK(fill[0].numbers == (std::vector<long long>{Number(&f.editor), Number(&f.paintDc), Number(&f.editor),
            1, 65, 20, 265, 38, RGB(0, 120, 215), 0}));
        // Without a rectangle the line says so.
        CHECK(f.Text(&f.paintDc, 4, 2, 0, nullptr, secret, 16) == TRUE);
        text = Logs(L"] text ");
        CHECK(text.size() == 2 && text[1].numbers[7] == 0 && text[1].numbers[8] == 0 && text[1].numbers[11] == 0);
        // The limit is per second and per channel.
        ResTest::logs.clear(); ResTest::clock += 1000;
        for (int i = 0; i < 40; ++i)
        {
            CHECK(ExeExtTextOutA(f.exe, &f.paintDc, 4, 2, 0, nullptr, secret, 16) == TRUE);
            CHECK(ExeFillRect(f.exe, &f.paintDc, &f.cell, &f.window) == 1);
        }
        CHECK(Logs(L"] text ").size() == 16 && Logs(L"] fill ").size() == 16);
        ResTest::clock += 1000;
        CHECK(ExeExtTextOutA(f.exe, &f.paintDc, 4, 2, 0, nullptr, secret, 16) == TRUE);
        CHECK(Logs(L"] text ").size() == 17);
        f.End(f.editor);
        for (int i = 0; i < 40; ++i) { f.Begin(f.editor); f.End(f.editor); }
        CHECK(Logs(L"] Begin ").size() == 16);
        // Nothing logged carries the text of a cell.
        const std::wstring wide(secret, secret + std::strlen(secret));
        CHECK(ResTest::logs.size() > 40);
        for (const auto& log : ResTest::logs)
        {
            CHECK(log.format.find(L"%hs") == std::wstring::npos && log.format.find(L"%S") == std::wstring::npos);
            for (const auto& value : log.strings) CHECK(value.find(wide) == std::wstring::npos);
        }
        Take();
        CHECK(f.Idle());
    }
}

int main(int argc, char** argv)
{
    try
    {
        if (argc != 2) throw std::runtime_error("one case required");
        const std::string name = argv[1];
        if (name == "cells") Cells();
        else if (name == "fills") Fills();
        else if (name == "text-gates") TextGates();
        else if (name == "ansi") Ansi();
        else if (name == "scope") Scope();
        else if (name == "isolation") Isolation();
        else if (name == "leases") Leases();
        else if (name == "diagnostics") Diagnostics();
        else throw std::runtime_error("unknown case");
        std::cout << "PASS " << name << '\n';
    }
    catch (const std::exception& e) { std::cerr << argv[1] << ": " << e.what() << '\n'; return 1; }
}
