// Compile the complete production unit, including the real providers, lease
// bookkeeping, W/A text hooks, fill hooks and premultiplied-alpha compositor.
#include "richedit_test_shim.h"
#include "../../GlassCore/src/Hooks/EditHooks.cpp"
#include <stdexcept>

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)
namespace
{
    using namespace GlassCore::EditControl;
    using Kind = GlassShared::GlassControlKind;
    constexpr size_t Exe = 0, User = 1, Re20 = 2, Re50 = 3, Full = 4, Gdi = 5;
    const std::array richKinds{Kind::RichEdit20W, Kind::WinForms_RichEdit20W, Kind::RichEdit50W, Kind::WinForms_RichEdit50W};
    constexpr COLORREF custom = RGB(200, 80, 40);
    constexpr Pixel tint{16, 31, 78, 100};
    struct Context
    {
        std::array<TestModule, 6> modules;
        TestWindow window;
        TestDC dc, memory, foreign;
        TestBrush brush{custom};
        RECT rect{4, 5, 64, 45};
        Context()
        {
            RichTest::modules.clear(); RichTest::stackOwners.clear();
            RichTest::host = RichTest::topWindow = RichTest::focus = nullptr;
            RichTest::currentGetDc = nullptr; RichTest::caller = nullptr;
            RichTest::full = true;
            RichTest::windowColor = 0xffffff; RichTest::faceColor = 0xf0f0f0;
            RichTest::highlightColor = 0xd77800;
            RichTest::fills = RichTest::pats = RichTest::blacks = RichTest::blends = 0;
            RichTest::nativeTexts = RichTest::inkTexts = RichTest::originalTag = 0;
            RichTest::failDc = RichTest::failBitmap = RichTest::failSelect = false;
            RichTest::failBlacken = RichTest::failBlend = RichTest::failInk = false;
            RichTest::inkBlend = 217;
            for (auto& m : modules)
                m.originals = {
                    {"FillRect", reinterpret_cast<void*>(&::FillRect)},
                    {"PatBlt", reinterpret_cast<void*>(&::PatBlt)},
                    {"ExtTextOutW", reinterpret_cast<void*>(&::ExtTextOutW)},
                    {"ExtTextOutA", reinterpret_cast<void*>(&::ExtTextOutA)},
                };
            const std::array names{L"probe.exe", L"user32.dll", L"riched20.dll", L"msftedit.dll", L"gdi32full.dll", L"gdi32.dll"};
            for (size_t i = 0; i < names.size(); ++i) RichTest::modules[names[i]] = &modules[i];
            GlassCore::ModuleHandler::g_entryPointModule = &modules[Exe];
            dc.owner = &window; dc.background = custom; dc.brush = &brush;
            memory.background = custom; memory.brush = &brush;
            Startup();
        }
        ~Context()
        {
            RichTest::duringBlend = {}; RichTest::duringInk = {};
            RichTest::failDc = RichTest::failBitmap = RichTest::failSelect = false;
            RichTest::failBlend = RichTest::failBlacken = false;
            Shutdown();
            assert(RichTest::liveDcs == 0 && RichTest::liveBitmaps == 0 && RichTest::liveBrushes == 0);
        }
        void Begin(size_t caller = Re50, bool bind = true)
        {
            OnBeginPaintEdit(&window, &modules[caller]);
            RichTest::host = RichTest::topWindow = &window;
            if (bind) Bind();
        }
        void Bind()
        {
            PAINTSTRUCT ps;
            CHECK(!OnEditAfterBeginPaint(&window, &dc, &ps, &modules[Re50]));
            CHECK(ps.fErase == 1); // binding alone must not claim an erase
            RichTest::stackOwners[&dc] = &window;
        }
        void End() { OnEndPaintEdit(&window, &modules[Exe]); }
        void Get()
        {
            RichTest::currentGetDc = &dc;
            OnGetDCEdit(&window, &modules[Re50]);
            RichTest::currentGetDc = nullptr;
            RichTest::host = RichTest::topWindow = &window;
        }
        void Release() { OnReleaseDCEdit(&window, &modules[Exe]); }
        void CheckZeroRefs()
        {
            for (const auto& m : modules) for (const auto& [name, ref] : m.refs)
            { (void)name; CHECK(ref == 0); }
        }
        template <typename Fn, typename... Args> auto Call(size_t mod, const char* name, Fn, Args... args)
        {
            const auto old = std::exchange(RichTest::caller, &modules[mod]);
            auto leave = wil::scope_exit([old] { RichTest::caller = old; });
            CHECK(modules[mod].hooks.count(name));
            return reinterpret_cast<Fn>(modules[mod].hooks.at(name))(args...);
        }
        int Fill(size_t mod = Re50, HDC target = nullptr, HBRUSH b = nullptr, const RECT* r = nullptr)
        { return Call(mod, "FillRect", &::FillRect, target ? target : &dc, r ? r : &rect, b ? b : &brush); }
        BOOL Pat(DWORD rop = PATCOPY, HDC target = nullptr)
        { return Call(Re50, "PatBlt", &::PatBlt, target ? target : &dc, 4, 5, 60, 40, rop); }
        BOOL Text(UINT options = ETO_OPAQUE, UINT count = 0, size_t mod = Re50, HDC target = nullptr)
        { return Call(mod, "ExtTextOutW", &::ExtTextOutW, target ? target : &dc, 4, 5, options, &rect, L"sample", count, nullptr); }
        Pixel At(int x = 5, int y = 6) const { return dc.surface.pixels[y*dc.surface.width+x]; }
        void CheckTint() const
        {
            CHECK(RichTest::blendPixel == tint && At() == tint);
            CHECK(RichTest::blend.BlendOp == AC_SRC_OVER && RichTest::blend.AlphaFormat == AC_SRC_ALPHA);
            CHECK(RichTest::blend.SourceConstantAlpha == 255); // no second multiplication by 100
            CHECK(!g_inRichEditBackground && !g_inRichEditText);
        }
    };

