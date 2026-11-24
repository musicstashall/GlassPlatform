#include "restore_probe_test_shim.h"
#include "../Root/DelphiRestoreProbe.h"

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)

namespace
{
    namespace Probe = WndCompat::DelphiRestoreProbe;
    using GlassShared::GlassControlKind;
    using PrbTest::Log;

    long long Number(const void* p) { return static_cast<long long>(reinterpret_cast<std::intptr_t>(p)); }
    bool On(HWND w, UINT message, WPARAM wParam = 0, LPARAM lParam = 0) { return Probe::OnRootMessage(w, message, wParam, lParam); }
    bool On(HWND w, UINT message, const WINDOWPOS& pos) { return On(w, message, 0, reinterpret_cast<LPARAM>(&pos)); }

    // A form with a panel that holds a combo box (and its edit), a button, a
    // page that is not shown (with a control on it) and a window of no size.
    struct Fixture
    {
        TestWindow root, panel, combo, edit, button, page, onPage, empty;
        WINDOWPOS show{}, move{};

        Fixture()
        {
            using namespace PrbTest;
            static ULONGLONG epoch = 0;
            PrbTest::clock = epoch += 1000000;     // a quiet period of its own for every fixture
            foreground = nullptr; failDc = failPost = failTimer = false;
            surface = [](int, int) -> COLORREF { return 0; };
            posted.clear(); timers.clear(); logs.clear();
            timersSet = timersKilled = dcsTaken = dcsReleased = updateErases = 0;
            assert(liveDcs.empty());
#if defined(_DEBUG)
            Probe::t_state = {};
#endif
            const auto child = [](TestWindow& w, TestWindow& parent, const wchar_t* cls, RECT screen) {
                w.cls = cls; w.parent = &parent; w.screen = screen; parent.children.push_back(&w);
            };
            root.cls = L"TDelphiControlsForm"; root.screen = RECT{100, 100, 900, 700}; root.clientOrigin = POINT{108, 131};
            root.style = 0x16cf0000;
            child(panel, root, L"TPanel", RECT{108, 131, 508, 331});            // client [0,0,400,200]
            child(combo, panel, L"TComboBox", RECT{128, 151, 273, 172});        // client [20,20,165,41]
            child(edit, combo, L"Edit", RECT{131, 154, 253, 169});              // client [23,23,145,38]
            child(button, root, L"TButton", RECT{608, 200, 698, 225});          // client [500,69,590,94]
            child(page, root, L"TTabSheet", RECT{108, 331, 508, 531});
            child(onPage, page, L"TMemo", RECT{118, 341, 318, 441});
            child(empty, root, L"TBevel", RECT{300, 300, 300, 340});
            panel.style = 0x56010000; combo.style = 0x54010242; button.style = 0x5c010000;
            combo.kind = GlassControlKind::ComboBox; edit.kind = GlassControlKind::Edit; button.kind = GlassControlKind::Button;
            page.visible = false;               // the control on it keeps its own visible style
            combo.pending = button.pending = true;
            show.flags = SWP_SHOWWINDOW | SWP_NOSIZE;
            move.flags = SWP_NOSIZE;
        }
        ~Fixture() { assert(PrbTest::liveDcs.empty()); }
    };

