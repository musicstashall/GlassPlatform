#include "vcl_background_test_shim.h"
#include "../../GlassCore/src/Hooks/delphi/TBackgroundHooks.cpp"

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)

namespace
{
    namespace Bg = GlassCore::DelphiBackground;
    using BgTest::Event;
    using BgTest::Kind;
    using GlassShared::GlassControlKind;

    std::vector<Event> Take() { return std::exchange(BgTest::events, {}); }
    bool Same(const RECT& a, const RECT& b)
    { return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom; }
    [[maybe_unused]] long long Number(const void* p) { return static_cast<long long>(reinterpret_cast<std::intptr_t>(p)); }
    std::vector<BgTest::Log> Logs(const wchar_t* part)
    {
        std::vector<BgTest::Log> found;
        for (const auto& log : BgTest::logs)
            if (log.format.find(part) != std::wstring::npos) found.push_back(log);
        return found;
    }

    struct Fixture
    {
        TestModule exe;
        TestWindow list, list2, other, edit, tree;
        TestBitmap surface{1}, surface2{2}, buffer{3};
        TestDC paintDc, paintDc2, canvasDc, memoryDc, foreignDc;
        TestDC eraseDc, editDc, treeDc;         // the DCs Windows brings with an erase of its own
        LOGBRUSH window{BS_SOLID, 0xffffff}, face{BS_SOLID, 0xf0f0f0}, custom{BS_SOLID, 0x00c0ff};
        LOGBRUSH hatch{BS_HATCHED, 0xffffff};
        const RECT client{0, 0, 200, 120};

        Fixture()
        {
            using namespace BgTest;
            full = true; insideDraw = false; topWindow = nullptr; paintDcs.clear();
            windowColor = 0xffffff; faceColor = 0xf0f0f0; failBlack = failObject = false;
            nativeFillResult = 1; duringErase = {}; duringNativeFill = {};
            thread = 1; windowLookups = classLookups = 0; beforeExclusive = {};
            static ULONGLONG epoch = 0;
            BgTest::clock = epoch += 1000000; // a fresh rate-limit window for every fixture
            events.clear(); logs.clear();
            exe.originals["FillRect"] = reinterpret_cast<void*>(&NativeFillRect);
            GlassCore::ModuleHandler::g_entryPointModule = &exe;
            GlassCore::PaintDispatcher::paintProviders.clear();
            GlassCore::PaintDispatcher::postProviders.clear();
            GlassCore::PaintDispatcher::dcProviders.clear();
            list.brush = list2.brush = other.brush = edit.brush = tree.brush = &window;
            other.cls = L"TPanel"; other.kind = GlassControlKind::None;
            edit.cls = L"TEdit"; edit.kind = GlassControlKind::Edit;
            tree.cls = L"TResTreeView"; tree.kind = GlassControlKind::TreeView;
            paintDc.owner = canvasDc.owner = eraseDc.owner = &list; paintDc2.owner = &list2; foreignDc.owner = &other;
            editDc.owner = &edit; treeDc.owner = &tree;
            paintDc.bitmap = canvasDc.bitmap = eraseDc.bitmap = &surface; paintDc2.bitmap = &surface2; memoryDc.bitmap = &buffer;
            Bg::Startup();
            CHECK(GlassCore::PaintDispatcher::paintProviders.size() == 1
                && GlassCore::PaintDispatcher::postProviders.size() == 1
                && GlassCore::PaintDispatcher::dcProviders.size() == 1);
            CHECK(Idle() && !Kept() && All() == 0);
        }
        ~Fixture()
        {
            BgTest::duringErase = {}; BgTest::duringNativeFill = {}; BgTest::beforeExclusive = {};
            Bg::Shutdown();
            assert(Bg::g_sessions.empty() && exe.hooks.empty() && !Bg::g_kept.load());
        }
        // One reference outlives the sessions once a control whose erase can be
        // replaced has been seen; `Refs` counts what the open sessions hold.
        bool Kept() { return Bg::g_kept.load(); }
        int All() { return exe.refs["FillRect"]; }
        int Refs() { return All() - (Kept() ? 1 : 0); }
        bool Idle() { return Bg::g_sessions.empty() && !Bg::g_inside && Refs() == 0 && exe.hooks.size() == (Kept() ? 1u : 0u); }

        // The EXE's own call through its import table: VCL erasing or painting.
        int Fill(HDC dc, const RECT& rect, HBRUSH brush) { return ExeFillRect(exe, dc, &rect, brush); }
        // Windows sends WM_ERASEBKGND on its own, outside any paint: right after
        // the window was shown, restored or resized.
        void EraseOutsidePaint(TestWindow& w, TestDC& dc) { SendMessageW(&w, WM_ERASEBKGND, reinterpret_cast<WPARAM>(&dc), 0); }

        // What PaintDispatcher does around the original BeginPaint: providers
        // first, then BeginPaint (which sends the erase only while it is still
        // pending), then the post providers, and only then the DC is recorded.
        bool Begin(TestWindow& w, TestDC& dc, bool erasePending, PAINTSTRUCT* ps = nullptr)
        {
            using namespace GlassCore::PaintDispatcher;
            paintProviders.front().onBegin(&w, &exe);
            if (erasePending) SendMessageW(&w, WM_ERASEBKGND, reinterpret_cast<WPARAM>(&dc), 0);
            PAINTSTRUCT local{&dc, FALSE, w.client};
            const bool cleared = postProviders.front().onAfterBeginPaint(&w, &dc, ps ? ps : &local, &exe);
            BgTest::paintDcs[&dc] = &w; BgTest::topWindow = &w;
            return cleared;
        }
        void End(TestWindow& w, TestDC& dc)
        {
            BgTest::paintDcs.erase(&dc); BgTest::topWindow = nullptr;
            GlassCore::PaintDispatcher::paintProviders.front().onEnd(&w, &exe);
        }
        void BeginDc(TestWindow& w) { GlassCore::PaintDispatcher::dcProviders.front().onBegin(&w, &exe); BgTest::topWindow = &w; }
        void EndDc(TestWindow& w) { BgTest::topWindow = nullptr; GlassCore::PaintDispatcher::dcProviders.front().onEnd(&w, &exe); }
    };

    // The erase is still pending when BeginPaint runs: VCL fills inside it, the
    // fill is observed natively and the client is cleared once BeginPaint has
    // returned that very DC. Nothing is asked of the window again.
    void EraseInside()
    {
        Fixture f;
        CHECK(f.Begin(f.list, f.paintDc, true) && f.Refs() == 1);
        auto e = Take();
        CHECK(e.size() == 3 && e[0].kind == Kind::EraseMessage && e[0].hwnd == &f.list);
        CHECK(e[1].kind == Kind::NativeFill && e[1].dc == &f.paintDc && Same(e[1].rect, f.client) && e[1].brush == &f.window);
        CHECK(e[2].kind == Kind::Black && e[2].dc == &f.paintDc && Same(e[2].rect, f.client));
        f.End(f.list, f.paintDc);
        CHECK(f.Idle());
        // A colour the application chose is its own: observed, left alone, and
        // not asked for a second time.
        f.list.brush = &f.custom;
        CHECK(!f.Begin(f.list, f.paintDc, true));
        e = Take();
        CHECK(e.size() == 2 && e[0].kind == Kind::EraseMessage && e[1].kind == Kind::NativeFill && e[1].brush == &f.custom);
        f.End(f.list, f.paintDc);
        // COLOR_BTNFACE is the other default background.
        f.list.brush = &f.face;
        CHECK(f.Begin(f.list, f.paintDc, true));
        e = Take();
        CHECK(e.size() == 3 && e[2].kind == Kind::Black && Same(e[2].rect, f.client));
        f.End(f.list, f.paintDc);
        // The observed DC must be the one BeginPaint returns, with the same surface.
        TestDC early; early.owner = &f.list; early.bitmap = &f.surface;
        using namespace GlassCore::PaintDispatcher;
        paintProviders.front().onBegin(&f.list, &f.exe);
        SendMessageW(&f.list, WM_ERASEBKGND, reinterpret_cast<WPARAM>(&early), 0);
        PAINTSTRUCT ps{&f.paintDc, FALSE, f.client};
        CHECK(!postProviders.front().onAfterBeginPaint(&f.list, &f.paintDc, &ps, &f.exe));
        e = Take();
        CHECK(e.size() == 2 && e[1].kind == Kind::NativeFill && e[1].dc == &early); // observed elsewhere: no clear, no second erase
        paintProviders.front().onEnd(&f.list, &f.exe);
        CHECK(f.Idle());
    }

