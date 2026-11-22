#include "listview_images_test_shim.h"
#include "../../GlassShared/src/BitmapUtils.cpp"
#include "../../GlassCore/src/Subsystems/OpaqueBitmapSource.cpp"
#include "../../GlassCore/src/Hooks/ListViewHooks.cpp"
#include <thread>

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)
namespace Lv = GlassCore::ListViewControl;

namespace
{
    BOOL WINAPI ChainedMask(HDC dc, int x, int y, int w, int h, HDC src, int sx, int sy,
        HBITMAP mask, int mx, int my, DWORD rop)
    {
        ++ListViewImageTest::chainMasks;
        const auto result = MaskBlt(dc, x, y, w, h, src, sx, sy, mask, mx, my, rop);
        return result ? ListViewImageTest::maskResult : FALSE;
    }
    struct Fixture
    {
        TestModule exe, modern, legacy, otherModule;
        TestWindow list, other;
        TestDC dc, source, memory, foreign;
        TestFont font{23};
        LOGBRUSH brush{BS_SOLID, RGB(9,8,7)};
        HBITMAP original{}, destination{}, memoryBitmap{}, mask{};
        std::vector<BYTE> bytes;
        explicit Fixture(bool generic = true)
        {
            using namespace ImageTest;
            CHECK(bitmaps.empty());
            creates=deletes=reads=flushes=selects=0;
            nativeMasks=nativeStretches=nativeBits=alphaBlends=keys=queries=0;
            failPlaceholder=failDib=nullBits=failRead=partialRead=failFlush=failMetadata=failNative=false;
            failSelectAt=-1; selectReturnsNull=glow=reentered=false;
            duringNative={}; calls.clear(); removal={}; registrationFailures.clear(); modulePaths.clear();
            Test::full=true; Test::stackOwners.clear(); Test::black=Test::nativeFill=Test::alphaText=0;
            Test::nativeText=Test::nativeExt=0; Test::alphaResult=S_OK;
            PanelTest::parents.clear(); PanelTest::instances.clear(); PanelTest::maps.clear();
            PanelTest::bitmaps.clear(); PanelTest::clients.clear();
            ListViewImageTest::backgrounds.clear(); ListViewImageTest::textBackgrounds.clear();
            ListViewImageTest::backgroundSets=ListViewImageTest::textBackgroundSets=0;
            ListViewImageTest::scrollDepth=ListViewImageTest::scrollEnsures=ListViewImageTest::chainMasks=0;
            ListViewImageTest::maskResult=TRUE; ListViewImageTest::themed=false;
            ListViewImageTest::shell=nullptr; ListViewImageTest::stack.clear();
            ListViewImageTest::themeClass=L"ListView";
            GlassCore::ModuleHandler::g_entryPointModule=&exe;
            GlassCore::ModuleHandler::g_comctl32New=&modern;
            GlassCore::ModuleHandler::g_comctl32Legacy=&legacy;
            caller=&modern; sysmon=nullptr; Test::ux=nullptr;
            GlassCore::PaintDispatcher::imageProviders.clear();
            GlassCore::PaintDispatcher::listViewPostProviders.clear();
            Lv::g_lvBgPrimed.clear();
            list.cls=L"TListView"; list.kind=GlassShared::GlassControlKind::ListView;
            other.cls=L"TButton";
            PanelTest::clients[&list]={0,0,200,60};
            ListViewImageTest::backgrounds[&list]=0xffffff;
            ListViewImageTest::textBackgrounds[&list]=0xf0f0f0;
            original=MakeBitmap(64,4); destination=MakeBitmap(200,60); memoryBitmap=MakeBitmap(200,60);
            mask=MakeBitmap(64,4,1,false);
            for (size_t i=0; i<original->pixels.size(); i+=4)
            {
                original->pixels[i]=BYTE((i/4)%3 ? 17 : 0);
                original->pixels[i+1]=BYTE((i/4)%3 ? 57 : 0);
                original->pixels[i+2]=BYTE((i/4)%3 ? 91 : 0);
                original->pixels[i+3]=BYTE(i/4); // black + zero/partial/255 strip alpha
            }
            bytes=original->pixels;
            dc.owner=&list; foreign.owner=&other;
            source.brush=&brush; source.font=&font; source.text=RGB(20,30,40);
            source.background=RGB(50,60,70); source.bkMode=OPAQUE;
            PanelTest::bitmaps[&dc]=destination; PanelTest::bitmaps[&source]=original;
            PanelTest::bitmaps[&memory]=memoryBitmap; PanelTest::bitmaps[&foreign]=PanelTest::stockBitmap;
            Host(&list);
            modern.originals=legacy.originals={{"MaskBlt",reinterpret_cast<void*>(ChainedMask)}};
            if (generic)
                for (auto* module : {&modern, &legacy})
                    module->originals.insert({
                        {"BitBlt",reinterpret_cast<void*>(BitBlt)},
                        {"StretchBlt",reinterpret_cast<void*>(StretchBlt)},
                        {"FillRect",reinterpret_cast<void*>(FillRect)},
                        {"ExtTextOutW",reinterpret_cast<void*>(ExtTextOutW)},
                        {"DrawTextW",reinterpret_cast<void*>(DrawTextW)},
                        {"DrawThemeTextEx",reinterpret_cast<void*>(DrawThemeTextEx)},
                        {"DrawThemeBackground",reinterpret_cast<void*>(DrawThemeBackground)},
                        {"SetBkColor",reinterpret_cast<void*>(SetBkColor)},
                    });
            exe.originals=otherModule.originals=modern.originals;
            Lv::Startup();
            CHECK(GlassCore::PaintDispatcher::imageProviders.size()==1);
            CHECK(GlassCore::PaintDispatcher::listViewPostProviders.size()==1);
        }
        ~Fixture()
        {
            ImageTest::duringNative={}; Lv::g_shutdown.store(false); Lv::Shutdown();
            for (auto* module : {&exe,&modern,&legacy,&otherModule})
                for (const auto& [name,ref] : module->refs) { (void)name; assert(ref==0); }
            PanelTest::bitmaps.clear();
            const auto owned=ImageTest::bitmaps;
            for (auto bmp : owned) ImageDeleteObject(bmp);
            assert(ImageTest::creates==ImageTest::deletes);
            Test::stackOwners.clear();
        }
        void Host(HWND w)
        {
            Test::topWindow=Test::host=w;
            Test::stackOwners[&dc]=Test::stackOwners[&memory]=w;
        }
        void Begin(HMODULE module) { GlassCore::PaintDispatcher::imageProviders.front().onBegin(&list,module); }
        void Begin() { Begin(&modern); }
        void End(HWND w=nullptr) { GlassCore::PaintDispatcher::imageProviders.front().onEnd(w?w:&list,nullptr); }
        BOOL Mask(HDC target=nullptr, HDC src=nullptr, DWORD rop=0xaacc0020, bool useMask=true,
            int w=8, int h=2, int sx=2, int sy=1, int mx=3, int my=4)
        {
            SetLastError(0x29);
            return Lv::Overall_MyMaskBlt(target?target:&dc,11,12,w,h,src?src:&source,sx,sy,
                useMask?mask:nullptr,mx,my,rop);
        }
        void Restored()
        {
            CHECK(PanelTest::bitmaps.at(&source)==original && original->pixels==bytes);
            CHECK(source.brush==&brush && source.font==&font && source.text==RGB(20,30,40));
            CHECK(source.background==RGB(50,60,70) && source.bkMode==OPAQUE);
            CHECK(ImageTest::bitmaps.size()==4);
        }
        void Prepared()
        {
            const auto& p=ImageTest::last.pixels;
            CHECK(p.size()==bytes.size() && ImageTest::last.selected!=original);
            for (size_t i=0; i<p.size(); i+=4)
            { CHECK(p[i]==bytes[i] && p[i+1]==bytes[i+1] && p[i+2]==bytes[i+2] && p[i+3]==255); }
            CHECK(ImageTest::last.entryError==0x29 && GetLastError()==0x51);
            CHECK(ImageTest::readDc==&source && ImageTest::alphaBlends==0 && ImageTest::keys==0);
            Restored();
        }
        void Native()
        { CHECK(ImageTest::last.selected==original && ImageTest::last.pixels==bytes); Restored(); }
    };
    void Routes()
    {
        for (auto cls : {L"TListView",L"TSystemListView",L"tlistview"}) for (bool legacy : {false,true})
        {
            Fixture f; f.list.cls=cls;
            auto* mod=legacy?&f.legacy:&f.modern;
            auto* other=legacy?&f.modern:&f.legacy;
            ImageTest::caller=mod; f.Begin(mod);
            CHECK(mod->refs["MaskBlt"]==1 && other->refs["MaskBlt"]==0);
            CHECK(!f.exe.hooks.count("MaskBlt"));
            CHECK(f.Mask()); CHECK(ImageTest::nativeMasks==1 && ListViewImageTest::chainMasks==1);
            f.Prepared(); f.End(); CHECK(mod->refs["MaskBlt"]==0);
            CHECK(f.modern.refs["BitBlt"]==1 && f.legacy.refs["BitBlt"]==1); // baseline unchanged
        }
        Fixture f;
        GlassCore::ModuleHandler::g_comctl32Legacy=&f.modern;
        f.Begin(); CHECK(f.modern.refs["MaskBlt"]==1); CHECK(f.Mask()); f.Prepared(); f.End();
        CHECK(f.modern.refs["MaskBlt"]==0 && f.legacy.refs["MaskBlt"]==0);
    }
    void Scope()
    {
        for (auto cls : {L"SysListView32",L"TListViewSuffix",L"PrefixTListView",L"TSystemListViewExtra"})
        {
            Fixture f; f.list.cls=cls; f.Begin();
            CHECK(f.Mask()); CHECK(ImageTest::reads==0); f.Native(); f.End();
        }
        for (int which=0; which<7; ++which)
        {
            Fixture f; HMODULE module=&f.modern;
            switch(which)
            {
            case 0:module=&f.exe;break; case 1:module=&f.otherModule;break; case 2:module=nullptr;break;
            case 3:f.list.kind=GlassShared::GlassControlKind::WinForms_ListView;break;
            case 4:f.list.kind=GlassShared::GlassControlKind::None;break;
            case 5:Test::full=false;break; case 6:f.list.backdrop=false;break;
            }
            f.Begin(module);
            CHECK(f.modern.refs["MaskBlt"]==0 && f.legacy.refs["MaskBlt"]==0 && f.exe.refs["MaskBlt"]==0);
            CHECK(f.Mask()); CHECK(ImageTest::reads==0); f.Native(); f.End();
        }
        // Incoming cross-comctl IAT references do not widen the donor caller.
        Fixture f; f.Begin(&f.legacy);
        CHECK(Lv::EnableHooksOnModule(&f.modern,Lv::OHK_MASKBLT,true));
        CHECK(f.Mask()); CHECK(ImageTest::reads==0); f.Native();
        CHECK(Lv::EnableHooksOnModule(&f.modern,Lv::OHK_MASKBLT,false)); f.End();
    }
    void NativeSemantics()
    {
        for (DWORD rop : {DWORD(0xaacc0020),DWORD(0x00ee0086),SRCCOPY,DWORD(0)})
            for (bool mask : {false,true})
        {
            Fixture f; f.Begin(); ListViewImageTest::maskResult=17;
            CHECK(f.Mask(nullptr,nullptr,rop,mask,8,2,-2,1,-3,4)==17);
            CHECK(ImageTest::nativeMasks==1 && ListViewImageTest::chainMasks==1);
            const auto& c=ImageTest::last;
            CHECK(c.dest==&f.dc && c.source==&f.source && c.x==11 && c.y==12 && c.w==8 && c.h==2);
            CHECK(c.sx==-2 && c.sy==1 && c.mask==(mask?f.mask:nullptr) && c.mx==-3 && c.my==4 && c.rop==rop);
            CHECK(c.state.brush==&f.brush && c.state.font==&f.font && c.state.background==f.source.background);
            f.Prepared(); f.End();
        }
    }
    void SourceTypes()
    {
        for (int bpp : {8,16,24,32}) for (bool dib : {false,true})
        {
            Fixture f; f.original->info.bmBitsPixel=bpp;
            f.original->info.bmBits=dib?f.original->Data():nullptr;
            ImageTest::removal={true,RGB(91,57,17),255}; // must not key even an exact icon RGB
            f.Begin(); CHECK(f.Mask()); CHECK(ImageTest::reads==1 && ImageTest::flushes==1);
            f.Prepared(); f.End();
        }
    }
    void DcIsolation()
    {
        for (int which=0; which<15; ++which)
        {
            Fixture f; f.Begin(); HDC dest=&f.dc;
            switch(which)
            {
            case 0:dest=&f.foreign;break;
            case 1:dest=&f.memory;Test::stackOwners.erase(&f.memory);break;
            case 2:Test::topWindow=&f.other;break;
            case 3:f.list.backdrop=false;break;
            case 4:Test::full=false;break;
            case 5:PanelTest::maps[&f.dc]=2;break;
            case 6:PanelTest::maps[&f.source]=2;break;
            case 7:Test::stackOwners.erase(&f.dc);break;
            case 8:f.list.cls=L"TListViewSuffix";break;
            case 9:Lv::g_shutdown.store(true);break;
            case 10:Test::stackOwners[&f.memory]=&f.other;dest=&f.memory;break;
            case 11:f.list.valid=false;break;
            case 12:f.list.kind=GlassShared::GlassControlKind::None;break;
            case 13:ListViewImageTest::scrollDepth=1;break;
            case 14:ImageTest::caller=&f.otherModule;break;
            }
            CHECK(f.Mask(dest)); CHECK(ImageTest::reads==0); f.Native();
            Lv::g_shutdown.store(false); f.End(); CHECK(f.modern.refs["MaskBlt"]==0);
        }
    }
    void MemoryAndThread()
    {
        {
            Fixture f; f.Begin(); CHECK(f.Mask(&f.memory)); f.Prepared(); f.End();
        }
        Fixture f; f.Begin();
        std::thread otherThread([&] { CHECK(f.Mask()); }); otherThread.join();
        CHECK(ImageTest::reads==0); f.Native(); CHECK(f.Mask()); f.Prepared(); f.End();
    }
    void Leases()
    {
        {
            Fixture f; f.Begin(); f.Begin(); CHECK(f.modern.refs["MaskBlt"]==2);
            f.End(&f.other); CHECK(f.modern.refs["MaskBlt"]==2 && Lv::g_lvSessionStack.size()==2);
            f.End(); CHECK(f.modern.refs["MaskBlt"]==1); CHECK(f.Mask()); f.Prepared();
            f.list.kind=GlassShared::GlassControlKind::None;
            f.End(); CHECK(f.modern.refs["MaskBlt"]==0 && Lv::g_lvSessionStack.empty());
        }
        // Only MaskBlt exists in this fixture: changes to generic hook routing
        // are outside this test. End must release the *captured* VCL module.
        Fixture f(false); f.Begin(&f.legacy); CHECK(f.legacy.refs["MaskBlt"]==1);
        GlassCore::ModuleHandler::g_comctl32Legacy=&f.otherModule;
        GlassCore::ModuleHandler::g_comctl32New=nullptr;
        f.list.cls=L"SysListView32"; f.list.kind=GlassShared::GlassControlKind::None;
        f.End(); CHECK(f.legacy.refs["MaskBlt"]==0 && f.otherModule.refs["MaskBlt"]==0);
    }
    void Barriers()
    {
        for (bool registrationFailure : {false,true})
        {
            Fixture f;
            if (registrationFailure)
            {
                ImageTest::registrationFailures[&f.modern].insert("MaskBlt");
                f.modern.hooks["MaskBlt"]=reinterpret_cast<void*>(std::uintptr_t(17));
            }
            else f.modern.originals.erase("MaskBlt");
            f.Begin(); CHECK(f.modern.refs["MaskBlt"]==0);
            CHECK(f.Mask()); CHECK(ImageTest::reads==0); f.Native(); f.End();
            if (registrationFailure)
            {
                CHECK(f.modern.hooks.at("MaskBlt")==reinterpret_cast<void*>(std::uintptr_t(17)));
                f.modern.hooks.clear();
            }
        }
        for (int which=0; which<4; ++which)
        {
            Fixture f; f.Begin(); HMODULE inner=&f.modern;
            switch(which)
            { case 0:Test::full=false;break; case 1:f.list.backdrop=false;break;
              case 2:inner=&f.exe;break; case 3:inner=&f.otherModule;break; }
            f.Begin(inner); CHECK(f.modern.refs["MaskBlt"]==1);
            CHECK(f.Mask()); CHECK(ImageTest::reads==0); f.Native(); f.End();
            Test::full=true; f.list.backdrop=true;
            CHECK(f.modern.refs["MaskBlt"]==1); CHECK(f.Mask()); f.Prepared(); f.End();
        }
    }
    void NativeFallbacks()
    {
        for (int which=0; which<14; ++which)
        {
            Fixture f; f.Begin(); HDC src=&f.source;
            int w=8,h=2;
            switch(which)
            {
            case 0:w=0;break; case 1:h=0;break; case 2:w=-8;break; case 3:h=-2;break;
            case 4:f.original->info.bmBitsPixel=1;break;
            case 5:f.original->info.bmBitsPixel=4;break;
            case 6:f.original->info.bmPlanes=2;break;
            case 7:f.original->info.bmWidth=0;break;
            case 8:f.original->info.bmWidth=200;f.original->info.bmHeight=60;break; // partial framebuffer copy too
            case 9:src=&f.dc;break;
            case 10:PanelTest::bitmaps[&f.dc]=f.original;break;
            case 11:PanelTest::bitmaps[&f.source]=PanelTest::stockBitmap;break;
            case 12:src=nullptr;break;
            case 13:f.source.owner=&f.other;break;
            }
            SetLastError(0x29);
            CHECK(Lv::Overall_MyMaskBlt(&f.dc,11,12,w,h,src,2,1,f.mask,3,4,0xaacc0020));
            CHECK(ImageTest::nativeMasks==1 && ImageTest::reads==0 && GetLastError()==0x51);
            CHECK(ImageTest::last.w==w && ImageTest::last.h==h && ImageTest::last.source==src);
            PanelTest::bitmaps[&f.source]=f.original; PanelTest::bitmaps[&f.dc]=f.destination;
            f.Restored(); f.End();
        }
        Fixture f; f.Begin(); SetLastError(0x29);
        CHECK(Lv::Overall_MyMaskBlt(nullptr,0,0,8,2,&f.source,0,0,nullptr,0,0,SRCCOPY));
        CHECK(ImageTest::reads==0 && !ImageTest::last.dest); f.Restored(); f.End();
    }
    void Failures()
    {
        for (int which=0; which<11; ++which)
        {
            Fixture f; f.Begin();
            switch(which)
            {
            case 0:ImageTest::failPlaceholder=true;break; case 1:ImageTest::failDib=true;break;
            case 2:ImageTest::nullBits=true;break; case 3:ImageTest::failRead=true;break;
            case 4:ImageTest::partialRead=true;break; case 5:ImageTest::failSelectAt=1;break;
            case 6:ImageTest::failSelectAt=2;break; case 7:ImageTest::failFlush=true;break;
            case 8:ImageTest::failMetadata=true;break; case 9:ImageTest::failNative=true;break;
            case 10:ImageTest::failSelectAt=2;ImageTest::selectReturnsNull=true;break;
            }
            CHECK(bool(f.Mask())==(which!=9));
            CHECK(ImageTest::nativeMasks==1 && ListViewImageTest::chainMasks==1);
            CHECK(ImageTest::last.entryError==0x29 && GetLastError()==0x51);
            if (which==9) f.Prepared(); else f.Native();
            f.End(); CHECK(f.modern.refs["MaskBlt"]==0);
        }
    }
    void Reentry()
    {
        Fixture f; f.Begin();
        ImageTest::duringNative=[&] {
            // Force vector reallocation while the outer native call is active.
            for (int i=0; i<64; ++i) f.Begin();
            CHECK(f.Mask());
            for (int i=0; i<64; ++i) f.End();
        };
        CHECK(f.Mask()); CHECK(ImageTest::reads==1 && ImageTest::nativeMasks==2);
        CHECK(f.modern.refs["MaskBlt"]==1 && Lv::g_lvSessionStack.size()==1); f.Prepared();
        ImageTest::duringNative={}; CHECK(f.Mask()); CHECK(ImageTest::reads==2); f.Prepared(); f.End();
    }
    void Lifecycle()
    {
        {
            Fixture f; f.Begin(); CHECK(f.modern.refs["MaskBlt"]==1);
            Lv::OnComctl32ModuleUnload(&f.modern); CHECK(f.modern.refs["MaskBlt"]==0);
            f.End(); CHECK(!Lv::g_overallMap.count(&f.modern));
        }
        {
            Fixture f; f.Begin(); // Same End callback compensates a NULL BeginPaint result.
            f.End(); f.End(); CHECK(f.modern.refs["MaskBlt"]==0 && Lv::g_lvSessionStack.empty());
        }
        {
            Fixture f; f.Begin(); Lv::Shutdown(); f.End();
            CHECK(f.modern.refs["MaskBlt"]==0 && Lv::g_lvSessionStack.empty());
        }
        Fixture f; Lv::g_shutdown.store(true); f.Begin();
        CHECK(Lv::g_lvSessionStack.empty() && f.modern.refs["MaskBlt"]==0);
        Lv::g_shutdown.store(false); f.End();
    }
    void ExistingPaths()
    {
        for (bool winforms : {false,true})
        {
            Fixture f; f.list.cls=winforms?L"WindowsForms10.SysListView32.app.0.test":L"SysListView32";
            f.list.kind=winforms?GlassShared::GlassControlKind::WinForms_ListView:GlassShared::GlassControlKind::ListView;
            f.Begin(); CHECK(f.Mask()); CHECK(ImageTest::reads==0); f.Native();
            CHECK(Lv::Overall_MyBitBlt(&f.dc,11,12,8,2,&f.source,2,1,SRCCOPY));
            CHECK(ImageTest::alphaBlends==1 && ImageTest::keys==0);
            if (!winforms)
            {
                CHECK(Lv::Overall_MyStretchBlt(&f.dc,11,12,20,12,&f.source,2,1,8,2,SRCCOPY));
                CHECK(ImageTest::alphaBlends==2);
            }
            ListViewImageTest::scrollDepth=1;
            CHECK(Lv::Overall_MyBitBlt(&f.dc,11,12,8,2,&f.source,2,1,SRCCOPY));
            CHECK(ImageTest::nativeBits==1); f.Restored(); f.End();
        }
        for (bool vcl : {false,true})
        {
            Fixture f; if (!vcl) f.list.cls=L"SysListView32";
            f.Begin(); CHECK(ListViewImageTest::backgroundSets==1 && ListViewImageTest::textBackgroundSets==1);
            CHECK(ListViewImageTest::scrollEnsures==1);
            PAINTSTRUCT ps{&f.dc,TRUE,{0,0,20,10}};
            const auto post=GlassCore::PaintDispatcher::listViewPostProviders.front().onAfterBeginPaint;
            CHECK(post(&f.list,&f.dc,&ps,&f.modern)); CHECK(Test::black==1);
            CHECK(post(&f.list,&f.dc,&ps,&f.modern)==vcl); CHECK(Test::black==(vcl?2:1));
            RECT r{0,0,20,10};
            CHECK(Lv::Overall_MyDrawTextW(&f.dc,L"item",4,&r,DT_CALCRECT)==17);
            CHECK(Test::alphaText==0);
            CHECK(Lv::Overall_MyDrawTextW(&f.dc,L"item",4,&r,DT_SINGLELINE)==15);
            CHECK(Test::alphaText==1);
            CHECK(Lv::Overall_MyExtTextOutW(&f.dc,0,0,ETO_GLYPH_INDEX,&r,L"x",1,nullptr));
            CHECK(Test::nativeExt==1 && Test::alphaText==1);
            LOGBRUSH window{BS_SOLID,GetSysColor(COLOR_WINDOW)};
            const int before=Test::black;
            CHECK(Lv::Overall_MyFillRect(&f.dc,&r,&window)); CHECK(Test::black==before+1);
            f.End(); f.Begin(); CHECK(ListViewImageTest::backgroundSets==1 && ListViewImageTest::textBackgroundSets==1);
            f.End(); f.Restored();
        }
    }
}
int main(int argc, char** argv)
{
    try
    {
        CHECK(argc==2); const std::string name=argv[1];
        if (name=="routes") Routes();
        else if (name=="scope") Scope();
        else if (name=="native-semantics") NativeSemantics();
        else if (name=="source-types") SourceTypes();
        else if (name=="dc-isolation") DcIsolation();
        else if (name=="memory-and-thread") MemoryAndThread();
        else if (name=="leases") Leases();
        else if (name=="barriers") Barriers();
        else if (name=="native-fallbacks") NativeFallbacks();
        else if (name=="failures") Failures();
        else if (name=="reentry") Reentry();
        else if (name=="lifecycle") Lifecycle();
        else if (name=="existing-paths") ExistingPaths();
        else return 2;
        std::cout<<"PASS "<<name<<'\n'; return 0;
    }
    catch (const std::exception& e) { std::cerr<<(argc>1?argv[1]:"unknown")<<": "<<e.what()<<'\n'; return 1; }
}