    [[maybe_unused]] std::vector<Log> Take() { return std::exchange(PrbTest::logs, {}); }
    // The line of the top-level window: the moment and the class; the window,
    // whether it is visible, minimised and in the foreground.
    [[maybe_unused]] bool Header(const Log& log, const wchar_t* moment, HWND root, int visible, int iconic, int foreground)
    {
        return log.format == L"[WndCompat][DelphiRestore] %s root=0x%p class=%s visible=%d iconic=%d foreground=%d"
            && log.texts == std::vector<std::wstring>{moment, root->cls}
            && log.numbers == std::vector<long long>{Number(root), visible, iconic, foreground};
    }
    // The line of a child: the moment, its class and its parent's; the window,
    // its parent, kind and style, its rectangle in the client of the top-level
    // window, the three pixels and whether a paint is pending.
    [[maybe_unused]] bool Child(const Log& log, const wchar_t* moment, HWND w, RECT rect,
        long long centre, long long quarter, long long corner)
    {
        return log.format == L"[WndCompat][DelphiRestore] %s hwnd=0x%p class=%s parent=0x%p pclass=%s kind=%u style=0x%08X "
                             L"rect=[%ld,%ld,%ld,%ld] px=0x%08X/0x%08X/0x%08X pending=%d"
            && log.texts == std::vector<std::wstring>{moment, w->cls, w->parent->cls}
            && log.numbers == std::vector<long long>{Number(w), Number(w->parent), static_cast<long long>(w->kind),
                static_cast<long long>(w->style), rect.left, rect.top, rect.right, rect.bottom,
                centre, quarter, corner, w->pending ? 1 : 0};
    }

#if defined(_DEBUG)
    // A form comes back: its frame is painted and its controls are erased at
    // once, they are painted later. The probe reads the surface under every
    // visible control before the erase, after it and after the paint.
    void Sequence()
    {
        Fixture f;
        // The surface: the button is white, the left half of the combo box is
        // light, everything else holds nothing yet.
        PrbTest::surface = [](int x, int y) -> COLORREF {
            if (x >= 500 && x < 590 && y >= 69 && y < 94) return 0xffffff;
            if (x >= 20 && x < 90 && y >= 20 && y < 41) return 0xf0f0f0;
            return 0;
        };
        PrbTest::foreground = &f.root;
        CHECK(!On(&f.root, WM_WINDOWPOSCHANGING, f.show));
        CHECK(PrbTest::logs.empty() && PrbTest::posted.empty() && PrbTest::dcsTaken == 0);
        // The first thing Windows does with the window once it is visible.
        CHECK(!On(&f.root, WM_NCPAINT, 1));
        auto lines = Take();
        CHECK(lines.size() == 5 && Header(lines[0], L"shown", &f.root, 1, 0, 1));
        CHECK(Child(lines[1], L"shown", &f.panel, RECT{0, 0, 400, 200}, 0, 0, 0));
        // centre (92,30) is past the light half, the left quarter (56,30) is in it
        CHECK(Child(lines[2], L"shown", &f.combo, RECT{20, 20, 165, 41}, 0, 0xf0f0f0, 0xf0f0f0));
        CHECK(Child(lines[3], L"shown", &f.edit, RECT{23, 23, 145, 38}, 0xf0f0f0, 0xf0f0f0, 0xf0f0f0));
        CHECK(Child(lines[4], L"shown", &f.button, RECT{500, 69, 590, 94}, 0xffffff, 0xffffff, 0xffffff));
        // One DC of the top-level window, not clipped by its children, and released.
        CHECK(PrbTest::dcsTaken == 1 && PrbTest::dcsReleased == 1 && PrbTest::updateErases == 0);
        // The next moment is asked for through the queue.
        CHECK(PrbTest::posted.size() == 1 && PrbTest::posted[0].hwnd == &f.root && PrbTest::posted[0].message == WM_NULL);
        CHECK(PrbTest::posted[0].wParam == 0x47504C52 && PrbTest::posted[0].lParam == 0x52455354);
        CHECK(PrbTest::timers.empty());
        // The erase of the window itself and the frames that follow change nothing.
        CHECK(!On(&f.root, WM_ERASEBKGND, 1) && !On(&f.root, WM_NCPAINT, 1) && !On(&f.root, WM_WINDOWPOSCHANGING, f.show));
        CHECK(PrbTest::logs.empty() && PrbTest::posted.size() == 1 && PrbTest::dcsTaken == 1);

        // Windows has erased the controls: the combo box is dark now.
        PrbTest::surface = [](int x, int y) -> COLORREF { return x >= 500 && x < 590 && y >= 69 && y < 94 ? 0xffffff : 0; };
        CHECK(On(&f.root, WM_NULL, PrbTest::posted[0].wParam, PrbTest::posted[0].lParam));     // the probe's own: not passed on
        lines = Take();
        CHECK(lines.size() == 5 && Header(lines[0], L"erased", &f.root, 1, 0, 1));
        CHECK(Child(lines[2], L"erased", &f.combo, RECT{20, 20, 165, 41}, 0, 0, 0));
        CHECK(Child(lines[4], L"erased", &f.button, RECT{500, 69, 590, 94}, 0xffffff, 0xffffff, 0xffffff));
        CHECK(PrbTest::dcsTaken == 2 && PrbTest::dcsReleased == 2);
        CHECK(PrbTest::timers.size() == 1 && PrbTest::timers[0].hwnd == &f.root && PrbTest::timers[0].id == 0x47504C52);
        CHECK(PrbTest::timers[0].elapse == 100 && PrbTest::posted.size() == 1);
        // The same message twice is not the probe's any more.
        CHECK(!On(&f.root, WM_NULL, PrbTest::posted[0].wParam, PrbTest::posted[0].lParam) && PrbTest::logs.empty());

        // The controls have painted.
        PrbTest::surface = [](int, int) -> COLORREF { return 0x202020; };
        f.combo.pending = f.button.pending = false;
        PrbTest::foreground = nullptr;
        CHECK(On(&f.root, WM_TIMER, 0x47504C52));
        lines = Take();
        CHECK(lines.size() == 5 && Header(lines[0], L"painted", &f.root, 1, 0, 0));
        CHECK(Child(lines[1], L"painted", &f.panel, RECT{0, 0, 400, 200}, 0x202020, 0x202020, 0x202020));
        CHECK(Child(lines[4], L"painted", &f.button, RECT{500, 69, 590, 94}, 0x202020, 0x202020, 0x202020));
        CHECK(PrbTest::timers.empty() && PrbTest::timersSet == 1 && PrbTest::timersKilled == 1);
        CHECK(PrbTest::dcsTaken == 3 && PrbTest::dcsReleased == 3 && PrbTest::updateErases == 0);
        // It is over: the timer of someone else with that number, a frame, an erase.
        CHECK(!On(&f.root, WM_TIMER, 0x47504C52) && !On(&f.root, WM_NCPAINT, 1) && !On(&f.root, WM_ERASEBKGND, 1));
        CHECK(PrbTest::logs.empty() && PrbTest::posted.size() == 1 && PrbTest::timersSet == 1);
    }

