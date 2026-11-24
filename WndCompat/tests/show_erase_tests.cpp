#include "show_erase_test_shim.h"
#include "../Root/DelphiShowErase.h"

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)

namespace
{
    namespace Erase = WndCompat::DelphiShowErase;

    void On(HWND w, UINT message, LPARAM lParam = 0) { Erase::OnRootMessage(w, message, lParam); }
    void On(HWND w, UINT message, const WINDOWPOS& pos) { On(w, message, reinterpret_cast<LPARAM>(&pos)); }

    // A world of its own for every scenario: nothing is armed, nothing is drawn.
    void Reset()
    {
        using namespace SeTest;
        assert(liveRegions.empty() && liveDcs.empty());
        static ULONGLONG epoch = 0;
        SeTest::clock = epoch += 1000000;
        thread = 1; process = 0x000073F8; mode = Shared::InjectionMode::Full;
        surface.clear(); logs.clear();
        regionsMade = dcsTaken = dcsReleased = blits = clipChildrenBlits = 0;
        updateQueries = updateErases = pixelReads = 0;
        failCreate = failCombine = failSetRect = failOffset = failSelect = 0;
        creates = combines = setRects = offsets = selects = 0;
        failDc = failBlit = failBox = false;
        failUpdate = failClient = failRect = nullptr;
        Erase::t_state = {};
    }
    // The rectangle on the screen of a rectangle given in the client of `root`.
    RECT At(const TestWindow& root, RECT r)
    {
        const RECT c = root.Client();
        return RECT{c.left + r.left, c.top + r.top, c.left + r.right, c.top + r.bottom};
    }
    // A child on top of the children its parent already has.
    void Child(TestWindow& w, TestWindow& parent, const TestWindow& root, RECT inRoot, RECT frame = {})
    {
        w.parent = &parent; w.screen = At(root, inRoot); w.frame = frame;
        parent.children.insert(parent.children.begin(), &w);
    }
    void Pending(TestWindow& w)
    {
        RECT client{};
        GetClientRect(&w, &client);
        w.update = RectPoints(client);
    }
    // A VCL form: a client of 60 x 40 behind a caption and a frame.
    void Form(TestWindow& root)
    {
        root.cls = L"TRstForm";
        root.screen = RECT{100, 100, 168, 150};
        root.frame = RECT{4, 6, 4, 4};
        root.props[L"Delphi000073F8"] = &root;
        Pending(root);
    }
    const WINDOWPOS kShow{nullptr, nullptr, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOSIZE};
    const WINDOWPOS kMove{nullptr, nullptr, 0, 0, 0, 0, SWP_NOSIZE};
    void Show(TestWindow& root) { On(&root, WM_WINDOWPOSCHANGING, kShow); On(&root, WM_NCPAINT); }

    bool Cleared(LONG x, LONG y)
    {
        const auto it = SeTest::surface.find({x, y});
        return it != SeTest::surface.end() && it->second == 0;
    }
    bool Untouched() { return SeTest::surface.empty() && SeTest::blits == 0; }
    bool Released() { return SeTest::liveRegions.empty() && SeTest::liveDcs.empty() && SeTest::dcsTaken == SeTest::dcsReleased; }
    bool Armed(HWND w) { return Erase::t_state.root == w; }