    void AllKindsAndNativeApis()
    {
        for (const auto kind : richKinds)
        {
            Context c; c.window.kind = kind;
            if (kind == Kind::RichEdit50W) c.window.cls = L"TRichEdit";
            c.Begin();
            CHECK(!g_activeEditBuffer); // RichEdit never borrows the TMemo DIB
            for (size_t m : {Re20, Re50, Full})
            {
                CHECK(c.modules[m].hooks.size() == 4);
                CHECK(c.Fill(m) == 1); c.CheckTint();
                CHECK(c.Text(ETO_OPAQUE, 0, m)); c.CheckTint();
            }
            CHECK(c.Pat()); c.CheckTint();
            CHECK(RichTest::fills == 0 && RichTest::nativeTexts == 0 && RichTest::inkTexts == 0);
            c.End(); c.CheckZeroRefs();
        }
    }
    void ColorsAndSelection()
    {
        Context c; c.Begin();
        for (COLORREF system : {RichTest::windowColor, RichTest::faceColor})
        {
            TestBrush b{system}; const int blends = RichTest::blends;
            CHECK(c.Fill(Re50, nullptr, &b) == 1);
            CHECK(c.At() == Pixel{} && RichTest::blends == blends);
            c.dc.background = system; CHECK(c.Text()); CHECK(RichTest::blends == blends);
        }
        // Custom black is still a tint with alpha=100, unless an OS system
        // background is itself black. Plain Edit's old black rule is separate.
        TestBrush black{0}; CHECK(c.Fill(Re50, nullptr, &black) == 1);
        CHECK(c.At() == (Pixel{0, 0, 0, 100}));
        RichTest::windowColor = 0; const int blends = RichTest::blends;
        CHECK(c.Fill(Re50, nullptr, &black) == 1);
        CHECK(c.At() == Pixel{} && RichTest::blends == blends);
        TestBrush selected{GetSysColor(COLOR_HIGHLIGHT)};
        // The same RGB used as an ordinary, unselected client tint is alpha100.
        CHECK(c.Fill(Re50, nullptr, &selected) == 1);
        CHECK(c.At()[3] == 100);
        c.window.selected = CHARRANGE{1, 4};
        const int selectionBlends = RichTest::blends;
        CHECK(c.Fill(Re50, nullptr, &selected) == 7);
        c.dc.brush = &selected; CHECK(c.Pat()); CHECK(RichTest::blends == selectionBlends);
        c.dc.background = selected.lb.lbColor;
        CHECK(c.Text(ETO_OPAQUE, 6));
        CHECK(c.At()[3] == 255 && RichTest::blends == selectionBlends && RichTest::inkTexts == 1);
        CHECK((RichTest::inkOptions & ETO_OPAQUE) == 0);
        c.End();
    }
    void EmptyAndBrushSources()
    {
        Context c; c.Begin(Exe);
        CHECK(c.modules[Exe].hooks.size() == 2); // background only, no arbitrary EXE text
        CHECK(c.Fill(Exe) == 1); c.CheckTint(); // empty client: no text API required
        c.dc.background = RGB(1, 2, 3); // FillRect must use its brush, not GetBkColor
        CHECK(c.Fill() == 1); c.CheckTint();
        c.dc.brush = &stockDcBrush; c.dc.dcBrush = custom;
        CHECK(c.Pat()); c.CheckTint();
        CHECK(c.Fill(Re50, nullptr, &stockDcBrush) == 1); c.CheckTint();
        const auto pseudo = reinterpret_cast<HBRUSH>(ULONG_PTR(COLOR_WINDOW + 1));
        CHECK(c.Fill(Re50, nullptr, pseudo) == 1); CHECK(c.At() == Pixel{});
        TestBrush pattern{custom, BS_HATCHED};
        const int blends = RichTest::blends;
        CHECK(c.Fill(Re50, nullptr, &pattern) == 7); CHECK(RichTest::blends == blends);
        CHECK(c.Pat(DSTINVERT)); CHECK(RichTest::blends == blends);
        c.End();
    }
    void TextOptionsAndFallbacks()
    {
        Context c; c.Begin();
        constexpr UINT opts = ETO_OPAQUE | ETO_GLYPH_INDEX | ETO_CLIPPED;
        const INT dx[]{9, 13}; const wchar_t glyphs[]{2, 7};
        RichTest::inkBlend = 19;
        CHECK(c.Call(Re20, "ExtTextOutW", &::ExtTextOutW, &c.dc, 10, 11, opts, &c.rect, glyphs, 2u, dx));
        c.CheckTint();
        CHECK(RichTest::inkText == glyphs && RichTest::inkCount == 2 && RichTest::inkDx == dx);
        CHECK(RichTest::inkOptions == (opts & ~ETO_OPAQUE) && RichTest::inkDc == &c.dc);
        CHECK(RichTest::inkOriginal == reinterpret_cast<void*>(&::ExtTextOutW));
        CHECK(RichTest::inkBlend == 19 && c.dc.background == custom);
        // A and W share the very same background path, including count==0.
        CHECK(c.Call(Re20, "ExtTextOutA", &::ExtTextOutA, &c.dc, 0, 0, opts, &c.rect, "", 0u, nullptr));
        c.CheckTint(); CHECK(RichTest::inkTexts == 1);
        CHECK(c.Call(Re20, "ExtTextOutA", &::ExtTextOutA, &c.dc, 0, 0, UINT(ETO_OPAQUE), &c.rect, "AB", 2u, dx));
        CHECK(RichTest::inkCount == 2 && RichTest::inkDx == dx);
        CHECK(c.Call(Re20, "ExtTextOutA", &::ExtTextOutA, &c.dc, 0, 0, opts, &c.rect,
            reinterpret_cast<LPCSTR>(glyphs), 2u, dx));
        CHECK(RichTest::inkText == glyphs && RichTest::inkCount == 2 && RichTest::inkDx == dx);
        RichTest::failInk = true;
        const int fills = RichTest::fills;
        CHECK(c.Text(ETO_OPAQUE | ETO_CLIPPED, 6));
        CHECK(RichTest::nativeOptions == ETO_CLIPPED && RichTest::fills == fills);
        RichTest::failInk = false; RichTest::failBlend = true;
        CHECK(c.Text(ETO_OPAQUE | ETO_CLIPPED, 6));
        CHECK(RichTest::nativeOptions == (ETO_OPAQUE | ETO_CLIPPED));
        CHECK(RichTest::fills == fills+1); // background failure retains native ETO_OPAQUE
        c.End();
    }
    void RectanglesClippingAndRepaints()
    {
        Context c; c.Begin();
        std::fill(c.dc.surface.pixels.begin(), c.dc.surface.pixels.end(), Pixel{9, 8, 7, 255});
        c.dc.clip = RECT{8, 9, 24, 29};
        for (int repaint = 0; repaint < 3; ++repaint)
        {
            CHECK(c.Fill() == 1);
            CHECK(c.At(10, 10) == tint && c.At(5, 6) == (Pixel{9, 8, 7, 255}));
        }
        CHECK(c.dc.clip.left == 8 && c.dc.clip.bottom == 29);
        c.dc.clip = c.window.client;
        const RECT oversized{-8, -9, 100, 100};
        CHECK(c.Fill(Re50, nullptr, nullptr, &oversized) == 1);
        CHECK(RichTest::lastRect.left == 0 && RichTest::lastRect.right == 80 && RichTest::lastRect.bottom == 60);
        c.dc.background = RGB(20, 40, 60);
        CHECK(c.Text());
        CHECK(c.At() == (Pixel{24, 16, 8, 100}));
        CHECK(c.At(70, 50) == tint); // no guessed full-client repaint from a run colour
        const int blends = RichTest::blends;
        CHECK(c.Call(Re50, "PatBlt", &::PatBlt, &c.dc, INT32_MAX, 0, 1, 1, PATCOPY));
        CHECK(RichTest::blends == blends);
        c.End();
    }
    void DcOwnershipAndModeGates()
    {
        Context c; TestWindow other;
        c.foreign.owner = &other;
        c.Begin(Re50, false);
        CHECK(c.Fill() == 7); CHECK(RichTest::blends == 0); // no successful BeginPaint binding yet
        c.Bind();
        CHECK(c.Fill(Re50, &c.foreign) == 7);
        const RECT scrollCorner{0, 0, 17, 17};
        TestBrush scroll{GetSysColor(COLOR_SCROLLBAR)};
        const int blacks = RichTest::blacks;
        CHECK(c.Fill(Re50, &c.foreign, &scroll, &scrollCorner) == 7);
        CHECK(RichTest::blacks == blacks); // no legacy corner blackening on a foreign DC
        CHECK(c.Fill(Re50, &c.memory) == 7); // host alone is not memory-DC ownership
        RichTest::stackOwners[&c.memory] = &other;
        CHECK(c.Fill(Re50, &c.memory) == 7);
        RichTest::stackOwners[&c.memory] = &c.window;
        RichTest::topWindow = &other; CHECK(c.Fill(Re50, &c.memory) == 7);
        RichTest::topWindow = &c.window; CHECK(c.Fill(Re50, &c.memory) == 1);
        const int blends = RichTest::blends;
        RichTest::full = false; CHECK(c.Fill() == 7); CHECK(c.Text(ETO_OPAQUE, 6));
        RichTest::full = true; c.window.backdrop = false; CHECK(c.Fill() == 7);
        c.window.backdrop = true; g_shutdown = true; CHECK(c.Fill() == 7); g_shutdown = false;
        CHECK(RichTest::blends == blends);
        c.End(); c.CheckZeroRefs();
    }
    void LeasesAndNestedSessions()
    {
        Context c;
        c.modules[Re20].originals.erase("PatBlt");
        c.modules[Re50].failAcquire.insert("ExtTextOutA");
        c.Begin();
        CHECK(c.modules[Re50].refs.at("FillRect") == 1); // caller==engine must be deduplicated
        CHECK(!c.modules[Re20].hooks.count("PatBlt") && !c.modules[Re50].hooks.count("ExtTextOutA"));
        const auto acquired = g_editSessionStack.back().richHooks.size();
        c.Begin();
        CHECK(g_editSessionStack.back().richHooks.size() == acquired);
        CHECK(c.modules[Re50].refs.at("FillRect") == 2); // nested EnableHook returned false, ref still acquired
        c.Get(); CHECK(c.modules[Re50].refs.at("FillRect") == 3);
        c.Release(); c.End(); CHECK(c.modules[Re50].refs.at("FillRect") == 1);
        // A newly visible module at release is not ours to disable; use the
        // exact modules/slots acquired when this session began.
        TestModule replacement;
        RichTest::modules[L"msftedit.dll"] = &replacement;
        c.window.kind = Kind::None;
        c.End(); c.CheckZeroRefs(); CHECK(replacement.refs.empty());
        c.window.kind = Kind::RichEdit50W;
        c.Begin(Re20, false); c.End(); c.CheckZeroRefs(); // BeginPaint(NULL) compensation
        CHECK(g_editSessionStack.empty() && g_editDcStack.empty());
    }
    void GetDcAndPlainMemoIsolation()
    {
        Context c;
        c.Get(); CHECK(g_editDcStack.back().paintDc == &c.dc);
        CHECK(c.Fill() == 1); c.CheckTint(); CHECK(c.Text(ETO_OPAQUE, 6));
        c.Release(); c.CheckZeroRefs();
        TestWindow memo; memo.kind = Kind::Edit; memo.cls = L"TMemo"; memo.style = ES_MULTILINE;
        TestDC memoDc; memoDc.owner = &memo;
        OnBeginPaintEdit(&memo, &c.modules[User]);
        CHECK(!OnEditAfterBeginPaint(&memo, &memoDc, nullptr, &c.modules[User]));
        RichTest::host = RichTest::topWindow = &memo; RichTest::stackOwners[&memoDc] = &memo;
        EnsureEditBuffer(&memo, &memoDc);
        auto* const buffer = g_activeEditBuffer;
        CHECK(buffer && buffer->delphiMemo);
        g_sessionBlacknessRects.push_back(c.rect);
        g_bypassEditBuffer = true;
        c.Get(); CHECK(c.Text(ETO_OPAQUE, 6)); c.Release();
        CHECK(g_activeEditBuffer == buffer && g_bypassEditBuffer && g_sessionBlacknessRects.size() == 1);
        c.Begin(); CHECK(c.Fill() == 1); c.End();
        CHECK(g_activeEditBuffer == buffer && g_bypassEditBuffer && g_sessionBlacknessRects.size() == 1);
        RichTest::host = RichTest::topWindow = &memo;
        OnEndPaintEdit(&memo, &c.modules[Exe]);
        CHECK(!g_activeEditBuffer); c.CheckZeroRefs();
    }
    void CachedNativeTextRows()
    {
        for (const auto kind : richKinds)
        {
            for (const COLORREF background : {GetSysColor(COLOR_WINDOW), custom})
            {
                Context c; c.window.kind = kind;
                if (kind == Kind::RichEdit50W) c.window.cls = L"TRichEdit";
                // The native engine can reuse a line DC allocated before this
                // BeginPaint. WindowFromDC and the current DC registry are both
                // empty; this is deliberately NOT c.Bind()'s window DC.
                CHECK(!WindowFromDC(&c.memory) && !RichTest::stackOwners.count(&c.memory));
                c.memory.background = background;
                c.Begin();
                const int native = RichTest::nativeTexts;
                const UINT opts = ETO_OPAQUE | ETO_CLIPPED | ETO_GLYPH_INDEX;
                CHECK(c.Text(opts, 6, Full, &c.memory));
                CHECK(RichTest::inkTexts == 1 && RichTest::nativeTexts == native);
                CHECK(RichTest::inkDc == &c.memory && RichTest::inkOptions == (opts & ~ETO_OPAQUE));
                const Pixel row = c.memory.surface.pixels[6*c.memory.surface.width+5];
                CHECK(row == (background == GetSysColor(COLOR_WINDOW) ? Pixel{} : tint));
                CHECK(c.At() == Pixel{}); // no window-wide repaint inferred from the row

                // The direct GetDC selection route must not be required to
                // "repair" normal text. Both selected and unselected draws use
                // alpha text, with only the selection background opaque.
                c.Get(); c.window.selected = CHARRANGE{1, 4};
                c.dc.background = GetSysColor(COLOR_HIGHLIGHT);
                CHECK(c.Text(opts, 6)); CHECK(RichTest::inkTexts == 2);
                CHECK(c.At()[3] == 255);
                c.window.selected = {}; c.Release();
                CHECK(c.Text(opts, 6, Re50, &c.memory)); CHECK(RichTest::inkTexts == 3);
                CHECK(c.Call(Re20, "ExtTextOutA", &::ExtTextOutA, &c.memory,
                    4, 5, opts, &c.rect, reinterpret_cast<LPCSTR>(L"glyphs"), 6u, nullptr));
                CHECK(RichTest::inkTexts == 4 && RichTest::nativeTexts == native);
                CHECK(c.memory.surface.pixels[6*c.memory.surface.width+5] == row);
                c.End(); c.CheckZeroRefs();
            }
        }
    }