    // A minimised window is not shown again, it stops being minimised: the
    // moment is the same.
    void Restore()
    {
        Fixture f;
        f.root.iconic = true;
        CHECK(!On(&f.root, WM_WINDOWPOSCHANGING, f.move));      // no SWP_SHOWWINDOW here
        // Still on the taskbar: its frame there is not the moment.
        CHECK(!On(&f.root, WM_NCPAINT, 1) && PrbTest::logs.empty() && PrbTest::posted.empty());
        f.root.iconic = false;
        CHECK(!On(&f.root, WM_ERASEBKGND, 1));                  // whichever of the two comes first
        auto lines = Take();
        CHECK(lines.size() == 5 && Header(lines[0], L"shown", &f.root, 1, 0, 0) && PrbTest::posted.size() == 1);
        // A window that is neither shown nor minimised is only moved.
        Fixture g;
        CHECK(!On(&g.root, WM_WINDOWPOSCHANGING, g.move) && !On(&g.root, WM_NCPAINT, 1));
        CHECK(PrbTest::logs.empty() && PrbTest::posted.empty());
        // Not visible yet when its frame is asked for: the moment has not come.
        Fixture h;
        h.root.visible = false;
        CHECK(!On(&h.root, WM_WINDOWPOSCHANGING, h.show) && !On(&h.root, WM_NCPAINT, 1) && PrbTest::logs.empty());
        h.root.visible = true;
        CHECK(!On(&h.root, WM_NCPAINT, 1));
        CHECK(Take().size() == 5);
    }