    // Independent of the unit, a point at a time: the topmost visible window
    // at this point of the top-level client, and whether that window is about
    // to repaint the point. A frame is never anybody's to clear.
    bool Expected(const TestWindow& root, LONG x, LONG y)
    {
        const RECT base = root.Client();
        const LONG sx = base.left + x, sy = base.top + y;
        const auto inside = [&](const RECT& r) { return sx >= r.left && sx < r.right && sy >= r.top && sy < r.bottom; };
        if (!inside(base)) return false;
        const TestWindow* w = &root;
        for (unsigned depth = 1;; ++depth)
        {
            const TestWindow* next = nullptr;
            for (const TestWindow* child : w->children)
            {
                if (!(child->style & WS_VISIBLE) || (child->ex & WS_EX_LAYERED) || !inside(child->screen)) continue;
                next = child;
                break;
            }
            if (!next) break;
            if ((next->ex & WS_EX_LAYOUTRTL) || next->thread != SeTest::thread || depth >= Erase::kMaxDepth) return false;
            if (!inside(next->Client())) return false;
            w = next;
        }
        const RECT c = w->Client();
        return w->update.count({sx - c.left, sy - c.top}) != 0;
    }
    // Every point of the client was cleared exactly where it was expected to be.
    bool Matches(const TestWindow& root)
    {
        RECT client{};
        GetClientRect(const_cast<TestWindow*>(&root), &client);
        for (LONG y = 0; y < client.bottom; ++y)
            for (LONG x = 0; x < client.right; ++x)
                if (Cleared(x, y) != Expected(root, x, y))
                {
                    std::cerr << "point " << x << ',' << y << " cleared=" << Cleared(x, y) << " expected=" << Expected(root, x, y) << '\n';
                    return false;
                }
        for (const auto& p : SeTest::surface)
            if (p.first.first < 0 || p.first.second < 0 || p.first.first >= client.right || p.first.second >= client.bottom) return false;
        return true;
    }
    bool AllIn(RECT r, bool cleared)
    {
        for (LONG y = r.top; y < r.bottom; ++y) for (LONG x = r.left; x < r.right; ++x) if (Cleared(x, y) != cleared) return false;
        return true;
    }

    // Restorator's form comes back: it had been hidden with its minimised
    // owner, and its surface holds a light picture under every control. All of
    // them are waiting for a paint, so all of it is cleared at once - before
    // Windows erases the controls, of which a button and a toolbar draw nothing.
    void Restored()
    {
        Reset();
        TestWindow root, panel, toolbar, button, combo, edit, list, status;
        Form(root);
        Child(panel, root, root, RECT{0, 0, 60, 10});
        Child(toolbar, panel, root, RECT{2, 2, 30, 9}, RECT{1, 1, 1, 1});       // the edge VCL draws around a toolbar
        Child(button, root, root, RECT{40, 20, 52, 26});
        Child(combo, root, root, RECT{10, 20, 30, 26});
        Child(edit, combo, root, RECT{11, 21, 29, 25});
        Child(list, combo, root, RECT{10, 26, 30, 28});                         // below the client of its combo box: never seen
        Child(status, root, root, RECT{0, 36, 60, 40});
        for (TestWindow* w : {&panel, &toolbar, &button, &combo, &edit, &status}) Pending(*w);

        On(&root, WM_WINDOWPOSCHANGING, kShow);
        CHECK(Untouched() && SeTest::dcsTaken == 0 && SeTest::regionsMade == 0 && Armed(&root));
        // The first thing Windows asks the window for once it is visible.
        On(&root, WM_NCPAINT);
        CHECK(Matches(root));
        // All of the client, but the frame of the toolbar: that is WM_NCPAINT's.
        CHECK(AllIn(RECT{0, 0, 60, 2}, true) && AllIn(RECT{3, 3, 29, 8}, true) && AllIn(RECT{0, 10, 60, 40}, true));
        CHECK(AllIn(RECT{2, 2, 30, 3}, false) && AllIn(RECT{2, 8, 30, 9}, false) && !Cleared(2, 5) && !Cleared(29, 5));
        // What lies under the list that nobody sees is the form's.
        CHECK(AllIn(RECT{10, 26, 30, 28}, true));
        // One DC of the form that its children do not clip; nothing is left behind.
        CHECK(SeTest::dcsTaken == 1 && SeTest::blits == 1 && SeTest::clipChildrenBlits == 0 && Released());
        CHECK(SeTest::regionsMade == 3 && SeTest::updateErases == 0 && SeTest::selects == 2);
        // Once: the erase that follows and the frames after it change nothing.
        CHECK(!Armed(&root));
        SeTest::surface.clear();
        On(&root, WM_ERASEBKGND); On(&root, WM_NCPAINT);
        CHECK(SeTest::surface.empty() && SeTest::dcsTaken == 1 && SeTest::regionsMade == 3);
        CHECK(SeTest::logs.empty() && SeTest::pixelReads == 0);

        // A window without a frame of its own is asked to erase first.
        Reset();
        TestWindow plain, memo;
        Form(plain);
        Child(memo, plain, plain, RECT{5, 5, 40, 30}, RECT{2, 2, 2, 2});
        Pending(memo);
        On(&plain, WM_WINDOWPOSCHANGING, kShow);
        On(&plain, WM_ERASEBKGND);
        CHECK(Matches(plain) && AllIn(RECT{7, 7, 38, 28}, true) && !Cleared(5, 5) && !Cleared(39, 29) && Cleared(0, 0));
        CHECK(Released() && !Armed(&plain));

        // A window that leaves the taskbar is armed by its state alone.
        Reset();
        TestWindow icon, track;
        Form(icon);
        Child(track, icon, icon, RECT{8, 8, 48, 14});
        Pending(track);
        icon.iconic = true;
        On(&icon, WM_WINDOWPOSCHANGING, kMove);
        On(&icon, WM_NCPAINT);                      // still on the taskbar: not yet
        CHECK(Untouched() && Armed(&icon));
        icon.iconic = false;
        On(&icon, WM_NCPAINT);
        CHECK(Matches(icon) && AllIn(RECT{0, 0, 60, 40}, true) && Released());
    }