    void CachedNativeTextGates()
    {
        Context c; TestWindow other;
        c.foreign.owner = &other;
        c.Begin(Re50, false);
        const auto nativeOnly = [&](HDC dc = nullptr) {
            const int ink = RichTest::inkTexts, native = RichTest::nativeTexts, blends = RichTest::blends;
            CHECK(c.Text(ETO_OPAQUE | ETO_CLIPPED, 6, Re50, dc ? dc : &c.memory));
            CHECK(RichTest::inkTexts == ink && RichTest::nativeTexts == native+1);
            CHECK(RichTest::blends == blends);
            CHECK(RichTest::nativeOptions == (ETO_OPAQUE | ETO_CLIPPED));
        };
        nativeOnly(); // pre/post-BeginPaint interval is not a bound text scope
        c.Bind();
        RichTest::host = nullptr; nativeOnly();
        RichTest::host = &other; nativeOnly();
        RichTest::host = &c.window;
        RichTest::topWindow = &other; nativeOnly();
        RichTest::topWindow = &c.window;
        nativeOnly(&c.foreign);
        RichTest::stackOwners[&c.memory] = &other; nativeOnly();
        RichTest::stackOwners.erase(&c.memory);
        RichTest::full = false; nativeOnly(); RichTest::full = true;
        c.window.backdrop = false; nativeOnly(); c.window.backdrop = true;
        g_shutdown = true; nativeOnly(); g_shutdown = false;
        g_inRichEditBackground = true; nativeOnly(); g_inRichEditBackground = false;
        g_inRichEditText = true; nativeOnly(); g_inRichEditText = false;
        CHECK(!IsRichEditTextDrawingDc(&c.window, &c.memory, &c.modules[Exe]));
        CHECK(!IsRichEditTextDrawingDc(&c.window, &c.memory, &c.modules[User]));
        CHECK(!IsRichEditTextDrawingDc(&c.window, &c.memory, nullptr));
        CHECK(!IsRichEditTextDrawingDc(nullptr, &c.memory, &c.modules[Re50]));
        CHECK(!IsRichEditTextDrawingDc(&c.window, nullptr, &c.modules[Re50]));
        CHECK(!c.modules[Exe].hooks.count("ExtTextOutW"));

        // Permission belongs only to this ExtTextOut, not to the DC. Neither
        // an earlier nor a later text call may loosen FillRect/PatBlt ownership.
        const auto fillsStayStrict = [&] {
            const int blends = RichTest::blends, blacks = RichTest::blacks, pats = RichTest::pats;
            CHECK(c.Fill(Re50, &c.memory) == 7);
            CHECK(c.Pat(PATCOPY, &c.memory));
            CHECK(RichTest::blends == blends && RichTest::blacks == blacks && RichTest::pats == pats+1);
            CHECK(!RichTest::stackOwners.count(&c.memory));
        };
        fillsStayStrict();
        const int native = RichTest::nativeTexts;
        CHECK(c.Text(ETO_OPAQUE, 6, Re50, &c.memory));
        CHECK(RichTest::inkTexts == 1 && RichTest::nativeTexts == native);
        fillsStayStrict();
        CHECK(c.Text(ETO_CLIPPED, 6, Re20, &c.memory)); // text without a background rectangle
        CHECK(RichTest::inkTexts == 2 && RichTest::inkOptions == ETO_CLIPPED);
        CHECK(c.Text(ETO_OPAQUE, 0, Full, &c.memory)); // background-only native text call
        CHECK(c.memory.surface.pixels[6*c.memory.surface.width+5] == tint);
        CHECK(RichTest::inkTexts == 2);
        CHECK(c.Call(Re20, "ExtTextOutA", &::ExtTextOutA, &c.memory,
            4, 5, UINT(ETO_OPAQUE), &c.rect, "AB", 2u, nullptr));
        CHECK(RichTest::inkTexts == 3 && RichTest::inkCount == 2);

        // A cached-DC background failure must keep the complete original
        // operation, not remove ETO_OPAQUE and leave an incomplete row.
        for (bool* failure : {&RichTest::failDc, &RichTest::failBitmap, &RichTest::failSelect,
                             &RichTest::failBlacken, &RichTest::failBlend})
        {
            *failure = true;
            const int ink = RichTest::inkTexts;
            CHECK(c.Text(ETO_OPAQUE | ETO_CLIPPED, 6, Re50, &c.memory));
            CHECK(RichTest::inkTexts == ink);
            CHECK(RichTest::nativeOptions == (ETO_OPAQUE | ETO_CLIPPED));
            CHECK(!g_inRichEditBackground && !g_inRichEditText);
            CHECK(!RichTest::liveDcs && !RichTest::liveBitmaps && !RichTest::liveBrushes);
            *failure = false;
        }
        c.End(); c.CheckZeroRefs();
        CHECK(!IsRichEditTextDrawingDc(&c.window, &c.memory, &c.modules[Re50])); // stale host alone
    }
    void CachedGetDcAndMemoIsolation()
    {
        Context c;
        c.Get(); // no BeginPaint at all
        CHECK(g_editSessionStack.empty());
        CHECK(c.Text(ETO_OPAQUE, 6, Re20, &c.memory));
        CHECK(RichTest::inkTexts == 1 && RichTest::inkDc == &c.memory);
        c.Release(); c.CheckZeroRefs();

        TestWindow memo; memo.kind = Kind::Edit; memo.cls = L"TMemo"; memo.style = ES_MULTILINE;
        TestDC memoDc; memoDc.owner = &memo;
        OnBeginPaintEdit(&memo, &c.modules[User]);
        CHECK(!OnEditAfterBeginPaint(&memo, &memoDc, nullptr, &c.modules[User]));
        RichTest::host = RichTest::topWindow = &memo; RichTest::stackOwners[&memoDc] = &memo;
        EnsureEditBuffer(&memo, &memoDc);
        auto* const buffer = g_activeEditBuffer;
        CHECK(buffer && buffer->delphiMemo);
        g_sessionBlacknessRects.push_back(c.rect);
        g_bypassEditBuffer = true;
        const int native = RichTest::nativeTexts;
        c.Get();
        CHECK(c.Text(ETO_OPAQUE, 6, Full, &c.memory));
        CHECK(RichTest::inkTexts == 2 && RichTest::nativeTexts == native && RichTest::inkDc == &c.memory);
        CHECK(c.memory.surface.pixels[6*c.memory.surface.width+5] == tint);
        CHECK(c.Call(Re20, "ExtTextOutA", &::ExtTextOutA, &c.memory,
            4, 5, UINT(ETO_OPAQUE), &c.rect, "AB", 2u, nullptr));
        CHECK(RichTest::inkTexts == 3 && RichTest::inkDc == &c.memory);
        c.Release();
        CHECK(g_activeEditBuffer == buffer && g_bypassEditBuffer && g_sessionBlacknessRects.size() == 1);
        c.Begin(); CHECK(c.Text(ETO_OPAQUE, 6, Re50, &c.memory)); c.End();
        CHECK(RichTest::inkTexts == 4 && RichTest::nativeTexts == native);
        CHECK(g_activeEditBuffer == buffer && g_bypassEditBuffer && g_sessionBlacknessRects.size() == 1);
        CHECK(!RichTest::stackOwners.count(&c.memory));
        RichTest::host = RichTest::topWindow = &memo;
        OnEndPaintEdit(&memo, &c.modules[Exe]);
        CHECK(!g_activeEditBuffer); c.CheckZeroRefs();
    }

