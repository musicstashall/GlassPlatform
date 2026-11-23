#include "bome_memo_test_shim.h"
#include <GlassDelphiPanel.h>
#include <GlassDelphiButtonGroups.h>
#include "../../GlassCore/src/Hooks/delphi/BomeMemoDraw.cpp"
#include <root_bome_memo_test.inc>
#include <stdexcept>

#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)
namespace
{
    using namespace GlassCore;
    constexpr UINT kFlags = DT_NOPREFIX | DT_SINGLELINE | DT_VCENTER;
    bool Same(const RECT& a, const RECT& b) { return std::memcmp(&a,&b,sizeof(a)) == 0; }
    struct Context
    {
        TestModule module, otherModule;
        TestWindow window, gutter, other;
        TestDC dc, memory, gutterDc, foreign, unknown;
        RECT rect{0,0,640,16};
        LOGBRUSH brush{BS_SOLID, 0xffffff};
        Context()
        {
            ModuleHandler::g_entryPointModule = &module;
            Test::full = true; Root::g_shutdown = false;
            window.cls = L"TBomeMemo"; gutter.cls = L"TBMGutter"; other.cls = L"TPanel";
            dc.owner = &window; gutterDc.owner = &gutter; foreign.owner = &other;
            module.originals = {{"DrawTextA", reinterpret_cast<void*>(ChainAnsi)},
                {"CreateBrushIndirect", reinterpret_cast<void*>(ChainBrush)},
                {"GetSysColor", reinterpret_cast<void*>(ChainSysColor)},
                {"FillRect", reinterpret_cast<void*>(ChainFill)}};
            Root::s_rootCache[&window] = Root::MemoSpecsForTest(&window,&module);
            Root::s_rootCache[&gutter] = Root::MemoSpecsForTest(&gutter,&module);
            CHECK(Root::s_rootCache[&window].bomeMemo && Root::s_rootCache[&gutter].bomeMemo);
            Test::host = Test::topWindow = nullptr; Test::stackOwners.clear();
        }
        ~Context()
        {
            while (!Root::g_rootPaintStack.empty()) Root::Root_OnEndPaint(Root::g_rootPaintStack.back().hWnd,&module);
            while (!Root::g_rootDcStack.empty()) Root::Root_OnReleaseDC(Root::g_rootDcStack.back().hWnd,&module);
            BomeMemo::ClearSessions(); Root::DisableAllRootHooks(); Root::s_rootCache.clear();
            for (const auto& [name, ref] : module.refs) { (void)name; assert(!ref); }
            assert(module.hooks.empty() && !MemoTest::formAttachCalls);
            Test::stackOwners.clear(); Test::host = Test::topWindow = nullptr;
            MemoTest::instances.clear();
        }
        void Activate(HWND host, HDC target)
        { Test::host = Test::topWindow = host; Test::stackOwners[target] = host; }
        void Begin(bool paint = true)
        {
            if (paint) Root::Root_OnBeginPaint(&window,&module); else Root::Root_OnGetDC(&window,&module);
            Activate(&window,&dc); Test::stackOwners[&memory] = &window;
        }
        template <typename Fn, typename... Args> auto Call(const char* name, Fn, Args... args)
        {
            const auto it = module.hooks.find(name);
            return reinterpret_cast<Fn>(it == module.hooks.end() ? module.originals.at(name) : it->second)(args...);
        }
        int Text(HDC target = nullptr, LPCSTR text = "row", int count = -1, UINT flags = kFlags, LPRECT r = nullptr)
        { return Call("DrawTextA", &DrawTextA, target ? target : &dc, text, count, r ? r : &rect, flags); }
        int Fill(HDC target = nullptr, HBRUSH b = nullptr)
        { return Call("FillRect", &::FillRect, target ? target : &dc, &rect, b ? b : &brush); }
        DWORD Color(int index) { return Call("GetSysColor", &GetSysColor, index); }
        HBRUSH Brush(const LOGBRUSH* b) { return Call("CreateBrushIndirect", &CreateBrushIndirect, b); }
        int Ref(size_t offset = 0)
        { return Root::g_rootMap.at(&module).GetRef(Root::OHK_BOME_MEMO_TEXT + offset); }
    };
    void Classes()
    {
        Context c;
        CHECK(!BomeMemo::IsClassName(nullptr));
        for (LPCWSTR cls : {L"TBomeMemo",L"TBMGutter",L"tbomememo"})
        {
            c.window.cls = cls;
            const auto cache = Root::MemoSpecsForTest(&c.window,&c.module);
            CHECK(cache.bomeMemo && cache.paintHooks.empty() && cache.dcHooks.empty());
            CHECK(!Root::MemoSpecsForTest(&c.window,&c.otherModule).bomeMemo);
        }
        for (LPCWSTR cls : {L"TMemo",L"Edit",L"TRichEdit",L"MyTBomeMemo",L"TBomeMemo2",L"TBMGutterChild",L""})
        { c.window.cls = cls; CHECK(!BomeMemo::IsWindow(&c.window)); CHECK(!Root::MemoSpecsForTest(&c.window,&c.module).bomeMemo); }
        c.window.cls = L"TBomeMemo";
        MemoTest::instances[&c.window] = &c.otherModule;
        CHECK(!BomeMemo::IsWindow(&c.window));
        CHECK(!Root::MemoSpecsForTest(&c.window,&c.module).bomeMemo);
        MemoTest::instances.clear();
        CHECK(BomeMemo::IsWindow(&c.window));
        c.window.valid = false; CHECK(!BomeMemo::IsWindow(&c.window));
    }
    void Transaction()
    {
        Context c;
        const char* names[]{"DrawTextA","CreateBrushIndirect","GetSysColor","FillRect"};
        for (size_t missing = 0; missing < 4; ++missing)
        {
            void* original = c.module.originals.at(names[missing]);
            c.module.originals.erase(names[missing]);
            const unsigned mask = Root::AcquireBomeMemoHooks();
            CHECK(mask == (15u & ~(1u << missing)) && c.module.hooks.empty());
            for (size_t i=0;i<4;++i) CHECK(!c.Ref(i));
            c.module.originals[names[missing]] = original;
            c.module.hooks[names[missing]] = original; // foreign IAT owner
            CHECK(Root::AcquireBomeMemoHooks() == mask);
            CHECK(c.module.hooks.size() == 1 && c.module.hooks.at(names[missing]) == original);
            for (size_t i=0;i<4;++i) CHECK(!c.Ref(i));
            c.module.hooks.clear();
        }
        // A prior independently owned ref must survive a failed transaction.
        CHECK(Root::EnableRootHook(&c.module,Root::OHK_BOME_MEMO_TEXT,true));
        c.module.originals.erase("FillRect");
        CHECK(Root::AcquireBomeMemoHooks() == 7 && c.Ref() == 1 && c.module.hooks.size() == 1);
        CHECK(Root::EnableRootHook(&c.module,Root::OHK_BOME_MEMO_TEXT,false));
        c.module.originals["FillRect"] = reinterpret_cast<void*>(ChainFill);
        CHECK(Root::AcquireBomeMemoHooks() == 15);
        CHECK(Root::AcquireBomeMemoHooks() == 15); // nested EnableHook returns false, still acquired
        for (size_t i=0;i<4;++i) CHECK(c.Ref(i) == 2);
        Root::ReleaseBomeMemoHooks(); Root::ReleaseBomeMemoHooks();
        CHECK(c.module.hooks.empty());
    }
    void Paint()
    {
        Context c;
        Root::Root_OnBeginPaint(&c.window,&c.module);
        CHECK(c.module.hooks.size() == 4 && c.Ref() == 1);
        CHECK(c.Text() == 43); // returned BeginPaint DC is not on the dispatcher stack yet
        PAINTSTRUCT ps{&c.dc,TRUE,{8,16,420,80}};
        const int black = Test::black;
        CHECK(Root::Root_OnBomeMemoAfterBeginPaint(&c.window,&c.dc,&ps,&c.module));
        CHECK(Test::black == black+1 && Same(MemoTest::blackRect,ps.rcPaint));
        CHECK(ps.fErase == TRUE); // only PaintDispatcher owns resetting fErase on success
        c.Activate(&c.window,&c.dc); Test::stackOwners[&c.memory] = &c.window;
        CHECK(c.Text(&c.memory) == 15);
        Root::Root_OnEndPaint(&c.window,&c.module);
        CHECK(c.module.hooks.empty() && Root::g_rootPaintStack.empty());
        CHECK(!Root::Root_OnBomeMemoAfterBeginPaint(&c.window,&c.dc,&ps,&c.module));
        // Incomplete acquisition cannot erase and cannot keep a partial import.
        c.module.originals.erase("CreateBrushIndirect");
        Root::Root_OnBeginPaint(&c.window,&c.module);
        CHECK(c.module.hooks.empty() && c.Text() == 43);
        CHECK(!Root::Root_OnBomeMemoAfterBeginPaint(&c.window,&c.dc,&ps,&c.module));
        Root::Root_OnEndPaint(&c.window,&c.module);
    }
    void GetDc()
    {
        Context c; c.Begin(false);
        CHECK(c.module.hooks.size() == 4 && c.Text() == 15 && c.Text(&c.memory) == 15);
        CHECK(c.Color(COLOR_WINDOW) == 0 && c.Color(COLOR_BTNFACE) == 0);
        Root::Root_OnGetDC(&c.window,&c.otherModule); // native barrier over an armed same-HWND session
        CHECK(c.Ref() == 1 && c.Text() == 43 && c.Color(COLOR_WINDOW) == MemoTest::windowColor);
        Root::Root_OnReleaseDC(&c.window,&c.otherModule);
        CHECK(c.Ref() == 1 && c.Text() == 15);
        Root::Root_OnReleaseDC(&c.window,&c.module);
        CHECK(c.module.hooks.empty());
    }
    void TextLines()
    {
        Context c; c.Begin(); TestFont font{41}; c.memory.font = &font; c.memory.text = RGB(12,34,56);
        // Captured shape: ten source lines then ten line numbers, not recovered
        // RECT/string memory from the deleted capture. All twenty use 0x824.
        for (int row=0;row<20;++row)
        {
            const std::string text = row < 10 ? "<node value=\"sample\"/>" : std::to_string(row-9);
            RECT r{0,(row%10)*16,640,(row%10+1)*16}, before = r;
            const int black = Test::black;
            CHECK(c.Text(&c.memory,text.data(),int(text.size()),kFlags,&r) == 15);
            CHECK(MemoTest::alphaText == std::wstring(text.begin(),text.end()));
            CHECK(MemoTest::alphaFlags == kFlags && MemoTest::alphaCount == int(text.size()));
            CHECK(Same(r,before) && Same(MemoTest::alphaRect,before));
            CHECK(Test::black == black+1 && Test::alphaBkMode == TRANSPARENT);
            CHECK(Test::alphaTextColor == c.memory.text && Test::alphaFont == &font);
            CHECK(c.memory.bkMode == OPAQUE && c.memory.saved.empty());
        }
        for (std::uint16_t bpp : {1,24,32}) { MemoTest::bitmap.bmBitsPixel = bpp; CHECK(c.Text(&c.memory) == 15); }
        MemoTest::haveBitmap = false; CHECK(c.Text(&c.memory) == 15);
    }
    void Conversion()
    {
        Context c; c.Begin();
        MemoTest::charsets[&c.dc] = 128;
        CHECK(c.Text(nullptr,"\x82\xa0!",3) == 15 && MemoTest::alphaText == L"\u3042!" && MemoTest::alphaCount == 2);
        CHECK(MemoTest::codepage == 932);
        CHECK(c.Text(nullptr,"\x82\xa0",-1) == 15 && MemoTest::alphaText == L"\u3042" && MemoTest::alphaCount == 1);
        MemoTest::charsets[&c.dc] = 204;
        CHECK(c.Text(nullptr,"\xc0\xe1",2) == 15 && MemoTest::alphaText == L"\u0410\u0431" && MemoTest::codepage == 1251);
        MemoTest::charsets[&c.dc] = DEFAULT_CHARSET;
        const char explicitBytes[]{'A',0,'B','Z'};
        CHECK(c.Text(nullptr,explicitBytes,3) == 15 && MemoTest::alphaText == std::wstring(L"A\0B",3));
        CHECK(MemoTest::codepage == CP_ACP);
        CHECK(c.Text(nullptr,explicitBytes,-1) == 15 && MemoTest::alphaText == L"A");
    }
    void Native()
    {
        Context c; c.Begin();
        const int alpha = Test::alphaText, black = Test::black;
        const char* text = "measure"; RECT r{1,2,3,4};
        CHECK(c.Text(nullptr,text,-1,DT_CALCRECT,&r) == 43 && r.right == 10);
        CHECK(MemoTest::ansiPointer == text && MemoTest::ansiRect == &r && MemoTest::ansiCount == -1);
        char mutableText[] = "change";
        CHECK(c.Text(nullptr,mutableText,-1,DT_MODIFYSTRING) == 43 && mutableText[0] == 'X');
        CHECK(c.Text(nullptr,text,0) == 43 && c.Text(nullptr,nullptr,3) == 43);
        CHECK(c.Text(nullptr,text,-2) == 43 && c.Text(nullptr,"",-1) == 43);
        CHECK(c.Text(nullptr,text,-1,DT_NOCLIP) == 43);
        RECT empty{}; CHECK(c.Text(nullptr,text,-1,kFlags,&empty) == 43);
        MemoTest::charsets[&c.dc] = SYMBOL_CHARSET; CHECK(c.Text() == 43);
        MemoTest::charsets[&c.dc] = 128; MemoTest::failTranslate = true; CHECK(c.Text() == 43);
        MemoTest::failTranslate = false; MemoTest::failConvert = true; CHECK(c.Text() == 43);
        CHECK(Test::alphaText == alpha && Test::black == black);
        CHECK(MemoTest::nativeMode == OPAQUE);
    }
    void Backgrounds()
    {
        Context c; c.Begin();
        for (LOGBRUSH input : {LOGBRUSH{BS_SOLID,MemoTest::windowColor},LOGBRUSH{BS_SOLID,MemoTest::faceColor},
                               LOGBRUSH{BS_SOLID,RGB(45,67,89)},LOGBRUSH{BS_SOLID,0},LOGBRUSH{3,0xffffff,9}})
        {
            const LOGBRUSH saved = input;
            std::unique_ptr<LOGBRUSH> created(c.Brush(&input));
            CHECK(created && created.get() != &input && created->lbColor == 0
                && created->lbStyle == BS_SOLID && created->lbHatch == 0);
            CHECK(input.lbStyle == saved.lbStyle && input.lbColor == saved.lbColor
                && input.lbHatch == saved.lbHatch);
            CHECK(MemoTest::brushPointer != &input && MemoTest::lastBrush.lbColor == 0
                && MemoTest::lastBrush.lbStyle == BS_SOLID);
        }
        c.brush.lbColor = MemoTest::windowColor; CHECK(c.Fill() == 1);
        c.brush.lbColor = RGB(45,67,89); CHECK(c.Fill() == 37);
        CHECK(!c.Brush(nullptr));
        CHECK(c.Fill(nullptr,reinterpret_cast<HBRUSH>(ULONG_PTR(COLOR_WINDOW+1))) == 1);
        CHECK(c.Fill(nullptr,reinterpret_cast<HBRUSH>(ULONG_PTR(9+1))) == 37);
        CHECK(c.Color(9) == MemoSysColor(9));
        MemoTest::windowColor = RGB(20,30,40); MemoTest::faceColor = RGB(60,70,80);
        c.brush.lbColor = 0xffffff; CHECK(c.Fill() == 37); // white is now custom, not a hardcoded key
        c.brush.lbColor = MemoTest::windowColor; CHECK(c.Fill() == 1);
        Root::Root_OnEndPaint(&c.window,&c.module);
        CHECK(c.Color(COLOR_WINDOW) == MemoTest::windowColor && c.Fill() == 37);
        LOGBRUSH after{BS_SOLID, RGB(45,67,89)};
        std::unique_ptr<LOGBRUSH> passed(c.Brush(&after));
        CHECK(passed && passed->lbColor == RGB(45,67,89) && passed->lbStyle == BS_SOLID);
    }
    void Opaque()
    {
        Context c; c.Begin(); c.dc.background = RGB(35,65,95);
        const int commits = MemoTest::commits, black = Test::black;
        CHECK(c.Text() == 15 && MemoTest::commits == commits+1 && MemoTest::normalized == 0);
        const auto p = MemoTest::committed;
        CHECK(p.rgbRed == 35 && p.rgbGreen == 65 && p.rgbBlue == 95 && p.rgbReserved == 255);
        CHECK(Test::black == black && c.dc.background == RGB(35,65,95) && c.dc.bkMode == OPAQUE);
        c.dc.bkMode = TRANSPARENT;
        CHECK(c.Text() == 15 && MemoTest::commits == commits+1 && Test::black == black);
        c.dc.bkMode = OPAQUE; c.dc.background = 0;
        CHECK(c.Text() == 15 && Test::black == black+1 && MemoTest::commits == commits+1);
    }
    void Isolation()
    {
        Context c; c.Begin();
        const int alpha = Test::alphaText;
        c.brush.lbColor = MemoTest::windowColor;
        // Another window's DC is never this route's: neither when that window
        // holds it in a session of its own, nor whatever else was recorded for it.
        CHECK(c.Text(&c.foreign) == 43 && c.Fill(&c.foreign) == 37);
        Test::stackOwners[&c.foreign] = &c.other; CHECK(c.Text(&c.foreign) == 43 && c.Fill(&c.foreign) == 37);
        Test::stackOwners[&c.foreign] = &c.window; CHECK(c.Text(&c.foreign) == 43);
        Test::stackOwners.erase(&c.foreign);
        // Neither a window's DC nor a buffer: a printer, a metafile.
        TestDC device; MemoTest::objectTypes[&device] = 3;
        CHECK(c.Text(&device) == 43 && c.Fill(&device) == 37);
        Test::stackOwners[&device] = &c.window; CHECK(c.Text(&device) == 43);
        Test::stackOwners.erase(&device); MemoTest::objectTypes.clear();
        // The window's own DC counts only while one of its open sessions holds it.
        TestDC cached; cached.owner = &c.window;
        CHECK(c.Text(&cached) == 43 && c.Fill(&cached) == 37);
        MemoTest::maps[&c.dc] = 2; CHECK(c.Text() == 43); MemoTest::maps.clear();
        MemoTest::maps[&c.memory] = 2; CHECK(c.Text(&c.memory) == 43); MemoTest::maps.clear();
        Test::full = false;
        CHECK(c.Text() == 43 && c.Text(&c.memory) == 43 && c.Color(COLOR_WINDOW) == MemoTest::windowColor);
        Test::full = true;
        c.window.backdrop = false;
        CHECK(c.Text() == 43 && c.Fill() == 37 && c.Text(&c.memory) == 43 && c.Text(&c.unknown) == 43);
        CHECK(c.Color(COLOR_WINDOW) == MemoTest::windowColor);
        c.window.backdrop = true;
        // The window of the session is no longer an exact memo class.
        c.window.cls = L"TMemo";
        CHECK(c.Text() == 43 && c.Text(&c.memory) == 43 && c.Color(COLOR_WINDOW) == MemoTest::windowColor);
        c.window.cls = L"TBomeMemo";
        // While another window's session is the innermost one, a buffer is this
        // route's only if the dispatcher recorded it for a served window; the
        // imports without a DC follow that innermost session.
        Test::topWindow = &c.other;
        CHECK(c.Text(&c.unknown) == 43 && c.Fill(&c.unknown) == 37);
        Test::stackOwners[&c.unknown] = &c.other; CHECK(c.Text(&c.unknown) == 43);
        CHECK(c.Color(COLOR_WINDOW) == MemoTest::windowColor && c.Color(COLOR_BTNFACE) == MemoTest::faceColor);
        LOGBRUSH custom{BS_SOLID, RGB(45,67,89)};
        { std::unique_ptr<LOGBRUSH> passed(c.Brush(&custom)); CHECK(passed && passed->lbColor == RGB(45,67,89)); }
        CHECK(Test::alphaText == alpha);
        // The ambient host of the dispatcher is not consulted at all.
        Test::host = &c.other;
        CHECK(c.Text() == 15 && c.Text(&c.memory) == 15 && Test::alphaText == alpha+2);
        Test::topWindow = Test::host = nullptr; // no window session at all: only what was recorded
        CHECK(c.Text() == 15 && c.Text(&c.memory) == 15 && c.Text(&c.unknown) == 43);
        CHECK(c.Color(COLOR_WINDOW) == MemoTest::windowColor);
    }
    // VCL keeps the canvas DCs of the memo and of its gutter until the message
    // ends: both GetDC sessions stay open and the EXE draws on either window's
    // DC and into the memo's buffer in turns.
    void Interleaving()
    {
        Context c;
        Root::Root_OnGetDC(&c.window,&c.module); c.Activate(&c.window,&c.dc);
        Test::stackOwners[&c.memory] = &c.window; // the buffer, created in the memo's session
        Root::Root_OnGetDC(&c.gutter,&c.module); c.Activate(&c.gutter,&c.gutterDc); // opened last
        for (size_t i=0;i<4;++i) CHECK(c.Ref(i) == 2);
        int alpha = Test::alphaText, black = Test::black;
        c.brush.lbColor = MemoTest::windowColor;
        // Each window's own DC and the memo's buffer, whichever session opened last.
        CHECK(c.Text(&c.gutterDc) == 15 && c.Text(&c.dc) == 15 && c.Text(&c.memory) == 15);
        CHECK(Test::alphaText == alpha+3);
        black = Test::black;
        CHECK(c.Fill(&c.gutterDc) == 1 && c.Fill(&c.dc) == 1 && c.Fill(&c.memory) == 1 && Test::black == black+3);
        CHECK(c.Color(COLOR_WINDOW) == 0 && c.Color(COLOR_BTNFACE) == 0);
        // The memo is painted again while the gutter's session is still open. Its
        // next buffer was recorded for the gutter (ambient when it was created),
        // another one was created before any session, a third one is recorded
        // for an unrelated window: all three are the drawing of the memo now.
        TestDC late; Test::stackOwners[&late] = &c.gutter;
        Test::topWindow = &c.window;
        alpha = Test::alphaText;
        CHECK(c.Text(&late) == 15 && c.Text(&c.unknown) == 15 && c.Fill(&c.unknown) == 1);
        Test::stackOwners[&c.unknown] = &c.other;
        CHECK(c.Text(&c.unknown) == 15 && Test::alphaText == alpha+3);
        CHECK(c.Text(&c.gutterDc) == 15 && c.Text(&c.foreign) == 43);
        Test::stackOwners.erase(&c.unknown);
        // The gutter's session closes first: its DC is native again, the memo is
        // served on, and the buffer recorded for the gutter is the memo's drawing.
        Root::Root_OnReleaseDC(&c.gutter,&c.module);
        CHECK(c.Ref() == 1 && c.Text(&c.gutterDc) == 43 && c.Fill(&c.gutterDc) == 37);
        CHECK(c.Text(&c.dc) == 15 && c.Text(&c.memory) == 15 && c.Text(&late) == 15);
        // Another module takes the gutter's DC: a native barrier for the gutter,
        // and for buffers while the gutter's session is the innermost one, but
        // not for the memo's own DC and recorded buffer.
        Root::Root_OnGetDC(&c.gutter,&c.otherModule); Test::topWindow = &c.gutter;
        alpha = Test::alphaText;
        CHECK(c.Ref() == 1 && c.Text(&c.gutterDc) == 43 && c.Text(&late) == 43 && c.Text(&c.unknown) == 43);
        CHECK(c.Color(COLOR_WINDOW) == MemoTest::windowColor && Test::alphaText == alpha);
        CHECK(c.Text(&c.dc) == 15 && c.Text(&c.memory) == 15);
        Root::Root_OnReleaseDC(&c.gutter,&c.otherModule); Test::topWindow = &c.window;
        CHECK(c.Text(&late) == 15 && c.Color(COLOR_WINDOW) == 0);
        Root::Root_OnReleaseDC(&c.window,&c.module);
        CHECK(c.module.hooks.empty() && Root::g_rootDcStack.empty());
        CHECK(c.Text(&c.dc) == 43 && c.Text(&c.memory) == 43 && c.Text(&late) == 43);
    }
    void Diagnostics()
    {
        Context c;
        const auto find = [](const wchar_t* part) {
            std::vector<MemoTest::Log> found;
            for (const auto& log : MemoTest::logs)
                if (log.format.find(part) != std::wstring::npos) found.push_back(log);
            return found;
        };
        const auto number = [](void* p) { return static_cast<long long>(reinterpret_cast<std::intptr_t>(p)); };
        std::vector<MemoTest::Log> seen; // everything logged in this test
        const auto nextSecond = [&] {
            seen.insert(seen.end(), MemoTest::logs.begin(), MemoTest::logs.end());
            MemoTest::logs.clear(); MemoTest::clock += 1000;
        };
        MemoTest::clock += 100000; MemoTest::logs.clear();
        Root::Root_OnGetDC(&c.window,&c.module); c.Activate(&c.window,&c.dc);
        Test::stackOwners[&c.memory] = &c.window;
        auto begin = find(L"] hooks route=2 ");
        CHECK(begin.size() == 1 && begin[0].strings == (std::vector<std::wstring>{L"TBomeMemo", L"GetDC"}));
        CHECK(begin[0].numbers == (std::vector<long long>{number(&c.window), 0xF, 1}));
        c.brush.lbColor = MemoTest::windowColor;
        const char* secret = "secret-row-text";
        CHECK(c.Text(&c.memory,secret) == 15 && c.Text(&c.dc,secret) == 15 && c.Fill(&c.memory) == 1);
        CHECK(find(L"] text-skip ").empty());
        // Why a DC stayed native, without the text.
        CHECK(c.Text(&c.foreign,secret) == 43);
        Test::topWindow = &c.other; CHECK(c.Text(&c.unknown,secret) == 43); Test::topWindow = &c.window;
        TestDC cached; cached.owner = &c.window; CHECK(c.Text(&cached,secret) == 43);
        MemoTest::maps[&c.memory] = 2; CHECK(c.Text(&c.memory,secret) == 43 && c.Fill(&c.memory) == 37);
        MemoTest::maps.clear();
        TestDC device; MemoTest::objectTypes[&device] = 3;
        CHECK(c.Text(&device,secret) == 43);
        MemoTest::objectTypes.clear();
        CHECK(c.Call("DrawTextA", &DrawTextA, static_cast<HDC>(nullptr), secret, -1, &c.rect, kFlags) == 43);
        const auto skips = find(L"] text-skip ");
        CHECK(skips.size() == 6 && skips[5].strings == std::vector<std::wstring>{L"dc"});
        CHECK(skips[5].numbers == (std::vector<long long>{0, 0, 0, 1}));
        const wchar_t* reasons[]{L"window", L"buffer", L"window-dc", L"map", L"dc"};
        TestDC* targets[]{&c.foreign, &c.unknown, &cached, &c.memory, &device};
        for (size_t i=0;i<5;++i)
        {
            CHECK(skips[i].format == L"[GlassCore][BomeMemo] text-skip dc=0x%p dcWnd=0x%p stackOwner=0x%p sessions=%zu reason=%s");
            CHECK(skips[i].strings == std::vector<std::wstring>{reasons[i]});
            CHECK(skips[i].numbers.size() == 4 && skips[i].numbers[0] == number(targets[i]) && skips[i].numbers[3] == 1);
            CHECK(skips[i].numbers[1] == number(targets[i]->owner));
        }
        CHECK(skips[3].numbers[2] == number(&c.window) && skips[1].numbers[2] == 0);
        // The limit is per second and per channel; a skipped call is counted even when it is not logged.
        for (int i=0;i<40;++i) CHECK(c.Text(&c.foreign,secret) == 43);
        CHECK(find(L"] text-skip ").size() == 14); // 16 per second on the text channel, two of them were `text` lines
        // The channels have budgets of their own: with the text channel
        // exhausted a session still reports its begin and its end.
        Root::Root_OnGetDC(&c.gutter,&c.module);
        Root::Root_OnReleaseDC(&c.gutter,&c.module);
        CHECK(find(L"] hooks route=2 ").size() == 2);
        const auto gutterEnd = find(L"] hooks-end ");
        CHECK(gutterEnd.size() == 1 && gutterEnd[0].numbers == (std::vector<long long>{number(&c.gutter), 1, 0, 0, 0, 1}));
        nextSecond();
        CHECK(c.Text(&c.foreign,secret) == 43 && find(L"] text-skip ").size() == 1);
        // What the session drew, when it closes: two texts, one fill, 48 native calls with a DC.
        Root::Root_OnReleaseDC(&c.window,&c.module);
        const auto end = find(L"] hooks-end ");
        CHECK(end.size() == 1 && end[0].strings == std::vector<std::wstring>{L"GetDC"});
        CHECK(end[0].format == L"[GlassCore][BomeMemo] hooks-end hwnd=0x%p channel=%s ready=%d text=%u fill=%u skip=%u open=%zu");
        CHECK(end[0].numbers == (std::vector<long long>{number(&c.window), 1, 2, 1, 48, 0}));
        // Two open sessions of one window: what is drawn counts for the newest.
        nextSecond();
        Root::Root_OnGetDC(&c.window,&c.module); c.Activate(&c.window,&c.dc);
        Root::Root_OnBeginPaint(&c.window,&c.module);
        CHECK(c.Text(&c.dc,secret) == 15 && c.Fill() == 1);
        Root::Root_OnEndPaint(&c.window,&c.module);
        Root::Root_OnReleaseDC(&c.window,&c.module);
        const auto pair = find(L"] hooks-end ");
        CHECK(pair.size() == 2 && pair[0].strings == std::vector<std::wstring>{L"Paint"});
        CHECK(pair[0].numbers == (std::vector<long long>{number(&c.window), 1, 1, 1, 0, 1}));
        CHECK(pair[1].strings == std::vector<std::wstring>{L"GetDC"});
        CHECK(pair[1].numbers == (std::vector<long long>{number(&c.window), 1, 0, 0, 0, 0}));
        // A session that could not be armed reports itself and counts nothing.
        nextSecond();
        Root::Root_OnBeginPaint(&c.window,&c.otherModule); c.Activate(&c.window,&c.dc);
        CHECK(c.Text() == 43);
        Root::Root_OnEndPaint(&c.window,&c.otherModule);
        const auto barrier = find(L"] hooks-end ");
        CHECK(barrier.size() == 1 && barrier[0].strings == std::vector<std::wstring>{L"Paint"});
        CHECK(barrier[0].numbers == (std::vector<long long>{number(&c.window), 0, 0, 0, 0, 0}));
        // Nothing logged carries the text of a row.
        nextSecond();
        const std::wstring wide(secret, secret + std::strlen(secret));
        CHECK(seen.size() >= 20); // the whole test, not only its last second
        for (const auto& log : seen)
        {
            CHECK(log.format.find(L"%hs") == std::wstring::npos && log.format.find(L"%S") == std::wstring::npos);
            for (const auto& value : log.strings) CHECK(value.find(wide) == std::wstring::npos);
        }
    }
    void Failures()
    {
        Context c; c.Begin(); TestFont font{57}; c.dc.font = &font;
        const auto state = static_cast<DcState&>(c.dc);
        MemoTest::failSave = true; CHECK(c.Text() == 43); MemoTest::failSave = false;
        MemoTest::failBlack = true; CHECK(c.Text() == 43 && c.Fill() == 37); MemoTest::failBlack = false;
        PAINTSTRUCT ps{&c.dc,TRUE,c.rect};
        MemoTest::failBlack = true; CHECK(!Root::Root_OnBomeMemoAfterBeginPaint(&c.window,&c.dc,&ps,&c.module)); MemoTest::failBlack = false;
        for (bool* fail : {&MemoTest::failDib,&MemoTest::failBrush})
        {
            c.dc.background = RGB(20,40,60); *fail = true;
            CHECK(c.Text() == 43 && MemoTest::nativeMode == OPAQUE && MemoTest::nativeBackground == c.dc.background);
            CHECK(c.dc.saved.empty()); *fail = false;
        }
        c.dc.background = state.background;
        Test::alphaResult = E_FAIL;
        CHECK(c.Text() == 43 && MemoTest::nativeMode == OPAQUE && MemoTest::nativeBackground == state.background);
        Test::alphaResult = S_OK;
        Test::duringAlpha = [&] { c.dc.text = 0x123456; c.dc.font = nullptr; throw std::runtime_error("renderer"); };
        CHECK(c.Text() == 43 && MemoTest::nativeMode == OPAQUE);
        Test::duringAlpha = {};
        CHECK(c.dc.text == state.text && c.dc.font == &font && c.dc.saved.empty());
        int reentry = 0;
        Test::duringAlpha = [&] { CHECK(c.Text() == 43); ++reentry; };
        CHECK(c.Text() == 15 && reentry == 1); Test::duringAlpha = {};
        CHECK(c.Text() == 15 && c.dc.bkMode == OPAQUE);
    }
    void Nesting()
    {
        Context c; c.Begin();
        Root::Root_OnGetDC(&c.gutter,&c.module); c.Activate(&c.gutter,&c.gutterDc);
        for (size_t i=0;i<4;++i) CHECK(c.Ref(i) == 2);
        CHECK(c.Text(&c.gutterDc) == 15 && c.Text() == 15); // both sessions are open: each DC is its window's
        Root::Root_OnEndPaint(&c.window,&c.module); // remove lower Paint, keep upper GetDC
        CHECK(c.Ref() == 1 && c.Text(&c.gutterDc) == 15 && c.Text() == 43); // a closed session's DC is native
        Root::Root_OnEndPaint(&c.window,&c.module); CHECK(c.Ref() == 1); // duplicate/no-open close
        Root::Root_OnReleaseDC(&c.gutter,&c.module); CHECK(c.module.hooks.empty());
        // The actual PaintDispatcher failure branch compensates onBegin with onEnd.
        Root::Root_OnBeginPaint(&c.window,&c.module); CHECK(c.Ref() == 1);
        Root::Root_OnEndPaint(&c.window,&c.module);
        CHECK(c.module.hooks.empty() && Root::g_rootPaintStack.empty() && Root::g_rootDcStack.empty());
        CHECK(c.Text() == 43);
    }
    void Lifecycle()
    {
        Context c; c.Begin();
        const int native = MemoTest::chainAnsi;
        std::thread otherThread([&] { CHECK(c.Text() == 43); }); otherThread.join();
        CHECK(MemoTest::chainAnsi == native+1 && c.Text() == 15);
        Root::g_shutdown = true;
        CHECK(c.Text() == 43 && c.Color(COLOR_WINDOW) == MemoTest::windowColor && c.Fill() == 37);
        LOGBRUSH input{BS_SOLID,0xffffff};
        { std::unique_ptr<LOGBRUSH> result(c.Brush(&input)); CHECK(result && result->lbColor == 0xffffff); }
        Root::g_shutdown = false;
        BomeMemo::ClearSessions(); Root::DisableAllRootHooks();
        CHECK(c.module.hooks.empty() && c.Text() == 43);
        // No original in a slot: thunk's explicit Win32 fallback, no guessed caller.
        CHECK(Root::Root_MyBomeMemoText(&c.dc,"native",6,&c.rect,kFlags) == 43);
        CHECK(Root::Root_MyBomeMemoColor(COLOR_WINDOW) == MemoTest::windowColor);
        CHECK(Root::Root_MyBomeMemoFill(&c.dc,&c.rect,&c.brush) == 37);
        { std::unique_ptr<LOGBRUSH> result(Root::Root_MyBomeMemoBrush(&input)); CHECK(result && result->lbColor == 0xffffff); }
    }
}
int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const std::pair<const char*,void(*)()> cases[]{
        {"classes",Classes},{"transaction",Transaction},{"paint",Paint},{"getdc",GetDc},
        {"text",TextLines},{"conversion",Conversion},{"native",Native},{"backgrounds",Backgrounds},
        {"opaque",Opaque},{"isolation",Isolation},{"interleaving",Interleaving},{"diagnostics",Diagnostics},
        {"failures",Failures},{"nesting",Nesting},{"lifecycle",Lifecycle}};
    try { for (const auto& [name,run] : cases) if (std::strcmp(argv[1],name) == 0) { run(); return 0; } }
    catch (const std::exception& error) { std::cerr << argv[1] << ": " << error.what() << '\n'; return 1; }
    return 2;
}