    // Only what its owner is about to repaint. A window with nothing pending
    // keeps its picture, whatever lies beneath it or around it.
    void Kept()
    {
        // A control that is not waiting for a paint, on a form that is.
        Reset();
        TestWindow root, valid, inner;
        Form(root);
        Child(valid, root, root, RECT{10, 10, 30, 20}, RECT{1, 1, 1, 1});
        Child(inner, valid, root, RECT{14, 13, 20, 17});
        Pending(inner);                             // ...with a child that is
        Show(root);
        CHECK(Matches(root));
        CHECK(AllIn(RECT{14, 13, 20, 17}, true));
        CHECK(AllIn(RECT{10, 10, 30, 13}, false) && AllIn(RECT{10, 17, 30, 20}, false) && !Cleared(13, 15) && !Cleared(20, 15));
        CHECK(AllIn(RECT{0, 0, 60, 10}, true) && AllIn(RECT{30, 10, 60, 20}, true));
        CHECK(Released());

        // Two controls that overlap: the one on top decides where both are.
        Reset();
        TestWindow form, below, above;
        Form(form);
        form.update.clear();                        // the form itself is valid
        Child(below, form, form, RECT{5, 5, 25, 15});
        Child(above, form, form, RECT{20, 10, 40, 20});
        Pending(below);
        Show(form);
        CHECK(Matches(form) && AllIn(RECT{5, 5, 20, 15}, true) && AllIn(RECT{20, 5, 25, 10}, true));
        CHECK(AllIn(RECT{20, 10, 40, 20}, false) && !Cleared(0, 0) && !Cleared(45, 12));
        Reset();
        below.update.clear(); Pending(above);
        Show(form);
        CHECK(Matches(form) && AllIn(RECT{20, 10, 40, 20}, true) && AllIn(RECT{5, 5, 20, 15}, false) && AllIn(RECT{20, 5, 25, 10}, false));

        // A part of a control is pending: that part.
        Reset();
        TestWindow whole, part;
        Form(whole);
        whole.update = RectPoints(RECT{0, 0, 60, 4});
        Child(part, whole, whole, RECT{10, 10, 40, 30}, RECT{2, 2, 2, 2});
        part.update = RectPoints(RECT{3, 4, 9, 7});                 // in its client, which starts at 12,12
        Show(whole);
        CHECK(Matches(whole) && AllIn(RECT{0, 0, 60, 4}, true) && AllIn(RECT{15, 16, 21, 19}, true));
        CHECK(!Cleared(14, 16) && !Cleared(21, 16) && !Cleared(15, 15) && !Cleared(15, 19) && !Cleared(0, 4));

        // A control reaches beyond its parent: what the parent does not let
        // through is not the control's, whatever its update region says.
        Reset();
        TestWindow host, box, wide, next;
        Form(host);
        host.update.clear();
        Child(box, host, host, RECT{10, 10, 30, 30}, RECT{1, 1, 1, 1});
        Child(wide, box, host, RECT{5, 15, 45, 25});                // from left of the box to right of it
        Child(next, host, host, RECT{32, 12, 44, 28});              // beside the box, valid
        wide.update = RectPoints(RECT{-10, -10, 60, 30});           // and more than its client
        Show(host);
        CHECK(Matches(host) && AllIn(RECT{11, 15, 29, 25}, true));
        CHECK(AllIn(RECT{5, 15, 11, 25}, false) && AllIn(RECT{29, 15, 45, 25}, false) && !Cleared(15, 14) && !Cleared(15, 25));
        CHECK(Released());

        // Of a control only a piece of its frame shows: the frame is kept, and
        // neither the control nor what is on it is asked what it is waiting for.
        Reset();
        TestWindow holder, frame, edge, onEdge;
        Form(holder);
        Child(frame, holder, holder, RECT{10, 10, 30, 30}, RECT{1, 1, 1, 1});       // its client is [11,11,29,29]
        Child(edge, frame, holder, RECT{27, 15, 45, 25}, RECT{3, 3, 3, 3});         // its client starts at 30
        Child(onEdge, edge, holder, RECT{32, 19, 40, 21});
        for (TestWindow* w : {&frame, &edge, &onEdge}) Pending(*w);
        Show(holder);
        CHECK(Matches(holder) && AllIn(RECT{27, 15, 29, 25}, false) && AllIn(RECT{11, 11, 27, 29}, true));
        CHECK(AllIn(RECT{30, 10, 60, 30}, true) && SeTest::updateQueries == 2 && Released());

        // A control that is not shown takes no part, and neither do its children.
        Reset();
        TestWindow owner, page, onPage;
        Form(owner);
        Child(page, owner, owner, RECT{10, 10, 50, 30});
        Child(onPage, page, owner, RECT{15, 15, 25, 20});
        page.style = 0;                             // the control on it keeps its own visible style
        onPage.update.clear();
        Show(owner);
        CHECK(Matches(owner) && AllIn(RECT{0, 0, 60, 40}, true));
        owner.update.clear(); Pending(onPage);
        Reset();
        Show(owner);
        CHECK(Untouched() && Released());
    }