    // Only a VCL top-level window, only its own messages.
    void Gates()
    {
        {
            Fixture f;
            // A control is shown, a control was minimised (an MDI child): not a top-level window.
            CHECK(!On(&f.panel, WM_WINDOWPOSCHANGING, f.show) && !On(&f.panel, WM_NCPAINT, 1) && !On(&f.panel, WM_ERASEBKGND, 1));
            f.button.iconic = true;
            CHECK(!On(&f.button, WM_WINDOWPOSCHANGING, f.move) && !On(&f.button, WM_NCPAINT, 1));
            CHECK(PrbTest::logs.empty() && PrbTest::posted.empty());
        }
        for (const wchar_t* cls : {L"#32770", L"CabinetWClass", L"tForm1", L"", L"MyTForm"})
        {
            Fixture f;
            f.root.cls = cls;
            CHECK(!On(&f.root, WM_WINDOWPOSCHANGING, f.show) && !On(&f.root, WM_NCPAINT, 1));
            CHECK(PrbTest::logs.empty() && PrbTest::posted.empty());
        }
        for (const wchar_t* cls : {L"TForm1", L"TfrmMain", L"T"})
        {
            Fixture f;
            f.root.cls = cls;
            CHECK(!On(&f.root, WM_WINDOWPOSCHANGING, f.show) && !On(&f.root, WM_NCPAINT, 1));
            CHECK(Take().size() == 5);
        }
        {
            Fixture f;
            // No position at all.
            CHECK(!On(&f.root, WM_WINDOWPOSCHANGING, 0, 0) && !On(&f.root, WM_NCPAINT, 1) && PrbTest::logs.empty());
            // Without the window having been shown, nothing is the probe's.
            CHECK(!On(&f.root, WM_NULL, 0x47504C52, 0x52455354) && !On(&f.root, WM_TIMER, 0x47504C52));
            CHECK(!On(&f.root, WM_ERASEBKGND, 1) && !On(&f.root, WM_PAINT) && PrbTest::logs.empty());
        }
        {
            Fixture f;
            TestWindow second; second.cls = L"TForm2"; second.screen = RECT{0, 0, 50, 50};
            CHECK(!On(&f.root, WM_WINDOWPOSCHANGING, f.show));
            // Another window's frame is not this one's moment.
            CHECK(!On(&second, WM_NCPAINT, 1) && !On(&second, WM_ERASEBKGND, 1) && PrbTest::logs.empty());
            CHECK(!On(&f.root, WM_NCPAINT, 1));
            Take();
            // The posted message is told by all three: the window and both numbers.
            CHECK(!On(&second, WM_NULL, 0x47504C52, 0x52455354));
            CHECK(!On(&f.root, WM_NULL, 0, 0) && !On(&f.root, WM_NULL, 0x47504C52, 0) && !On(&f.root, WM_NULL, 0, 0x52455354));
            CHECK(!On(&f.root, WM_TIMER, 0x47504C52));          // the timer is not set yet
            CHECK(PrbTest::logs.empty() && PrbTest::timers.empty());
            CHECK(On(&f.root, WM_NULL, 0x47504C52, 0x52455354));
            Take();
            // ...and the timer by its number and its window.
            CHECK(!On(&f.root, WM_TIMER, 1) && !On(&f.root, WM_TIMER, 0x47504C53) && !On(&second, WM_TIMER, 0x47504C52));
            CHECK(PrbTest::logs.empty() && PrbTest::timers.size() == 1);
            CHECK(On(&f.root, WM_TIMER, 0x47504C52));
            CHECK(Take().size() == 5);
        }
    }

