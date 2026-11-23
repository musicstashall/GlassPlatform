#include "restorator_hexdump_test_shim.h"
#include "../../GlassCore/src/Hooks/delphi/RestoratorHexDump.cpp"

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)

namespace
{
    namespace Hd = GlassCore::RestoratorHexDump;
    using HexTest::Event;
    using HexTest::Kind;
    const auto kExportExt = &::ExtTextOutA;
    const auto kExportTabbed = &::TabbedTextOutA;
    const auto kTrampolineExt = &HexTrampolineExtTextOutA;
    const auto kTrampolineTabbed = &HexTrampolineTabbedTextOutA;
    void* const kWide = reinterpret_cast<void*>(&ExtTextOutW); // what the renderer must draw with

    void ResetFaults()
    {
        using namespace HexTest;
        failBegin = failCommit = false; failAttachAt = failDetachAt = 0;
        nativeExtResult = TRUE; nativeTabbedResult = MAKELONG(77, 13); nativeLeavesError = false;
        renderResult = S_OK; throwRenderer = failBlack = failBrush = failFill = false;
        failMetrics = failExtent = shrinkingExtent = false;
        failTranslate = failConvert = partialConvert = throwConvert = false;
        duringRender = {};
    }
    bool Attached() { return HexTest::extDetour && HexTest::tabbedDetour; }
    bool Detached()
    {
        return !HexTest::extDetour && !HexTest::tabbedDetour && Hd::g_actualExtTextOut == kExportExt
            && Hd::g_actualTabbedTextOut == kExportTabbed && !Hd::g_attached;
    }
    std::vector<Event> Take() { return std::exchange(HexTest::events, {}); }
    bool Same(const RECT& a, const RECT& b)
    { return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom; }
    int TextLogs()
    {
        return static_cast<int>(std::count_if(HexTest::logFormats.begin(), HexTest::logFormats.end(),
            [](const std::wstring& s) { return s.find(L"] text api=") != std::wstring::npos; }));
    }
    // The reason is the last string of a `text` line.
    std::wstring LastReason() { CHECK(!HexTest::logStrings.empty()); return HexTest::logStrings.back(); }

    struct Fixture
    {
        TestModule exe, otherModule;
        TestWindow hex, hex2, other;
        TestDC paintDc, canvasDc, paintDc2, memoryDc, foreignDc;
        TestFont font{7};
        LOGBRUSH brush{BS_SOLID, RGB(9, 8, 7)};
        // One row of the viewer, as HexDump.pas writes it.
        const char* addressText = "00000010:";
        const char* bytesText = "4D\t5A";
        const RECT cell{1, 32, 81, 48};
        const RECT bytesRow{81, 32, 121, 48}; // "4D" at 81, a stop every 24 from 81, "5A" at 105
        const INT stop = 24;

        Fixture()
        {
            using namespace HexTest;
            ResetFaults();
            extra.clear(); instances.clear(); newestOwner.clear(); events.clear();
            gdiAvailable = userAvailable = extExport = tabbedExport = true;
            resolutions = exportCalls = trampolineCalls = 0;
            writes = attachCalls = detachCalls = attaches = detaches = 0;
            pending.clear(); codepage = 0; conversions = 0;
            static ULONGLONG epoch = 0;
            HexTest::clock = epoch += 1000000; // a fresh rate-limit window for every fixture
            logFormats.clear(); logStrings.clear();
            CHECK(!extDetour && !tabbedDetour);
            Test::full = true; Test::stackOwners.clear(); Test::topWindow = Test::host = nullptr;
            GlassCore::ModuleHandler::g_entryPointModule = &exe;
            GlassCore::PaintDispatcher::hexProviders.clear();
            hex.cls = hex2.cls = L"THexDump"; other.cls = L"TPanel";
            paintDc.owner = canvasDc.owner = &hex; paintDc2.owner = &hex2; foreignDc.owner = &other;
            for (TestDC* dc : {&paintDc, &canvasDc, &paintDc2, &memoryDc, &foreignDc})
            {
                dc->brush = &brush; dc->font = &font; dc->text = RGB(0, 0, 128);
                dc->background = 0xffffff; dc->bkMode = OPAQUE; // VCL: Brush.Color = clWindow
            }
            // What BeginPaint returned for each window; other DCs are tracked by the tests.
            Test::stackOwners[&paintDc] = &hex; Test::stackOwners[&paintDc2] = &hex2;
            caller = &exe;
            Hd::Startup();
            CHECK(GlassCore::PaintDispatcher::hexProviders.size() == 1 && Detached());
        }
        ~Fixture()
        {
            ResetFaults();
            Hd::g_shutdown.store(false); // a test may have left the gate closed; Shutdown below must detach
            Hd::Shutdown();
            assert(Detached() && Hd::g_refs == 0 && Hd::g_sessions.empty() && !Hd::g_inside);
            assert(HexTest::attaches == HexTest::detaches);
        }

        auto& Provider()
        {
            CHECK(GlassCore::PaintDispatcher::hexProviders.size() == 1);
            return GlassCore::PaintDispatcher::hexProviders.front();
        }
        void Begin(TestWindow& w, HMODULE module = nullptr)
        { Test::topWindow = &w; Provider().onBegin(&w, module ? module : &exe); }
        void End(TestWindow& w) { Provider().onEnd(&w, &exe); }
        // Every call opens a new rate-limit window, so that each one is logged.
        BOOL Ext(HDC dc, int x, int y, UINT options, const RECT* rect, LPCSTR text, UINT count, const INT* dx = nullptr)
        {
            HexTest::clock += 1000; SetLastError(0x29);
            return ExtTextOutA(dc, x, y, options, rect, text, count, dx);
        }
        LONG Tabbed(HDC dc, int x, int y, LPCSTR text, int count, int tabCount, const INT* tabs, int origin)
        {
            HexTest::clock += 1000; SetLastError(0x29);
            return TabbedTextOutA(dc, x, y, text, count, tabCount, tabs, origin);
        }
        BOOL Address(HDC dc = nullptr)
        { return Ext(dc ? dc : &paintDc, 1, 32, ETO_OPAQUE | ETO_CLIPPED, &cell, addressText, 9); }
        LONG Bytes(HDC dc = nullptr) { return Tabbed(dc ? dc : &paintDc, 81, 32, bytesText, 5, 1, &stop, 81); }