    // Left alone: a window of another thread with all it holds, a mirrored
    // one, what lies too deep. A layered child has a surface of its own.
    void Others()
    {
        Reset();
        TestWindow root, foreign, inForeign, mirrored, layered, onLayered, under;
        Form(root);
        Child(foreign, root, root, RECT{2, 2, 20, 12});
        Child(inForeign, foreign, root, RECT{5, 4, 15, 10});
        Child(mirrored, root, root, RECT{22, 2, 40, 12});
        Child(under, root, root, RECT{2, 20, 20, 30}, RECT{1, 1, 1, 1});    // valid, beneath a part of the layered one
        Child(layered, root, root, RECT{10, 16, 50, 36});
        Child(onLayered, layered, root, RECT{30, 20, 40, 30});
        foreign.thread = 2; mirrored.ex = WS_EX_LAYOUTRTL; layered.ex = WS_EX_LAYERED;
        for (TestWindow* w : {&foreign, &inForeign, &mirrored, &layered, &onLayered}) Pending(*w);
        Show(root);
        CHECK(Matches(root));
        CHECK(AllIn(RECT{2, 2, 20, 12}, false) && AllIn(RECT{22, 2, 40, 12}, false) && AllIn(RECT{2, 20, 20, 30}, false));
        // Under the layered window the form decides, as if it were not there.
        CHECK(AllIn(RECT{20, 16, 50, 36}, true) && AllIn(RECT{10, 16, 20, 20}, true) && AllIn(RECT{0, 12, 60, 16}, true));
        CHECK(Released());
        // ...and so does a valid form.
        Reset();
        root.update.clear();
        Show(root);
        CHECK(Untouched() && Released() && Armed(&root));       // nothing was pending anywhere: not yet

        // Nested deeper than the walk goes: kept from there on.
        Reset();
        TestWindow deep;
        Form(deep);
        std::vector<TestWindow> chain(40);
        for (size_t i = 0; i < chain.size(); ++i)
        {
            TestWindow& parent = i ? chain[i - 1] : deep;
            Child(chain[i], parent, deep, RECT{static_cast<LONG>(i), 0, 60, 40});
            Pending(chain[i]);
        }
        Show(deep);
        CHECK(Matches(deep));
        // The form is depth 0: the 31 windows below it are walked, the 32nd is kept with the rest.
        CHECK(AllIn(RECT{0, 0, 31, 40}, true) && AllIn(RECT{31, 0, 60, 40}, false));
        CHECK(SeTest::updateQueries == 32 && Released());

        // More windows than the walk allows - the form and two thousand - and nothing at all.
        Reset();
        TestWindow crowd;
        Form(crowd);
        std::vector<TestWindow> many(2000);
        for (TestWindow& w : many) { Child(w, crowd, crowd, RECT{0, 0, 1, 1}); Pending(w); }
        Show(crowd);
        CHECK(Untouched() && SeTest::dcsTaken == 0 && Released() && !Armed(&crowd));
        many.pop_back(); crowd.children.erase(crowd.children.begin());      // one fewer: the form and 1999
        Reset();
        Show(crowd);
        CHECK(Matches(crowd) && AllIn(RECT{0, 0, 60, 40}, true) && Released());
    }