    // What is listed: every visible descendant with an area, at most 200 of them.
    void Children()
    {
        Fixture f;
        CHECK(!On(&f.root, WM_WINDOWPOSCHANGING, f.show) && !On(&f.root, WM_NCPAINT, 1));
        auto lines = Take();
        // Neither the page that is not shown, nor the control on it, nor the window of no size.
        CHECK(lines.size() == 5);
        for (const Log& log : lines)
            for (const std::wstring& text : log.texts) CHECK(text != L"TTabSheet" && text != L"TMemo" && text != L"TBevel");
        // A window that is gone is passed over, and with it its edit, which is not visible then.
        Fixture g;
        g.combo.valid = false;
        CHECK(!On(&g.root, WM_WINDOWPOSCHANGING, g.show) && !On(&g.root, WM_NCPAINT, 1));
        lines = Take();
        CHECK(lines.size() == 3 && Child(lines[1], L"shown", &g.panel, RECT{0, 0, 400, 200}, 0, 0, 0));
        CHECK(Child(lines[2], L"shown", &g.button, RECT{500, 69, 590, 94}, 0, 0, 0));
        // So is a window that cannot tell its class; its children are still listed.
        Fixture n;
        n.combo.cls = L"";
        CHECK(!On(&n.root, WM_WINDOWPOSCHANGING, n.show) && !On(&n.root, WM_NCPAINT, 1));
        lines = Take();
        CHECK(lines.size() == 4 && Child(lines[1], L"shown", &n.panel, RECT{0, 0, 400, 200}, 0, 0, 0));
        CHECK(Child(lines[2], L"shown", &n.edit, RECT{23, 23, 145, 38}, 0, 0, 0));
        CHECK(Child(lines[3], L"shown", &n.button, RECT{500, 69, 590, 94}, 0, 0, 0));
        // The three points: the centre, the left quarter on the same line, and
        // four pixels in from the top left corner.
        Fixture q;
        TestWindow narrow, flat;    // too small for four pixels across, or down: half of what there is
        narrow.cls = flat.cls = L"TEdit"; narrow.parent = flat.parent = &q.root;
        narrow.screen = RECT{708, 431, 714, 451};     // client [600,300,606,320]
        flat.screen = RECT{728, 431, 748, 434};       // client [620,300,640,303]
        q.root.children.push_back(&narrow); q.root.children.push_back(&flat);
        PrbTest::surface = [](int x, int y) -> COLORREF { return static_cast<COLORREF>((y << 12) | x); };
        CHECK(!On(&q.root, WM_WINDOWPOSCHANGING, q.show) && !On(&q.root, WM_NCPAINT, 1));
        lines = Take();
        CHECK(lines.size() == 7);
        CHECK(Child(lines[1], L"shown", &q.panel, RECT{0, 0, 400, 200}, (100 << 12) | 200, (100 << 12) | 100, (4 << 12) | 4));
        CHECK(Child(lines[2], L"shown", &q.combo, RECT{20, 20, 165, 41}, (30 << 12) | 92, (30 << 12) | 56, (24 << 12) | 24));
        CHECK(Child(lines[3], L"shown", &q.edit, RECT{23, 23, 145, 38}, (30 << 12) | 84, (30 << 12) | 53, (27 << 12) | 27));
        CHECK(Child(lines[4], L"shown", &q.button, RECT{500, 69, 590, 94}, (81 << 12) | 545, (81 << 12) | 522, (73 << 12) | 504));
        CHECK(Child(lines[5], L"shown", &narrow, RECT{600, 300, 606, 320}, (310 << 12) | 603, (310 << 12) | 601, (304 << 12) | 603));
        CHECK(Child(lines[6], L"shown", &flat, RECT{620, 300, 640, 303}, (301 << 12) | 630, (301 << 12) | 625, (301 << 12) | 624));
        // A parent of no class still has a child that is listed.
        Fixture h;
        std::vector<TestWindow> many(250);
        for (size_t i = 0; i < many.size(); ++i)
        {
            many[i].cls = L"TEdit"; many[i].parent = &h.panel;
            many[i].screen = RECT{108, 131 + static_cast<LONG>(i), 208, 132 + static_cast<LONG>(i)};
            h.panel.children.push_back(&many[i]);
        }
        CHECK(!On(&h.root, WM_WINDOWPOSCHANGING, h.show) && !On(&h.root, WM_NCPAINT, 1));
        lines = Take();
        CHECK(lines.size() == 201 && Header(lines[0], L"shown", &h.root, 1, 0, 0));    // the line of the window and 200 of its children
        CHECK(PrbTest::dcsTaken == 1 && PrbTest::dcsReleased == 1);
        // The budget is each moment's own.
        CHECK(On(&h.root, WM_NULL, 0x47504C52, 0x52455354));
        CHECK(Take().size() == 201);
        // A point the DC cannot read is reported as it is.
        Fixture k;
        PrbTest::surface = [](int x, int) -> COLORREF { return x > 300 ? CLR_INVALID : 0x010203; };
        CHECK(!On(&k.root, WM_WINDOWPOSCHANGING, k.show) && !On(&k.root, WM_NCPAINT, 1));
        lines = Take();
        CHECK(Child(lines[4], L"shown", &k.button, RECT{500, 69, 590, 94}, 0xffffffffll, 0xffffffffll, 0xffffffffll));
        CHECK(Child(lines[2], L"shown", &k.combo, RECT{20, 20, 165, 41}, 0x010203, 0x010203, 0x010203));
    }