    // The window was shown, restored or resized: Windows erased it at once,
    // outside any paint, and VCL filled the client with the control's brush.
    // The WM_PAINT that follows erases nothing and the list repaints only its
    // rows. The erase is asked for again on the DC BeginPaint has just returned
    // and its default fill is replaced there.
    void EraseBefore()
    {
        Fixture f;
        f.EraseOutsidePaint(f.list, f.paintDc);
        auto e = Take();
        CHECK(e.size() == 2 && e[1].kind == Kind::NativeFill && e[1].brush == &f.window); // no such control seen yet: no hook
        CHECK(f.Idle() && !f.Kept());

        PAINTSTRUCT ps{&f.paintDc, FALSE, RECT{0, 0, 200, 120}};
        CHECK(f.Begin(f.list, f.paintDc, false, &ps));
        e = Take();
        CHECK(e.size() == 2);
        // Exactly the message Windows sends: the DC, and nothing in lParam.
        CHECK(e[0].kind == Kind::EraseMessage && e[0].hwnd == &f.list && e[0].dc == &f.paintDc);
        CHECK(e[0].wParam == reinterpret_cast<WPARAM>(&f.paintDc) && e[0].lParam == 0);
        // The fill is replaced, not followed: no native white in between.
        CHECK(e[1].kind == Kind::Black && e[1].dc == &f.paintDc && Same(e[1].rect, f.client));
        CHECK(ps.fErase == FALSE && Same(ps.rcPaint, f.client)); // the dispatcher owns fErase
        const auto& s = Bg::g_sessions.back();
        CHECK(!s.erasing && s.erasedAgain && s.afterBegin && s.paintDc == &f.paintDc && !s.observedDc && !s.defaultBrush);
        // The rest of the paint is as before: rows are the list's own.
        f.End(f.list, f.paintDc);
        CHECK(f.Idle() && Take().empty());

        // Every paint without an erase asks once, also a partial one.
        PAINTSTRUCT part{&f.paintDc, FALSE, RECT{0, 32, 200, 48}};
        CHECK(f.Begin(f.list, f.paintDc, false, &part));
        e = Take();
        CHECK(e.size() == 2 && e[0].kind == Kind::EraseMessage && e[1].kind == Kind::Black && Same(e[1].rect, f.client));
        f.End(f.list, f.paintDc);

        // COLOR_BTNFACE and the current system colours, not constants.
        f.list.brush = &f.face;
        CHECK(f.Begin(f.list, f.paintDc, false));
        CHECK(BgTest::Count(Kind::Black) == 1 && BgTest::Count(Kind::NativeFill) == 0); Take();
        f.End(f.list, f.paintDc);
        BgTest::windowColor = 0x202020; BgTest::faceColor = 0x303030;
        CHECK(!f.Begin(f.list, f.paintDc, false)); // 0xf0f0f0 is a custom colour now
        CHECK(BgTest::Count(Kind::Black) == 0 && BgTest::Count(Kind::NativeFill) == 1); Take();
        f.End(f.list, f.paintDc);
        LOGBRUSH dark{BS_SOLID, 0x202020};
        f.list.brush = &dark;
        CHECK(f.Begin(f.list, f.paintDc, false));
        CHECK(BgTest::Count(Kind::Black) == 1 && BgTest::Count(Kind::NativeFill) == 0); Take();
        f.End(f.list, f.paintDc);
        CHECK(f.Idle());
    }

    // What the window answers is its own business: a custom colour, a pattern,
    // no fill at all. The erase is asked once and nothing is replaced.
    void OwnBackground()
    {
        Fixture f;
        f.list.brush = &f.custom;
        CHECK(!f.Begin(f.list, f.paintDc, false));
        auto e = Take();
        CHECK(e.size() == 2 && e[0].kind == Kind::EraseMessage && e[1].kind == Kind::NativeFill);
        CHECK(e[1].dc == &f.paintDc && e[1].brush == &f.custom && Same(e[1].rect, f.client));
        CHECK(!Bg::g_sessions.back().erasing && !Bg::g_sessions.back().erasedAgain);
        f.End(f.list, f.paintDc);

        f.list.brush = &f.hatch;
        CHECK(!f.Begin(f.list, f.paintDc, false));
        e = Take();
        CHECK(e.size() == 2 && e[1].kind == Kind::NativeFill && e[1].brush == &f.hatch);
        f.End(f.list, f.paintDc);

        f.list.brush = &f.window; BgTest::failObject = true; // a brush without metadata
        CHECK(!f.Begin(f.list, f.paintDc, false));
        e = Take();
        CHECK(e.size() == 2 && e[1].kind == Kind::NativeFill);
        f.End(f.list, f.paintDc); BgTest::failObject = false;

        // A double-buffered VCL control skips the system's erase; a control may
        // not erase at all. The message is still sent once, and only once.
        f.list.doubleBuffered = true;
        CHECK(!f.Begin(f.list, f.paintDc, false));
        e = Take();
        CHECK(e.size() == 1 && e[0].kind == Kind::EraseMessage && e[0].lParam == 0);
        f.End(f.list, f.paintDc); f.list.doubleBuffered = false;
        f.list.erases = false;
        CHECK(!f.Begin(f.list, f.paintDc, false));
        CHECK(Take().size() == 1);
        f.End(f.list, f.paintDc); f.list.erases = true;

        // The clear itself failed: the application's fill runs instead.
        BgTest::failBlack = true;
        CHECK(!f.Begin(f.list, f.paintDc, false));
        e = Take();
        CHECK(e.size() == 2 && e[1].kind == Kind::NativeFill && e[1].brush == &f.window);
        f.End(f.list, f.paintDc); BgTest::failBlack = false;
        CHECK(f.Idle());
    }