    // Only a VCL top-level window with glass, only after it was shown, only
    // the paint of that show.
    void Gates()
    {
        TestWindow root, child, other;
        const auto build = [&] {
            Reset();
            root = TestWindow{}; child = TestWindow{}; other = TestWindow{};
            Form(root);
            Child(child, root, root, RECT{10, 10, 30, 20});
            Pending(child);
            Form(other);
        };
        const auto done = [&] { return Cleared(0, 0) && Cleared(15, 15) && Released(); };

        build(); Show(root); CHECK(done());
        // Not shown: a frame and an erase of a window that was there all along.
        build(); On(&root, WM_NCPAINT); On(&root, WM_ERASEBKGND); CHECK(Untouched() && SeTest::regionsMade == 0);
        build(); On(&root, WM_WINDOWPOSCHANGING, kMove); On(&root, WM_NCPAINT); CHECK(Untouched() && !Armed(&root));
        build(); On(&root, WM_WINDOWPOSCHANGING, 0); On(&root, WM_NCPAINT); CHECK(Untouched());
        // A container inside a form is not a window that is shown again.
        build(); On(&child, WM_WINDOWPOSCHANGING, kShow); CHECK(!Armed(&child)); On(&child, WM_NCPAINT); CHECK(Untouched());
        // The frame of another window.
        build(); On(&root, WM_WINDOWPOSCHANGING, kShow); On(&other, WM_NCPAINT); On(&child, WM_ERASEBKGND);
        CHECK(Untouched() && Armed(&root));
        On(&root, WM_NCPAINT); CHECK(done());
        // The window that was shown last.
        build(); On(&other, WM_WINDOWPOSCHANGING, kShow); On(&root, WM_WINDOWPOSCHANGING, kShow);
        On(&other, WM_NCPAINT); CHECK(Untouched());
        On(&root, WM_NCPAINT); CHECK(done());
        // Not visible yet, or still on the taskbar: it waits.
        build(); On(&root, WM_WINDOWPOSCHANGING, kShow);
        root.style = 0; On(&root, WM_NCPAINT); CHECK(Untouched() && Armed(&root));
        root.style = WS_VISIBLE; root.iconic = true; On(&root, WM_ERASEBKGND); CHECK(Untouched() && Armed(&root));
        root.iconic = false; On(&root, WM_NCPAINT); CHECK(done());
        // Nothing is pending yet: this frame was not the paint of that show.
        build(); root.update.clear(); child.update.clear();
        On(&root, WM_WINDOWPOSCHANGING, kShow); On(&root, WM_NCPAINT);
        CHECK(Untouched() && Armed(&root) && Released());
        Pending(root); Pending(child); On(&root, WM_ERASEBKGND); CHECK(done() && !Armed(&root));
        // Too late to be the paint of that show.
        build(); On(&root, WM_WINDOWPOSCHANGING, kShow);
        SeTest::clock += 1000; On(&root, WM_NCPAINT); CHECK(Untouched() && !Armed(&root) && SeTest::regionsMade == 0);
        SeTest::clock += 1; On(&root, WM_NCPAINT); CHECK(Untouched());
        build(); On(&root, WM_WINDOWPOSCHANGING, kShow);
        SeTest::clock += 999; On(&root, WM_NCPAINT); CHECK(done());
        // Shown again: the time runs from the last one.
        build(); On(&root, WM_WINDOWPOSCHANGING, kShow);
        SeTest::clock += 900; On(&root, WM_WINDOWPOSCHANGING, kShow);
        SeTest::clock += 900; On(&root, WM_NCPAINT); CHECK(done());
        // Glass is not painted here: another mode, no backdrop on the window.
        build(); SeTest::mode = Shared::InjectionMode::PopupOnly; Show(root); CHECK(Untouched() && !Armed(&root));
        build(); SeTest::mode = Shared::InjectionMode::Forbidden; Show(root); CHECK(Untouched());
        build(); root.backdrop = false; Show(root); CHECK(Untouched() && !Armed(&root) && SeTest::regionsMade == 0);
        // A surface or a layout of another kind.
        build(); root.ex = WS_EX_LAYERED; Show(root); CHECK(Untouched() && !Armed(&root));
        build(); root.ex = WS_EX_LAYOUTRTL; Show(root); CHECK(Untouched() && !Armed(&root));
        // Not VCL: no property, the property of another process, a name that is close.
        build(); root.props.clear(); Show(root); CHECK(Untouched() && !Armed(&root) && SeTest::regionsMade == 0);
        build(); root.props.clear(); root.props[L"Delphi000073F9"] = &root; Show(root); CHECK(Untouched());
        build(); root.props.clear(); root.props[L"Delphi73F8"] = &root; Show(root); CHECK(Untouched());
        build(); root.props.clear(); root.props[L"delphi000073f8"] = &root; Show(root); CHECK(Untouched());
        build(); SeTest::process = 0xABCDEF01; Show(root); CHECK(Untouched());
        root.props[L"DelphiABCDEF01"] = &root; Show(root); CHECK(done());      // all eight digits, upper case
        // The window is destroyed before its frame is asked for.
        build(); On(&root, WM_WINDOWPOSCHANGING, kShow);
        On(&child, WM_NCDESTROY); CHECK(Armed(&root));                          // a child's end is not the window's
        On(&root, WM_NCDESTROY); CHECK(!Armed(&root));
        On(&root, WM_NCPAINT); CHECK(Untouched() && SeTest::regionsMade == 0);
        // No client at all.
        build(); root.frame = RECT{4, 6, 4, 44};
        Show(root); CHECK(Untouched() && SeTest::dcsTaken == 0 && Released() && !Armed(&root));
        // Any other message is not looked at.
        build(); On(&root, WM_WINDOWPOSCHANGING, kShow); On(&root, WM_PAINT); CHECK(Untouched() && Armed(&root));
    }