    // One probe in three seconds; a probe that could not go on is given up.
    void Limits()
    {
        Fixture f;
        const auto whole = [&] {
            CHECK(!On(&f.root, WM_WINDOWPOSCHANGING, f.show) && !On(&f.root, WM_NCPAINT, 1));
            CHECK(On(&f.root, WM_NULL, 0x47504C52, 0x52455354) && On(&f.root, WM_TIMER, 0x47504C52));
            return Take().size();
        };
        CHECK(whole() == 15);
        // Shown again at once: nothing.
        PrbTest::clock += 2999;
        CHECK(!On(&f.root, WM_WINDOWPOSCHANGING, f.show) && !On(&f.root, WM_NCPAINT, 1) && PrbTest::logs.empty());
        PrbTest::clock += 1;
        CHECK(whole() == 15);
        // Armed and never shown: the same window stays armed, however often it is asked.
        PrbTest::clock += 3000;
        CHECK(!On(&f.root, WM_WINDOWPOSCHANGING, f.show));
        PrbTest::clock += 10;
        CHECK(!On(&f.root, WM_WINDOWPOSCHANGING, f.show));
        PrbTest::clock += 100000;
        CHECK(!On(&f.root, WM_NCPAINT, 1));
        CHECK(Take().size() == 5);
        // A probe in progress is not restarted by another show within the quiet time...
        CHECK(!On(&f.root, WM_WINDOWPOSCHANGING, f.show) && !On(&f.root, WM_NCPAINT, 1) && PrbTest::logs.empty());
        CHECK(On(&f.root, WM_NULL, 0x47504C52, 0x52455354));
        Take();
        CHECK(PrbTest::timers.size() == 1);
        // ...and after it a new one takes its place: the timer of the old one is gone.
        PrbTest::clock += 3000;
        const int killed = PrbTest::timersKilled;
        CHECK(!On(&f.root, WM_WINDOWPOSCHANGING, f.show));
        CHECK(PrbTest::timers.empty() && PrbTest::timersKilled == killed + 1);
        CHECK(!On(&f.root, WM_TIMER, 0x47504C52) && PrbTest::logs.empty());
        CHECK(!On(&f.root, WM_NCPAINT, 1));
        CHECK(Take().size() == 5);

        // The message could not be posted, the timer could not be set: given up, nothing is left waiting.
        Fixture g;
        PrbTest::failPost = true;
        CHECK(!On(&g.root, WM_WINDOWPOSCHANGING, g.show) && !On(&g.root, WM_NCPAINT, 1));
        CHECK(Take().size() == 5 && PrbTest::posted.empty());
        PrbTest::failPost = false;
        CHECK(!On(&g.root, WM_NULL, 0x47504C52, 0x52455354) && !On(&g.root, WM_NCPAINT, 1) && PrbTest::logs.empty());
        Fixture h;
        PrbTest::failTimer = true;
        CHECK(!On(&h.root, WM_WINDOWPOSCHANGING, h.show) && !On(&h.root, WM_NCPAINT, 1));
        CHECK(On(&h.root, WM_NULL, 0x47504C52, 0x52455354));
        CHECK(Take().size() == 10 && PrbTest::timers.empty());
        PrbTest::failTimer = false;
        CHECK(!On(&h.root, WM_TIMER, 0x47504C52) && PrbTest::logs.empty());
        // No DC: the line of the window alone, and the probe goes on.
        Fixture k;
        PrbTest::failDc = true;
        CHECK(!On(&k.root, WM_WINDOWPOSCHANGING, k.show) && !On(&k.root, WM_NCPAINT, 1));
        auto lines = Take();
        CHECK(lines.size() == 1 && Header(lines[0], L"shown", &k.root, 1, 0, 0) && PrbTest::dcsTaken == 0);
        PrbTest::failDc = false;
        CHECK(On(&k.root, WM_NULL, 0x47504C52, 0x52455354));
        CHECK(Take().size() == 5);
        // The window is destroyed in the middle: its timer is killed and nothing is the probe's.
        CHECK(PrbTest::timers.size() == 1);
        CHECK(!On(&k.panel, WM_NCDESTROY) && PrbTest::timers.size() == 1);     // a child's end is not the window's
        CHECK(!On(&k.root, WM_NCDESTROY));
        CHECK(PrbTest::timers.empty() && !On(&k.root, WM_TIMER, 0x47504C52) && PrbTest::logs.empty());
        // Destroyed before its frame was asked for.
        Fixture m;
        CHECK(!On(&m.root, WM_WINDOWPOSCHANGING, m.show) && !On(&m.root, WM_NCDESTROY) && !On(&m.root, WM_NCPAINT, 1));
        CHECK(PrbTest::logs.empty() && PrbTest::timersKilled == 0);
    }
#else
    // A release build has no probe: no message is its own, nothing is read,
    // posted, timed or written.
    void Absent()
    {
        Fixture f;
        f.root.iconic = true;
        CHECK(!On(&f.root, WM_WINDOWPOSCHANGING, f.show) && !On(&f.root, WM_WINDOWPOSCHANGING, f.move));
        f.root.iconic = false;
        CHECK(!On(&f.root, WM_NCPAINT, 1) && !On(&f.root, WM_ERASEBKGND, 1));
        CHECK(!On(&f.root, WM_NULL, 0x47504C52, 0x52455354) && !On(&f.root, WM_TIMER, 0x47504C52) && !On(&f.root, WM_NCDESTROY));
        CHECK(PrbTest::logs.empty() && PrbTest::posted.empty() && PrbTest::timers.empty());
        CHECK(PrbTest::dcsTaken == 0 && PrbTest::timersSet == 0 && PrbTest::updateErases == 0);
    }
#endif
}

int main(int argc, char** argv)
{
    try
    {
        if (argc != 2) throw std::runtime_error("one case required");
        const std::string name = argv[1];
#if defined(_DEBUG)
        if (name == "sequence") Sequence();
        else if (name == "restore") Restore();
        else if (name == "gates") Gates();
        else if (name == "children") Children();
        else if (name == "limits") Limits();
        else if (name == "absent") {}
#else
        if (name == "absent") Absent();
        else if (name == "sequence" || name == "restore" || name == "gates" || name == "children" || name == "limits") {}
#endif
        else throw std::runtime_error("unknown case");
        std::cout << "PASS " << name << '\n';
    }
    catch (const std::exception& e) { std::cerr << argv[1] << ": " << e.what() << '\n'; return 1; }
}