    // Only the exact VCL list classes marked as lists, in Full mode with glass.
    void Classes()
    {
        Fixture f;
        for (LPCWSTR cls : {L"TListBox", L"tlistbox", L"TCheckListBox", L"TColorListBox", L"TFileListBox", L"TDirectoryListBox"})
        {
            f.list.cls = cls;
            CHECK(f.Begin(f.list, f.paintDc, false) && f.Refs() == 1);
            const auto e = Take();
            CHECK(e.size() == 2 && e[0].kind == Kind::EraseMessage && e[1].kind == Kind::Black);
            f.End(f.list, f.paintDc);
        }
        // Not armed at all: nothing is sent and the session takes no reference.
        const auto untouched = [&] {
            CHECK(!f.Begin(f.list, f.paintDc, false) && f.Refs() == 0 && Take().empty());
            CHECK(Bg::g_sessions.size() == 1 && !Bg::g_sessions.back().armed);
            f.End(f.list, f.paintDc);
            CHECK(f.Idle());
        };
        for (LPCWSTR cls : {L"ListBox", L"TListBox2", L"MyTListBox", L"TListView", L"TComboBox", L"TEdit", L"TPanel"})
        { f.list.cls = cls; untouched(); }
        f.list.cls = L"TListBox";
        f.list.kind = GlassControlKind::None; untouched();
        f.list.kind = GlassControlKind::Edit; untouched();
        f.list.kind = GlassControlKind::ListBox;
        BgTest::full = false; untouched(); BgTest::full = true;
        f.list.backdrop = false; untouched(); f.list.backdrop = true;
        // The two input classes of this unit keep their own rule: armed, their
        // fills after BeginPaint are replaced, but they are never asked to erase.
        for (LPCWSTR cls : {L"TNumberBox", L"THotKey"})
        {
            f.list.cls = cls; f.list.kind = GlassControlKind::Edit; f.list.inputDefault = true;
            CHECK(!f.Begin(f.list, f.paintDc, false) && f.Refs() == 1 && Take().empty());
            CHECK(f.Fill(&f.paintDc, f.client, &f.window) == 1);
            const auto e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::Black);
            f.End(f.list, f.paintDc);
        }
        f.list.cls = L"TListBox"; f.list.kind = GlassControlKind::ListBox;
        CHECK(f.Idle());
        // Without the import nothing can be replaced, so nothing is asked.
        Bg::Shutdown(); Bg::Startup();
        f.exe.originals.clear();
        CHECK(!f.Begin(f.list, f.paintDc, false) && Take().empty() && !Bg::g_sessions.back().armed);
        f.End(f.list, f.paintDc);
        CHECK(f.Idle() && !f.Kept() && f.All() == 0);
        f.exe.originals["FillRect"] = reinterpret_cast<void*>(&NativeFillRect);
    }

    // The repeated erase replaces one thing: the default fill of the whole
    // client on the DC BeginPaint returned.
    void Gates()
    {
        Fixture f;
        const auto during = [&](const std::function<void()>& draw) {
            BgTest::duringErase = [&](HWND, HDC) { draw(); };
            f.list.erases = false; // only what `draw` does
            const bool cleared = f.Begin(f.list, f.paintDc, false);
            BgTest::duringErase = {}; f.list.erases = true;
            auto e = Take();
            CHECK(!e.empty() && e[0].kind == Kind::EraseMessage);
            e.erase(e.begin());
            f.End(f.list, f.paintDc);
            return std::pair{cleared, e};
        };
        // Another DC of the same window, a buffer, another window's DC.
        for (TestDC* dc : {&f.canvasDc, &f.memoryDc, &f.foreignDc})
        {
            const auto [cleared, e] = during([&] { CHECK(f.Fill(dc, f.client, &f.window) == 1); });
            CHECK(!cleared && e.size() == 1 && e[0].kind == Kind::NativeFill && e[0].dc == dc);
        }
        // Not the whole client.
        for (const RECT& rect : {RECT{0, 0, 200, 119}, RECT{1, 0, 200, 120}, RECT{0, 0, 199, 120}, RECT{0, 16, 200, 32}})
        {
            const auto [cleared, e] = during([&] { CHECK(f.Fill(&f.paintDc, rect, &f.window) == 1); });
            CHECK(!cleared && e.size() == 1 && e[0].kind == Kind::NativeFill && Same(e[0].rect, rect));
        }
        // A mapped or shifted DC: the rectangle is not the client there.
        {
            f.paintDc.map = MM_ANISOTROPIC;
            const auto [cleared, e] = during([&] { f.Fill(&f.paintDc, f.client, &f.window); });
            CHECK(!cleared && e.size() == 1 && e[0].kind == Kind::NativeFill);
            f.paintDc.map = MM_TEXT; f.paintDc.viewport = POINT{0, 4};
            const auto [cleared2, e2] = during([&] { f.Fill(&f.paintDc, f.client, &f.window); });
            CHECK(!cleared2 && e2.size() == 1 && e2[0].kind == Kind::NativeFill);
            f.paintDc.viewport = POINT{}; f.paintDc.window = POINT{2, 0};
            const auto [cleared3, e3] = during([&] { f.Fill(&f.paintDc, f.client, &f.window); });
            CHECK(!cleared3 && e3.size() == 1 && e3[0].kind == Kind::NativeFill);
            f.paintDc.window = POINT{};
        }
        // Inside an owner-draw item of the list nothing of this unit applies.
        {
            BgTest::insideDraw = true;
            const auto [cleared, e] = during([&] { f.Fill(&f.paintDc, f.client, &f.window); });
            CHECK(!cleared && e.size() == 1 && e[0].kind == Kind::NativeFill);
            BgTest::insideDraw = false;
        }
        // A custom fill first and the default one after it: each by its own brush.
        {
            const auto [cleared, e] = during([&] {
                CHECK(f.Fill(&f.paintDc, f.client, &f.custom) == 1);
                CHECK(f.Fill(&f.paintDc, f.client, &f.window) == 1);
            });
            CHECK(cleared && e.size() == 2 && e[0].kind == Kind::NativeFill && e[0].brush == &f.custom && e[1].kind == Kind::Black);
        }
        // Nothing to repaint, or no paint structure: nothing is asked.
        PAINTSTRUCT empty{&f.paintDc, FALSE, RECT{10, 10, 10, 40}};
        CHECK(!f.Begin(f.list, f.paintDc, false, &empty) && Take().empty());
        f.End(f.list, f.paintDc);
        using namespace GlassCore::PaintDispatcher;
        paintProviders.front().onBegin(&f.list, &f.exe);
        CHECK(!postProviders.front().onAfterBeginPaint(&f.list, &f.paintDc, nullptr, &f.exe) && Take().empty());
        paintProviders.front().onEnd(&f.list, &f.exe);
        paintProviders.front().onBegin(&f.list, &f.exe);
        PAINTSTRUCT whole{nullptr, FALSE, f.client};
        CHECK(!postProviders.front().onAfterBeginPaint(&f.list, nullptr, &whole, &f.exe) && Take().empty());
        paintProviders.front().onEnd(&f.list, &f.exe);
        // The post provider of another window's BeginPaint is not this session's.
        paintProviders.front().onBegin(&f.list, &f.exe);
        PAINTSTRUCT ps{&f.paintDc2, FALSE, f.client};
        CHECK(!postProviders.front().onAfterBeginPaint(&f.list2, &f.paintDc2, &ps, &f.exe) && Take().empty());
        CHECK(!Bg::g_sessions.back().afterBegin);
        paintProviders.front().onEnd(&f.list, &f.exe);
        CHECK(f.Idle());
    }

    // What the unit did before is unchanged: fills after BeginPaint on the
    // recorded paint DC, and a GetDC session that only observes until the first
    // row is drawn.
    void Existing()
    {
        Fixture f;
        CHECK(f.Begin(f.list, f.paintDc, true)); Take();
        // After BeginPaint: only the exact paint DC the dispatcher recorded.
        CHECK(f.Fill(&f.paintDc, f.client, &f.window) == 1);
        auto e = Take();
        CHECK(e.size() == 1 && e[0].kind == Kind::Black && Same(e[0].rect, f.client));
        CHECK(f.Fill(&f.paintDc, f.client, &f.custom) == 1);
        CHECK(f.Fill(&f.canvasDc, f.client, &f.window) == 1);
        CHECK(f.Fill(&f.paintDc, RECT{0, 0, 200, 16}, &f.window) == 1);
        BgTest::paintDcs.erase(&f.paintDc);                 // the dispatcher does not know this DC
        CHECK(f.Fill(&f.paintDc, f.client, &f.window) == 1);
        e = Take();
        CHECK(e.size() == 4 && std::all_of(e.begin(), e.end(), [](const Event& x) { return x.kind == Kind::NativeFill; }));
        f.End(f.list, f.paintDc);

        // GetDC: no post provider, so nothing is asked; a default full-client fill
        // into the top session's DC is observed and its tail is cleared when the
        // first visible row is drawn, once.
        f.BeginDc(f.list);
        CHECK(f.Refs() == 1 && Take().empty());
        CHECK(f.Fill(&f.memoryDc, f.client, &f.window) == 1);
        e = Take();
        CHECK(e.size() == 1 && e[0].kind == Kind::NativeFill);
        DRAWITEMSTRUCT dis{}; dis.hwndItem = &f.list; dis.hDC = &f.memoryDc; dis.itemAction = ODA_DRAWENTIRE;
        dis.itemID = 0; dis.rcItem = RECT{0, 0, 200, 16};
        Bg::ClearObservedListTail(&f.list, &dis);
        e = Take();
        CHECK(e.size() == 1 && e[0].kind == Kind::Black && e[0].dc == &f.memoryDc && Same(e[0].rect, RECT{0, 48, 200, 120}));
        Bg::ClearObservedListTail(&f.list, &dis);
        CHECK(Take().empty());
        f.EndDc(f.list);
        CHECK(f.Idle());
    }

    // The window's erase handler is the application's code: it may paint other
    // windows before it returns.
    void Reentry()
    {
        Fixture f;
        // Another list is painted from inside the repeated erase; it had been
        // erased outside a paint as well. Each gets its own question and answer.
        int depth = 0;
        BgTest::duringErase = [&](HWND w, HDC) {
            if (w != &f.list || depth++) return;
            CHECK(Bg::g_sessions.size() == 1 && Bg::g_sessions[0].erasing);
            CHECK(f.Begin(f.list2, f.paintDc2, false) && f.Refs() == 2);
            CHECK(Bg::g_sessions.size() == 2 && !Bg::g_sessions[1].erasing && Bg::g_sessions[1].erasedAgain);
            // While the inner session is on top, the outer window's DC is not its concern.
            CHECK(f.Fill(&f.paintDc, f.client, &f.window) == 1);
            f.End(f.list2, f.paintDc2);
        };
        CHECK(f.Begin(f.list, f.paintDc, false));
        auto e = Take();
        CHECK(e.size() == 5);
        CHECK(e[0].kind == Kind::EraseMessage && e[0].hwnd == &f.list);
        CHECK(e[1].kind == Kind::EraseMessage && e[1].hwnd == &f.list2);
        CHECK(e[2].kind == Kind::Black && e[2].dc == &f.paintDc2);
        CHECK(e[3].kind == Kind::NativeFill && e[3].dc == &f.paintDc);
        CHECK(e[4].kind == Kind::Black && e[4].dc == &f.paintDc);
        CHECK(Bg::g_sessions.size() == 1 && !Bg::g_sessions[0].erasing && Bg::g_sessions[0].erasedAgain && f.Refs() == 1);
        f.End(f.list, f.paintDc);
        CHECK(f.Idle());

        // Many nested sessions move the session storage: the outer one is found
        // by its position and serial, not by a kept pointer.
        depth = 0;
        std::vector<TestWindow> windows(48);
        std::vector<TestDC> dcs(48);
        for (size_t i = 0; i < windows.size(); ++i)
        { windows[i].brush = &f.window; dcs[i].owner = &windows[i]; dcs[i].bitmap = &f.buffer; }
        BgTest::duringErase = [&](HWND w, HDC) {
            if (w != &f.list || depth++) return;
            for (size_t i = 0; i < windows.size(); ++i) CHECK(f.Begin(windows[i], dcs[i], true));
            for (size_t i = windows.size(); i--;) f.End(windows[i], dcs[i]);
        };
        CHECK(f.Begin(f.list, f.paintDc, false));
        e = Take();
        CHECK(e.back().kind == Kind::Black && e.back().dc == &f.paintDc);
        CHECK(BgTest::Count(Kind::EraseMessage) == 0 && Bg::g_sessions.size() == 1 && Bg::g_sessions[0].erasedAgain);
        f.End(f.list, f.paintDc);
        CHECK(f.Idle());

        // The outer session is closed from inside the handler: nothing of it is touched afterwards.
        BgTest::duringErase = [&](HWND w, HDC) {
            if (w == &f.list) GlassCore::PaintDispatcher::paintProviders.front().onEnd(&f.list, &f.exe);
        };
        CHECK(!f.Begin(f.list, f.paintDc, false));
        e = Take();
        // The session is gone, the kept import is not: the fill is an erase
        // outside every session of the list now, and is replaced as one.
        CHECK(e.size() == 2 && e[1].kind == Kind::Black && e[1].dc == &f.paintDc);
        CHECK(Bg::g_sessions.empty());
        BgTest::paintDcs.clear(); BgTest::topWindow = nullptr;
        // Even when a fill had been replaced before the session went away: a
        // closed session answers nothing.
        BgTest::duringErase = [&](HWND w, HDC) {
            if (w != &f.list) return;
            CHECK(f.Fill(&f.paintDc, f.client, &f.window) == 1 && Bg::g_sessions.back().erasedAgain);
            GlassCore::PaintDispatcher::paintProviders.front().onEnd(&f.list, &f.exe);
        };
        CHECK(!f.Begin(f.list, f.paintDc, false));
        e = Take();
        CHECK(e.size() == 3 && e[1].kind == Kind::Black && e[2].kind == Kind::Black && Bg::g_sessions.empty());
        BgTest::paintDcs.clear(); BgTest::topWindow = nullptr;
        // ...and a session of another window that took its place, with an answer
        // of its own, is not mistaken for it.
        BgTest::duringErase = [&](HWND w, HDC) {
            if (w != &f.list) return;
            GlassCore::PaintDispatcher::paintProviders.front().onEnd(&f.list, &f.exe);
            CHECK(f.Begin(f.list2, f.paintDc2, false)); // stays open
        };
        CHECK(!f.Begin(f.list, f.paintDc, false));
        CHECK(Bg::g_sessions.size() == 1 && Bg::g_sessions[0].hwnd == &f.list2);
        CHECK(!Bg::g_sessions[0].erasing && Bg::g_sessions[0].erasedAgain && Bg::g_sessions[0].paintDc == &f.paintDc2);
        BgTest::duringErase = {};
        GlassCore::PaintDispatcher::paintProviders.front().onEnd(&f.list2, &f.exe);
        BgTest::paintDcs.clear(); BgTest::topWindow = nullptr; Take();
        CHECK(f.Idle());
    }

    void Lifecycle()
    {
        Fixture f;
        // Nested paints of two lists: one hook, two refs, released one by one.
        CHECK(f.Begin(f.list, f.paintDc, false) && f.Refs() == 1);
        CHECK(f.Begin(f.list2, f.paintDc2, false) && f.Refs() == 2 && f.exe.hooks.size() == 1);
        f.End(f.list2, f.paintDc2);
        CHECK(f.Refs() == 1 && f.exe.hooks.size() == 1);
        f.End(f.list, f.paintDc);
        CHECK(f.Idle());
        Take();
        // Shutdown: the import is released and later calls do nothing.
        CHECK(f.Begin(f.list, f.paintDc, false)); Take();
        Bg::Shutdown();
        CHECK(f.exe.hooks.empty() && f.Refs() == 0 && Bg::g_sessions.empty());
        f.End(f.list, f.paintDc);
        CHECK(!f.Begin(f.list, f.paintDc, false) && Take().empty() && Bg::g_sessions.empty());
        f.End(f.list, f.paintDc);
        CHECK(f.Fill(&f.paintDc, f.client, &f.window) == 1);
        const auto e = Take();
        CHECK(e.size() == 1 && e[0].kind == Kind::NativeFill);
        // Shutdown from inside the repeated erase.
        Bg::Startup();
        BgTest::duringErase = [&](HWND, HDC) { Bg::Shutdown(); };
        CHECK(!f.Begin(f.list, f.paintDc, false));
        BgTest::duringErase = {};
        CHECK(f.exe.hooks.empty() && Bg::g_sessions.empty());
        f.End(f.list, f.paintDc);
        Take();
        Bg::Startup();
    }

    // One line per repeated erase: the window, the DC, the paint rectangle and
    // whether the default fill was replaced. Debug builds only, 12 a second.
    void Diagnostics()
    {
        Fixture f;
        PAINTSTRUCT ps{&f.paintDc, FALSE, RECT{0, 32, 200, 48}};
        CHECK(f.Begin(f.list, f.paintDc, false, &ps));
        f.End(f.list, f.paintDc);
        f.list.brush = &f.custom;
        CHECK(!f.Begin(f.list, f.paintDc, false));
        f.End(f.list, f.paintDc);
        f.list.brush = &f.window;
        const auto lines = Logs(L"] erase-again ");
#if defined(_DEBUG)
        CHECK(lines.size() == 2);
        CHECK(lines[0].format == L"[GlassCore][DelphiBackground] erase-again hwnd=0x%p dc=0x%p paint=[%ld,%ld,%ld,%ld] cleared=%d");
        CHECK(lines[0].numbers == (std::vector<long long>{Number(&f.list), Number(&f.paintDc), 0, 32, 200, 48, 1}));
        CHECK(lines[1].numbers == (std::vector<long long>{Number(&f.list), Number(&f.paintDc), 0, 0, 200, 120, 0}));
        for (int i = 0; i < 30; ++i) { CHECK(f.Begin(f.list, f.paintDc, false)); f.End(f.list, f.paintDc); }
        CHECK(Logs(L"] erase-again ").size() == 12);
        BgTest::clock += 1000;
        CHECK(f.Begin(f.list, f.paintDc, false)); f.End(f.list, f.paintDc);
        CHECK(Logs(L"] erase-again ").size() == 13);
        // An erase that came inside BeginPaint is not this line.
        BgTest::clock += 1000; BgTest::logs.clear();
        CHECK(f.Begin(f.list, f.paintDc, true)); f.End(f.list, f.paintDc);
        CHECK(Logs(L"] erase-again ").empty() && Logs(L"] post-BeginPaint ").size() == 1);
#else
        CHECK(lines.empty() && BgTest::logs.empty());
#endif
        Take();
        CHECK(f.Idle());
    }

    // The classes whose erase is replaced, with the kind the classifier marks
    // them with; `face`: COLOR_BTNFACE counts as their default background too.
    struct EraseRow { LPCWSTR cls; GlassControlKind kind; bool face; };
    const EraseRow kEraseRows[] = {
        {L"TListBox", GlassControlKind::ListBox, true}, {L"TCheckListBox", GlassControlKind::ListBox, true},
        {L"TColorListBox", GlassControlKind::ListBox, true}, {L"TFileListBox", GlassControlKind::ListBox, true},
        {L"TDirectoryListBox", GlassControlKind::ListBox, true},
        {L"TEdit", GlassControlKind::Edit, true}, {L"tedit", GlassControlKind::Edit, true},
        {L"TLabeledEdit", GlassControlKind::Edit, true}, {L"TButtonedEdit", GlassControlKind::Edit, true},
        {L"TMaskEdit", GlassControlKind::Edit, true}, {L"TSearchBox", GlassControlKind::Edit, true},
        {L"TNumberBox", GlassControlKind::Edit, true}, {L"TSpinEdit", GlassControlKind::Edit, true},
        {L"THotKey", GlassControlKind::Edit, true}, {L"TInplaceEdit", GlassControlKind::Edit, true},
        {L"TInplaceEditList", GlassControlKind::Edit, true},
        {L"TMemo", GlassControlKind::Edit, false}, {L"TRichEdit", GlassControlKind::RichEdit50W, false},
        {L"TListView", GlassControlKind::ListView, true}, {L"TSystemListView", GlassControlKind::ListView, true},
        {L"TTreeView", GlassControlKind::TreeView, true}, {L"TResTreeView", GlassControlKind::TreeView, true},
    };

    // The window comes back from the taskbar. Windows erases every control at
    // once, outside any paint, and paints them only later; until then a control
    // shows what its erase left. VCL's default fill of the whole client is
    // replaced where it is made, for the controls whose client Glass paints.
    void EraseOutside()
    {
        Fixture f;
        // The first paint of such a control keeps the import hooked.
        CHECK(f.Begin(f.list, f.paintDc, true)); f.End(f.list, f.paintDc); Take();
        CHECK(f.Kept() && f.Idle());

        // Restored: the erase arrives with a DC of the system.
        SetLastError(0x77);
        f.EraseOutsidePaint(f.list, f.eraseDc);
        auto e = Take();
        CHECK(e.size() == 2 && e[0].kind == Kind::EraseMessage && e[0].hwnd == &f.list);
        // Replaced, not followed: there is no native white in between.
        CHECK(e[1].kind == Kind::Black && e[1].dc == &f.eraseDc && Same(e[1].rect, f.client));
        CHECK(GetLastError() == 0x77);              // a fill that succeeded leaves the error alone
        CHECK(Bg::g_sessions.empty() && f.Idle());  // no session is opened for it
        // The application is told that its fill succeeded.
        BgTest::nativeFillResult = 7;
        CHECK(f.Fill(&f.eraseDc, f.client, &f.window) == 1);
        CHECK(Take().size() == 1);
        // The WM_PAINT that follows brings no erase; a list is asked again as before.
        CHECK(f.Begin(f.list, f.paintDc, false));
        e = Take();
        CHECK(e.size() == 2 && e[0].kind == Kind::EraseMessage && e[1].kind == Kind::Black && e[1].dc == &f.paintDc);
        f.End(f.list, f.paintDc);
        // Controls that were never painted themselves are served by the same import.
        f.EraseOutsidePaint(f.edit, f.editDc);
        f.EraseOutsidePaint(f.tree, f.treeDc);
        e = Take();
        CHECK(e.size() == 4 && e[1].kind == Kind::Black && e[1].dc == &f.editDc && Same(e[1].rect, f.client));
        CHECK(e[3].kind == Kind::Black && e[3].dc == &f.treeDc && Same(e[3].rect, f.client));

        // Every class, with its own client.
        for (const EraseRow& row : kEraseRows)
        {
            TestWindow w; w.cls = row.cls; w.kind = row.kind; w.brush = &f.window; w.inputDefault = true;
            w.client = RECT{0, 0, 90, 21};
            TestDC dc; dc.owner = &w; dc.bitmap = &f.surface;
            f.EraseOutsidePaint(w, dc);
            e = Take();
            CHECK(e.size() == 2 && e[1].kind == Kind::Black && e[1].dc == &dc && Same(e[1].rect, w.client));
            // COLOR_BTNFACE is the other default background - except where it is a
            // colour of its own, which the edit hooks keep.
            w.brush = &f.face;
            f.EraseOutsidePaint(w, dc);
            e = Take();
            CHECK(e.size() == 2 && e[1].kind == (row.face ? Kind::Black : Kind::NativeFill) && e[1].dc == &dc);
        }
        // A number box only once its default background was confirmed.
        TestWindow number; number.cls = L"TNumberBox"; number.kind = GlassControlKind::Edit; number.brush = &f.window;
        TestDC numberDc; numberDc.owner = &number;
        f.EraseOutsidePaint(number, numberDc);
        e = Take();
        CHECK(e.size() == 2 && e[1].kind == Kind::NativeFill && e[1].brush == &f.window);
        number.inputDefault = true;
        f.EraseOutsidePaint(number, numberDc);
        e = Take();
        CHECK(e.size() == 2 && e[1].kind == Kind::Black);
        f.edit.inputDefault = false;                // ...which no other class is asked for
        f.EraseOutsidePaint(f.edit, f.editDc);
        CHECK(BgTest::Count(Kind::Black) == 1); Take();

        // What is not the default background stays the application's: a colour
        // it chose, a pattern, a brush that tells nothing about itself.
        for (HBRUSH brush : {&f.custom, &f.hatch, static_cast<HBRUSH>(nullptr)})
        {
            f.list.brush = brush;
            f.EraseOutsidePaint(f.list, f.eraseDc);
            e = Take();
            CHECK(e.size() == 2 && e[1].kind == Kind::NativeFill && e[1].dc == &f.eraseDc);
            CHECK(e[1].brush == brush && Same(e[1].rect, f.client));
        }
        f.list.brush = &f.window; BgTest::failObject = true;
        CHECK(f.Fill(&f.eraseDc, f.client, &f.window) == 7);
        BgTest::failObject = false;
        // The current system colours, not constants.
        BgTest::windowColor = 0x202020; BgTest::faceColor = 0x303030;
        CHECK(f.Fill(&f.eraseDc, f.client, &f.window) == 7);    // 0xffffff is a custom colour now
        LOGBRUSH dark{BS_SOLID, 0x202020}, darkFace{BS_SOLID, 0x303030}, flagged{BS_SOLID, 0x01202020};
        CHECK(f.Fill(&f.eraseDc, f.client, &dark) == 1);
        CHECK(f.Fill(&f.eraseDc, f.client, &darkFace) == 1);
        CHECK(f.Fill(&f.eraseDc, f.client, &flagged) == 1);     // the high byte of a COLORREF is not a colour
        BgTest::windowColor = 0xffffff; BgTest::faceColor = 0xf0f0f0;
        e = Take();
        CHECK(e.size() == 5 && e[0].kind == Kind::NativeFill && e[1].kind == Kind::NativeFill);
        CHECK(e[2].kind == Kind::Black && e[3].kind == Kind::Black && e[4].kind == Kind::Black);

        // A double-buffered VCL control skips the system's erase; a control may
        // not erase at all. Nothing is drawn for them.
        f.list.doubleBuffered = true;
        f.EraseOutsidePaint(f.list, f.eraseDc);
        f.list.doubleBuffered = false; f.list.erases = false;
        f.EraseOutsidePaint(f.list, f.eraseDc);
        f.list.erases = true;
        e = Take();
        CHECK(e.size() == 2 && e[0].kind == Kind::EraseMessage && e[1].kind == Kind::EraseMessage);

        // The clear itself failed: the application's fill runs instead, and sees
        // the error its caller had, not what the checks left.
        BgTest::failBlack = true;
        DWORD seen = 0;
        BgTest::duringNativeFill = [&] { seen = GetLastError(); };
        SetLastError(0x55);
        CHECK(f.Fill(&f.eraseDc, f.client, &f.window) == 7);
        CHECK(seen == 0x55 && GetLastError() == 0xb102);        // ...and its own result is what remains
        BgTest::failBlack = false; BgTest::duringNativeFill = {};
        e = Take();
        CHECK(e.size() == 1 && e[0].kind == Kind::NativeFill && e[0].brush == &f.window);

        // Windows erases from wherever the application showed or moved the
        // window: also from inside the paint of another one.
        CHECK(!f.Begin(f.other, f.foreignDc, false) && Take().empty());
        f.EraseOutsidePaint(f.edit, f.editDc);
        e = Take();
        CHECK(e.size() == 2 && e[1].kind == Kind::Black && e[1].dc == &f.editDc);
        f.End(f.other, f.foreignDc);
        CHECK(f.Begin(f.list, f.paintDc, true)); Take();
        const auto before = Bg::g_sessions.back();
        f.EraseOutsidePaint(f.edit, f.editDc);
        f.EraseOutsidePaint(f.list2, f.paintDc2);
        e = Take();
        CHECK(e.size() == 4 && e[1].kind == Kind::Black && e[1].dc == &f.editDc);
        CHECK(e[3].kind == Kind::Black && e[3].dc == &f.paintDc2);
        // The open session is not theirs and learns nothing from them.
        const auto& after = Bg::g_sessions.back();
        CHECK(Bg::g_sessions.size() == 1 && after.serial == before.serial && after.observedDc == before.observedDc);
        CHECK(after.defaultBrush == before.defaultBrush && after.erasedAgain == before.erasedAgain && f.Refs() == 1);
        f.End(f.list, f.paintDc);
        CHECK(f.Idle() && f.Kept());
    }

    // Only that fill. Everything else stays what the application made it.
    void OutsideGates()
    {
        Fixture f;
        CHECK(f.Begin(f.list, f.paintDc, true)); f.End(f.list, f.paintDc); Take();
        BgTest::nativeFillResult = 7;
        const auto native = [&](HDC dc, const RECT& rect, HBRUSH brush) {
            CHECK(f.Fill(dc, rect, brush) == 7);
            const auto e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::NativeFill && e[0].dc == dc);
            CHECK(Same(e[0].rect, rect) && e[0].brush == brush);
        };
        const auto black = [&](HDC dc) {
            CHECK(f.Fill(dc, f.client, &f.window) == 1);
            const auto e = Take();
            CHECK(e.size() == 1 && e[0].kind == Kind::Black && e[0].dc == dc && Same(e[0].rect, f.client));
        };
        black(&f.eraseDc);                                      // what all of the below is compared with

        // A window that is not such a control; a buffer, which is not even asked
        // for its window.
        native(&f.foreignDc, f.client, &f.window);
        BgTest::windowLookups = 0;
        native(&f.memoryDc, f.client, &f.window);
        CHECK(BgTest::windowLookups == 0);
        black(&f.eraseDc);
        CHECK(BgTest::windowLookups == 1);
        // Not the whole client.
        for (const RECT& rect : {RECT{0, 0, 200, 119}, RECT{1, 0, 200, 120}, RECT{0, 1, 200, 120}, RECT{0, 0, 199, 120},
                                 RECT{0, 16, 200, 32}, RECT{0, 0, 0, 0}})
            native(&f.eraseDc, rect, &f.window);
        // A mapped or shifted DC: the rectangle is not the client there.
        f.eraseDc.map = MM_ANISOTROPIC; native(&f.eraseDc, f.client, &f.window); f.eraseDc.map = MM_TEXT;
        f.eraseDc.viewport = POINT{0, 4}; native(&f.eraseDc, f.client, &f.window); f.eraseDc.viewport = POINT{};
        f.eraseDc.window = POINT{2, 0}; native(&f.eraseDc, f.client, &f.window); f.eraseDc.window = POINT{};
        // A window DC of the same control - its frame is being painted: the clip
        // leaves the client on some side.
        f.eraseDc.frame = true;
        for (const RECT& clip : {RECT{0, 0, 204, 124}, RECT{-2, -2, 200, 120}, RECT{-1, 0, 200, 120}, RECT{0, -1, 200, 120},
                                 RECT{0, 0, 201, 120}, RECT{0, 0, 200, 121}})
        { f.eraseDc.frameClip = clip; native(&f.eraseDc, f.client, &f.window); }
        // A clip inside the client is the erase of a part of it.
        for (const RECT& clip : {RECT{0, 0, 200, 120}, RECT{10, 20, 60, 40}, RECT{199, 119, 200, 120}})
        { f.eraseDc.frameClip = clip; black(&f.eraseDc); }
        f.eraseDc.frame = false;
        // Nothing of the client is to be erased, or the DC cannot tell.
        for (int kind : {NULLREGION, 0, -1})
        { f.eraseDc.clipKind = kind; native(&f.eraseDc, f.client, &f.window); }
        f.eraseDc.clipKind = 3; black(&f.eraseDc);              // a complex region is a region
        f.eraseDc.clipKind = SIMPLEREGION;
        // A fill from another thread is not an erase: a window is erased by its own.
        BgTest::thread = 2; native(&f.eraseDc, f.client, &f.window); BgTest::thread = 1;
        f.list.thread = 2; native(&f.eraseDc, f.client, &f.window); f.list.thread = 1;
        // Glass does not paint the control: another mode, no glass on its root.
        BgTest::full = false; native(&f.eraseDc, f.client, &f.window); BgTest::full = true;
        f.list.backdrop = false; native(&f.eraseDc, f.client, &f.window); f.list.backdrop = true;
        // The exact class under the kind the classifier gives it; nothing else.
        struct Pair { LPCWSTR cls; GlassControlKind kind; };
        for (const Pair& x : {Pair{L"TListBox", GlassControlKind::None}, Pair{L"TListBox", GlassControlKind::Edit},
                              Pair{L"TEdit", GlassControlKind::ListBox}, Pair{L"TEdit", GlassControlKind::None},
                              Pair{L"TEdit", GlassControlKind::ComboBox}, Pair{L"TMemo", GlassControlKind::RichEdit50W},
                              Pair{L"TRichEdit", GlassControlKind::Edit}, Pair{L"TListView", GlassControlKind::TreeView},
                              Pair{L"TTreeView", GlassControlKind::ListView},
                              Pair{L"Edit", GlassControlKind::Edit}, Pair{L"ListBox", GlassControlKind::ListBox},
                              Pair{L"SysListView32", GlassControlKind::ListView}, Pair{L"SysTreeView32", GlassControlKind::TreeView},
                              Pair{L"RICHEDIT50W", GlassControlKind::RichEdit50W},
                              Pair{L"TEdit2", GlassControlKind::Edit}, Pair{L"MyTEdit", GlassControlKind::Edit},
                              Pair{L"TEdi", GlassControlKind::Edit}, Pair{L"", GlassControlKind::Edit},
                              Pair{L"TComboBox", GlassControlKind::ComboBox}, Pair{L"TPanel", GlassControlKind::None}})
        {
            f.list.cls = x.cls; f.list.kind = x.kind;
            native(&f.eraseDc, f.client, &f.window);
        }
        f.list.cls = L"TListBox"; f.list.kind = GlassControlKind::ListBox;
        f.list.valid = false; native(&f.eraseDc, f.client, &f.window); f.list.valid = true;
        // Most windows are of another kind, and every fill of the EXE comes
        // through here: such a window is not asked for its class.
        for (const GlassControlKind kind : {GlassControlKind::None, GlassControlKind::ComboBox})
        {
            f.list.kind = kind; BgTest::classLookups = 0;
            native(&f.eraseDc, f.client, &f.window);
            CHECK(BgTest::classLookups == 0);
        }
        f.list.kind = GlassControlKind::ListBox;
        black(&f.eraseDc);
        CHECK(BgTest::classLookups == 1);

        // A session of this very window is open: the DC is BeginPaint's or
        // GetDC's, and the fill belongs to the rules of a session.
        f.BeginDc(f.edit);
        native(&f.editDc, f.client, &f.window);
        f.EndDc(f.edit);
        black(&f.editDc);
        using namespace GlassCore::PaintDispatcher;
        paintProviders.front().onBegin(&f.edit, &f.exe);
        native(&f.editDc, f.client, &f.window);                 // an erase inside BeginPaint
        PAINTSTRUCT ps{&f.editDc, FALSE, f.client};
        CHECK(!postProviders.front().onAfterBeginPaint(&f.edit, &f.editDc, &ps, &f.exe));
        native(&f.editDc, f.client, &f.window);
        // ...also when it is not the innermost one.
        CHECK(!f.Begin(f.other, f.foreignDc, false));
        native(&f.editDc, f.client, &f.window);
        f.End(f.other, f.foreignDc);
        paintProviders.front().onEnd(&f.edit, &f.exe);
        black(&f.editDc);

        // The screen has no window behind its DC.
        TestDC screen; screen.screen = true;
        native(&screen, f.client, &f.window);
        // Nothing to look at.
        CHECK(ExeFillRect(f.exe, &f.eraseDc, nullptr, &f.window) == 7);
        CHECK(ExeFillRect(f.exe, nullptr, &f.client, &f.window) == 7);
        auto e = Take();
        CHECK(e.size() == 2 && e[0].kind == Kind::NativeFill && e[0].dc == &f.eraseDc && e[1].kind == Kind::NativeFill && !e[1].dc);
        // The unit's own drawing is in progress.
        Bg::g_inside = true; native(&f.eraseDc, f.client, &f.window); Bg::g_inside = false;
        black(&f.eraseDc);
        CHECK(f.Idle() && f.Kept());
    }

    // The import is hooked for good when the first control of such a class is
    // seen in a paint or a GetDC, and is released by shutdown only.
    void KeptImport()
    {
        Fixture f;
        // Nothing seen: nothing hooked, and the erase is the application's.
        CHECK(!f.Kept() && f.exe.hooks.empty());
        f.EraseOutsidePaint(f.list, f.eraseDc);
        auto e = Take();
        CHECK(e.size() == 2 && e[1].kind == Kind::NativeFill);
        // Windows that are not such controls do not hook it.
        CHECK(!f.Begin(f.other, f.foreignDc, true)); f.End(f.other, f.foreignDc); Take();
        f.BeginDc(f.other); f.EndDc(f.other);
        CHECK(!f.Kept() && f.exe.hooks.empty() && f.All() == 0);
        // ...nor does a control Glass does not paint.
        const auto unseen = [&] { f.BeginDc(f.edit); CHECK(!f.Kept() && f.All() == 0); f.EndDc(f.edit); };
        BgTest::full = false; unseen(); BgTest::full = true;
        f.edit.backdrop = false; unseen(); f.edit.backdrop = true;
        f.edit.kind = GlassControlKind::None; unseen(); f.edit.kind = GlassControlKind::ListBox; unseen();
        f.edit.kind = GlassControlKind::Edit;
        f.edit.cls = L"Edit"; unseen(); f.edit.cls = L"TEdit2"; unseen(); f.edit.cls = L"TEdit";
        f.edit.valid = false; unseen(); f.edit.valid = true;
        // The themed frame of an edit is painted through GetDC right before its
        // first erase: seeing the control there is enough.
        f.BeginDc(f.edit);
        CHECK(f.Kept() && f.All() == 1 && f.Refs() == 0);       // an edit's session takes no reference of its own
        f.EraseOutsidePaint(f.list, f.eraseDc);                 // already while that session is open
        f.EndDc(f.edit);
        CHECK(f.Kept() && f.exe.hooks.size() == 1 && f.All() == 1 && f.Idle());
        f.EraseOutsidePaint(f.list, f.eraseDc);
        e = Take();
        CHECK(e.size() == 4 && e[1].kind == Kind::Black && e[3].kind == Kind::Black);
        // Kept once: further controls and sessions add only their own references.
        CHECK(f.Begin(f.list, f.paintDc, false) && f.All() == 2 && f.Refs() == 1);
        f.BeginDc(f.tree);
        CHECK(f.Begin(f.list2, f.paintDc2, false) && f.All() == 3 && f.exe.hooks.size() == 1);
        f.End(f.list2, f.paintDc2); f.EndDc(f.tree); f.End(f.list, f.paintDc);
        CHECK(f.All() == 1 && f.Idle() && f.Kept());
        Take();
        // ...and a session no longer asks whether its window is such a control.
        BgTest::classLookups = 0;
        f.BeginDc(f.edit); f.EndDc(f.edit);
        CHECK(BgTest::classLookups == 1);           // the session's own question, as before
        // Shutdown releases it; after a restart nothing is hooked until a control
        // is seen again.
        Bg::Shutdown();
        CHECK(!f.Kept() && f.exe.hooks.empty() && f.All() == 0);
        Bg::Startup();
        CHECK(!f.Kept() && f.exe.hooks.empty());
        f.EraseOutsidePaint(f.list, f.eraseDc);
        e = Take();
        CHECK(e.size() == 2 && e[1].kind == Kind::NativeFill);
        // Every class is such a control, in a paint as well as in a GetDC.
        using namespace GlassCore::PaintDispatcher;
        for (const EraseRow& row : kEraseRows)
            for (const bool paint : {true, false})
            {
                TestWindow w; w.cls = row.cls; w.kind = row.kind; w.brush = &f.window;
                auto& provider = paint ? paintProviders.front() : dcProviders.front();
                provider.onBegin(&w, &f.exe);
                CHECK(f.Kept() && f.exe.hooks.size() == 1);
                provider.onEnd(&w, &f.exe);
                CHECK(f.Kept() && f.All() == 1 && f.Idle());
                Bg::Shutdown(); Bg::Startup();
                CHECK(!f.Kept() && f.exe.hooks.empty());
            }
        // Shutdown while sessions are open: their ends release nothing twice.
        CHECK(f.Begin(f.list, f.paintDc, false) && f.All() == 2); Take();
        Bg::Shutdown();
        CHECK(!f.Kept() && f.exe.hooks.empty() && f.All() == 0 && Bg::g_sessions.empty());
        f.End(f.list, f.paintDc);
        CHECK(f.All() == 0 && f.exe.hooks.empty());
        Bg::Startup();
        // Without the import, or without the EXE, nothing can be kept.
        f.exe.originals.clear();
        f.BeginDc(f.edit); f.EndDc(f.edit);
        CHECK(!f.Kept() && f.exe.hooks.empty() && f.All() == 0);
        f.EraseOutsidePaint(f.edit, f.editDc);
        e = Take();
        CHECK(e.size() == 2 && e[1].kind == Kind::NativeFill);
        f.exe.originals["FillRect"] = reinterpret_cast<void*>(&NativeFillRect);
        GlassCore::ModuleHandler::g_entryPointModule = nullptr;
        f.BeginDc(f.edit); f.EndDc(f.edit);
        CHECK(!f.Kept() && f.exe.hooks.empty());
        GlassCore::ModuleHandler::g_entryPointModule = &f.exe;
        // ...and it is tried again the next time such a control is seen.
        f.BeginDc(f.edit); f.EndDc(f.edit);
        CHECK(f.Kept() && f.All() == 1 && f.Idle());
        // Two threads see their first control at the same moment. The one that
        // comes second to the lock finds the import kept and adds nothing.
        Bg::Shutdown(); Bg::Startup();
        int raced = 0;
        BgTest::beforeExclusive = [&] {
            ++raced;
            f.BeginDc(f.tree); f.EndDc(f.tree);     // the other thread got there first
            CHECK(f.Kept() && f.All() == 1);
        };
        f.BeginDc(f.edit);
        CHECK(raced == 1 && f.Kept() && f.All() == 1);
        f.EndDc(f.edit);
        CHECK(f.Kept() && f.All() == 1 && f.Idle());
        // Shutdown comes between a session's first look and the lock: nothing
        // is hooked behind its back, neither for good nor for the session.
        for (const bool kept : {false, true})
        {
            Bg::Shutdown(); Bg::Startup();
            if (kept) { f.BeginDc(f.edit); f.EndDc(f.edit); CHECK(f.Kept()); }
            BgTest::beforeExclusive = [] { Bg::Shutdown(); };
            CHECK(!f.Begin(f.list, f.paintDc, false));
            CHECK(!BgTest::beforeExclusive && !f.Kept() && f.exe.hooks.empty() && f.All() == 0);
            f.End(f.list, f.paintDc);
            CHECK(Bg::g_sessions.empty() && Take().empty());
        }
        // ...and after it a session asks nothing about its window.
        BgTest::classLookups = 0;
        CHECK(!f.Begin(f.list, f.paintDc, false)); f.End(f.list, f.paintDc);
        f.BeginDc(f.edit); f.EndDc(f.edit);
        CHECK(BgTest::classLookups == 0 && !f.Kept() && f.exe.hooks.empty() && Take().empty());
    }

    // One line per fill that reached such a control outside its sessions: the
    // window, its class, the DC, the rectangle against the client and the clip,
    // the brush, and whether the fill was replaced. Debug builds only, 32 a
    // second.
    void OutsideDiagnostics()
    {
        Fixture f;
        CHECK(f.Begin(f.list, f.paintDc, true)); f.End(f.list, f.paintDc); Take();
        BgTest::clock += 1000; BgTest::logs.clear();
        f.EraseOutsidePaint(f.list, f.eraseDc);
        f.list.brush = &f.custom; f.EraseOutsidePaint(f.list, f.eraseDc);
        f.list.brush = nullptr; f.EraseOutsidePaint(f.list, f.eraseDc);
        f.list.brush = &f.hatch; f.EraseOutsidePaint(f.list, f.eraseDc);
        f.list.brush = &f.window;
        f.tree.client = RECT{0, 0, 285, 568};
        f.EraseOutsidePaint(f.tree, f.treeDc);
        LOGBRUSH flagged{BS_SOLID, 0x02ffffff};     // the high byte of a COLORREF is not a colour
        f.tree.brush = &flagged; f.EraseOutsidePaint(f.tree, f.treeDc); f.tree.brush = &f.window;
        BgTest::failObject = true;                  // a brush that tells nothing about itself
        f.EraseOutsidePaint(f.tree, f.treeDc);
        BgTest::failObject = false;
        // The erase of a part: the clip is the update region.
        f.treeDc.frame = true; f.treeDc.frameClip = RECT{10, 20, 60, 40}; f.treeDc.clipKind = 3;
        f.EraseOutsidePaint(f.tree, f.treeDc);
        f.treeDc.frame = false; f.treeDc.clipKind = SIMPLEREGION;
        [[maybe_unused]] auto lines = Logs(L"] erase-outside ");
#if defined(_DEBUG)
        // hwnd, dc, the rectangle, the client's size, the clip, whole, the brush, cleared
        const auto line = [](HWND w, HDC dc, const RECT& rect, int clipKind, const RECT& clip, int whole,
                             int style, long long rgb, int cleared) {
            return std::vector<long long>{Number(w), Number(dc), rect.left, rect.top, rect.right, rect.bottom,
                w->client.right, w->client.bottom, clipKind, clip.left, clip.top, clip.right, clip.bottom,
                whole, style, rgb, cleared};
        };
        const RECT big = f.tree.client;
        CHECK(lines.size() == 8);
        CHECK(lines[0].format == L"[GlassCore][DelphiBackground] erase-outside hwnd=0x%p class=%s dc=0x%p rect=[%ld,%ld,%ld,%ld] "
                                 L"client=[%ld,%ld] clip=%d[%ld,%ld,%ld,%ld] whole=%d style=%d rgb=0x%06X cleared=%d");
        CHECK(lines[0].texts == std::vector<std::wstring>{L"TListBox"});
        CHECK(lines[0].numbers == line(&f.list, &f.eraseDc, f.client, SIMPLEREGION, f.client, 1, 0, 0xffffff, 1));
        CHECK(lines[1].numbers == line(&f.list, &f.eraseDc, f.client, SIMPLEREGION, f.client, 1, 0, 0x00c0ff, 0));
        CHECK(lines[2].numbers == line(&f.list, &f.eraseDc, f.client, SIMPLEREGION, f.client, 1, -1, 0, 0));
        CHECK(lines[3].numbers == line(&f.list, &f.eraseDc, f.client, SIMPLEREGION, f.client, 1, 2, 0xffffff, 0));
        CHECK(lines[4].texts == std::vector<std::wstring>{L"TResTreeView"});
        CHECK(lines[4].numbers == line(&f.tree, &f.treeDc, big, SIMPLEREGION, big, 1, 0, 0xffffff, 1));
        CHECK(lines[5].numbers == line(&f.tree, &f.treeDc, big, SIMPLEREGION, big, 1, 0, 0xffffff, 1));
        CHECK(lines[6].numbers == line(&f.tree, &f.treeDc, big, SIMPLEREGION, big, 1, -1, 0, 0));
        CHECK(lines[7].numbers == line(&f.tree, &f.treeDc, big, 3, RECT{10, 20, 60, 40}, 1, 0, 0xffffff, 1));
        CHECK(BgTest::logs.size() == 8);            // nothing else is written for them
        // A fill that is not the erase is reported too, and says why: a part
        // of the client, a window DC of the frame, nothing to erase, no rectangle.
        BgTest::clock += 1000; BgTest::logs.clear();
        CHECK(f.Fill(&f.eraseDc, RECT{0, 16, 200, 32}, &f.window) == 1);
        f.eraseDc.frame = true; f.eraseDc.frameClip = RECT{-2, -2, 202, 122};
        CHECK(f.Fill(&f.eraseDc, f.client, &f.window) == 1);
        f.eraseDc.frame = false; f.eraseDc.clipKind = NULLREGION;
        CHECK(f.Fill(&f.eraseDc, f.client, &f.window) == 1);
        f.eraseDc.clipKind = SIMPLEREGION;
        CHECK(ExeFillRect(f.exe, &f.eraseDc, nullptr, &f.window) == 1);
        lines = Logs(L"] erase-outside ");
        CHECK(lines.size() == 4 && BgTest::logs.size() == 4);
        CHECK(lines[0].numbers == line(&f.list, &f.eraseDc, RECT{0, 16, 200, 32}, SIMPLEREGION, f.client, 0, 0, 0xffffff, 0));
        CHECK(lines[1].numbers == line(&f.list, &f.eraseDc, f.client, SIMPLEREGION, RECT{-2, -2, 202, 122}, 0, 0, 0xffffff, 0));
        CHECK(lines[2].numbers == line(&f.list, &f.eraseDc, f.client, NULLREGION, f.client, 0, 0, 0xffffff, 0));
        CHECK(lines[3].numbers == line(&f.list, &f.eraseDc, RECT{}, SIMPLEREGION, f.client, 0, 0, 0xffffff, 0));
        // Not reported: another window, a buffer, another thread, and a fill
        // inside a session of the window.
        BgTest::clock += 1000; BgTest::logs.clear();
        CHECK(f.Fill(&f.foreignDc, f.client, &f.window) == 1);
        CHECK(f.Fill(&f.memoryDc, f.client, &f.window) == 1);
        BgTest::thread = 2; CHECK(f.Fill(&f.eraseDc, f.client, &f.window) == 1); BgTest::thread = 1;
        f.BeginDc(f.edit); CHECK(f.Fill(&f.editDc, f.client, &f.window) == 1); f.EndDc(f.edit);
        CHECK(Logs(L"] erase-outside ").empty());
        // The repeated erase of a paint is another line.
        CHECK(f.Begin(f.list, f.paintDc, false)); f.End(f.list, f.paintDc);
        CHECK(Logs(L"] erase-outside ").empty() && Logs(L"] erase-again ").size() == 1);
        // 32 a second.
        BgTest::clock += 1000; BgTest::logs.clear(); Take();
        for (int i = 0; i < 50; ++i) f.EraseOutsidePaint(f.list, f.eraseDc);
        CHECK(Logs(L"] erase-outside ").size() == 32);
        CHECK(BgTest::Count(Kind::Black) == 50);    // the budget is the line's, not the fill's
        BgTest::clock += 999;
        f.EraseOutsidePaint(f.list, f.eraseDc);
        CHECK(Logs(L"] erase-outside ").size() == 32);
        BgTest::clock += 1;
        f.EraseOutsidePaint(f.list, f.eraseDc);
        CHECK(Logs(L"] erase-outside ").size() == 33);
#else
        CHECK(lines.empty() && BgTest::logs.empty());
#endif
        Take();
        CHECK(f.Idle() && f.Kept());
    }
}

int main(int argc, char** argv)
{
    try
    {
        if (argc != 2) throw std::runtime_error("one case required");
        const std::string name = argv[1];
        if (name == "erase-inside") EraseInside();
        else if (name == "erase-before") EraseBefore();
        else if (name == "own-background") OwnBackground();
        else if (name == "classes") Classes();
        else if (name == "gates") Gates();
        else if (name == "existing") Existing();
        else if (name == "reentry") Reentry();
        else if (name == "lifecycle") Lifecycle();
        else if (name == "diagnostics") Diagnostics();
        else if (name == "erase-outside") EraseOutside();
        else if (name == "outside-gates") OutsideGates();
        else if (name == "kept-import") KeptImport();
        else if (name == "outside-diagnostics") OutsideDiagnostics();
        else throw std::runtime_error("unknown case");
        std::cout << "PASS " << name << '\n';
    }
    catch (const std::exception& e) { std::cerr << argv[1] << ": " << e.what() << '\n'; return 1; }
}
