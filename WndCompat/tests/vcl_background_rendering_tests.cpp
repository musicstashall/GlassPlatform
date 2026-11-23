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
        TestWindow list, list2, other;
        TestBitmap surface{1}, surface2{2}, buffer{3};
        TestDC paintDc, paintDc2, canvasDc, memoryDc, foreignDc;
        LOGBRUSH window{BS_SOLID, 0xffffff}, face{BS_SOLID, 0xf0f0f0}, custom{BS_SOLID, 0x00c0ff};
        LOGBRUSH hatch{BS_HATCHED, 0xffffff};
        const RECT client{0, 0, 200, 120};

        Fixture()
        {
            using namespace BgTest;
            full = true; insideDraw = false; topWindow = nullptr; paintDcs.clear();
            windowColor = 0xffffff; faceColor = 0xf0f0f0; failBlack = failObject = false;
            nativeFillResult = 1; duringErase = {}; duringNativeFill = {};
            static ULONGLONG epoch = 0;
            BgTest::clock = epoch += 1000000; // a fresh rate-limit window for every fixture
            events.clear(); logs.clear();
            exe.originals["FillRect"] = reinterpret_cast<void*>(&NativeFillRect);
            GlassCore::ModuleHandler::g_entryPointModule = &exe;
            GlassCore::PaintDispatcher::paintProviders.clear();
            GlassCore::PaintDispatcher::postProviders.clear();
            GlassCore::PaintDispatcher::dcProviders.clear();
            list.brush = list2.brush = other.brush = &window;
            other.cls = L"TPanel"; other.kind = GlassControlKind::None;
            paintDc.owner = canvasDc.owner = &list; paintDc2.owner = &list2; foreignDc.owner = &other;
            paintDc.bitmap = canvasDc.bitmap = &surface; paintDc2.bitmap = &surface2; memoryDc.bitmap = &buffer;
            Bg::Startup();
            CHECK(GlassCore::PaintDispatcher::paintProviders.size() == 1
                && GlassCore::PaintDispatcher::postProviders.size() == 1
                && GlassCore::PaintDispatcher::dcProviders.size() == 1);
            CHECK(Idle());
        }
        ~Fixture()
        {
            BgTest::duringErase = {}; BgTest::duringNativeFill = {};
            Bg::Shutdown();
            assert(Bg::g_sessions.empty() && exe.hooks.empty());
        }
        bool Idle() { return exe.hooks.empty() && exe.refs["FillRect"] == 0 && Bg::g_sessions.empty() && !Bg::g_inside; }
        int Refs() { return exe.refs["FillRect"]; }

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
        CHECK(e.size() == 2 && e[1].kind == Kind::NativeFill && e[1].brush == &f.window); // not seen: no session, no hook
        CHECK(f.Idle());

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
        // Not armed at all: nothing is sent, the import is not hooked.
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
        // Without the import nothing can be replaced, so nothing is asked.
        f.exe.originals.clear();
        CHECK(!f.Begin(f.list, f.paintDc, false) && Take().empty() && !Bg::g_sessions.back().armed);
        f.End(f.list, f.paintDc);
        f.exe.originals["FillRect"] = reinterpret_cast<void*>(&NativeFillRect);
        CHECK(f.Idle());
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
        CHECK(e.size() == 2 && e[1].kind == Kind::NativeFill); // the hook is gone with the session
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
        CHECK(e.size() == 3 && e[1].kind == Kind::Black && e[2].kind == Kind::NativeFill && Bg::g_sessions.empty());
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
        else throw std::runtime_error("unknown case");
        std::cout << "PASS " << name << '\n';
    }
    catch (const std::exception& e) { std::cerr << argv[1] << ": " << e.what() << '\n'; return 1; }
}
