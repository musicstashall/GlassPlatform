// Exercise actual public BeginDraw/EndDraw and installed production IAT bodies.
// Argument/state checks only: these doubles do not validate Windows alpha pixels.
#include "listbox_background_test_shim.h"
#include "../../GlassCore/src/Hooks/delphi/TListBoxDraw.cpp"

#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__func__) + ": " + #x); } while(false)
namespace
{
    using namespace GlassCore::DelphiListBox;
    using namespace ListBackgroundTest;
    struct Context
    {
        TestModule module;
        TestWindow window, parent, other;
        TestDC dc, foreign, memory;
        RECT row{0,0,200,20};
        DRAWITEMSTRUCT dis{};
        Context()
        {
            window.cls=L"TListBox"; window.kind=GlassShared::GlassControlKind::ListBox;
            dc.owner=&window; foreign.owner=&other;
            PanelTest::parents[&window]=&parent; PanelTest::clients[&window]=row;
            dis={ODT_LISTBOX,0,0,ODA_DRAWENTIRE,0,&window,&dc,row,0};
            module.originals={{"FillRect",reinterpret_cast<void*>(&::FillRect)},
                {"ExtTextOutW",reinterpret_cast<void*>(&::ExtTextOutW)},
                {"DrawTextW",reinterpret_cast<void*>(&::DrawTextW)},
                {"Rectangle",reinterpret_cast<void*>(&::Rectangle)},
                {"BitBlt",reinterpret_cast<void*>(&::BitBlt)},
                {"StretchBlt",reinterpret_cast<void*>(&::StretchBlt)}};
            GlassCore::ModuleHandler::g_entryPointModule=&module;
            Test::full=true; Test::alphaResult=S_OK; Test::duringAlpha={};
            Test::alphaGlyph=Test::alphaText=0;
            failBrush=failFill=failBlack=failSave=failClip=false;
            brushCalls=fills=blackens=extCalls=0; align=0; extResult=TRUE;
            ImageTest::registrationFailures.clear(); Startup();
        }
        ~Context()
        {
            duringFill={};
            while (IsInsideDraw()) EndDraw();
            Shutdown();
            assert(module.hooks.empty() && liveBrushes==0 && themes==0 && dc.saved.empty());
            for (const auto& [name, ref] : module.refs) { (void)name; assert(ref==0); }
            PanelTest::parents.erase(&window); PanelTest::clients.erase(&window);
        }
        bool Begin(UINT state=0) { dis.itemState=state; return BeginDraw(&parent,&dis); }
        template<class Fn,class... Args> auto Call(const char* name,Fn,Args... args)
        { CHECK(module.hooks.count(name)); return reinterpret_cast<Fn>(module.hooks.at(name))(args...); }
        int Fill(COLORREF color,const RECT* rect=nullptr,HDC target=nullptr)
        {
            LOGBRUSH brush{BS_SOLID,color};
            return Call("FillRect",&::FillRect,target ? target : dis.hDC,rect ? rect : &row,&brush);
        }
        BOOL Ext(COLORREF color,UINT flags=ETO_OPAQUE|ETO_CLIPPED,UINT count=0,const INT* dx=nullptr)
        {
            dis.hDC->background=color;
            return Call("ExtTextOutW",&::ExtTextOutW,dis.hDC,3,4,flags,&row,L"row",count,dx);
        }
        void Refs(int expected,int rectangles=0)
        { for (const auto& [name,ref] : module.refs) CHECK(ref==(name=="Rectangle" ? rectangles : expected)); }
    };
    void CustomColours()
    {
        Context c; CHECK(c.Begin());
        for (const COLORREF color : {RGB(244,244,244),RGB(72,144,216),RGB(255,0,0),0x7f123456u})
        {
            CHECK(c.Fill(color)==7); CHECK(fillAlpha==100 && lastColor==color);
            CHECK(c.Ext(color)); CHECK(fillAlpha==100 && lastColor==color);
            CHECK(liveBrushes==0);
        }
        CHECK(brushCalls==8 && blackens==0);
    }
    void DefaultsAndSelection()
    {
        Context c;
        for (UINT state : {0u,ODS_SELECTED,ODS_SELECTED|ODS_FOCUS,ODS_DISABLED})
        {
            CHECK(c.Begin(state));
            for (int index : {COLOR_WINDOW,COLOR_BTNFACE})
            {
                const int n=brushCalls, b=blackens;
                CHECK(c.Fill(GetSysColor(index))==1); CHECK(c.Ext(GetSysColor(index)));
                CHECK(brushCalls==n && blackens==b+2);
            }
            CHECK(c.Fill(GetSysColor(COLOR_HIGHLIGHT))==7); CHECK(fillAlpha==255);
            CHECK(c.Ext(GetSysColor(COLOR_HIGHLIGHT))); CHECK(fillAlpha==255);
            if (state & ODS_SELECTED)
            {
                CHECK(c.Fill(RGB(90,110,130))==7 && fillAlpha==255);
                CHECK(c.Ext(RGB(90,110,130)) && fillAlpha==255);
            }
            EndDraw();
        }
        PanelTest::windowColor=RGB(18,22,26); PanelTest::faceColor=RGB(40,44,48);
        highlight=RGB(80,120,160); CHECK(c.Begin());
        CHECK(c.Fill(GetSysColor(COLOR_WINDOW))==1);
        CHECK(c.Ext(GetSysColor(COLOR_BTNFACE)));
        CHECK(c.Fill(highlight)==7 && fillAlpha==255);
        CHECK(c.Fill(RGB(255,255,255))==7 && fillAlpha==100);
    }
    void BlackBackground()
    {
        Context c; CHECK(c.Begin());
        // Unknown black can still be Root's mapping of a system background.
        CHECK(c.Ext(0)); CHECK(brushCalls==0 && blackens==1);
        CHECK(c.Fill(0)==7 && fillAlpha==100);
        CHECK(c.Ext(0) && fillAlpha==100 && blackens==1);
        // A later full row fill supersedes the black-origin evidence.
        CHECK(c.Fill(GetSysColor(COLOR_WINDOW))==1); const int n=brushCalls;
        CHECK(c.Ext(0)); CHECK(brushCalls==n && blackens==3);
        CHECK(c.Fill(0)==7 && fillAlpha==100);
        CHECK(c.Ext(GetSysColor(COLOR_WINDOW))); const int systemBrushes=brushCalls;
        CHECK(c.Ext(0)); CHECK(brushCalls==systemBrushes);
        EndDraw(); CHECK(c.Begin());
        CHECK(c.Ext(0)); CHECK(brushCalls==systemBrushes);
        EndDraw(); CHECK(c.Begin(ODS_SELECTED));
        CHECK(c.Fill(0)==7 && fillAlpha==255); CHECK(c.Ext(0) && fillAlpha==255);
    }
    void RowGeometryAndClip()
    {
        Context c; CHECK(c.Begin());
        const RECT inset{5,2,40,18}, crossing{-1,0,200,20}, client{0,0,200,100};
        CHECK(c.Fill(RGB(30,60,90),&inset)==7 && fillAlpha==255);
        const int n=brushCalls;
        CHECK(c.Fill(RGB(30,60,90),&crossing)==7 && fillAlpha==-1);
        CHECK(c.Fill(RGB(30,60,90),&client)==7 && fillAlpha==-1 && brushCalls==n);
        clips[&c.dc]={10,3,90,17}; const RECT before=ListClip(&c.dc);
        CHECK(c.Fill(RGB(30,60,90))==7 && fillAlpha==100 && EqualRect(&lastRect,&c.row));
        CHECK(c.Ext(RGB(30,60,90)) && fillAlpha==100);
        const RECT after=ListClip(&c.dc); CHECK(EqualRect(&before,&after));
    }
    void ExactClasses()
    {
        Context c;
        for (LPCWSTR cls : {L"TCheckListBox",L"TColorListBox",L"TDirectoryListBox",L"TFileListBox"})
        {
            c.window.cls=cls; CHECK(c.Begin());
            CHECK(c.Fill(RGB(40,80,160))==7 && fillAlpha==255);
            CHECK(c.Ext(RGB(40,80,160)) && fillAlpha==255);
            CHECK(c.Call("DrawTextW",&::DrawTextW,&c.dc,L"row",3,&c.row,0u)==15);
            CHECK(!drawTransparent); EndDraw();
        }
        for (LPCWSTR cls : {L"TColorBox",L"TDriveComboBox",L"TFilterComboBox"})
        {
            c.window.cls=cls; c.window.kind=GlassShared::GlassControlKind::ComboBox;
            c.dis.CtlType=ODT_COMBOBOX; c.dis.itemID=UINT(-1); CHECK(c.Begin(ODS_COMBOBOXEDIT));
            CHECK(c.Fill(RGB(40,80,160))==7 && fillAlpha==255);
            CHECK(c.Call("DrawTextW",&::DrawTextW,&c.dc,L"row",3,&c.row,0u)==15);
            CHECK(drawTransparent); EndDraw();
        }
        c.dis.CtlType=ODT_LISTBOX; c.window.kind=GlassShared::GlassControlKind::ListBox;
        for (LPCWSTR cls : {L"ListBox",L"MyTListBox",L"TListBox2",L"WindowsForms10.LISTBOX.app.0"})
        { c.window.cls=cls; CHECK(!c.Begin()); }
        c.window.cls=L"tlistbox"; CHECK(c.Begin()); CHECK(c.Fill(RGB(40,80,160))==7 && fillAlpha==100);
    }
    void TextArgumentsAndFallback()
    {
        Context c; CHECK(c.Begin()); const INT dx[]{6,7,8};
        const int n=fills; CHECK(c.Ext(RGB(40,80,160),ETO_OPAQUE|ETO_CLIPPED|ETO_GLYPH_INDEX,3,dx));
        CHECK(fills==n+1 && fillAlpha==100 && Test::alphaGlyph==1);
        CHECK(Test::glyphOptions==(ETO_CLIPPED|ETO_GLYPH_INDEX));
        CHECK(Test::glyphCount==3 && Test::glyphDx==dx && extCalls==0);
        CHECK(Test::glyphOriginal==reinterpret_cast<void*>(&::ExtTextOutW));
        Test::alphaResult=E_FAIL;
        CHECK(c.Ext(RGB(40,80,160),ETO_OPAQUE|ETO_CLIPPED,3,dx));
        CHECK(extCalls==1 && extOptions==ETO_CLIPPED && extCount==3 && extDx==dx);
        CHECK(std::wstring_view(extText)==L"row" && fillAlpha==100);
        const int b=brushCalls;
        CHECK(c.Ext(RGB(40,80,160),ETO_CLIPPED,3,dx)); CHECK(brushCalls==b);
        CHECK(c.Ext(RGB(40,80,160),ETO_OPAQUE|ETO_PDY,3,dx));
        CHECK(extOptions==(ETO_OPAQUE|ETO_PDY) && brushCalls==b);
        CHECK(c.Call("ExtTextOutW",&::ExtTextOutW,&c.dc,3,4,ETO_OPAQUE,
                     static_cast<const RECT*>(nullptr),L"row",3,dx));
        CHECK(extOptions==ETO_OPAQUE && brushCalls==b);
        align=TA_UPDATECP; CHECK(c.Ext(RGB(40,80,160),ETO_OPAQUE,3,dx)); CHECK(brushCalls==b);
    }
    void DrawTextPreservesBackground()
    {
        Context c;
        for (UINT state : {0u,ODS_SELECTED})
        {
            CHECK(c.Begin(state));
            CHECK(c.Fill(RGB(40,80,160))==7 && fillAlpha==(state ? 255 : 100));
            const int b=brushCalls, a=Test::alphaText;
            const wchar_t text[]=L"row";
            CHECK(c.Call("DrawTextW",&::DrawTextW,&c.dc,text,-1,&c.row,0x20u)==15);
            CHECK(Test::alphaText==a+1 && drawTransparent && brushCalls==b);
            CHECK(drawDc==&c.dc && drawText==text && drawCount==-1);
            CHECK(drawRect==&c.row && drawFormat==0x20u && c.dc.saved.empty());
            Test::alphaResult=E_FAIL;
            CHECK(c.Call("DrawTextW",&::DrawTextW,&c.dc,text,3,&c.row,0x24u)==17);
            CHECK(nativeDrawDc==&c.dc && nativeDrawText==text && nativeDrawCount==3);
            CHECK(nativeDrawRect==&c.row && nativeDrawFormat==0x24u && brushCalls==b);
            Test::alphaResult=S_OK;
            const int calls=Test::alphaText;
            CHECK(c.Call("DrawTextW",&::DrawTextW,&c.dc,text,3,&c.row,DT_MODIFYSTRING)==17);
            CHECK(Test::alphaText==calls && nativeDrawFormat==DT_MODIFYSTRING);
            EndDraw();
        }
    }
    void SessionIsolation()
    {
        Context c; CHECK(!BeginDraw(&c.other,&c.dis));
        Test::full=false; CHECK(!c.Begin()); Test::full=true;
        c.window.backdrop=false; CHECK(!c.Begin()); c.window.backdrop=true;
        c.window.kind=GlassShared::GlassControlKind::None; CHECK(!c.Begin());
        c.window.kind=GlassShared::GlassControlKind::ListBox; CHECK(c.Begin());
        CHECK(c.Fill(RGB(40,80,160),nullptr,&c.foreign)==7 && fillAlpha==-1 && brushCalls==0);
        const auto fill=reinterpret_cast<decltype(&::FillRect)>(c.module.hooks.at("FillRect"));
        LOGBRUSH brush{BS_SOLID,RGB(40,80,160)};
        std::thread t([&] { CHECK(fill(&c.dc,&c.row,&brush)==7); }); t.join(); CHECK(brushCalls==0);
        EndDraw(); CHECK(fill(&c.dc,&c.row,&brush)==7 && brushCalls==0);
    }
    void NestedSessions()
    {
        Context c; CHECK(c.Begin()); c.Refs(1);
        CHECK(c.Fill(0)==7 && fillAlpha==100);
        c.window.cls=L"TCheckListBox"; CHECK(c.Begin()); c.Refs(2,1);
        CHECK(c.Fill(RGB(40,80,160))==7 && fillAlpha==255);
        const int b=brushCalls; CHECK(c.Ext(0) && brushCalls==b);
        EndDraw(); c.Refs(1);
        CHECK(c.Ext(0) && fillAlpha==100);
        CHECK(c.Fill(RGB(40,80,160))==7 && fillAlpha==100); EndDraw(); c.Refs(0);
    }
    void UnsupportedBrushesAndFailures()
    {
        Context c; CHECK(c.Begin());
        LOGBRUSH pattern{BS_PATTERN,RGB(40,80,160)};
        CHECK(c.Call("FillRect",&::FillRect,&c.dc,&c.row,&pattern)==7 && brushCalls==0);
        CHECK(c.Call("FillRect",&::FillRect,&c.dc,&c.row,static_cast<HBRUSH>(nullptr))==7 && brushCalls==0);
        failFill=true; CHECK(c.Fill(0)==0); failFill=false;
        const int b=brushCalls; CHECK(c.Ext(0) && brushCalls==b);
        failBrush=true; const int f=fills; CHECK(c.Fill(RGB(40,80,160))==7 && fillAlpha==-1 && fills==f+1);
        failBrush=false; failFill=true; CHECK(c.Fill(RGB(40,80,160))==0 && fillAlpha==100);
        failFill=false; failBlack=true; CHECK(c.Fill(GetSysColor(COLOR_WINDOW))==7 && fillAlpha==-1);
        failBlack=false; failSave=true; const int n=brushCalls;
        CHECK(c.Ext(RGB(40,80,160),ETO_OPAQUE,3)); CHECK(brushCalls==n && extOptions==ETO_OPAQUE);
        failSave=false; failClip=true; CHECK(c.Ext(RGB(40,80,160),ETO_OPAQUE,3)); CHECK(brushCalls==n);
        CHECK(c.dc.saved.empty());
    }
    void MissingSlotsAndLifecycle()
    {
        Context c; ImageTest::registrationFailures[&c.module].insert("FillRect"); CHECK(c.Begin());
        CHECK(!c.module.hooks.count("FillRect")); CHECK(c.Ext(RGB(40,80,160)) && fillAlpha==100);
        EndDraw(); ImageTest::registrationFailures.clear(); CHECK(c.Begin());
        const auto fill=reinterpret_cast<decltype(&::FillRect)>(c.module.hooks.at("FillRect"));
        Shutdown(); c.Refs(0); LOGBRUSH brush{BS_SOLID,RGB(40,80,160)}; const int n=brushCalls;
        CHECK(fill(&c.dc,&c.row,&brush)==7 && brushCalls==n); EndDraw(); CHECK(!IsInsideDraw());
    }
    void ReentryAndMemoryDc()
    {
        Context c; c.dis.hDC=&c.memory; CHECK(c.Begin());
        duringFill=[&] {
            // Grow the real TLS vector while its original callback is active.
            for (int i=0;i<80;++i) CHECK(c.Begin());
            for (int i=0;i<80;++i) EndDraw();
        };
        CHECK(c.Fill(0)==7 && brushCalls==1); duringFill={};
        CHECK(c.Ext(0) && fillAlpha==100); CHECK(c.memory.saved.empty());
        c.Refs(1); CHECK(liveBrushes==0);
    }
    void NativeGraphicsUnaffected()
    {
        Context c; CHECK(c.Begin());
        TestDC source; PanelTest::bitmaps[&source]=PanelTest::stockBitmap;
        CHECK(c.Call("BitBlt",&::BitBlt,&c.dc,2,2,8,8,&source,0,0,SRCCOPY));
        CHECK(ImageTest::nativeBits==1 && ImageTest::alphaBlends==0 && brushCalls==0);
        CHECK(c.Call("DrawTextW",&::DrawTextW,&c.dc,L"measure",7,&c.row,DT_CALCRECT)==17);
        CHECK(Test::alphaText==0 && brushCalls==0); PanelTest::bitmaps.erase(&source);
    }
}
int main(int argc,char** argv)
{
    try
    {
        if (argc!=2) throw std::runtime_error("one test case required");
        const std::string name=argv[1];
        if (name=="custom") CustomColours();
        else if (name=="defaults-selection") DefaultsAndSelection();
        else if (name=="black") BlackBackground();
        else if (name=="geometry") RowGeometryAndClip();
        else if (name=="classes") ExactClasses();
        else if (name=="text") TextArgumentsAndFallback();
        else if (name=="drawtext") DrawTextPreservesBackground();
        else if (name=="isolation") SessionIsolation();
        else if (name=="nesting") NestedSessions();
        else if (name=="failures") UnsupportedBrushesAndFailures();
        else if (name=="lifecycle") MissingSlotsAndLifecycle();
        else if (name=="reentry-memory") ReentryAndMemoryDc();
        else if (name=="graphics") NativeGraphicsUnaffected();
        else throw std::runtime_error("unknown case");
        std::cout << "PASS " << name << '\n';
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