    // Whatever refuses, nothing is cleared, nothing is left behind, and the
    // window is not asked again.
    void Failures()
    {
        TestWindow root, panel, edit, late;
        const auto build = [&] {
            Reset();
            root = TestWindow{}; panel = TestWindow{}; edit = TestWindow{}; late = TestWindow{};
            Form(root);
            Child(panel, root, root, RECT{5, 5, 45, 30}, RECT{1, 1, 1, 1});
            Child(edit, panel, root, RECT{10, 10, 30, 16}, RECT{1, 1, 1, 1});
            Child(late, root, root, RECT{48, 5, 58, 15});       // above the panel: walked after it and what it holds
            Pending(panel); Pending(edit); Pending(late);
        };
        const auto nothing = [&] { return Untouched() && Released() && !Armed(&root); };

        build(); Show(root); CHECK(Matches(root) && Released());
        // Three regions; for each of the four windows two rectangles, three combinations and one shift.
        const int creates = SeTest::creates, combines = SeTest::combines, setRects = SeTest::setRects, offsets = SeTest::offsets;
        CHECK(creates == 3 && combines == 12 && setRects == 8 && offsets == 4);
        // A region cannot be made: the ones that were are deleted.
        for (int n = 1; n <= creates; ++n)
        { build(); SeTest::failCreate = n; Show(root); CHECK(nothing() && SeTest::dcsTaken == 0 && SeTest::regionsMade == 2); }
        // A step of the walk fails, whichever it is: the walk ends there.
        for (int n = 1; n <= combines; ++n)
        { build(); SeTest::failCombine = n; Show(root); CHECK(nothing() && SeTest::dcsTaken == 0 && SeTest::combines == n); }
        for (int n = 1; n <= setRects; ++n)
        { build(); SeTest::failSetRect = n; Show(root); CHECK(nothing() && SeTest::dcsTaken == 0 && SeTest::setRects == n); }
        for (int n = 1; n <= offsets; ++n)
        { build(); SeTest::failOffset = n; Show(root); CHECK(nothing() && SeTest::dcsTaken == 0 && SeTest::offsets == n); }
        // A window does not answer.
        for (TestWindow* w : {&root, &panel, &edit, &late})
        {
            build(); SeTest::failUpdate = w; Show(root); CHECK(nothing() && SeTest::dcsTaken == 0);
            build(); SeTest::failClient = w; Show(root); CHECK(nothing() && SeTest::dcsTaken == 0);
        }
        for (TestWindow* w : {&panel, &edit, &late})
        { build(); SeTest::failRect = w; Show(root); CHECK(nothing() && SeTest::dcsTaken == 0); }
        // The window that did not answer is the last one asked.
        build(); SeTest::failUpdate = &edit; Show(root); CHECK(SeTest::updateQueries == 3);
        build(); SeTest::failClient = &panel; Show(root); CHECK(SeTest::updateQueries == 1);
        build(); SeTest::failRect = &root; Show(root); CHECK(Matches(root));    // the form is never asked for its frame
        // The region cannot be measured, there is no DC, the DC does not take the region.
        build(); SeTest::failBox = true; Show(root); CHECK(nothing() && SeTest::dcsTaken == 0);
        build(); SeTest::failDc = true; Show(root); CHECK(nothing() && SeTest::dcsTaken == 0);
        build(); SeTest::failSelect = 1; Show(root); CHECK(nothing() && SeTest::dcsTaken == 1 && SeTest::blits == 0 && SeTest::selects == 1);
        // The fill itself fails: the clip is taken back all the same.
        build(); SeTest::failBlit = true; Show(root);
        CHECK(SeTest::surface.empty() && SeTest::blits == 1 && SeTest::selects == 2 && Released() && !Armed(&root));
    }