    void FailuresAndPerModuleOriginals()
    {
        Context c; c.Begin();
        for (bool* failure : {&RichTest::failDc, &RichTest::failBitmap, &RichTest::failSelect, &RichTest::failBlacken, &RichTest::failBlend})
        {
            *failure = true;
            CHECK(c.Fill() == 7);
            CHECK(RichTest::liveDcs == 0 && RichTest::liveBitmaps == 0 && RichTest::liveBrushes == 0);
            CHECK(!g_inRichEditBackground && !g_inRichEditText);
            *failure = false;
        }
        c.End(); c.CheckZeroRefs();
        c.modules[Re20].originals["FillRect"] = reinterpret_cast<void*>(+[](HDC, const RECT*, HBRUSH) -> int {
            RichTest::originalTag = 20; return 20;
        });
        c.modules[Re50].originals["FillRect"] = reinterpret_cast<void*>(+[](HDC, const RECT*, HBRUSH) -> int {
            RichTest::originalTag = 50; return 50;
        });
        c.Begin(); RichTest::failBlend = true;
        CHECK(c.Fill(Re20) == 20 && RichTest::originalTag == 20);
        CHECK(c.Fill(Re50) == 50 && RichTest::originalTag == 50);
        c.End();
    }
    void ReentryAndLegacyEditBackgrounds()
    {
        Context c; c.Begin();
        RichTest::duringBlend = [&] {
            CHECK(c.Fill(Re20, &c.memory) == 7); CHECK(c.Pat(PATCOPY, &c.memory));
            CHECK(c.Text(ETO_OPAQUE, 6, Re50, &c.memory));
        };
        CHECK(c.Fill() == 1); c.CheckTint();
        CHECK(RichTest::blends == 1 && RichTest::inkTexts == 0);
        RichTest::duringBlend = {};
        RichTest::duringInk = [&] {
            CHECK(c.Text(ETO_GLYPH_INDEX, 6, Full));
            CHECK(c.Fill(Re20) == 7);
        };
        CHECK(c.Text(0, 6)); CHECK(RichTest::inkTexts == 1);
        RichTest::duringInk = {};
        c.End();
        // Accepted plain Edit/TMemo backgrounds still use alpha-255 brushes,
        // including deferred memo replay. No change to their composition.
        TestWindow edit; edit.kind = Kind::Edit; edit.cls = L"TEdit";
        c.dc.owner = &edit; c.dc.background = custom;
        const int blends = RichTest::blends;
        ApplyEditBackgroundRect(&c.dc, &c.dc, &edit, &c.rect, false);
        CHECK(c.At() == (Pixel{40, 80, 200, 255}) && RichTest::blends == blends);
        EditBufferedSession memo; memo.delphiMemo = true;
        memo.memoBackgrounds.push_back({c.rect, custom, true});
        ReplayMemoBackgrounds(&c.dc, memo);
        CHECK(c.At() == (Pixel{40, 80, 200, 255}) && RichTest::blends == blends);
    }
}
int main(int argc, char** argv)
{
    try
    {
        CHECK(argc == 2);
        const std::unordered_map<std::string, void(*)()> cases{
            {"all-kinds-and-apis", AllKindsAndNativeApis},
            {"colors-and-selection", ColorsAndSelection},
            {"empty-and-brush-sources", EmptyAndBrushSources},
            {"text-options-and-fallbacks", TextOptionsAndFallbacks},
            {"rectangles-and-repaints", RectanglesClippingAndRepaints},
            {"dc-and-mode-gates", DcOwnershipAndModeGates},
            {"leases-and-nesting", LeasesAndNestedSessions},
            {"getdc-and-memo-isolation", GetDcAndPlainMemoIsolation},
            {"cached-native-text-rows", CachedNativeTextRows},
            {"cached-native-text-gates", CachedNativeTextGates},
            {"cached-getdc-and-memo-isolation", CachedGetDcAndMemoIsolation},
            {"failures-and-originals", FailuresAndPerModuleOriginals},
            {"reentry-and-legacy-edit", ReentryAndLegacyEditBackgrounds},
        };
        cases.at(argv[1])();
        std::cout << "PASS " << argv[1] << '\n';
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