        // The caller's DC is as it was: nothing selected, saved or switched.
        void Unchanged(TestDC& dc, COLORREF background = 0xffffff, int mode = OPAQUE)
        {
            CHECK(dc.brush == &brush && dc.font == &font && dc.text == RGB(0, 0, 128));
            CHECK(dc.background == background && dc.bkMode == mode && dc.saved.empty());
            CHECK(!Hd::g_inside);
        }
        // Background first, then the glyphs at the caller's position with the
        // caller's clipping; the opaque fill is not asked of the renderer again.
        void DrewAddress(HDC dc = nullptr)
        {
            const HDC target = dc ? dc : &paintDc;
            const auto e = Take();
            CHECK(e.size() == 2 && e[0].kind == Kind::Black && e[0].dc == target && Same(e[0].rect, cell));
            CHECK(e[1].kind == Kind::Glyphs && e[1].dc == target && e[1].x == 1 && e[1].y == 32);
            CHECK(e[1].options == ETO_CLIPPED && e[1].rectPointer == &cell && e[1].text == L"00000010:");
            CHECK(!e[1].dxPointer && e[1].original == kWide);
            CHECK(e[1].state.text == RGB(0, 0, 128) && e[1].state.font == &font);
            CHECK(GetLastError() == 0x29 && LastReason() == L"none");
            Unchanged(*target);
        }
        void DrewBytes(HDC dc = nullptr)
        {
            const HDC target = dc ? dc : &paintDc;
            const auto e = Take();
            CHECK(e.size() == 2 && e[0].kind == Kind::Black && e[0].dc == target && Same(e[0].rect, bytesRow));
            CHECK(e[1].kind == Kind::Glyphs && e[1].dc == target && e[1].x == 81 && e[1].y == 32);
            CHECK(e[1].options == 0 && Same(e[1].rect, bytesRow) && e[1].text == L"4D5A");
            CHECK((e[1].dx == std::vector<INT>{8, 16, 8, 8}) && e[1].original == kWide);
            CHECK(GetLastError() == 0x29 && LastReason() == L"none");
            Unchanged(*target);
        }
        // Exactly one call of the original, with the caller's own arguments and
        // entry error, and nothing else on the surface.
        void NativeAddress(LPCWSTR reason = nullptr, HDC dc = nullptr)
        {
            const HDC target = dc ? dc : &paintDc;
            const auto e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::NativeExt && e[0].dc == target && e[0].x == 1 && e[0].y == 32);
            CHECK(e[0].options == (ETO_OPAQUE | ETO_CLIPPED) && e[0].rectPointer == &cell);
            CHECK(e[0].ansi == addressText && e[0].count == 9 && !e[0].dxPointer && e[0].entryError == 0x29);
            CHECK(GetLastError() == 0x51);
            if (reason) CHECK(LastReason() == reason);
            Unchanged(*target);
        }
        void NativeBytes(LPCWSTR reason = nullptr, HDC dc = nullptr)
        {
            const HDC target = dc ? dc : &paintDc;
            const auto e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::NativeTabbed && e[0].dc == target && e[0].x == 81 && e[0].y == 32);
            CHECK(e[0].ansi == bytesText && e[0].count == 5 && e[0].tabCount == 1 && e[0].dxPointer == &stop);
            CHECK(e[0].origin == 81 && e[0].entryError == 0x29 && GetLastError() == 0x51);
            if (reason) CHECK(LastReason() == reason);
            Unchanged(*target);
        }
    };

    void Rows()
    {
        Fixture f; f.Begin(f.hex);
        CHECK(Attached() && HexTest::writes == 1 && HexTest::attaches == 2);
        // Address column: opaque, clipped to its cells.
        CHECK(f.Address() == TRUE); f.DrewAddress();
        // Bytes: one tab-stop distance of three cells, expanded from where the text starts.
        const char* bytes = "4D\t5A\t90\t00";
        const LONG extent = f.Tabbed(&f.paintDc, 81, 32, bytes, 11, 1, &f.stop, 81);
        {
            const RECT row{81, 32, 169, 48};
            const auto e = Take();
            CHECK(extent == MAKELONG(88, 16)); // what user32 returns: width low, height high
            CHECK(e.size() == 2 && e[0].kind == Kind::Black && Same(e[0].rect, row));
            CHECK(e[1].kind == Kind::Glyphs && e[1].x == 81 && e[1].y == 32 && e[1].options == 0);
            CHECK(Same(e[1].rect, row) && e[1].text == L"4D5A9000" && e[1].original == kWide);
            // Each pair is 16 wide and the next one starts 24 further on.
            CHECK((e[1].dx == std::vector<INT>{8, 16, 8, 16, 8, 16, 8, 8}));
            CHECK(GetLastError() == 0x29); f.Unchanged(f.paintDc);
        }
        // Character column up to the right edge; bytes above 0x7f are text as well.
        const RECT chars{465, 32, 600, 48};
        CHECK(f.Ext(&f.paintDc, 465, 32, ETO_OPAQUE | ETO_CLIPPED, &chars, "MZ\x90.", 4));
        {
            const auto e = Take();
            CHECK(e.size() == 2 && e[0].kind == Kind::Black && Same(e[0].rect, chars));
            CHECK(e[1].kind == Kind::Glyphs && e[1].x == 465 && e[1].y == 32 && e[1].options == ETO_CLIPPED);
            CHECK(e[1].rectPointer == &chars && (e[1].text == std::wstring{L'M', L'Z', wchar_t(0x90), L'.'}));
        }
        // A row past the end of the data is only its background.
        const RECT blank{1, 48, 600, 64};
        CHECK(f.Ext(&f.paintDc, 1, 48, ETO_OPAQUE | ETO_CLIPPED, &blank, nullptr, 0));
        {
            const auto e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::Black && Same(e[0].rect, blank) && GetLastError() == 0x29);
        }
        CHECK(HexTest::exportCalls == 4 && HexTest::trampolineCalls == 0);
        f.End(f.hex);
        CHECK(Detached() && HexTest::writes == 2 && HexTest::detaches == 2);
        // Outside the paint session the very same calls are the application's own.
        const int logs = TextLogs();
        CHECK(f.Address() == TRUE); f.NativeAddress();
        CHECK(f.Bytes() == MAKELONG(77, 13)); f.NativeBytes();
        CHECK(TextLogs() == logs && HexTest::trampolineCalls == 0);
    }

    void Tabs()
    {
        struct Case { const char* text; int x, origin, step, first; const wchar_t* glyphs; std::vector<INT> dx; int width; };
        const Case cases[]{
            {"ABC", 10, 3, 24, 10, L"ABC", {8, 8, 8}, 24},               // no tab: the stops do not matter
            {"A\tB", 10, 10, 24, 10, L"AB", {24, 8}, 32},                // next stop beyond the text
            {"A\t\tB", 10, 10, 24, 10, L"AB", {48, 8}, 56},              // two tabs, two stops
            {"\tAB", 10, 10, 24, 34, L"AB", {8, 8}, 40},                 // leading tab moves the first glyph
            {"AB\t", 10, 10, 24, 10, L"AB", {8, 8}, 24},                 // trailing tab only widens the extent
            {"ABC\tD", 10, 10, 24, 10, L"ABCD", {8, 8, 32, 8}, 56},      // text ending on a stop goes to the next one
            {"ABCDE\tF", 10, 10, 24, 10, L"ABCDEF", {8, 8, 8, 8, 16, 8}, 56}, // a run wider than the distance
            {"A\tB", 15, 10, 24, 15, L"AB", {19, 8}, 27},                // text starting off the grid of its origin
            {"Wi\tiW", 10, 10, 24, 10, L"WiiW", {16, 32, 8, 16}, 72},    // advances are per glyph, not per cell
            {"A\tB", 10, 10, 1, 10, L"AB", {9, 8}, 17},                  // the smallest distance
            {"AB\tC", 0, 0, 100, 0, L"ABC", {8, 92, 8}, 108},
            {"A\tB", -20, -20, 24, -20, L"AB", {24, 8}, 32},             // left of the window origin
            {"\t\t", 10, 10, 24, 10, L"", {}, 48},                       // no glyphs at all
            {"A\tB\tC\tD\tE", 81, 81, 24, 81, L"ABCDE", {24, 24, 24, 24, 8}, 104},
        };
        for (const Case& c : cases)
        {
            Fixture f; f.Begin(f.hex);
            const INT step = c.step;
            const int count = static_cast<int>(std::strlen(c.text));
            const LONG result = f.Tabbed(&f.paintDc, c.x, 40, c.text, count, 1, &step, c.origin);
            const RECT row{c.x, 40, c.x + c.width, 56};
            const auto e = Take();
            const std::wstring glyphs = c.glyphs;
            CHECK(result == MAKELONG(c.width, 16) && GetLastError() == 0x29 && LastReason() == L"none");
            CHECK(e.size() == (glyphs.empty() ? 1u : 2u) && e[0].kind == Kind::Black && Same(e[0].rect, row));
            if (!glyphs.empty())
            {
                CHECK(e[1].kind == Kind::Glyphs && e[1].x == c.first && e[1].y == 40 && e[1].options == 0);
                CHECK(Same(e[1].rect, row) && e[1].text == glyphs && e[1].dx == c.dx && e[1].original == kWide);
                // The glyphs end exactly at the extent user32 would report.
                int end = c.first; for (INT a : c.dx) end += a;
                CHECK(end <= c.x + c.width);
            }
            CHECK(HexTest::Count(Kind::NativeTabbed) == 0 && HexTest::trampolineCalls == 0);
            f.Unchanged(f.paintDc); f.End(f.hex);
        }
        {
            // TRANSPARENT text has no background of its own.
            Fixture f; f.paintDc.bkMode = TRANSPARENT; f.Begin(f.hex);
            CHECK(f.Bytes() == MAKELONG(40, 16));
            const auto e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::Glyphs && e[0].text == L"4D5A");
            f.Unchanged(f.paintDc, 0xffffff, TRANSPARENT); f.End(f.hex);
        }
        {
            // A highlight colour behind the row is filled opaque over the same extent.
            Fixture f; f.paintDc.background = RGB(10, 20, 30); f.Begin(f.hex);
            CHECK(f.Bytes() == MAKELONG(40, 16));
            const auto e = Take();
            CHECK(e.size() == 2 && e[0].kind == Kind::Fill && Same(e[0].rect, f.bytesRow));
            CHECK(e[0].color == RGB(10, 20, 30) && e[0].alpha == 255 && e[1].kind == Kind::Glyphs);
            f.Unchanged(f.paintDc, RGB(10, 20, 30)); f.End(f.hex);
        }
        {
            // A run is as wide as GDI measures the whole of it, which need not be
            // the last partial extent: the next stop is counted from that width.
            Fixture f; HexTest::extra[&f.paintDc].overhang = 3; f.Begin(f.hex);
            const INT step = 24;
            CHECK(f.Tabbed(&f.paintDc, 10, 40, "AB\tC", 4, 1, &step, 10) == MAKELONG(35, 16));
            const auto e = Take();
            const RECT row{10, 40, 45, 56};
            CHECK(e.size() == 2 && Same(e[0].rect, row) && e[1].text == L"ABC");
            CHECK((e[1].dx == std::vector<INT>{8, 16, 11}));
            f.End(f.hex);
        }
        {
            // The font's own height and cell are used, whatever they are.
            Fixture f; HexTest::extra[&f.paintDc].cell = 9; HexTest::extra[&f.paintDc].height = 19; f.Begin(f.hex);
            const INT step = 27;
            CHECK(f.Tabbed(&f.paintDc, 91, 5, "4D\t5A", 5, 1, &step, 91) == MAKELONG(45, 19));
            const auto e = Take();
            const RECT row{91, 5, 136, 24};
            CHECK(e.size() == 2 && Same(e[0].rect, row) && (e[1].dx == std::vector<INT>{9, 18, 9, 9}));
            f.End(f.hex);
        }
    }

    void TabGates()
    {
        // Every layout this module does not reproduce is one native call.
        struct Gate { const wchar_t* reason; std::function<LONG(Fixture&)> call; };
        const INT two[]{24, 48}, zero = 0, negative = -24;
        {
            Fixture f; f.Begin(f.hex);
            const auto native = [&](const wchar_t* reason, int x, const char* text, int count, int tabCount,
                const INT* tabs, int origin) {
                CHECK(f.Tabbed(&f.paintDc, x, 32, text, count, tabCount, tabs, origin) == MAKELONG(77, 13));
                const auto e = Take();
                CHECK(e.size() == 1 && e[0].kind == Kind::NativeTabbed && e[0].dc == &f.paintDc && e[0].x == x);
                CHECK(e[0].y == 32 && e[0].ansi == text && e[0].count == count && e[0].tabCount == tabCount);
                CHECK(e[0].dxPointer == tabs && e[0].origin == origin && e[0].entryError == 0x29);
                CHECK(GetLastError() == 0x51 && LastReason() == reason);
                f.Unchanged(f.paintDc);
            };
            native(L"tabs", 81, f.bytesText, 5, 0, nullptr, 81);   // default stops of eight average characters
            native(L"tabs", 81, f.bytesText, 5, 0, &f.stop, 81);
            native(L"tabs", 81, f.bytesText, 5, 2, two, 81);       // explicit stop positions
            native(L"tabs", 81, f.bytesText, 5, 1, nullptr, 81);
            native(L"tabs", 81, f.bytesText, 5, 1, &zero, 81);
            native(L"tabs", 81, f.bytesText, 5, 1, &negative, 81); // right-aligned stops
            native(L"tabs", 81, f.bytesText, 5, -1, &f.stop, 81);
            native(L"tabs", 80, f.bytesText, 5, 1, &f.stop, 81);   // text left of the tab origin
            native(L"conversion", 81, nullptr, 5, 1, &f.stop, 81);
            native(L"conversion", 81, f.bytesText, 0, 1, &f.stop, 81);
            native(L"conversion", 81, f.bytesText, -1, 1, &f.stop, 81);
            CHECK(HexTest::conversions == 0); // none of these reached the decoder
            for (UINT align : {TA_RIGHT, TA_CENTER, TA_UPDATECP, TA_BOTTOM, TA_BASELINE})
            {
                HexTest::extra[&f.paintDc].align = align;
                CHECK(f.Bytes() == MAKELONG(77, 13)); f.NativeBytes(L"align");
            }
            HexTest::extra[&f.paintDc].align = TA_LEFT | TA_TOP;
            CHECK(f.Bytes() == MAKELONG(40, 16)); f.DrewBytes();
            f.End(f.hex);
        }
        const Gate gates[]{
            {L"conversion", [](Fixture& f) { HexTest::extra[&f.paintDc].charset = SYMBOL_CHARSET; return f.Bytes(); }},
            {L"conversion", [](Fixture& f) { HexTest::extra[&f.paintDc].charset = 77; return f.Bytes(); }},
            {L"conversion", [](Fixture& f) { HexTest::extra[&f.paintDc].charset = -1; return f.Bytes(); }},
            {L"conversion", [](Fixture& f) {
                HexTest::extra[&f.paintDc].charset = RUSSIAN_CHARSET; HexTest::failTranslate = true; return f.Bytes(); }},
            {L"conversion", [](Fixture& f) { HexTest::failConvert = true; return f.Bytes(); }},
            {L"conversion", [](Fixture& f) { HexTest::partialConvert = true; return f.Bytes(); }},
            {L"conversion", [](Fixture& f) { HexTest::throwConvert = true; return f.Bytes(); }},
            {L"font", [](Fixture& f) { HexTest::failMetrics = true; return f.Bytes(); }},
            {L"font", [](Fixture& f) { HexTest::extra[&f.paintDc].underline = true; return f.Bytes(); }},
            {L"font", [](Fixture& f) { HexTest::extra[&f.paintDc].strikeout = true; return f.Bytes(); }},
            {L"font", [](Fixture& f) { HexTest::extra[&f.paintDc].height = 0; return f.Bytes(); }},
            {L"layout", [](Fixture& f) { HexTest::failExtent = true; return f.Bytes(); }},
            {L"layout", [](Fixture& f) { HexTest::shrinkingExtent = true; return f.Bytes(); }},
            {L"background", [](Fixture& f) { HexTest::failBlack = true; return f.Bytes(); }},
            {L"background", [](Fixture& f) {
                f.paintDc.background = RGB(1, 2, 3); HexTest::failBrush = true; return f.Bytes(); }},
            {L"background", [](Fixture& f) {
                f.paintDc.background = RGB(1, 2, 3); HexTest::failFill = true; return f.Bytes(); }},
        };
        for (const Gate& gate : gates)
        {
            Fixture f; f.Begin(f.hex);
            const COLORREF before = f.paintDc.background; (void)before;
            CHECK(gate.call(f) == MAKELONG(77, 13));
            const auto e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::NativeTabbed && e[0].ansi == f.bytesText && e[0].count == 5);
            CHECK(e[0].dxPointer == &f.stop && e[0].origin == 81 && e[0].entryError == 0x29);
            CHECK(GetLastError() == 0x51 && LastReason() == gate.reason && HexTest::trampolineCalls == 1);
            CHECK(f.paintDc.saved.empty() && !Hd::g_inside);
            ResetFaults(); f.End(f.hex);
        }
        {
            // Coordinates that leave the integer range are not laid out.
            Fixture f; f.Begin(f.hex);
            CHECK(f.Tabbed(&f.paintDc, INT_MAX - 4, 32, "AB", 2, 1, &f.stop, 0) == MAKELONG(77, 13));
            CHECK(Take().size() == 1 && LastReason() == L"layout");
            // The text fits; the tab stop after it does not.
            CHECK(f.Tabbed(&f.paintDc, INT_MAX - 10, 32, "A\tB", 3, 1, &f.stop, 0) == MAKELONG(77, 13));
            CHECK(Take().size() == 1 && LastReason() == L"layout");
            // Just inside the range everything is drawn.
            CHECK(f.Tabbed(&f.paintDc, INT_MAX - 40, 32, "A\tB", 3, 1, &f.stop, 0) == MAKELONG(17, 16));
            {
                const auto e = Take();
                CHECK(e.size() == 2 && e[1].kind == Kind::Glyphs && e[1].x == INT_MAX - 40);
                CHECK((e[1].dx == std::vector<INT>{9, 8}) && e[0].rect.right == INT_MAX - 23);
            }
            CHECK(f.Tabbed(&f.paintDc, 81, INT_MAX - 3, "AB", 2, 1, &f.stop, 81) == MAKELONG(77, 13));
            CHECK(Take().size() == 1 && LastReason() == L"layout");
            f.End(f.hex);
        }
        for (bool thrown : {false, true})
        {
            // A renderer failure after the background: the original repaints the call once.
            Fixture f; f.Begin(f.hex);
            if (thrown) HexTest::throwRenderer = true; else HexTest::renderResult = E_FAIL;
            CHECK(f.Bytes() == MAKELONG(77, 13));
            const auto e = Take();
            CHECK(e.size() == 3 && e[0].kind == Kind::Black && e[1].kind == Kind::Glyphs && e[2].kind == Kind::NativeTabbed);
            CHECK(e[2].ansi == f.bytesText && e[2].entryError == 0x29 && GetLastError() == 0x51);
            CHECK(LastReason() == L"renderer"); f.Unchanged(f.paintDc);
            f.End(f.hex);
        }
    }

    void Background()
    {
        // Black and the two system colours are the window's own background: cleared.
        for (COLORREF color : {COLORREF(0), COLORREF(0xffffff), COLORREF(0xf0f0f0), COLORREF(0x02ffffff), COLORREF(0xff000000)})
        {
            Fixture f; f.paintDc.background = color; f.Begin(f.hex);
            CHECK(f.Address());
            const auto e = Take();
            CHECK(e.size() == 2 && e[0].kind == Kind::Black && Same(e[0].rect, f.cell) && e[1].kind == Kind::Glyphs);
            CHECK(e[1].options == ETO_CLIPPED);
            f.Unchanged(f.paintDc, color); f.End(f.hex);
        }
        // Any other colour is a highlight: filled with a fully opaque brush.
        for (COLORREF color : {COLORREF(RGB(10, 20, 30)), COLORREF(RGB(255, 255, 254)), COLORREF(RGB(1, 0, 0)),
            COLORREF(0x01000000 | RGB(10, 20, 30))})
        {
            Fixture f; f.paintDc.background = color; f.Begin(f.hex);
            CHECK(f.Address());
            const auto e = Take();
            CHECK(e.size() == 2 && e[0].kind == Kind::Fill && Same(e[0].rect, f.cell));
            CHECK(e[0].color == (color & 0xffffff) && e[0].alpha == 255 && e[1].kind == Kind::Glyphs);
            f.Unchanged(f.paintDc, color); f.End(f.hex);
        }
        {
            Fixture f; f.Begin(f.hex);
            // Without ETO_OPAQUE nothing is filled, whatever the background mode.
            CHECK(f.Ext(&f.paintDc, 1, 32, ETO_CLIPPED, &f.cell, f.addressText, 9));
            auto e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::Glyphs && e[0].options == ETO_CLIPPED && e[0].rectPointer == &f.cell);
            // An empty or inverted rectangle has no area to fill; the text is still drawn.
            for (const RECT r : {RECT{5, 32, 5, 48}, RECT{9, 32, 5, 48}, RECT{1, 48, 81, 32}, RECT{1, 32, 81, 32}})
            {
                CHECK(f.Ext(&f.paintDc, 1, 32, ETO_OPAQUE, &r, f.addressText, 9));
                e = Take();
                CHECK(e.size() == 1 && e[0].kind == Kind::Glyphs && e[0].options == 0 && e[0].rectPointer == &r);
            }
            // The background alone, in either colour class.
            CHECK(f.Ext(&f.paintDc, 1, 32, ETO_OPAQUE, &f.cell, nullptr, 0));
            e = Take(); CHECK(e.size() == 1 && e[0].kind == Kind::Black && Same(e[0].rect, f.cell));
            CHECK(f.Ext(&f.paintDc, 1, 32, ETO_OPAQUE, &f.cell, f.addressText, 0));
            e = Take(); CHECK(e.size() == 1 && e[0].kind == Kind::Black);
            f.paintDc.background = RGB(7, 7, 7);
            CHECK(f.Ext(&f.paintDc, 1, 32, ETO_OPAQUE | ETO_CLIPPED, &f.cell, nullptr, 0));
            e = Take(); CHECK(e.size() == 1 && e[0].kind == Kind::Fill && e[0].color == RGB(7, 7, 7) && e[0].alpha == 255);
            CHECK(GetLastError() == 0x29 && HexTest::trampolineCalls == 0);
            f.End(f.hex);
        }
        {
            // An opaque fill needs its rectangle, and a fill that fails is the original's.
            Fixture f; f.Begin(f.hex);
            CHECK(f.Ext(&f.paintDc, 1, 32, ETO_OPAQUE, nullptr, f.addressText, 9));
            auto e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::NativeExt && e[0].options == ETO_OPAQUE && !e[0].rectPointer);
            CHECK(LastReason() == L"rect");
            const RECT huge{INT_MIN, 0, INT_MAX, 16};
            CHECK(f.Ext(&f.paintDc, 1, 32, ETO_OPAQUE, &huge, f.addressText, 9));
            e = Take(); CHECK(e.size() == 1 && e[0].kind == Kind::NativeExt && LastReason() == L"background");
            HexTest::failBlack = true; CHECK(f.Address()); f.NativeAddress(L"background"); HexTest::failBlack = false;
            f.paintDc.background = RGB(1, 2, 3);
            HexTest::failBrush = true; CHECK(f.Address());
            e = Take(); CHECK(e.size() == 1 && e[0].kind == Kind::NativeExt && LastReason() == L"background");
            HexTest::failBrush = false; HexTest::failFill = true; CHECK(f.Address());
            e = Take(); CHECK(e.size() == 1 && e[0].kind == Kind::NativeExt && LastReason() == L"background");
            HexTest::failFill = false; CHECK(f.Address());
            e = Take(); CHECK(e.size() == 2 && e[0].kind == Kind::Fill && e[1].kind == Kind::Glyphs);
            f.End(f.hex);
        }
    }

    void Ansi()
    {
        {
            // The font's charset selects the code page; the default charset is the system one.
            Fixture f; f.Begin(f.hex);
            CHECK(f.Ext(&f.paintDc, 3, 4, 0, nullptr, "AZ", 2));
            auto e = Take();
            CHECK(e.size() == 1 && e[0].text == L"AZ" && HexTest::codepage == UINT(CP_ACP) && HexTest::conversions == 2);
            HexTest::extra[&f.paintDc].charset = RUSSIAN_CHARSET;
            CHECK(f.Ext(&f.paintDc, 3, 4, 0, nullptr, "\xC0\xC1\xC2", 3));
            e = Take();
            CHECK(e.size() == 1 && e[0].text == L"\u0410\u0411\u0412" && HexTest::codepage == 1251);
            // Per-byte advances pass through untouched when every byte is a character.
            const INT dx[]{9, 9, 9};
            CHECK(f.Ext(&f.paintDc, 3, 4, 0, nullptr, "\xC0\xC1\xC2", 3, dx));
            e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::Glyphs && e[0].dxPointer == dx && e[0].count == 3);
            // A double-byte pair is one character: fewer glyphs than bytes.
            HexTest::extra[&f.paintDc].charset = SHIFTJIS_CHARSET;
            const char* pair = "\x82\xA0Z";
            CHECK(f.Ext(&f.paintDc, 3, 4, 0, nullptr, pair, 3));
            e = Take();
            CHECK(e.size() == 1 && e[0].text == L"\u3042Z" && e[0].count == 2 && HexTest::codepage == 932);
            // The caller's per-byte advances cannot be applied to them.
            CHECK(f.Ext(&f.paintDc, 3, 4, 0, nullptr, pair, 3, dx));
            e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::NativeExt && e[0].ansi == pair && e[0].dxPointer == dx);
            CHECK(LastReason() == L"dx");
            // In a tabbed row the pair is laid out as the two cells it occupies.
            const INT step = 24;
            CHECK(f.Tabbed(&f.paintDc, 10, 4, "\x82\xA0\tA", 4, 1, &step, 10) == MAKELONG(32, 16));
            e = Take();
            CHECK(e.size() == 2 && e[1].text == L"\u3042" L"A" && (e[1].dx == std::vector<INT>{24, 8}));
            f.End(f.hex);
        }
        const std::function<void(Fixture&)> faults[]{
            [](Fixture& f) { HexTest::extra[&f.paintDc].charset = SYMBOL_CHARSET; },
            [](Fixture& f) { HexTest::extra[&f.paintDc].charset = 77; },
            [](Fixture& f) { HexTest::extra[&f.paintDc].charset = -1; },
            [](Fixture& f) { HexTest::extra[&f.paintDc].charset = RUSSIAN_CHARSET; HexTest::failTranslate = true; },
            [](Fixture&) { HexTest::failConvert = true; },
            [](Fixture&) { HexTest::partialConvert = true; },
            [](Fixture&) { HexTest::throwConvert = true; },
        };
        for (const auto& fault : faults)
        {
            // Text that cannot be decoded is drawn by the original, before anything is filled.
            Fixture f; f.Begin(f.hex); fault(f);
            CHECK(f.Address()); f.NativeAddress(L"conversion");
            ResetFaults(); f.End(f.hex);
        }
        {
            Fixture f; f.Begin(f.hex);
            CHECK(f.Ext(&f.paintDc, 1, 32, ETO_OPAQUE | ETO_CLIPPED, &f.cell, nullptr, 9));
            auto e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::NativeExt && !e[0].ansi && e[0].count == 9);
            CHECK(LastReason() == L"conversion" && HexTest::conversions == 0);
            CHECK(f.Ext(&f.paintDc, 1, 32, 0, nullptr, f.addressText, UINT(INT_MAX) + 1u));
            e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::NativeExt && LastReason() == L"conversion");
            CHECK(HexTest::conversions == 0);
            f.End(f.hex);
        }
    }

    void Scope()
    {
        // The exact class name, in any case.
        for (const wchar_t* cls : {L"THexDump", L"thexdump", L"THEXDUMP"})
        {
            Fixture f; f.hex.cls = cls; f.Begin(f.hex);
            CHECK(Attached() && Hd::g_refs == 1 && Hd::g_sessions.size() == 1 && Hd::g_sessions.back().leased);
            CHECK(f.Address()); f.DrewAddress(); CHECK(f.Bytes() == MAKELONG(40, 16)); f.DrewBytes();
            f.End(f.hex); CHECK(Detached() && Hd::g_sessions.empty());
        }
        for (const wchar_t* cls : {L"THexDump2", L"THexDum", L"HexDump", L"TBomeMemo", L"TPanel", L"TImgViewBox", L"",
            L"THexDumpTHexDumpTHexDumpTHexDumpTHexDump"})
        {
            // Other classes are not even recorded.
            Fixture f; f.hex.cls = cls; f.Begin(f.hex);
            CHECK(Detached() && Hd::g_sessions.empty() && HexTest::writes == 0 && HexTest::logFormats.size() == 1);
            CHECK(f.Address()); f.NativeAddress(); CHECK(f.Bytes() == MAKELONG(77, 13)); f.NativeBytes();
            f.End(f.hex);
        }
        for (int which = 0; which < 5; ++which)
        {
            // BeginPaint from the EXE, a window of the EXE, Full mode, backdrop allowed.
            Fixture f;
            HMODULE beginCaller = &f.exe;
            switch (which)
            {
            case 0: beginCaller = &f.otherModule; break;
            case 1: HexTest::instances[&f.hex] = &f.otherModule; break;
            case 2: Test::full = false; break;
            case 3: f.hex.backdrop = false; break;
            case 4: GlassCore::ModuleHandler::g_entryPointModule = nullptr; break;
            }
            f.Begin(f.hex, beginCaller);
            CHECK(Detached() && Hd::g_refs == 0 && HexTest::writes == 0);
            CHECK(Hd::g_sessions.size() == 1 && !Hd::g_sessions.back().leased); // recorded, not leased
            Test::full = true; f.hex.backdrop = true; HexTest::instances.clear();
            GlassCore::ModuleHandler::g_entryPointModule = &f.exe;
            CHECK(f.Address()); f.NativeAddress(); CHECK(f.Bytes() == MAKELONG(77, 13)); f.NativeBytes();
            CHECK(TextLogs() == 0 && HexTest::trampolineCalls == 0);
            f.End(f.hex); CHECK(Hd::g_sessions.empty());
        }
        {
            // A BeginPaint that is not ours, opened on top, bars the text path and
            // its End does not end our session.
            Fixture f; f.Begin(f.hex); f.Begin(f.hex, &f.otherModule);
            CHECK(Hd::g_sessions.size() == 2 && Hd::g_refs == 1 && Attached());
            CHECK(f.Address()); f.NativeAddress(); CHECK(HexTest::trampolineCalls == 1 && TextLogs() == 0);
            f.End(f.hex);
            CHECK(Hd::g_sessions.size() == 1 && Attached() && Hd::g_refs == 1);
            // Another window painting in between is not ours to record or to end.
            f.Begin(f.other); f.End(f.other);
            CHECK(Hd::g_sessions.size() == 1 && Attached());
            Test::topWindow = &f.hex;
            CHECK(f.Address()); f.DrewAddress();
            f.End(f.hex); CHECK(Detached() && Hd::g_sessions.empty());
        }
        {
            // Two viewers painting inside one another: the text belongs to the top one.
            Fixture f; f.Begin(f.hex); f.Begin(f.hex2);
            CHECK(Attached() && Hd::g_refs == 2 && HexTest::writes == 1 && HexTest::attaches == 2);
            CHECK(f.Address(&f.paintDc2)); f.DrewAddress(&f.paintDc2);
            CHECK(f.Address()); f.NativeAddress(L"dc");
            f.End(f.hex2);
            CHECK(Attached() && Hd::g_refs == 1 && HexTest::writes == 1);
            Test::topWindow = &f.hex;
            CHECK(f.Address()); f.DrewAddress();
            CHECK(f.Address(&f.paintDc2)); f.NativeAddress(L"dc", &f.paintDc2);
            f.End(f.hex); CHECK(Detached() && HexTest::writes == 2);
        }
    }

    void Isolation()
    {
        {
            Fixture f; f.Begin(f.hex);
            // Only calls made by the EXE; anyone else passes straight through.
            HexTest::caller = &f.otherModule;
            CHECK(f.Address()); f.NativeAddress(); CHECK(f.Bytes() == MAKELONG(77, 13)); f.NativeBytes();
            HexTest::caller = nullptr;
            CHECK(f.Address()); f.NativeAddress();
            CHECK(TextLogs() == 0 && HexTest::trampolineCalls == 3);
            HexTest::caller = &f.exe;
            // Another window's DC, and this window's DC that the dispatcher does not track.
            CHECK(f.Address(&f.foreignDc)); f.NativeAddress(L"dc", &f.foreignDc);
            CHECK(f.Bytes(&f.foreignDc) == MAKELONG(77, 13)); f.NativeBytes(L"dc", &f.foreignDc);
            CHECK(f.Address(&f.canvasDc)); f.NativeAddress(L"dc", &f.canvasDc);
            Test::stackOwners[&f.foreignDc] = &f.hex; // tracked for us, yet owned by another window
            CHECK(f.Address(&f.foreignDc)); f.NativeAddress(L"dc", &f.foreignDc);
            Test::stackOwners.erase(&f.foreignDc);
            // A canvas DC of this window taken with GetDC while it paints.
            Test::stackOwners[&f.canvasDc] = &f.hex;
            CHECK(f.Address(&f.canvasDc)); f.DrewAddress(&f.canvasDc);
            CHECK(f.Bytes(&f.canvasDc) == MAKELONG(40, 16)); f.DrewBytes(&f.canvasDc);
            // A memory DC: only one the dispatcher saw this window create.
            CHECK(f.Address(&f.memoryDc)); f.NativeAddress(L"dc", &f.memoryDc);
            Test::stackOwners[&f.memoryDc] = &f.other;
            CHECK(f.Address(&f.memoryDc)); f.NativeAddress(L"dc", &f.memoryDc);
            Test::stackOwners[&f.memoryDc] = &f.hex;
            CHECK(f.Address(&f.memoryDc)); f.DrewAddress(&f.memoryDc);
            CHECK(f.Bytes(&f.memoryDc) == MAKELONG(40, 16)); f.DrewBytes(&f.memoryDc);
            // A handle this window once had on the stack, since recorded for another one.
            HexTest::newestOwner[&f.memoryDc] = &f.other;
            CHECK(f.Address(&f.memoryDc)); f.NativeAddress(L"dc", &f.memoryDc);
            CHECK(f.Bytes(&f.memoryDc) == MAKELONG(77, 13)); f.NativeBytes(L"dc", &f.memoryDc);
            HexTest::newestOwner[&f.memoryDc] = nullptr;
            CHECK(f.Address(&f.memoryDc)); f.NativeAddress(L"dc", &f.memoryDc);
            HexTest::newestOwner.clear();
            // No DC at all.
            CHECK(f.Ext(nullptr, 1, 32, ETO_OPAQUE | ETO_CLIPPED, &f.cell, f.addressText, 9));
            auto e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::NativeExt && !e[0].dc && LastReason() == L"dc");
            CHECK(f.Tabbed(nullptr, 81, 32, f.bytesText, 5, 1, &f.stop, 81) == MAKELONG(77, 13));
            e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::NativeTabbed && !e[0].dc && LastReason() == L"dc");
            // Another window's drawing session opened above ours.
            Test::topWindow = &f.other;
            CHECK(f.Address()); f.NativeAddress(L"dc");
            Test::topWindow = &f.hex;
            // A mapping mode whose units are not pixels.
            HexTest::extra[&f.paintDc].map = MM_ANISOTROPIC;
            CHECK(f.Address()); f.NativeAddress(L"map");
            CHECK(f.Bytes() == MAKELONG(77, 13)); f.NativeBytes(L"map");
            HexTest::extra[&f.paintDc].map = MM_TEXT;
            // Mode, window and backdrop are checked at the moment of the call.
            Test::full = false; CHECK(f.Address()); f.NativeAddress(L"mode"); Test::full = true;
            f.hex.backdrop = false; CHECK(f.Bytes() == MAKELONG(77, 13)); f.NativeBytes(L"mode"); f.hex.backdrop = true;
            f.hex.valid = false; CHECK(f.Address()); f.NativeAddress(L"mode"); f.hex.valid = true;
            CHECK(f.Address()); f.DrewAddress();
            f.End(f.hex);
        }
        {
            // Sessions belong to their thread: the same call from another one is native.
            Fixture f; f.Begin(f.hex);
            std::thread worker([&] {
                HexTest::caller = &f.exe; SetLastError(0x29);
                CHECK(ExtTextOutA(&f.paintDc, 1, 32, ETO_OPAQUE | ETO_CLIPPED, &f.cell, f.addressText, 9, nullptr));
                CHECK(HexTest::events.size() == 1 && HexTest::events[0].kind == Kind::NativeExt);
                CHECK(HexTest::events[0].entryError == 0x29 && HexTest::trampolineCalls == 1 && GetLastError() == 0x51);
                CHECK(TabbedTextOutA(&f.paintDc, 81, 32, f.bytesText, 5, 1, &f.stop, 81) == MAKELONG(77, 13));
                CHECK(HexTest::events.size() == 2 && HexTest::events[1].kind == Kind::NativeTabbed);
                CHECK(HexTest::logFormats.empty());
            });
            worker.join();
            CHECK(HexTest::events.empty());
            CHECK(f.Address()); f.DrewAddress();
            f.End(f.hex);
        }
        {
            // While a row is being drawn, a nested call from the EXE is the original's.
            Fixture f; f.Begin(f.hex);
            int nested = 0;
            HexTest::duringRender = [&] {
                if (nested++) return;
                CHECK(Hd::g_inside);
                const DWORD saved = GetLastError();
                SetLastError(0x77);
                CHECK(ExtTextOutA(&f.paintDc, 2, 3, ETO_CLIPPED, &f.cell, "zz", 2, nullptr));
                CHECK(TabbedTextOutA(&f.paintDc, 2, 3, "z\tz", 3, 1, &f.stop, 2) == MAKELONG(77, 13));
                SetLastError(saved);
            };
            CHECK(f.Address());
            const auto e = Take();
            CHECK(e.size() == 4 && e[0].kind == Kind::Black && e[1].kind == Kind::Glyphs);
            CHECK(e[2].kind == Kind::NativeExt && e[2].x == 2 && e[2].entryError == 0x77);
            CHECK(e[3].kind == Kind::NativeTabbed && nested == 1 && HexTest::trampolineCalls == 2);
            CHECK(GetLastError() == 0x29 && !Hd::g_inside);
            HexTest::duringRender = {};
            CHECK(f.Address()); f.DrewAddress();
            f.End(f.hex);
        }
    }

    void Options()
    {
        Fixture f; f.Begin(f.hex);
        // Glyph indices and paired offsets are not ANSI text.
        for (UINT options : {UINT(ETO_GLYPH_INDEX), UINT(ETO_PDY), UINT(ETO_OPAQUE | ETO_GLYPH_INDEX), UINT(ETO_CLIPPED | ETO_PDY)})
        {
            CHECK(f.Ext(&f.paintDc, 1, 32, options, &f.cell, f.addressText, 9));
            const auto e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::NativeExt && e[0].options == options && e[0].rectPointer == &f.cell);
            CHECK(LastReason() == L"options");
        }
        // The renderer's own DC has the default alignment and no current position:
        // text placed by anything else stays with GDI, before anything is filled.
        for (UINT align : {TA_UPDATECP, TA_RIGHT, TA_CENTER, TA_BOTTOM, TA_BASELINE, TA_RIGHT | TA_BASELINE,
            TA_UPDATECP | TA_RIGHT, UINT(0x100)})
        {
            HexTest::extra[&f.paintDc].align = align;
            CHECK(f.Address()); f.NativeAddress(L"align");
            // A background alone does not depend on where text would go.
            CHECK(f.Ext(&f.paintDc, 1, 32, ETO_OPAQUE, &f.cell, nullptr, 0));
            const auto e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::Black && Same(e[0].rect, f.cell));
        }
        HexTest::extra[&f.paintDc].align = TA_LEFT | TA_TOP;
        {
            // Character spacing set on the DC: only explicit advances reproduce it.
            HexTest::extra[&f.paintDc].spacing = 2;
            CHECK(f.Address()); f.NativeAddress(L"spacing");
            const INT dx[]{10, 10, 10, 10, 10, 10, 10, 10, 10};
            CHECK(f.Ext(&f.paintDc, 1, 32, ETO_OPAQUE | ETO_CLIPPED, &f.cell, f.addressText, 9, dx));
            auto spaced = Take();
            CHECK(spaced.size() == 2 && spaced[1].kind == Kind::Glyphs && spaced[1].dxPointer == dx);
            // A tabbed row measures its advances on the caller's DC, spacing included.
            CHECK(f.Bytes() == MAKELONG(44, 16));
            spaced = Take();
            CHECK(spaced.size() == 2 && spaced[1].text == L"4D5A" && (spaced[1].dx == std::vector<INT>{10, 14, 10, 10}));
            HexTest::extra[&f.paintDc].spacing = 0;
        }
        // Nothing to draw and nothing to fill.
        CHECK(f.Ext(&f.paintDc, 1, 32, 0, nullptr, f.addressText, 0));
        auto e = Take(); CHECK(e.size() == 1 && e[0].kind == Kind::NativeExt && LastReason() == L"empty");
        CHECK(f.Ext(&f.paintDc, 1, 32, ETO_CLIPPED, &f.cell, nullptr, 0));
        e = Take(); CHECK(e.size() == 1 && e[0].kind == Kind::NativeExt && LastReason() == L"empty");
        // Plain text without a rectangle.
        CHECK(f.Ext(&f.paintDc, 7, 9, 0, nullptr, f.addressText, 9));
        e = Take();
        CHECK(e.size() == 1 && e[0].kind == Kind::Glyphs && e[0].x == 7 && e[0].y == 9 && e[0].options == 0);
        CHECK(!e[0].rectPointer && e[0].text == L"00000010:");
        // A rectangle that is neither clipping nor opaque is passed as given.
        CHECK(f.Ext(&f.paintDc, 7, 9, 0, &f.cell, f.addressText, 9));
        e = Take(); CHECK(e.size() == 1 && e[0].kind == Kind::Glyphs && e[0].rectPointer == &f.cell && e[0].options == 0);
        // Only the opaque bit is consumed; every other option reaches the renderer.
        CHECK(f.Ext(&f.paintDc, 7, 9, ETO_OPAQUE | ETO_CLIPPED | ETO_RTLREADING, &f.cell, f.addressText, 9));
        e = Take();
        CHECK(e.size() == 2 && e[0].kind == Kind::Black && e[1].options == (ETO_CLIPPED | ETO_RTLREADING));
        CHECK(f.Ext(&f.paintDc, 7, 9, ETO_CLIPPED, nullptr, f.addressText, 9));
        e = Take(); CHECK(e.size() == 1 && e[0].kind == Kind::Glyphs && e[0].options == ETO_CLIPPED && !e[0].rectPointer);
        // Text that starts left of its clipping rectangle keeps its own origin.
        CHECK(f.Ext(&f.paintDc, -40, 32, ETO_OPAQUE | ETO_CLIPPED, &f.cell, f.addressText, 9));
        e = Take(); CHECK(e.size() == 2 && e[1].x == -40 && e[1].rectPointer == &f.cell);
        CHECK(HexTest::trampolineCalls == 15);
        f.End(f.hex);
    }

    void Leases()
    {
        {
            // First fixture of the process: the lease generation is still the one
            // an unleased session carries, so only its flag keeps its End harmless.
            Fixture f;
            CHECK(Hd::g_generation == 0);
            f.Begin(f.hex); f.Begin(f.hex, &f.otherModule);
            CHECK(Hd::g_sessions.size() == 2 && Hd::g_refs == 1);
            f.End(f.hex);
            CHECK(Hd::g_sessions.size() == 1 && Attached() && Hd::g_refs == 1);
            f.End(f.hex); CHECK(Detached() && Hd::g_sessions.empty());
            // Ends without a Begin, or for a window we never recorded, change nothing.
            f.End(f.hex); f.End(f.other); f.Provider().onEnd(nullptr, nullptr);
            CHECK(Detached() && Hd::g_refs == 0 && HexTest::writes == 2);
            // A release that matches no lease does not underflow, and nothing is
            // leased once Shutdown has begun, whoever asks.
            Hd::Release(Hd::g_generation);
            CHECK(Hd::g_refs == 0 && HexTest::writes == 2);
            Hd::Shutdown();
            unsigned generation = 0;
            CHECK(!Hd::Acquire(generation) && Hd::g_refs == 0 && Detached());
            GlassCore::PaintDispatcher::hexProviders.clear(); Hd::Startup();
        }
        for (int which = 0; which < 4; ++which)
        {
            // A transaction that fails installs neither detour and leases nothing.
            Fixture f;
            switch (which)
            {
            case 0: HexTest::failBegin = true; break;
            case 1: HexTest::failAttachAt = 1; break;
            case 2: HexTest::failAttachAt = 2; break;
            case 3: HexTest::failCommit = true; break;
            }
            f.Begin(f.hex);
            CHECK(Detached() && Hd::g_refs == 0 && HexTest::attaches == 0 && HexTest::writes == 1);
            CHECK(Hd::g_sessions.size() == 1 && !Hd::g_sessions.back().leased);
            CHECK(f.Address()); f.NativeAddress(); CHECK(HexTest::trampolineCalls == 0);
            f.End(f.hex);
            CHECK(HexTest::detachCalls == 0 && HexTest::writes == 1);
            ResetFaults();
            f.Begin(f.hex); CHECK(Attached() && Hd::g_refs == 1); // the next paint simply tries again
            CHECK(f.Address()); f.DrewAddress(); f.End(f.hex); CHECK(Detached());
        }
        for (int which = 0; which < 3; ++which)
        {
            // A failed last Detach keeps both trampolines; calls pass through them
            // and the next session reuses the attachment.
            Fixture f; f.Begin(f.hex);
            switch (which)
            {
            case 0: HexTest::failDetachAt = 1; break;
            case 1: HexTest::failDetachAt = 2; break;
            case 2: HexTest::failCommit = true; break;
            }
            f.End(f.hex);
            CHECK(Attached() && Hd::g_attached && Hd::g_refs == 0 && Hd::g_sessions.empty());
            CHECK(Hd::g_actualExtTextOut == kTrampolineExt && Hd::g_actualTabbedTextOut == kTrampolineTabbed);
            CHECK(f.Address()); f.NativeAddress(); CHECK(HexTest::trampolineCalls == 1);
            CHECK(f.Bytes() == MAKELONG(77, 13)); f.NativeBytes(); CHECK(HexTest::trampolineCalls == 2);
            ResetFaults();
            const int writes = HexTest::writes;
            f.Begin(f.hex); CHECK(Hd::g_refs == 1 && HexTest::writes == writes && HexTest::attaches == 2);
            CHECK(f.Address()); f.DrewAddress();
            f.End(f.hex); CHECK(Detached() && HexTest::detaches == 2);
        }
        {
            // Shutdown retries the Detach; a Startup in between must not resolve
            // the exports again over live trampolines.
            Fixture f; f.Begin(f.hex);
            HexTest::failCommit = true; f.End(f.hex);
            Hd::Shutdown(); CHECK(Attached() && Hd::g_attached && Hd::g_refs == 0);
            const int resolved = HexTest::resolutions;
            GlassCore::PaintDispatcher::hexProviders.clear();
            Hd::Startup();
            CHECK(HexTest::resolutions == resolved && Hd::g_actualExtTextOut == kTrampolineExt);
            ResetFaults();
            const int writes = HexTest::writes;
            f.Begin(f.hex); CHECK(HexTest::writes == writes && Hd::g_refs == 1);
            CHECK(f.Address()); f.DrewAddress(); f.End(f.hex); CHECK(Detached());
            Hd::Shutdown(); CHECK(Detached());
            GlassCore::PaintDispatcher::hexProviders.clear();
            Hd::Startup(); CHECK(HexTest::resolutions == resolved + 2 && Detached());
        }
        for (int which = 0; which < 4; ++which)
        {
            // Without both exports there is nothing to lease.
            Fixture f;
            Hd::Shutdown(); GlassCore::PaintDispatcher::hexProviders.clear();
            if (which == 0) HexTest::gdiAvailable = false;
            if (which == 1) HexTest::userAvailable = false;
            if (which == 2) HexTest::extExport = false;
            if (which == 3) HexTest::tabbedExport = false;
            Hd::Startup();
            f.Begin(f.hex);
            CHECK(HexTest::writes == 0 && Hd::g_refs == 0 && !HexTest::extDetour && !HexTest::tabbedDetour);
            CHECK(f.Address()); f.NativeAddress(); CHECK(f.Bytes() == MAKELONG(77, 13)); f.NativeBytes();
            f.End(f.hex);
            // A detour entered without its original calls nothing.
            HexTest::exportCalls = 0;
            if (which == 0 || which == 2)
            {
                SetLastError(0x29);
                CHECK(!Hd::g_actualExtTextOut && !Hd::RowExtTextOutA(&f.paintDc, 1, 2, 0, nullptr, "a", 1, nullptr));
            }
            else
            {
                SetLastError(0x29);
                CHECK(!Hd::g_actualTabbedTextOut && !Hd::RowTabbedTextOutA(&f.paintDc, 1, 2, "a", 1, 1, &f.stop, 1));
            }
            CHECK(Take().empty() && HexTest::exportCalls == 0 && GetLastError() == 0x29);
            Hd::Shutdown(); GlassCore::PaintDispatcher::hexProviders.clear();
            HexTest::gdiAvailable = HexTest::userAvailable = HexTest::extExport = HexTest::tabbedExport = true;
            Hd::Startup();
        }
        {
            // Startup is idempotent; a provider is registered once per start.
            Fixture f;
            const int resolved = HexTest::resolutions;
            CHECK(resolved == 2);
            Hd::Startup();
            CHECK(GlassCore::PaintDispatcher::hexProviders.size() == 1 && HexTest::resolutions == resolved);
            // Before Startup and after Shutdown the provider records nothing.
            f.Begin(f.hex); Hd::Shutdown();
            CHECK(Detached() && Hd::g_refs == 0 && Hd::g_sessions.empty() && HexTest::detaches == 2);
            CHECK(f.Address()); f.NativeAddress();
            f.End(f.hex); f.Begin(f.hex);
            CHECK(Hd::g_sessions.empty() && HexTest::writes == 2);
            f.End(f.hex);
            GlassCore::PaintDispatcher::hexProviders.clear();
            Hd::Startup();
        }
        for (bool restart : {false, true})
        {
            // A session left open on another thread across Shutdown belongs to the
            // old lease generation: its End releases nothing, restarted or not.
            Fixture f;
            const auto provider = f.Provider();
            std::atomic<int> stage{0};
            std::thread worker([&] {
                provider.onBegin(&f.hex, &f.exe);
                stage = 1; while (stage != 2) std::this_thread::yield();
                provider.onEnd(&f.hex, &f.exe);
                CHECK(Hd::g_sessions.empty());
                stage = 3;
            });
            while (stage != 1) std::this_thread::yield();
            CHECK(Attached() && Hd::g_refs == 1);
            Hd::Shutdown(); CHECK(Detached() && Hd::g_refs == 0);
            GlassCore::PaintDispatcher::hexProviders.clear();
            if (restart) { Hd::Startup(); f.Begin(f.hex2); CHECK(Attached() && Hd::g_refs == 1); }
            const int writes = HexTest::writes;
            stage = 2; worker.join(); CHECK(stage == 3);
            CHECK(HexTest::writes == writes && Hd::g_refs == (restart ? 1u : 0u));
            if (restart) { CHECK(f.Address(&f.paintDc2)); f.DrewAddress(&f.paintDc2); f.End(f.hex2); CHECK(Detached()); }
            else Hd::Startup();
        }
    }

    void Failures()
    {
        {
            // What the original returns, and the error it leaves, are the caller's.
            Fixture f; f.Begin(f.hex);
            HexTest::nativeExtResult = FALSE; HexTest::nativeTabbedResult = 0;
            CHECK(f.Address(&f.foreignDc) == FALSE);
            auto e = Take(); CHECK(e.size() == 1 && e[0].kind == Kind::NativeExt && GetLastError() == 0x51);
            CHECK(f.Bytes(&f.foreignDc) == 0);
            e = Take(); CHECK(e.size() == 1 && e[0].kind == Kind::NativeTabbed && GetLastError() == 0x51);
            // An original that leaves LastError alone leaves the caller's entry value.
            HexTest::nativeLeavesError = true;
            CHECK(f.Address(&f.foreignDc) == FALSE); CHECK(Take().size() == 1 && GetLastError() == 0x29);
            CHECK(f.Bytes(&f.foreignDc) == 0); CHECK(Take().size() == 1 && GetLastError() == 0x29);
            // Outside our scope the same holds with no session logic at all.
            HexTest::caller = &f.otherModule;
            CHECK(f.Address() == FALSE); CHECK(Take().size() == 1 && GetLastError() == 0x29);
            HexTest::caller = &f.exe;
            f.End(f.hex);
        }
        for (bool thrown : {false, true})
        {
            // A renderer failure after the background: one original call, its result.
            Fixture f; f.Begin(f.hex);
            if (thrown) HexTest::throwRenderer = true; else HexTest::renderResult = E_FAIL;
            HexTest::nativeExtResult = FALSE;
            CHECK(f.Address() == FALSE);
            const auto e = Take();
            CHECK(e.size() == 3 && e[0].kind == Kind::Black && e[1].kind == Kind::Glyphs && e[2].kind == Kind::NativeExt);
            CHECK(e[2].options == (ETO_OPAQUE | ETO_CLIPPED) && e[2].rectPointer == &f.cell && e[2].ansi == f.addressText);
            CHECK(e[2].entryError == 0x29 && GetLastError() == 0x51 && LastReason() == L"renderer");
            f.Unchanged(f.paintDc);
            ResetFaults();
            CHECK(f.Address() == TRUE); f.DrewAddress();
            f.End(f.hex);
        }
        {
            // A drawn call succeeds with the caller's LastError, whatever the helpers set.
            Fixture f; f.Begin(f.hex);
            SetLastError(0); HexTest::clock += 1000;
            CHECK(ExtTextOutA(&f.paintDc, 1, 32, ETO_OPAQUE | ETO_CLIPPED, &f.cell, f.addressText, 9, nullptr));
            CHECK(GetLastError() == 0 && Take().size() == 2);
            SetLastError(0xabcd);
            CHECK(TabbedTextOutA(&f.paintDc, 81, 32, f.bytesText, 5, 1, &f.stop, 81) == MAKELONG(40, 16));
            CHECK(GetLastError() == 0xabcd && Take().size() == 2);
            f.End(f.hex);
        }
    }

    void Diagnostics()
    {
        {
            // One metadata line per in-session EXE call, naming why it stayed native.
            Fixture f;
            CHECK(HexTest::logFormats.size() == 1 && HexTest::logFormats[0].find(L"] resolve gdi32=") != std::wstring::npos);
            f.Begin(f.hex);
            CHECK(HexTest::logFormats.size() == 3); // resolve, detour, Begin
            CHECK(HexTest::logFormats[1].find(L"] detour api=ExtTextOutA+TabbedTextOutA enable=1") != std::wstring::npos);
            CHECK(HexTest::logFormats[2].find(L"] Begin hwnd=") != std::wstring::npos && HexTest::logStrings.empty());
            for (const auto& format : HexTest::logFormats)
                CHECK(format.rfind(L"[GlassCore][RestoratorHexDump] ", 0) == 0);
            CHECK(f.Address()); CHECK(LastReason() == L"none");
            CHECK(HexTest::logStrings.size() == 2 && HexTest::logStrings[0] == L"ExtTextOutA");
            CHECK(f.Bytes()); CHECK(LastReason() == L"none");
            CHECK(HexTest::logStrings.size() == 4 && HexTest::logStrings[2] == L"TabbedTextOutA");
            CHECK(TextLogs() == 2);
            // Calls that are not ours are not logged at all.
            HexTest::caller = &f.otherModule; CHECK(f.Address()); CHECK(f.Bytes()); HexTest::caller = &f.exe;
            CHECK(TextLogs() == 2);
            f.End(f.hex);
            CHECK(HexTest::logFormats.back().find(L"] detour api=ExtTextOutA+TabbedTextOutA enable=0") != std::wstring::npos);
            CHECK(f.Address()); CHECK(TextLogs() == 2);
        }
        {
            // Sixteen lines a second per channel; the text of a row is never logged.
            Fixture f; f.Begin(f.hex);
            for (int i = 0; i < 40; ++i)
            {
                SetLastError(0x29);
                CHECK(ExtTextOutA(&f.paintDc, 1, 32, ETO_OPAQUE | ETO_CLIPPED, &f.cell, f.addressText, 9, nullptr));
                CHECK(TabbedTextOutA(&f.paintDc, 81, 32, f.bytesText, 5, 1, &f.stop, 81) == MAKELONG(40, 16));
            }
            CHECK(TextLogs() == 16 && HexTest::Count(Kind::Glyphs) == 80); // unlogged calls are drawn all the same
            HexTest::clock += 999;
            CHECK(ExtTextOutA(&f.foreignDc, 1, 32, ETO_OPAQUE | ETO_CLIPPED, &f.cell, f.addressText, 9, nullptr));
            CHECK(TextLogs() == 16);
            HexTest::clock += 1;
            CHECK(ExtTextOutA(&f.foreignDc, 1, 32, ETO_OPAQUE | ETO_CLIPPED, &f.cell, f.addressText, 9, nullptr));
            CHECK(TextLogs() == 17 && LastReason() == L"dc");
            f.End(f.hex);
            for (const auto& s : HexTest::logStrings)
                CHECK(s == L"ExtTextOutA" || s == L"TabbedTextOutA" || s == L"none" || s == L"dc");
        }
    }
}

int main(int argc, char** argv)
{
    const std::string name = argc == 2 ? argv[1] : "";
    const std::pair<const char*, void(*)()> cases[]{
        {"rows", Rows}, {"tabs", Tabs}, {"tab-gates", TabGates}, {"background", Background}, {"ansi", Ansi},
        {"scope", Scope}, {"isolation", Isolation}, {"options", Options}, {"leases", Leases},
        {"failures", Failures}, {"diagnostics", Diagnostics},
    };
    for (const auto& [id, run] : cases)
    {
        if (name != id) continue;
        try { run(); return 0; }
        catch (const std::exception& e) { std::cerr << id << ": " << e.what() << "\n"; return 1; }
    }
    std::cerr << "unknown case\n";
    return 2;
}