    // The unit only clears: whatever it does or declines, no build of it
    // writes a line or reads the surface.
    void Quiet()
    {
        Reset();
        TestWindow root, valid, button, away;
        Form(root);
        Child(valid, root, root, RECT{0, 0, 60, 8});
        Child(button, root, root, RECT{40, 20, 52, 26}, RECT{1, 1, 1, 1});
        Child(away, root, root, RECT{70, 5, 80, 15});       // beside the client: visited, and that is all
        away.thread = 2;
        Pending(button);
        const auto quiet = [] { return SeTest::logs.empty() && SeTest::pixelReads == 0; };
        // Cleared: the form below the valid window, around the button's frame.
        WINDOWPOS pos = kShow; pos.flags |= SWP_FRAMECHANGED;
        On(&root, WM_WINDOWPOSCHANGING, pos);
        SeTest::clock += 7;
        On(&root, WM_NCPAINT);
        CHECK(Matches(root) && Released() && SeTest::blits == 1 && quiet());
        // Nothing pending yet: the window stays armed.
        Reset();
        root.update.clear(); button.update.clear();
        Show(root);
        CHECK(Armed(&root) && SeTest::dcsTaken == 0 && quiet());
        // The walk failed.
        Reset();
        Pending(root); SeTest::failUpdate = &button;
        Show(root);
        CHECK(Untouched() && !Armed(&root) && quiet());
        // A shown window that is passed over: late, another mode, no glass,
        // another surface or layout, not VCL.
        const auto passed = [&] { return Untouched() && !Armed(&root) && SeTest::regionsMade == 0 && quiet(); };
        Reset(); On(&root, WM_WINDOWPOSCHANGING, kShow); SeTest::clock += 1000; On(&root, WM_NCPAINT); CHECK(passed());
        Reset(); SeTest::mode = Shared::InjectionMode::PopupOnly; Show(root); CHECK(passed());
        Reset(); root.backdrop = false; Show(root); CHECK(passed()); root.backdrop = true;
        Reset(); root.ex = WS_EX_LAYERED; Show(root); CHECK(passed());
        Reset(); root.ex = WS_EX_LAYOUTRTL; Show(root); CHECK(passed()); root.ex = 0;
        Reset(); root.props.clear(); Show(root); CHECK(passed());
    }

    // Layouts nobody thought of: windows of any place, frame, order and state,
    // nested as they come. Every point is compared with what its owner says.
    void Random()
    {
        std::uint32_t seed = 0x9E3779B9u;
        const auto next = [&](std::uint32_t bound) {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<LONG>((seed >> 8) % bound);
        };
        int cleared = 0, kept = 0;
        for (int layout = 0; layout < 400; ++layout)
        {
            Reset();
            TestWindow root;
            Form(root);
            if (next(4) == 0) root.update.clear();
            else if (next(4) == 0) root.update = RectPoints(RECT{next(30), next(20), 30 + next(31), 20 + next(21)});
            std::vector<TestWindow> windows(1 + next(10));
            for (size_t i = 0; i < windows.size(); ++i)
            {
                TestWindow& w = windows[i];
                TestWindow& parent = (i && next(3)) ? windows[next(static_cast<std::uint32_t>(i))] : root;
                const LONG l = next(70) - 5, t = next(50) - 5;
                const LONG frame = next(3);
                Child(w, parent, root, RECT{l, t, l + 2 + next(40), t + 2 + next(30)}, RECT{frame, frame, frame, frame});
                if (next(2)) std::rotate(parent.children.begin(), parent.children.begin() + 1, parent.children.end());   // to the bottom
                switch (next(12))
                {
                case 0: w.style = 0; break;
                case 1: w.ex = WS_EX_LAYERED; break;
                case 2: w.ex = WS_EX_LAYOUTRTL; break;
                case 3: w.thread = 2; break;
                default: break;
                }
                switch (next(4))
                {
                case 0: break;                                      // valid
                case 1: w.update = RectPoints(RECT{next(10) - 3, next(10) - 3, 5 + next(40), 5 + next(30)}); break;
                default: Pending(w); break;
                }
            }
            Show(root);
            if (!Matches(root)) throw std::runtime_error("layout " + std::to_string(layout));
            CHECK(Released() && SeTest::clipChildrenBlits == 0 && SeTest::updateErases == 0);
            (SeTest::surface.empty() ? kept : cleared)++;
        }
        // Both happen: the layouts are not all of one kind.
        CHECK(cleared > 300 && kept > 0);
    }
}

int main(int argc, char** argv)
{
    try
    {
        if (argc != 2) throw std::runtime_error("one case required");
        const std::string name = argv[1];
        if (name == "restored") Restored();
        else if (name == "kept") Kept();
        else if (name == "others") Others();
        else if (name == "gates") Gates();
        else if (name == "failures") Failures();
        else if (name == "quiet") Quiet();
        else if (name == "random") Random();
        else throw std::runtime_error("unknown case");
        std::cout << "PASS " << name << '\n';
    }
    catch (const std::exception& e) { std::cerr << argv[1] << ": " << e.what() << '\n'; return 1; }
}
