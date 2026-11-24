#include "vcl_images_test_shim.h"
#include "../../GlassShared/src/BitmapUtils.cpp"
#include "../../GlassCore/src/Subsystems/OpaqueBitmapSource.cpp"
#include "../../GlassCore/src/Hooks/ToolbarHooks.cpp"
#include "../../GlassCore/src/Hooks/delphi/TPanelDraw.cpp"
#include <thread>

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)

namespace
{
    inline int chainMasks{}, chainStretches{}, chainIcons{};
    BOOL WINAPI ChainedIcon(HDC dc, int x, int y, HICON icon, int cx, int cy, UINT step, HBRUSH flicker, UINT flags)
    { ++chainIcons; return DrawIconEx(dc,x,y,icon,cx,cy,step,flicker,flags); }
    BOOL WINAPI ChainedMask(HDC dc, int x, int y, int w, int h, HDC src, int sx, int sy,
        HBITMAP mask, int mx, int my, DWORD rop)
    { ++chainMasks; return MaskBlt(dc,x,y,w,h,src,sx,sy,mask,mx,my,rop); }
    BOOL WINAPI ChainedStretch(HDC dc, int x, int y, int w, int h, HDC src, int sx, int sy,
        int sw, int sh, DWORD rop)
    { ++chainStretches; return StretchBlt(dc,x,y,w,h,src,sx,sy,sw,sh,rop); }

    struct Fixture
    {
        TestModule exe, modern, legacy, otherModule;
        TestWindow toolbar, panel, parent, other;
        TestDC dc, source, memory, foreign;
        TestFont font{23};
        LOGBRUSH brush{BS_SOLID,RGB(9,8,7)};
        HBITMAP original{}, destination{}, memoryBitmap{}, mask{};
        std::vector<BYTE> bytes;
        COLORREF canvas{RGB(50,60,70)}; // the brush colour of the source bitmap's canvas
        Fixture()
        {
            using namespace ImageTest;
            CHECK(bitmaps.empty());
            creates=deletes=reads=flushes=selects=0;
            nativeMasks=nativeStretches=nativeBits=alphaBlends=keys=queries=0;
            failPlaceholder=failDib=nullBits=failRead=partialRead=failFlush=failMetadata=failNative=false;
            failSelectAt=-1; selectReturnsNull=glow=reentered=false;
            dibs=failDibAt=nullBitsAt=0; dirtyDib=false;
            duringNative={}; calls.clear(); removal={}; registrationFailures.clear(); chainMasks=chainStretches=chainIcons=0;
            styleQueries=themeQueries=0; styles.clear(); composited=FALSE; themeAsked=nullptr;
            partAsked=stateAsked=propertyAsked=0;
            IconTest::calls.clear(); IconTest::failDrawAt=0; IconTest::failInfo=IconTest::failBlend=false;
            IconTest::duringDraw={};
            IconTest::infos=IconTest::metrics=IconTest::layoutQueries=0; IconTest::layouts.clear(); IconTest::blend={};
            PanelTest::failDc=false; PanelTest::faceColor=0xf0f0f0; PanelTest::windowColor=0xffffff;
            Test::full=true; Test::stackOwners.clear();
            GlassCore::ModuleHandler::g_entryPointModule=&exe;
            GlassCore::ModuleHandler::g_comctl32New=&modern;
            GlassCore::ModuleHandler::g_comctl32Legacy=&legacy;
            caller=&modern; sysmon=nullptr; Test::ux=nullptr;
            GlassCore::PaintDispatcher::imageProviders.clear();
            PanelTest::parents.clear(); PanelTest::instances.clear(); PanelTest::maps.clear();
            PanelTest::bitmaps.clear(); PanelTest::clients.clear();
            toolbar.cls=L"TToolBar"; toolbar.kind=GlassShared::GlassControlKind::Toolbar;
            panel.cls=parent.cls=L"TPanel"; other.cls=L"TButton";
            PanelTest::parents[&panel]=&parent;
            PanelTest::clients[&panel]=PanelTest::clients[&toolbar]=RECT{0,0,200,60};
            original=MakeBitmap(64,4); destination=MakeBitmap(200,60); memoryBitmap=MakeBitmap(200,60);
            mask=MakeBitmap(64,4,1,false);
            for (size_t i=0;i<original->pixels.size();i+=4)
            {
                original->pixels[i]=BYTE((i/4)%3 ? 17 : 0);
                original->pixels[i+1]=BYTE((i/4)%3 ? 57 : 0);
                original->pixels[i+2]=BYTE((i/4)%3 ? 91 : 0);
                original->pixels[i+3]=BYTE(i/4); // zero/partial/255 and opaque black, not an alpha classifier
            }
            bytes=original->pixels;
            dc.owner=&toolbar; foreign.owner=&other;
            source.brush=&brush; source.font=&font; source.text=RGB(20,30,40);
            source.background=RGB(50,60,70); source.bkMode=OPAQUE;
            PanelTest::bitmaps[&dc]=destination; PanelTest::bitmaps[&source]=original;
            PanelTest::bitmaps[&memory]=memoryBitmap; PanelTest::bitmaps[&foreign]=PanelTest::stockBitmap;
            Host(&toolbar);
            modern.originals=legacy.originals={
                {"MaskBlt",reinterpret_cast<void*>(ChainedMask)},
                {"BitBlt",reinterpret_cast<void*>(BitBlt)},
                {"DrawThemeText",reinterpret_cast<void*>(DrawThemeText)},
            };
            exe.originals={
                {"FillRect",reinterpret_cast<void*>(FillRect)},
                {"PatBlt",reinterpret_cast<void*>(PatBlt)},
                {"ExtTextOutW",reinterpret_cast<void*>(ExtTextOutW)},
                {"StretchBlt",reinterpret_cast<void*>(ChainedStretch)},
            };
            GlassCore::ToolbarControl::Startup(); GlassCore::DelphiPanel::Startup();
        }
        ~Fixture()
        {
            ImageTest::duringNative={};
            GlassCore::DelphiPanel::Shutdown(); GlassCore::ToolbarControl::Shutdown();
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
        void BeginToolbar(HMODULE module=nullptr)
        {
            CHECK(GlassCore::PaintDispatcher::imageProviders.size()==1);
            GlassCore::PaintDispatcher::imageProviders.front().onBegin(&toolbar,module?module:&exe);
        }
        void EndToolbar()
        { GlassCore::PaintDispatcher::imageProviders.front().onEnd(&toolbar,nullptr); }
        bool BeginPanel(UINT msg=WM_PAINT)
        {
            dc.owner=&panel; Host(&panel);
            return GlassCore::DelphiPanel::BeginPaint(&panel,msg,nullptr);
        }
        void EndPanel() { GlassCore::DelphiPanel::EndPaint(); }
        BOOL Mask(HDC target=nullptr, HDC src=nullptr, DWORD rop=0xaacc0020, bool useMask=true,
            int w=8, int h=2)
        {
            SetLastError(0x29);
            return GlassCore::ToolbarControl::Overall_MyMaskBlt(target?target:&dc,11,12,w,h,
                src?src:&source,2,1,useMask?mask:nullptr,3,4,rop);
        }
        BOOL Stretch(HDC target=nullptr, HDC src=nullptr, DWORD rop=SRCCOPY,
            int w=20,int h=12,int sw=8,int sh=2)
        {
            SetLastError(0x29);
            return GlassCore::DelphiPanel::PanelStretch(target?target:&dc,11,12,w,h,
                src?src:&source,2,1,sw,sh,rop);
        }
        void Restored()
        {
            CHECK(PanelTest::bitmaps.at(&source)==original && original->pixels==bytes);
            CHECK(source.brush==&brush && source.font==&font && source.text==RGB(20,30,40));
            CHECK(source.background==canvas && source.bkMode==OPAQUE && GetMapMode(&source)==MM_TEXT);
            CHECK(ImageTest::bitmaps.size()==4 && !GlassCore::ToolbarControl::g_inOpaqueIcon);
            CHECK(!GlassCore::DelphiPanel::g_inHook);
        }
        void Prepared()
        {
            const auto& p=ImageTest::last.pixels;
            CHECK(p.size()==bytes.size() && ImageTest::last.selected!=original);
            for (size_t i=0;i<p.size();i+=4)
            { CHECK(p[i]==bytes[i] && p[i+1]==bytes[i+1] && p[i+2]==bytes[i+2] && p[i+3]==255); }
            CHECK(ImageTest::last.entryError==0x29 && GetLastError()==0x51);
            CHECK(ImageTest::readDc==&source && ImageTest::alphaBlends==0 && ImageTest::keys==0);
            Restored();
        }
        void Native()
        { CHECK(ImageTest::last.selected==original && ImageTest::last.pixels==bytes); Restored(); }
    };

    void UtilityTypesAndBytes()
    {
        for (int bpp : {8,16,24,32}) for (bool dib : {false,true})
        {
            Fixture f;
            f.original->info.bmBitsPixel=bpp;
            f.original->info.bmBits=dib?f.original->Data():nullptr;
            const auto state=static_cast<DcState>(f.source);
            {
                GlassCore::GdiUtils::OpaqueBitmapSource copy{&f.source}; CHECK(copy);
                auto* selected=ImageTest::Selected(&f.source); CHECK(selected && selected!=f.original);
                CHECK(selected->info.bmBitsPixel==32);
                for (size_t i=0;i<f.bytes.size();i+=4)
                {
                    const auto* p=selected->Data()+i;
                    CHECK(p[0]==f.bytes[i] && p[1]==f.bytes[i+1] && p[2]==f.bytes[i+2] && p[3]==255);
                }
                CHECK(f.source.font==state.font && f.source.brush==state.brush && f.source.background==state.background);
            }
            CHECK(ImageTest::reads==1 && ImageTest::flushes==1 && ImageTest::selects==3);
            f.Restored();
        }
    }
    void UtilityRejectsAndFailures()
    {
        for (int which=0;which<18;++which)
        {
            Fixture f;
            switch (which)
            {
            case 0:ImageTest::failPlaceholder=true;break;
            case 1:ImageTest::failDib=true;break;
            case 2:ImageTest::nullBits=true;break;
            case 3:ImageTest::failRead=true;break;
            case 4:ImageTest::partialRead=true;break;
            case 5:ImageTest::failFlush=true;break;
            case 6:ImageTest::failMetadata=true;break;
            case 7:ImageTest::failSelectAt=1;break;
            case 8:ImageTest::failSelectAt=2;break;
            case 9:f.original->info.bmBitsPixel=1;break;
            case 10:f.original->info.bmBitsPixel=4;break;
            case 11:f.original->info.bmBitsPixel=64;break;
            case 12:f.original->info.bmPlanes=2;break;
            case 13:f.original->info.bmWidth=0;break;
            case 14:f.original->info.bmHeight=-1;break;
            case 15:f.original->info.bmWidth=(std::numeric_limits<LONG>::max)();break;
            case 16:ImageTest::failSelectAt=1;ImageTest::selectReturnsNull=true;break;
            case 17:ImageTest::failSelectAt=2;ImageTest::selectReturnsNull=true;break;
            }
            { GlassCore::GdiUtils::OpaqueBitmapSource copy{&f.source}; CHECK(!copy); }
            f.Restored();
        }
        Fixture f;
        { GlassCore::GdiUtils::OpaqueBitmapSource copy{&f.dc}; CHECK(!copy); }
        PanelTest::bitmaps[&f.source]=PanelTest::stockBitmap;
        { GlassCore::GdiUtils::OpaqueBitmapSource copy{&f.source}; CHECK(!copy); }
        PanelTest::bitmaps[&f.source]=f.original;
        f.Restored();
    }
    void ToolbarExeAndComctlRoutes()
    {
        for (int caller=0;caller<3;++caller) for (bool legacy : {false,true})
        {
            Fixture f;
            f.BeginToolbar(caller==0?&f.exe:caller==1?&f.modern:&f.legacy);
            CHECK(f.modern.refs["MaskBlt"]==1 && f.legacy.refs["MaskBlt"]==1);
            CHECK(!f.exe.hooks.count("MaskBlt") && !f.modern.hooks.count("BitBlt"));
            CHECK(GlassCore::ToolbarControl::g_modStack.back().vclIcons);
            ImageTest::caller=legacy?&f.legacy:&f.modern;
            CHECK(f.Mask()); CHECK(ImageTest::nativeMasks==1 && chainMasks==1);
            const auto& c=ImageTest::last;
            CHECK(c.dest==&f.dc && c.source==&f.source && c.x==11 && c.y==12 && c.w==8 && c.h==2);
            CHECK(c.sx==2 && c.sy==1 && c.mask==f.mask && c.mx==3 && c.my==4 && c.rop==0xaacc0020);
            f.Prepared(); f.EndToolbar();
            CHECK(f.modern.refs["MaskBlt"]==0 && f.legacy.refs["MaskBlt"]==0);
        }
    }
    void ToolbarMasksRopsAndModernPath()
    {
        for (DWORD rop : {DWORD(0xaacc0020),DWORD(0x00ee0086),SRCCOPY}) for (bool mask : {false,true})
        {
            Fixture f; f.BeginToolbar(); CHECK(f.Mask(nullptr,nullptr,rop,mask));
            CHECK(ImageTest::last.rop==rop && ImageTest::last.mask==(mask?f.mask:nullptr)); f.Prepared(); f.EndToolbar();
        }
        {
            Fixture f; f.BeginToolbar();
            CHECK(GlassCore::ToolbarControl::EnableHooksOnModule(&f.modern,GlassCore::ToolbarControl::OHK_BITBLT,true));
            GlassCore::ToolbarControl::g_toolbarSuppressMaskBitBlt=true;
            PanelTest::bitmaps[&f.source]=f.mask;
            CHECK(GlassCore::ToolbarControl::Overall_MyBitBlt(&f.dc,11,12,8,2,&f.source,2,1,SRCAND));
            CHECK(ImageTest::nativeBits==1 && ImageTest::last.selected==f.mask && ImageTest::last.rop==SRCAND);
            CHECK(ImageTest::reads==0 && ImageTest::keys==0 && ImageTest::alphaBlends==0);
            PanelTest::bitmaps[&f.source]=f.original;
            CHECK(GlassCore::ToolbarControl::EnableHooksOnModule(&f.modern,GlassCore::ToolbarControl::OHK_BITBLT,false));
            f.EndToolbar(); f.Restored();
        }
        // Existing common-controls renderer remains a real-alpha direct path.
        Fixture f; f.toolbar.cls=L"ToolbarWindow32"; f.BeginToolbar(&f.modern);
        CHECK(!GlassCore::ToolbarControl::g_modStack.back().vclIcons);
        CHECK(f.Mask(nullptr,nullptr,SRCCOPY,false));
        CHECK(ImageTest::alphaBlends==1 && ImageTest::nativeMasks==0 && ImageTest::reads==0);
        f.EndToolbar(); f.Restored();
    }
    void ToolbarDcAndSessionIsolation()
    {
        for (int which=0;which<11;++which)
        {
            Fixture f; f.BeginToolbar(); HDC dest=&f.dc;
            switch (which)
            {
            case 0:dest=&f.foreign;break;
            case 1:dest=&f.memory;Test::stackOwners.erase(&f.memory);break;
            case 2:Test::topWindow=&f.other;break;
            case 3:f.toolbar.backdrop=false;break;
            case 4:Test::full=false;break;
            case 5:PanelTest::maps[&f.dc]=2;break;
            case 6:Test::stackOwners.erase(&f.dc);break;
            case 7:PanelTest::instances[&f.toolbar]=&f.otherModule;break;
            case 8:f.toolbar.cls=L"TToolBarSuffix";break;
            case 9:GlassCore::ToolbarControl::g_shutdown.store(true);break;
            case 10:Test::stackOwners[&f.memory]=&f.other;dest=&f.memory;break;
            }
            CHECK(f.Mask(dest)); CHECK(ImageTest::reads==0); f.Native();
            GlassCore::ToolbarControl::g_shutdown.store(false); f.EndToolbar();
        }
        {
            Fixture f; f.BeginToolbar(); CHECK(f.Mask(&f.memory)); f.Prepared(); f.EndToolbar();
        }
        {
            Fixture f; f.BeginToolbar();
            std::thread otherThread([&] { CHECK(f.Mask()); }); otherThread.join();
            CHECK(ImageTest::reads==0); f.Native(); f.EndToolbar();
        }
    }
    void ToolbarLeasesAndMissingImports()
    {
        {
            Fixture f; f.BeginToolbar(); f.BeginToolbar();
            CHECK(f.modern.refs["MaskBlt"]==2 && f.legacy.refs["MaskBlt"]==2);
            f.EndToolbar(); CHECK(f.modern.refs["MaskBlt"]==1); CHECK(f.Mask()); f.Prepared();
            GlassCore::ModuleHandler::g_comctl32Legacy=&f.otherModule;
            f.toolbar.kind=GlassShared::GlassControlKind::None;
            f.EndToolbar(); CHECK(f.modern.refs["MaskBlt"]==0 && f.legacy.refs["MaskBlt"]==0);
        }
        {
            Fixture f; GlassCore::ModuleHandler::g_comctl32Legacy=&f.modern;
            f.BeginToolbar(); CHECK(f.modern.refs["MaskBlt"]==1 && f.legacy.refs["MaskBlt"]==0); f.EndToolbar();
        }
        for (bool conflict : {false,true})
        {
            Fixture f;
            if (conflict)
            {
                f.modern.hooks["MaskBlt"]=reinterpret_cast<void*>(std::uintptr_t(17));
                ImageTest::registrationFailures[&f.modern].insert("MaskBlt");
            }
            else f.modern.originals.erase("MaskBlt");
            f.BeginToolbar(); CHECK(!GlassCore::ToolbarControl::g_modStack.back().iconModules[0]);
            CHECK(f.Mask()); CHECK(ImageTest::reads==0); f.Native();
            ImageTest::caller=&f.legacy; CHECK(f.Mask()); f.Prepared(); f.EndToolbar();
            if (conflict)
            { CHECK(f.modern.hooks.at("MaskBlt")==reinterpret_cast<void*>(std::uintptr_t(17))); f.modern.hooks.clear(); }
        }
    }
    void ToolbarUnreadyNativeBarriers()
    {
        for (int which=0;which<4;++which)
        {
            Fixture f; f.BeginToolbar(); HMODULE innerCaller=&f.exe;
            switch(which)
            {
            case 0:Test::full=false;break;
            case 1:f.toolbar.backdrop=false;break;
            case 2:PanelTest::instances[&f.toolbar]=&f.otherModule;break;
            case 3:innerCaller=&f.otherModule;break;
            }
            f.BeginToolbar(innerCaller);
            const auto& s=GlassCore::ToolbarControl::g_modStack.back();
            CHECK(s.vclIcons && !s.iconModules[0] && !s.iconModules[1]);
            CHECK(f.Mask()); CHECK(ImageTest::reads==0 && ImageTest::alphaBlends==0); f.Native();
            f.EndToolbar();
            Test::full=true; f.toolbar.backdrop=true; PanelTest::instances.erase(&f.toolbar);
            CHECK(f.Mask()); f.Prepared(); f.EndToolbar();
        }
    }
    void ToolbarNativeFailureAndReentry()
    {
        for (int which=0;which<8;++which)
        {
            Fixture f; f.BeginToolbar();
            switch(which)
            {
            case 0:ImageTest::failPlaceholder=true;break;case 1:ImageTest::failDib=true;break;
            case 2:ImageTest::nullBits=true;break;case 3:ImageTest::failRead=true;break;
            case 4:ImageTest::partialRead=true;break;case 5:ImageTest::failSelectAt=2;break;
            case 6:ImageTest::failFlush=true;break;case 7:ImageTest::failNative=true;break;
            }
            CHECK(bool(f.Mask())==(which!=7)); CHECK(ImageTest::nativeMasks==1 && chainMasks==1);
            CHECK(GetLastError()==0x51 && ImageTest::last.entryError==0x29);
            if(which==7) f.Prepared(); else f.Native();
            f.EndToolbar();
        }
        Fixture f; f.BeginToolbar();
        ImageTest::duringNative=[&] { CHECK(f.Mask()); };
        CHECK(f.Mask()); CHECK(ImageTest::reads==1 && ImageTest::nativeMasks==2);
        f.Prepared(); f.EndToolbar();
    }
    // The captions of a VCL toolbar are drawn by comctl32 v6, whoever began the
    // paint. On the EXE route (TWinControl.PaintHandler) the session takes the
    // text hook of that module by name - the one the comctl32 route takes by
    // its caller - and gives back exactly what it took.
    constexpr UINT kFlags=0x24;                              // DT_SINGLELINE | DT_VCENTER
    void ToolbarCaptionsOnTheExeRoute()
    {
        using namespace GlassCore::ToolbarControl;
        const auto reset=[] {
            Test::alphaText=Test::nativeTheme=0; Test::alphaResult=S_OK; ImageTest::glow=false;
            ImageTest::textDc=nullptr; ImageTest::textString.clear(); ImageTest::textFlags=0; ImageTest::textRect={};
        };
        const auto caption=[&](Fixture& f, HMODULE from, UINT flags=kFlags, const RECT* where=nullptr) {
            static const RECT rect{21,3,90,25};
            ImageTest::caller=from;
            return Overall_MyDrawThemeText(nullptr,&f.dc,1,1,L"Open",4,flags,0,where?where:&rect);
        };
        // The EXE began the paint: one reference on comctl32 v6, none on the legacy module or the EXE.
        {
            reset(); Fixture f; f.BeginToolbar(&f.exe);
            CHECK(f.modern.refs["DrawThemeText"]==1 && f.legacy.refs["DrawThemeText"]==0);
            CHECK(!f.exe.hooks.count("DrawThemeText") && g_modStack.back().textModule==&f.modern);
            CHECK(!g_modStack.back().glowModule && g_modStack.back().texts==0);
            CHECK(caption(f,&f.modern)==S_OK);
            CHECK(Test::alphaText==1 && Test::nativeTheme==0 && ImageTest::textDc==&f.dc);
            CHECK(ImageTest::textString==L"Open" && ImageTest::textFlags==kFlags);
            CHECK(ImageTest::textRect.left==21 && ImageTest::textRect.top==3 && ImageTest::textRect.right==90
                && ImageTest::textRect.bottom==25);
            CHECK(caption(f,&f.modern)==S_OK); CHECK(g_modStack.back().texts==2);
            // A measurement and a call without a rectangle are comctl32's own.
            CHECK(caption(f,&f.modern,kFlags|DT_CALCRECT)==S_FALSE); CHECK(Test::nativeTheme==1 && Test::alphaText==2);
            ImageTest::caller=&f.modern;
            CHECK(Overall_MyDrawThemeText(nullptr,&f.dc,1,1,L"Open",4,kFlags,0,nullptr)==S_FALSE);
            CHECK(Test::nativeTheme==2 && g_modStack.back().texts==2);
            // The legacy module was not asked for anything: its call is not ours.
            CHECK(caption(f,&f.legacy)==S_FALSE); CHECK(Test::nativeTheme==3 && Test::alphaText==2);
            // The compositor refused: the caption is drawn as comctl32 would draw it, and is not counted.
            Test::alphaResult=E_FAIL; CHECK(caption(f,&f.modern)==S_FALSE);
            CHECK(Test::alphaText==3 && Test::nativeTheme==4 && g_modStack.back().texts==2);
            f.EndToolbar();
            CHECK(f.modern.refs["DrawThemeText"]==0 && g_modStack.empty());
            // Outside a session the import is not hooked at all.
            Test::alphaResult=S_OK; CHECK(caption(f,&f.modern)==S_FALSE); CHECK(Test::alphaText==3);
        }
        // comctl32 v6 began the paint: its own route takes the reference, and only one is taken.
        {
            reset(); Fixture f; f.BeginToolbar(&f.modern);
            CHECK(f.modern.refs["DrawThemeText"]==1 && !g_modStack.back().textModule);
            CHECK(caption(f,&f.modern)==S_OK); CHECK(Test::alphaText==1 && g_modStack.back().texts==1);
            f.EndToolbar(); CHECK(f.modern.refs["DrawThemeText"]==0);
        }
        // The legacy module began it: it draws without a theme, and nothing is taken.
        {
            reset(); Fixture f; f.BeginToolbar(&f.legacy);
            CHECK(f.modern.refs["DrawThemeText"]==0 && f.legacy.refs["DrawThemeText"]==0 && !g_modStack.back().textModule);
            CHECK(caption(f,&f.modern)==S_FALSE); CHECK(Test::alphaText==0 && Test::nativeTheme==1);
            f.EndToolbar();
        }
        // A toolbar of common controls is not this route.
        {
            reset(); Fixture f; f.toolbar.cls=L"ToolbarWindow32"; f.BeginToolbar(&f.exe);
            CHECK(f.modern.refs["DrawThemeText"]==0 && !g_modStack.back().textModule && !g_modStack.back().vclIcons);
            f.EndToolbar();
        }
        // A VCL toolbar the session does not serve takes nothing: another mode, no glass, another module's
        // window class, a caller that is neither the EXE nor comctl32, no comctl32 v6 in the process.
        for (int which=0;which<5;++which)
        {
            reset(); Fixture f; HMODULE from=&f.exe;
            switch (which)
            {
            case 0:Test::full=false;break;
            case 1:f.toolbar.backdrop=false;break;
            case 2:PanelTest::instances[&f.toolbar]=&f.otherModule;break;
            case 3:from=&f.otherModule;break;
            case 4:GlassCore::ModuleHandler::g_comctl32New=nullptr;break;
            }
            f.BeginToolbar(from);
            CHECK(f.modern.refs["DrawThemeText"]==0 && !g_modStack.back().textModule && !g_modStack.back().glowModule);
            f.EndToolbar(); Test::full=true;
        }
        // The import is missing, or its slot cannot be taken: no reference, and none is given back.
        for (bool conflict : {false,true})
        {
            reset(); Fixture f; TestModule ux; ux.originals={{"DrawTextExW",reinterpret_cast<void*>(DrawTextExW)}};
            Test::ux=&ux; ImageTest::glow=true;
            if (conflict)
            {
                f.modern.hooks["DrawThemeText"]=reinterpret_cast<void*>(std::uintptr_t(17));
                ImageTest::registrationFailures[&f.modern].insert("DrawThemeText");
            }
            else f.modern.originals.erase("DrawThemeText");
            f.BeginToolbar(&f.exe);
            // Without the text the glow has nothing to follow.
            CHECK(!g_modStack.back().textModule && !g_modStack.back().glowModule && ux.refs["DrawTextExW"]==0);
            CHECK(f.modern.refs["MaskBlt"]==1);
            f.EndToolbar(); CHECK(f.modern.refs["DrawThemeText"]==0 && ux.refs["DrawTextExW"]==0);
            if (conflict)
            { CHECK(f.modern.hooks.at("DrawThemeText")==reinterpret_cast<void*>(std::uintptr_t(17))); f.modern.hooks.clear(); }
            Test::ux=nullptr;
        }
        // Glow: the text of uxtheme is taken with it, and given back even though the setting changed meanwhile.
        for (bool changes : {false,true})
        {
            reset(); Fixture f; TestModule ux; ux.originals={{"DrawTextExW",reinterpret_cast<void*>(DrawTextExW)}};
            Test::ux=&ux; ImageTest::glow=true;
            f.BeginToolbar(&f.exe);
            CHECK(f.modern.refs["DrawThemeText"]==1 && ux.refs["DrawTextExW"]==1);
            CHECK(g_modStack.back().textModule==&f.modern && g_modStack.back().glowModule==&ux);
            if (changes) { ImageTest::glow=false; Test::ux=nullptr; }
            f.EndToolbar();
            CHECK(f.modern.refs["DrawThemeText"]==0 && ux.refs["DrawTextExW"]==0);
            Test::ux=nullptr;
        }
        // No uxtheme to hook: the caption alone.
        {
            reset(); Fixture f; ImageTest::glow=true; f.BeginToolbar(&f.exe);
            CHECK(g_modStack.back().textModule==&f.modern && !g_modStack.back().glowModule);
            f.EndToolbar();
        }
        // uxtheme is there, the glow is off: the caption alone.
        {
            reset(); Fixture f; TestModule ux; ux.originals={{"DrawTextExW",reinterpret_cast<void*>(DrawTextExW)}};
            Test::ux=&ux; f.BeginToolbar(&f.exe);
            CHECK(g_modStack.back().textModule==&f.modern && !g_modStack.back().glowModule && ux.refs["DrawTextExW"]==0);
            f.EndToolbar(); CHECK(f.modern.refs["DrawThemeText"]==0 && ux.refs["DrawTextExW"]==0);
            Test::ux=nullptr;
        }
        // The glow text of uxtheme cannot be taken - no such import, or its slot is someone else's:
        // the caption alone, and nothing of uxtheme is given back.
        for (bool conflict : {false,true})
        {
            reset(); Fixture f; TestModule ux; Test::ux=&ux; ImageTest::glow=true;
            if (conflict)
            {
                ux.originals={{"DrawTextExW",reinterpret_cast<void*>(DrawTextExW)}};
                ux.hooks["DrawTextExW"]=reinterpret_cast<void*>(std::uintptr_t(19));
                ImageTest::registrationFailures[&ux].insert("DrawTextExW");
            }
            f.BeginToolbar(&f.exe);
            CHECK(g_modStack.back().textModule==&f.modern && !g_modStack.back().glowModule && ux.refs["DrawTextExW"]==0);
            f.EndToolbar(); CHECK(f.modern.refs["DrawThemeText"]==0 && ux.refs["DrawTextExW"]==0);
            if (conflict)
            { CHECK(ux.hooks.at("DrawTextExW")==reinterpret_cast<void*>(std::uintptr_t(19))); ux.hooks.clear(); }
            Test::ux=nullptr;
        }
        // Nested paints count their references; the end gives back what its own begin took, whatever
        // the process says about its modules by then.
        {
            reset(); Fixture f; f.BeginToolbar(&f.exe); f.BeginToolbar(&f.exe);
            CHECK(f.modern.refs["DrawThemeText"]==2 && g_modStack.back().textModule==&f.modern);
            CHECK(caption(f,&f.modern)==S_OK); CHECK(g_modStack.back().texts==1 && g_modStack.front().texts==0);
            f.EndToolbar(); CHECK(f.modern.refs["DrawThemeText"]==1);
            CHECK(caption(f,&f.modern)==S_OK); CHECK(g_modStack.back().texts==1);
            GlassCore::ModuleHandler::g_comctl32New=&f.otherModule;
            GlassCore::ModuleHandler::g_entryPointModule=&f.otherModule;
            f.toolbar.kind=GlassShared::GlassControlKind::None;
            f.EndToolbar(); CHECK(f.modern.refs["DrawThemeText"]==0 && f.otherModule.refs["DrawThemeText"]==0);
        }
        // Another thread has no session of its own: its caption is comctl32's.
        {
            reset(); Fixture f; f.BeginToolbar(&f.exe);
            std::thread otherThread([&] { CHECK(caption(f,&f.modern)==S_FALSE); }); otherThread.join();
            CHECK(Test::alphaText==0 && Test::nativeTheme==1 && g_modStack.back().texts==0);
            f.EndToolbar();
        }
        reset();
    }
    void PanelNativeImageAndMirroring()
    {
        for (DWORD rop : {SRCCOPY,SRCAND,DWORD(0x00e20746)}) for (bool mirror : {false,true})
        {
            Fixture f; CHECK(f.BeginPanel()); CHECK(f.exe.refs["StretchBlt"]==1);
            CHECK(f.Stretch(nullptr,nullptr,rop,20,mirror?-12:12,mirror?-8:8,2));
            CHECK(ImageTest::nativeStretches==1 && chainStretches==1);
            const auto& c=ImageTest::last;
            CHECK(c.dest==&f.dc && c.source==&f.source && c.x==11 && c.y==12 && c.w==20 && c.h==(mirror?-12:12));
            CHECK(c.sx==2 && c.sy==1 && c.sw==(mirror?-8:8) && c.sh==2 && c.rop==rop);
            f.Prepared(); f.EndPanel(); CHECK(f.exe.refs["StretchBlt"]==0);
        }
    }
    void PanelExactClassAndMessageScope()
    {
        for (int which=0;which<10;++which)
        {
            Fixture f;
            switch(which)
            {
            case 0:f.parent.cls=L"TForm";break;case 1:f.parent.cls=L"TPanelSuffix";break;
            case 2:PanelTest::parents.erase(&f.panel);break;case 3:f.panel.cls=L"TFrame";break;
            case 4:f.panel.cls=L"TFlowPanel";break;case 5:f.panel.cls=L"TScrollBoxEx";break;
            case 6:f.panel.cls=L"TImgViewBox";break;case 7:f.panel.cls=L"TPanelSuffix";break;
            case 8:PanelTest::instances[&f.panel]=&f.otherModule;break;case 9:f.panel.cls=L"TCategoryPanel";break;
            }
            const bool begun=f.BeginPanel();
            CHECK(f.exe.refs["StretchBlt"]==0); CHECK(f.Stretch()); CHECK(ImageTest::reads==0); f.Native();
            if (begun) f.EndPanel();
        }
        for (UINT msg : {WM_ERASEBKGND,WM_PRINTCLIENT})
        {
            Fixture f; CHECK(f.BeginPanel(msg)); CHECK(f.exe.refs["StretchBlt"]==0);
            CHECK(f.Stretch()); CHECK(ImageTest::reads==0); f.Native(); f.EndPanel();
        }
    }
    void PanelDcIsolationAndClientBuffers()
    {
        for (int which=0;which<12;++which)
        {
            Fixture f; CHECK(f.BeginPanel()); HDC dest=&f.dc;
            switch(which)
            {
            case 0:dest=&f.foreign;break;
            case 1:dest=&f.memory;Test::stackOwners.erase(&f.memory);break;
            case 2:Test::topWindow=&f.other;break;case 3:f.panel.backdrop=false;break;
            case 4:Test::full=false;break;case 5:PanelTest::maps[&f.dc]=2;break;
            case 6:Test::stackOwners.erase(&f.dc);break;
            case 7:f.original->info.bmWidth=200;f.original->info.bmHeight=60;break; // partial final-copy too
            case 8:f.parent.cls=L"TForm";break;
            case 9:GlassCore::DelphiPanel::g_shutdown.store(true);break;
            case 10:Test::stackOwners[&f.memory]=&f.other;dest=&f.memory;break;
            case 11:f.original->info.bmBitsPixel=1;break;
            }
            CHECK(f.Stretch(dest)); CHECK(ImageTest::reads==0); f.Native();
            GlassCore::DelphiPanel::g_shutdown.store(false); f.EndPanel();
        }
        Fixture f; CHECK(f.BeginPanel()); CHECK(f.Stretch(&f.memory)); f.Prepared(); f.EndPanel();
    }
    void PanelLeasesNativeBarriersAndShutdown()
    {
        {
            Fixture f; CHECK(f.BeginPanel()); CHECK(f.BeginPanel()); CHECK(f.exe.refs["StretchBlt"]==2);
            f.EndPanel(); CHECK(f.exe.refs["StretchBlt"]==1); CHECK(f.Stretch()); f.Prepared();
            CHECK(GlassCore::DelphiPanel::BeginPaint(&f.panel,WM_ERASEBKGND,&f.dc));
            CHECK(f.Stretch()); CHECK(ImageTest::last.selected==f.original); // outer slot doesn't authorize erase
            f.EndPanel(); CHECK(f.exe.refs["StretchBlt"]==1); f.EndPanel(); CHECK(f.exe.refs["StretchBlt"]==0);
        }
        {
            Fixture f; f.exe.originals.erase("StretchBlt"); CHECK(f.BeginPanel());
            CHECK(f.Stretch()); CHECK(ImageTest::reads==0); f.Native(); f.EndPanel();
        }
        {
            Fixture f; f.exe.hooks["StretchBlt"]=reinterpret_cast<void*>(std::uintptr_t(17));
            ImageTest::registrationFailures[&f.exe].insert("StretchBlt"); CHECK(f.BeginPanel());
            CHECK(!GlassCore::DelphiPanel::g_stack.back().acquired[GlassCore::DelphiPanel::StretchSlot]);
            CHECK(f.Stretch()); CHECK(ImageTest::reads==0); f.Native(); f.EndPanel();
            CHECK(f.exe.hooks.at("StretchBlt")==reinterpret_cast<void*>(std::uintptr_t(17))); f.exe.hooks.clear();
        }
        Fixture f; CHECK(f.BeginPanel()); GlassCore::DelphiPanel::Shutdown();
        f.EndPanel(); CHECK(f.exe.refs["StretchBlt"]==0 && !f.BeginPanel());
    }
    void PanelFailureAndReentry()
    {
        for(int which=0;which<5;++which)
        {
            Fixture f; CHECK(f.BeginPanel());
            switch(which)
            { case 0:ImageTest::failRead=true;break;case 1:ImageTest::nullBits=true;break;
              case 2:ImageTest::partialRead=true;break;case 3:ImageTest::failSelectAt=2;break;
              case 4:ImageTest::failNative=true;break; }
            CHECK(bool(f.Stretch())==(which!=4)); CHECK(ImageTest::nativeStretches==1 && chainStretches==1);
            CHECK(GetLastError()==0x51 && ImageTest::last.entryError==0x29);
            if(which==4)f.Prepared();else f.Native(); f.EndPanel();
        }
        Fixture f; CHECK(f.BeginPanel()); ImageTest::duringNative=[&] { CHECK(f.Stretch()); };
        CHECK(f.Stretch()); CHECK(ImageTest::nativeStretches==2 && ImageTest::reads==1); f.Prepared(); f.EndPanel();
    }
    // A scroll box of the EXE, a picture of a chosen size for it, and an icon
    // import. Which pixels must be cut is stated by each test; everything else
    // must reach the native call as it is, opaque.
    struct Scroll : Fixture
    {
        int width, height;
        std::vector<bool> cut;
        explicit Scroll(int w=12, int h=8) : width(w), height(h)
        {
            panel.cls=L"TScrollBox"; PanelTest::parents.erase(&panel);
            PanelTest::bitmaps[&source]=PanelTest::stockBitmap; ImageDeleteObject(original);
            original=ImageTest::MakeBitmap(w,h); PanelTest::bitmaps[&source]=original;
            bytes=original->pixels; cut.assign(static_cast<size_t>(w)*h,false);
            exe.originals["DrawIconEx"]=reinterpret_cast<void*>(ChainedIcon);
            ImageTest::dirtyDib=true; // a new buffer holds whatever was there before
        }
        void Put(int x, int y, BYTE r, BYTE g, BYTE b)
        {
            assert(x>=0 && y>=0 && x<width && y<height);
            const size_t at=(static_cast<size_t>(y)*width+x)*4;
            for (auto* pixels : {&original->pixels, &bytes})
            { (*pixels)[at]=b; (*pixels)[at+1]=g; (*pixels)[at+2]=r; }
            cut[at/4]=false;
        }
        // [x0,x1) x [y0,y1)
        void Box(int x0, int y0, int x1, int y1, BYTE r, BYTE g, BYTE b)
        { for (int y=y0;y<y1;++y) for (int x=x0;x<x1;++x) Put(x,y,r,g,b); }
        // The same area in the window's colour (the button face of this fixture).
        void Face(int x0, int y0, int x1, int y1, bool expectCut)
        {
            Box(x0,y0,x1,y1,240,240,240);
            for (int y=y0;y<y1;++y) for (int x=x0;x<x1;++x) cut[static_cast<size_t>(y)*width+x]=expectCut;
        }
        void Canvas(COLORREF colour) { canvas=source.background=colour; }
        void OnWindowColour() { Canvas(RGB(240,240,240)); }
        BOOL Copy(DWORD rop=SRCCOPY) { return Stretch(nullptr,nullptr,rop,width,height,width,height); }
        void Drawn()
        {
            const auto& p=ImageTest::last.pixels;
            CHECK(p.size()==bytes.size() && ImageTest::last.selected!=original);
            for (size_t i=0;i<cut.size();++i)
            {
                const BYTE* out=p.data()+i*4; const BYTE* in=bytes.data()+i*4;
                const bool ok=cut[i] ? !out[0] && !out[1] && !out[2] && !out[3]
                    : out[0]==in[0] && out[1]==in[1] && out[2]==in[2] && out[3]==255;
                if (!ok)
                {
                    std::cerr<<"pixel "<<i%width<<","<<i/width<<" expected "<<(cut[i]?"cut":"opaque")
                        <<" out="<<int(out[2])<<","<<int(out[1])<<","<<int(out[0])<<","<<int(out[3])<<"\n";
                    CHECK(ok);
                }
            }
            CHECK(ImageTest::last.entryError==0x29 && GetLastError()==0x51);
            CHECK(ImageTest::readDc==&source && ImageTest::alphaBlends==0 && ImageTest::keys==0);
            Restored();
        }
        // An icon drawn by the EXE at (21,13) of the scroll box's DC.
        BOOL Icon(HICON icon, int cx=0, int cy=0, UINT flags=DI_NORMAL, HDC target=nullptr, HBRUSH flicker=nullptr)
        {
            SetLastError(0x29);
            return GlassCore::DelphiPanel::PanelIcon(target?target:&dc,21,13,icon,cx,cy,5,flicker,flags);
        }
        using Bgra=std::array<BYTE,4>;
        // What was blended into the DC: the picture, pixel by pixel as the test
        // states it, at the caller's place, whole, over what is there.
        void Blended(HICON icon, int w, int h, UINT flags, const std::vector<Bgra>& picture)
        {
            const auto& b=IconTest::blend;
            CHECK(ImageTest::alphaBlends==1 && b.dest==&dc && b.x==21 && b.y==13 && b.w==w && b.h==h);
            CHECK(b.sx==0 && b.sy==0 && b.sw==w && b.sh==h && b.width==w && b.height==h);
            CHECK(b.mode.BlendOp==AC_SRC_OVER && b.mode.BlendFlags==0);
            CHECK(b.mode.SourceConstantAlpha==255 && b.mode.AlphaFormat==AC_SRC_ALPHA);
            CHECK(picture.size()==static_cast<size_t>(w)*h && b.pixels.size()==picture.size()*4);
            for (size_t i=0;i<picture.size();++i)
            {
                const BYTE* out=b.pixels.data()+i*4;
                if (std::memcmp(out,picture[i].data(),4)!=0)
                {
                    std::cerr<<"icon pixel "<<i%w<<","<<i/w<<" out="<<int(out[0])<<","<<int(out[1])<<","
                        <<int(out[2])<<","<<int(out[3])<<"\n";
                    CHECK(false);
                }
            }
            // The original drew twice, into a DC of ours and never into the caller's.
            CHECK(IconTest::calls.size()==2 && chainIcons==2);
            for (const auto& c : IconTest::calls)
            {
                CHECK(c.dc!=&dc && c.dc==IconTest::calls[0].dc && c.x==0 && c.y==0 && c.cx==w && c.cy==h);
                CHECK(c.icon==icon && c.step==5 && c.flags==flags && !c.flicker);
            }
            CHECK(GetLastError()==0x29 && PanelTest::liveDcs==0 && ImageTest::flushes==1);
            Restored();
        }
        // One call of the EXE's own, with its own arguments, and nothing of ours.
        void IconNative(HICON icon, int cx, int cy, UINT flags, HDC target=nullptr, HBRUSH flicker=nullptr)
        {
            CHECK(IconTest::calls.size()==1 && ImageTest::alphaBlends==0);
            const auto& c=IconTest::calls.back();
            CHECK(c.dc==(target?target:&dc) && c.x==21 && c.y==13 && c.cx==cx && c.cy==cy && c.icon==icon);
            CHECK(c.step==5 && c.flags==flags && c.flicker==flicker && c.entryError==0x29);
            CHECK(GetLastError()==0x52 && PanelTest::liveDcs==0);
            Restored();
        }
    };
    TestIcon::Pixel Px(BYTE r, BYTE g, BYTE b, BYTE alpha, bool inverts=false)
    { TestIcon::Pixel p; p.r=r; p.g=g; p.b=b; p.alpha=alpha; p.inverts=inverts; return p; }
    TestIcon MakeIcon(int w, int h, std::vector<TestIcon::Pixel> pixels, bool alpha=true, bool colour=true)
    {
        TestIcon icon; icon.width=w; icon.height=h; icon.alpha=alpha; icon.colour=colour;
        icon.pixels=std::move(pixels); CHECK(icon.pixels.size()==static_cast<size_t>(w)*h);
        return icon;
    }

    void ScrollPicturesAndTheWindowColour()
    {
        {
            // A picture the EXE composed on the colour of its window: the margin
            // goes, the picture stays whole - a hole of the same colour included.
            Scroll f(16,12); f.OnWindowColour();
            f.Face(0,0,16,12,true); f.Box(3,2,13,10,200,30,30);
            f.Face(6,4,9,7,false); f.Face(13,5,14,7,true);
            f.Put(15,0,240,240,241);
            CHECK(f.BeginPanel()); CHECK(f.exe.refs["StretchBlt"]==1);
            CHECK(f.Copy());
            CHECK(ImageTest::nativeStretches==1 && chainStretches==1 && ImageTest::reads==1);
            const auto& c=ImageTest::last;
            CHECK(c.dest==&f.dc && c.source==&f.source && c.x==11 && c.y==12 && c.w==16 && c.h==12);
            CHECK(c.sx==2 && c.sy==1 && c.sw==16 && c.sh==12 && c.rop==SRCCOPY);
            f.Drawn();
            const auto* margin=ImageTest::last.pixels.data()+(1u*16+1)*4;
            CHECK(!margin[0] && !margin[1] && !margin[2] && !margin[3]);
            const auto* hole=ImageTest::last.pixels.data()+(5u*16+7)*4;
            CHECK(hole[0]==240 && hole[1]==240 && hole[2]==240 && hole[3]==255);
            f.EndPanel(); CHECK(f.exe.refs["StretchBlt"]==0);
        }
        {
            // Scale, mirroring and offsets are the caller's; the picture is cut all the same.
            Scroll f(10,6); f.OnWindowColour(); f.Face(0,0,10,6,true); f.Box(2,1,8,5,10,90,170);
            CHECK(f.BeginPanel());
            CHECK(f.Stretch(nullptr,nullptr,SRCCOPY,-30,24,10,-6));
            const auto& c=ImageTest::last;
            CHECK(c.w==-30 && c.h==24 && c.sw==10 && c.sh==-6 && c.rop==SRCCOPY);
            f.Drawn(); f.EndPanel();
        }
        // The canvas of the bitmap says what it was composed on. Anything but the
        // system's face - white where nobody set it - and the picture is whole.
        struct Case { COLORREF canvas; COLORREF face; COLORREF window; bool expectCut; };
        for (const Case c : {
            Case{RGB(255,255,255), RGB(240,240,240), RGB(255,255,255), false},
            Case{RGB(50,60,70),    RGB(240,240,240), RGB(255,255,255), false},
            Case{RGB(240,240,240), RGB(212,208,200), RGB(240,240,240), false},
            Case{0x02f0f0f0u,      RGB(240,240,240), RGB(255,255,255), true},
        })
        {
            Scroll f(10,6); f.Canvas(c.canvas);
            PanelTest::faceColor=c.face; PanelTest::windowColor=c.window;
            f.Face(0,0,10,6,c.expectCut); f.Box(2,1,8,5,10,90,170);
            CHECK(f.BeginPanel()); CHECK(f.Copy()); f.Drawn(); f.EndPanel();
            PanelTest::faceColor=0xf0f0f0; PanelTest::windowColor=0xffffff;
        }
        {
            // A picture too large to walk keeps its margin, and is opaque all the same.
            Scroll f(4097,1024); f.OnWindowColour();
            f.Face(0,0,4097,2,false); f.Box(0,2,4097,1024,10,90,170);
            CHECK(f.BeginPanel()); CHECK(f.Copy()); f.Drawn(); f.EndPanel();
        }
        {
            // A panel inside a panel keeps the policy it had: the picture as it is,
            // margin included, whatever its canvas says.
            Scroll f(10,6); f.OnWindowColour();
            f.panel.cls=L"TPanel"; PanelTest::parents[&f.panel]=&f.parent;
            f.Face(0,0,10,6,false); f.Box(2,1,8,5,10,90,170);
            CHECK(f.BeginPanel()); CHECK(f.exe.refs["StretchBlt"]==1 && f.exe.refs["DrawIconEx"]==0);
            CHECK(f.Copy()); f.Drawn(); f.EndPanel();
        }
    }
    void ScrollPictureScopeAndIsolation()
    {
        // A scroll box may hold graphic controls of any kind: only a plain copy is
        // a picture as it is. Masks and other ROPs are their callers' own.
        for (DWORD rop : {SRCAND,DWORD(0x00e20746),DWORD(0x00660046),DWORD(0x00ee0086)})
        {
            Scroll f; f.OnWindowColour(); f.Face(0,0,12,8,false);
            CHECK(f.BeginPanel()); CHECK(f.Copy(rop));
            CHECK(ImageTest::reads==0 && ImageTest::last.rop==rop); f.Native(); f.EndPanel();
        }
        for (int which=0;which<14;++which)
        {
            Scroll f; f.OnWindowColour(); f.Face(0,0,12,8,false); HDC dest=&f.dc; bool begun=true;
            switch (which)
            {
            case 0:f.panel.cls=L"TScrollBoxEx";begun=false;break;                // not a panel class at all
            case 1:f.panel.cls=L"TFrame";break;                                   // panels that hold no pictures of this kind
            case 2:f.panel.cls=L"TFlowPanel";break;
            case 3:PanelTest::instances[&f.panel]=&f.otherModule;break;           // another module's class of that name
            case 4:GlassCore::ModuleHandler::g_entryPointModule=nullptr;break;    // no EXE to compare its class with
            case 5:dest=&f.foreign;break;
            case 6:dest=&f.memory;break;
            case 8:PanelTest::maps[&f.dc]=2;break;
            case 10:f.original->info.bmWidth=200;f.original->info.bmHeight=60;break; // the size of its client: maybe its frame buffer
            case 11:f.original->info.bmBitsPixel=1;break;
            case 12:f.panel.backdrop=false;begun=false;break;
            case 13:Test::full=false;begun=false;break;
            }
            CHECK(f.BeginPanel()==begun);
            // What the dispatcher says once the paint has begun.
            if (which==6) Test::stackOwners.erase(&f.memory);                     // a memory DC nobody tracks
            if (which==7) Test::topWindow=&f.other;                               // a drawing session above ours
            if (which==9) Test::stackOwners.erase(&f.dc);
            if (which>=1 && which<=4) CHECK(f.exe.refs["StretchBlt"]==0 && f.exe.refs["DrawIconEx"]==0);
            CHECK(f.Stretch(dest,nullptr,SRCCOPY,12,8,12,8)); CHECK(ImageTest::reads==0);
            f.original->info.bmWidth=12; f.original->info.bmHeight=8; f.original->info.bmBitsPixel=32;
            f.Native();
            if (begun) f.EndPanel();
            GlassCore::ModuleHandler::g_entryPointModule=&f.exe; Test::full=true;
        }
        {
            // The class is compared whole and without case; a tracked memory DC of the window is its surface.
            Scroll f; f.panel.cls=L"tscrollBOX"; f.OnWindowColour(); f.Face(0,0,12,8,true); f.Put(5,4,1,2,3);
            CHECK(f.BeginPanel()); CHECK(f.exe.refs["StretchBlt"]==1 && f.exe.refs["DrawIconEx"]==1);
            CHECK(f.Stretch(&f.memory,nullptr,SRCCOPY,12,8,12,8)); f.Drawn(); f.EndPanel();
        }
        for (UINT msg : {WM_ERASEBKGND,WM_PRINTCLIENT})
        {
            // Only its paint: an erase and a print take neither import.
            Scroll f; f.OnWindowColour(); f.Face(0,0,12,8,false);
            CHECK(f.BeginPanel(msg)); CHECK(f.exe.refs["StretchBlt"]==0 && f.exe.refs["DrawIconEx"]==0);
            CHECK(f.Copy()); CHECK(ImageTest::reads==0); f.Native(); f.EndPanel();
        }
        for (int which=0;which<3;++which)
        {
            // Whatever fails while the copy is made, the caller's bitmap is drawn once
            // and stays the caller's: nothing of it is cut, though here it is itself a
            // 32bpp DIB whose fourth byte is 255, margin and all.
            Scroll f; f.OnWindowColour(); f.Face(0,0,12,8,false);
            for (size_t i=3;i<f.bytes.size();i+=4) f.original->pixels[i]=f.bytes[i]=255;
            CHECK(f.BeginPanel());
            switch (which)
            { case 0:ImageTest::failRead=true;break; case 1:ImageTest::nullBits=true;break; case 2:ImageTest::failSelectAt=2;break; }
            CHECK(f.Copy()); CHECK(ImageTest::nativeStretches==1 && chainStretches==1); f.Native(); f.EndPanel();
        }
    }

    void ScrollIconsWithTheirTransparency()
    {
        {
            // An icon with alpha of its own, as it is: opaque, half, nothing, a faint dark one.
            Scroll f;
            auto icon=MakeIcon(4,2,{Px(255,0,0,255), Px(255,255,255,128), Px(9,9,9,0), Px(10,20,30,64),
                                    Px(0,255,0,255), Px(0,0,255,1), Px(0,0,0,255), Px(255,255,255,255)});
            CHECK(f.BeginPanel()); CHECK(f.exe.refs["DrawIconEx"]==1);
            CHECK(f.Icon(&icon));
            f.Blended(&icon,4,2,DI_NORMAL,{{0,0,255,255}, {128,128,128,128}, {0,0,0,0}, {8,5,3,64},
                                           {0,255,0,255}, {1,0,0,1}, {0,0,0,255}, {255,255,255,255}});
            CHECK(IconTest::infos==1 && IconTest::metrics==0);
            f.EndPanel(); CHECK(f.exe.refs["DrawIconEx"]==0);
        }
        {
            // An icon made of a mask and colours: GDI leaves no alpha under it. What
            // the mask covers is opaque - black too - and what it cuts is not there.
            Scroll f;
            auto icon=MakeIcon(3,1,{Px(255,128,0,255), Px(77,77,77,0), Px(0,0,0,255)},false);
            CHECK(f.BeginPanel()); CHECK(f.Icon(&icon));
            f.Blended(&icon,3,1,DI_NORMAL,{{0,128,255,255}, {0,0,0,0}, {0,0,0,255}});
            f.EndPanel();
        }
        {
            // A cursor: pixels that invert what is under them are drawn as they
            // look over black; a faint inversion is as faint as its strongest channel.
            Scroll f;
            auto icon=MakeIcon(6,1,{Px(255,255,255,0,true), Px(200,40,40,0,true), Px(40,200,40,0,true),
                                    Px(40,40,200,0,true), Px(40,90,10,0,true), Px(10,40,90,0,true)},false);
            CHECK(f.BeginPanel()); CHECK(f.Icon(&icon));
            f.Blended(&icon,6,1,DI_NORMAL,{{255,255,255,255}, {40,40,200,255}, {40,200,40,255},
                                           {200,40,40,255}, {10,90,40,180}, {90,40,10,180}});
            f.EndPanel();
        }
        {
            // A monochrome cursor has no colour bitmap: its two masks are one bitmap, twice as high.
            Scroll f;
            auto icon=MakeIcon(2,2,{Px(255,255,255,255), Px(0,0,0,0), Px(0,0,0,255), Px(255,255,255,0,true)},false,false);
            CHECK(f.BeginPanel()); CHECK(f.Icon(&icon));
            f.Blended(&icon,2,2,DI_NORMAL,{{255,255,255,255}, {0,0,0,0}, {0,0,0,255}, {255,255,255,255}});
            CHECK(IconTest::infos==1);
            f.EndPanel();
        }
        {
            // The size the caller asked for: the icon is stretched by the original, and no one asks its size.
            Scroll f;
            auto icon=MakeIcon(2,1,{Px(255,0,0,255), Px(0,0,255,255)});
            CHECK(f.BeginPanel()); CHECK(f.Icon(&icon,4,2,DI_NORMAL|DI_COMPAT));
            f.Blended(&icon,4,2,DI_NORMAL|DI_COMPAT,{{0,0,255,255}, {0,0,255,255}, {255,0,0,255}, {255,0,0,255},
                                                     {0,0,255,255}, {0,0,255,255}, {255,0,0,255}, {255,0,0,255}});
            CHECK(IconTest::infos==0 && IconTest::metrics==0);
            f.EndPanel();
        }
        {
            // One side given, the other the icon's own.
            Scroll f;
            auto icon=MakeIcon(1,2,{Px(1,2,3,255), Px(4,5,6,255)});
            CHECK(f.BeginPanel()); CHECK(f.Icon(&icon,3,0));
            f.Blended(&icon,3,2,DI_NORMAL,{{3,2,1,255}, {3,2,1,255}, {3,2,1,255}, {6,5,4,255}, {6,5,4,255}, {6,5,4,255}});
            CHECK(IconTest::infos==1);
            f.EndPanel();
        }
        {
            Scroll f;
            auto icon=MakeIcon(2,1,{Px(1,2,3,255), Px(4,5,6,255)});
            CHECK(f.BeginPanel()); CHECK(f.Icon(&icon,0,3));
            f.Blended(&icon,2,3,DI_NORMAL,{{3,2,1,255}, {6,5,4,255}, {3,2,1,255}, {6,5,4,255}, {3,2,1,255}, {6,5,4,255}});
            f.EndPanel();
        }
        {
            // The default size is the system's: 32 by 40 in this fixture, and only the sides not given.
            Scroll f;
            auto icon=MakeIcon(1,1,{Px(9,8,7,255)});
            CHECK(f.BeginPanel()); CHECK(f.Icon(&icon,0,0,DI_NORMAL|DI_DEFAULTSIZE));
            f.Blended(&icon,32,40,DI_NORMAL|DI_DEFAULTSIZE,std::vector<Scroll::Bgra>(32u*40u,Scroll::Bgra{7,8,9,255}));
            CHECK(IconTest::infos==0 && IconTest::metrics==2);
            f.EndPanel();
        }
        {
            Scroll f;
            auto icon=MakeIcon(1,1,{Px(9,8,7,255)});
            CHECK(f.BeginPanel()); CHECK(f.Icon(&icon,0,5,DI_NORMAL|DI_DEFAULTSIZE));
            f.Blended(&icon,32,5,DI_NORMAL|DI_DEFAULTSIZE,std::vector<Scroll::Bgra>(32u*5u,Scroll::Bgra{7,8,9,255}));
            CHECK(IconTest::metrics==1);
            f.EndPanel();
        }
        {
            Scroll f;
            auto icon=MakeIcon(1,1,{Px(9,8,7,255)});
            CHECK(f.BeginPanel()); CHECK(f.Icon(&icon,6,0,DI_NORMAL|DI_DEFAULTSIZE));
            f.Blended(&icon,6,40,DI_NORMAL|DI_DEFAULTSIZE,std::vector<Scroll::Bgra>(6u*40u,Scroll::Bgra{7,8,9,255}));
            CHECK(IconTest::metrics==1);
            f.EndPanel();
        }
        {
            // The largest side that is drawn here, and both sides at once.
            Scroll f;
            auto icon=MakeIcon(1,1,{Px(0,0,0,0)});
            CHECK(f.BeginPanel()); CHECK(f.Icon(&icon,1024,1024));
            f.Blended(&icon,1024,1024,DI_NORMAL,std::vector<Scroll::Bgra>(1024u*1024u,Scroll::Bgra{0,0,0,0}));
            f.EndPanel();
        }
        {
            // A tracked memory DC of the scroll box is its surface too.
            Scroll f;
            auto icon=MakeIcon(1,1,{Px(9,8,7,255)});
            CHECK(f.BeginPanel());
            SetLastError(0x29);
            CHECK(GlassCore::DelphiPanel::PanelIcon(&f.memory,21,13,&icon,0,0,5,nullptr,DI_NORMAL));
            CHECK(ImageTest::alphaBlends==1 && IconTest::blend.dest==&f.memory && IconTest::calls.size()==2);
            f.EndPanel();
        }
    }
    void ScrollIconScopeAndIsolation()
    {
        LOGBRUSH under{BS_SOLID,RGB(1,2,3)};
        for (int which=0;which<26;++which)
        {
            Scroll f; auto icon=MakeIcon(2,2,{Px(255,0,0,255), Px(0,0,0,0), Px(0,0,0,0), Px(0,0,255,255)});
            HICON drawn=&icon; HDC dest=nullptr; HBRUSH flicker=nullptr; int cx=0, cy=0; UINT flags=DI_NORMAL;
            bool begun=true, chained=true;
            switch (which)
            {
            case 0:flicker=&under;break;                                          // the caller's own fill under the icon
            case 1:flags=DI_IMAGE;break;                                          // passes the caller combines itself
            case 2:flags=DI_MASK;break;
            case 3:flags=DI_COMPAT;break;
            case 4:drawn=nullptr;break;
            case 5:dest=&f.foreign;break;                                         // another window's DC
            case 6:dest=&f.memory;break;
            case 9:PanelTest::maps[&f.dc]=2;break;                                // not device units
            case 10:IconTest::layouts[&f.dc]=1;break;                             // a mirrored DC
            case 11:cx=-2;break;
            case 12:cy=-2;break;
            case 13:cx=1025;cy=4;break;
            case 14:cx=4;cy=1025;break;
            case 15:IconTest::failInfo=true;break;                                // the icon would not tell its size
            case 16:ImageTest::failMetadata=true;break;                           // nor would its bitmap
            case 17:f.panel.cls=L"TPanel";PanelTest::parents[&f.panel]=&f.parent;chained=false;break; // not a scroll box
            case 18:f.panel.cls=L"TFrame";chained=false;break;
            case 19:PanelTest::instances[&f.panel]=&f.otherModule;chained=false;break;
            case 20:f.exe.originals.erase("DrawIconEx");chained=false;break;      // the EXE imports no such function
            case 21:f.panel.backdrop=false;begun=false;chained=false;break;
            case 22:Test::full=false;begun=false;chained=false;break;
            case 23:icon.width=0;break;                                           // an icon of no size
            case 24:icon.height=0;break;
            case 25:dest=&f.foreign;break;
            }
            CHECK(f.BeginPanel()==begun);
            // What the dispatcher says once the paint has begun.
            if (which==6) Test::stackOwners.erase(&f.memory);                     // a memory DC nobody tracks
            if (which==7) Test::topWindow=&f.other;                               // a drawing session above ours
            if (which==8) Test::stackOwners.erase(&f.dc);
            if (which==25) Test::stackOwners[&f.foreign]=&f.panel;                // tracked for us, owned by another window
            const bool leased=begun && which!=17 && which!=18 && which!=19 && which!=20;
            CHECK(f.exe.refs["DrawIconEx"]==(leased?1:0));
            SetLastError(0x29);
            CHECK(GlassCore::DelphiPanel::PanelIcon(dest?dest:&f.dc,21,13,drawn,cx,cy,5,flicker,flags));
            ImageTest::failMetadata=false;
            CHECK(chainIcons==(chained?1:0));
            f.IconNative(drawn,cx,cy,flags,dest,flicker);
            CHECK(ImageTest::creates==ImageTest::deletes+4); // whatever was asked of the icon was given back
            if (begun) f.EndPanel();
            Test::full=true;
        }
        for (UINT msg : {WM_ERASEBKGND,WM_PRINTCLIENT})
        {
            Scroll f; auto icon=MakeIcon(1,1,{Px(9,8,7,255)});
            CHECK(f.BeginPanel(msg)); CHECK(f.exe.refs["DrawIconEx"]==0);
            CHECK(f.Icon(&icon)); CHECK(chainIcons==0); f.IconNative(&icon,0,0,DI_NORMAL); f.EndPanel();
        }
        {
            // Sessions are per thread; outside one, and after Shutdown, the import is not ours.
            Scroll f; auto icon=MakeIcon(1,1,{Px(9,8,7,255)});
            CHECK(f.BeginPanel());
            std::thread worker([&] { CHECK(f.Icon(&icon)); CHECK(ImageTest::alphaBlends==0 && IconTest::calls.size()==1); });
            worker.join();
            IconTest::calls.clear(); chainIcons=0;
            GlassCore::DelphiPanel::g_shutdown.store(true);
            CHECK(f.Icon(&icon)); f.IconNative(&icon,0,0,DI_NORMAL);
            GlassCore::DelphiPanel::g_shutdown.store(false);
            f.EndPanel();
            IconTest::calls.clear(); chainIcons=0;
            CHECK(f.Icon(&icon)); CHECK(chainIcons==0); f.IconNative(&icon,0,0,DI_NORMAL);
        }
    }
    void ScrollIconLeasesAndFailures()
    {
        {
            // Nested paints count their references; an erase inside takes none and draws none.
            Scroll f; auto icon=MakeIcon(1,1,{Px(9,8,7,255)});
            CHECK(f.BeginPanel()); CHECK(f.BeginPanel()); CHECK(f.exe.refs["DrawIconEx"]==2);
            f.EndPanel(); CHECK(f.exe.refs["DrawIconEx"]==1);
            CHECK(GlassCore::DelphiPanel::BeginPaint(&f.panel,WM_ERASEBKGND,&f.dc));
            CHECK(f.Icon(&icon)); CHECK(chainIcons==1); f.IconNative(&icon,0,0,DI_NORMAL);
            f.EndPanel(); CHECK(f.exe.refs["DrawIconEx"]==1);
            IconTest::calls.clear(); chainIcons=0;
            CHECK(f.Icon(&icon)); CHECK(ImageTest::alphaBlends==1);
            f.EndPanel(); CHECK(f.exe.refs["DrawIconEx"]==0);
        }
        {
            // The slot is someone else's and cannot be taken: nothing is leased, nothing is given back.
            Scroll f; auto icon=MakeIcon(1,1,{Px(9,8,7,255)});
            f.exe.hooks["DrawIconEx"]=reinterpret_cast<void*>(std::uintptr_t(23));
            ImageTest::registrationFailures[&f.exe].insert("DrawIconEx");
            CHECK(f.BeginPanel()); CHECK(!GlassCore::DelphiPanel::g_stack.back().iconAcquired);
            CHECK(f.exe.refs["DrawIconEx"]==0 && f.exe.refs["StretchBlt"]==1);
            CHECK(f.Icon(&icon)); CHECK(chainIcons==0); f.IconNative(&icon,0,0,DI_NORMAL);
            f.EndPanel();
            CHECK(f.exe.hooks.at("DrawIconEx")==reinterpret_cast<void*>(std::uintptr_t(23))); f.exe.hooks.clear();
        }
        {
            // An EXE that imports nothing else of a panel, in a process whose user32
            // gives no DrawTextA to take: the icons alone are worth the session.
            Scroll f; auto icon=MakeIcon(1,1,{Px(9,8,7,255)});
            GlassCore::DelphiPanel::Shutdown(); PanelTest::textExportAvailable=false; GlassCore::DelphiPanel::Startup();
            PanelTest::textExportAvailable=true;
            f.exe.originals={{"DrawIconEx",reinterpret_cast<void*>(ChainedIcon)}};
            CHECK(f.BeginPanel()); CHECK(f.exe.refs["DrawIconEx"]==1 && GlassCore::DelphiPanel::g_stack.size()==1);
            CHECK(!GlassCore::DelphiPanel::g_stack.back().ansiTextAcquired);
            CHECK(f.Icon(&icon)); CHECK(ImageTest::alphaBlends==1);
            f.EndPanel(); CHECK(f.exe.refs["DrawIconEx"]==0 && GlassCore::DelphiPanel::g_stack.empty());
        }
        {
            // Shutdown gives the import back; the session that is still open ends without it.
            Scroll f;
            CHECK(f.BeginPanel()); GlassCore::DelphiPanel::Shutdown();
            CHECK(f.exe.refs["DrawIconEx"]==0);
            f.EndPanel(); CHECK(f.exe.refs["DrawIconEx"]==0 && !f.BeginPanel());
        }
        // Whatever fails before the blend, the icon is drawn once by the caller's
        // own call, and every buffer is released.
        for (int which=0;which<16;++which)
        {
            Scroll f; auto icon=MakeIcon(2,1,{Px(255,0,0,255), Px(0,0,255,128)});
            CHECK(f.BeginPanel());
            int before=2; // calls of the original before the caller's own
            switch (which)
            {
            case 0:PanelTest::failDc=true;before=0;break;
            case 1:ImageTest::failDib=true;before=0;break;
            case 2:ImageTest::nullBits=true;before=0;break;
            case 11:ImageTest::failDibAt=2;before=0;break;                        // the second buffer is not there
            case 12:ImageTest::nullBitsAt=2;before=0;break;                       // or has no bits
            case 3:ImageTest::failSelectAt=1;before=0;break;                      // the first buffer cannot be selected
            case 4:ImageTest::failSelectAt=2;before=1;break;                      // nor the second
            case 5:ImageTest::failSelectAt=3;break;                               // nor the picture, for the blend
            case 6:IconTest::failDrawAt=1;before=1;break;                         // the original refuses over black
            case 7:IconTest::failDrawAt=2;break;                                  // or over white
            case 8:ImageTest::failFlush=true;break;
            case 9:IconTest::failBlend=true;break;
            case 10:ImageTest::failSelectAt=1;ImageTest::selectReturnsNull=true;before=0;break;
            case 13:ImageTest::failSelectAt=2;ImageTest::selectReturnsNull=true;before=1;break;
            case 14:ImageTest::failSelectAt=3;ImageTest::selectReturnsNull=true;break;
            case 15:ImageTest::nullBitsAt=1;before=0;break;                       // the first buffer has no bits
            }
            CHECK(f.Icon(&icon));
            PanelTest::failDc=false;
            CHECK(static_cast<int>(IconTest::calls.size())==before+1 && chainIcons==before+1);
            const auto& own=IconTest::calls.back();
            CHECK(own.dc==&f.dc && own.x==21 && own.y==13 && own.cx==0 && own.cy==0 && own.icon==&icon);
            CHECK(own.step==5 && own.flags==DI_NORMAL && !own.flicker && own.entryError==0x29);
            CHECK(ImageTest::alphaBlends==(which==9?1:0) && GetLastError()==0x52 && PanelTest::liveDcs==0);
            f.Restored();
            f.EndPanel();
        }
        {
            // The caller's own call fails: FALSE and its error, once.
            Scroll f; auto icon=MakeIcon(1,1,{Px(9,8,7,255)});
            CHECK(f.BeginPanel());
            IconTest::failDrawAt=1; ImageTest::failDib=true;
            CHECK(!f.Icon(&icon)); CHECK(IconTest::calls.size()==1 && GetLastError()==0x52);
            f.EndPanel();
        }
        {
            // While the original draws into our buffers, whatever reaches the import
            // again is not ours: it is drawn by its caller's own call, into its DC.
            Scroll f; auto icon=MakeIcon(1,1,{Px(9,8,7,255)}); auto inner=MakeIcon(1,1,{Px(1,2,3,255)});
            CHECK(f.BeginPanel());
            IconTest::duringDraw=[&] {
                CHECK(GlassCore::DelphiPanel::g_inHook);
                SetLastError(0x29);
                CHECK(GlassCore::DelphiPanel::PanelIcon(&f.dc,1,2,&inner,0,0,5,nullptr,DI_NORMAL));
                CHECK(ImageTest::alphaBlends==0 && IconTest::calls.size()==2);
            };
            CHECK(f.Icon(&icon));
            CHECK(ImageTest::alphaBlends==1 && IconTest::calls.size()==3 && chainIcons==3);
            CHECK(IconTest::calls[1].dc==&f.dc && IconTest::calls[1].icon==&inner && IconTest::calls[1].x==1);
            CHECK(IconTest::blend.dest==&f.dc && IconTest::blend.x==21 && !GlassCore::DelphiPanel::g_inHook);
            f.EndPanel();
        }
        {
            // An icon drawn from inside the original is not ours to draw again.
            Scroll f; auto icon=MakeIcon(1,1,{Px(9,8,7,255)});
            CHECK(f.BeginPanel());
            CHECK(!GlassCore::DelphiPanel::g_inHook);
            GlassCore::DelphiPanel::g_inHook=true;
            CHECK(f.Icon(&icon));
            GlassCore::DelphiPanel::g_inHook=false;
            CHECK(chainIcons==1); f.IconNative(&icon,0,0,DI_NORMAL);
            f.EndPanel();
        }
    }
    void ToolbarWindowReadout()
    {
        // What tells a toolbar whose captions the system draws with alpha from a
        // VCL one is read for the Debug line only: once per paint, never in Release.
        TestTheme theme;
        for (bool themed : {false,true})
        {
            Fixture f; f.toolbar.theme=themed?&theme:nullptr; ImageTest::styles[&f.toolbar]=0x5600884e;
            f.BeginToolbar(&f.exe);
#if defined(_DEBUG)
            CHECK(ImageTest::styleQueries==1 && ImageTest::themeQueries==(themed?1:0));
            if (themed)
            {
                CHECK(ImageTest::themeAsked==&theme && ImageTest::partAsked==1 && ImageTest::stateAsked==0);
                CHECK(ImageTest::propertyAsked==2204);
            }
#else
            CHECK(ImageTest::styleQueries==0 && ImageTest::themeQueries==0);
#endif
            // What it says changes nothing: the caption is taken as before.
            CHECK(f.modern.refs["DrawThemeText"]==1 && GlassCore::ToolbarControl::g_modStack.back().textModule==&f.modern);
            f.EndToolbar();
            ImageTest::composited=TRUE; f.BeginToolbar(&f.exe);
            CHECK(f.modern.refs["DrawThemeText"]==1); f.EndToolbar();
        }
    }
    void NativeExtentsAndSelfCopies()
    {
        for (bool panel : {false,true}) for (int which=0;which<6;++which)
        {
            Fixture f; if(panel) CHECK(f.BeginPanel()); else f.BeginToolbar();
            if(which<4)
            {
                if(panel) CHECK(f.Stretch(nullptr,nullptr,SRCCOPY,which==0?0:20,which==1?0:12,which==2?0:8,which==3?0:2));
                else CHECK(f.Mask(nullptr,nullptr,SRCCOPY,true,which%2?8:0,which%2?0:2));
            }
            else if(which==4)
            { if(panel) CHECK(f.Stretch(&f.source,&f.source)); else CHECK(f.Mask(&f.source,&f.source)); }
            else
            {
                PanelTest::maps[&f.source]=2;
                if(panel) CHECK(f.Stretch());
                else { f.original->info.bmBitsPixel=4; CHECK(f.Mask()); }
                PanelTest::maps[&f.source]=MM_TEXT;
            }
            CHECK(ImageTest::reads==0); f.Native(); if(panel)f.EndPanel();else f.EndToolbar();
        }
    }
}
int main(int argc,char** argv)
{
    try
    {
        CHECK(argc==2); const std::string name=argv[1];
        if(name=="utility-types")UtilityTypesAndBytes();
        else if(name=="utility-failures")UtilityRejectsAndFailures();
        else if(name=="toolbar-routes")ToolbarExeAndComctlRoutes();
        else if(name=="toolbar-native-semantics")ToolbarMasksRopsAndModernPath();
        else if(name=="toolbar-isolation")ToolbarDcAndSessionIsolation();
        else if(name=="toolbar-leases")ToolbarLeasesAndMissingImports();
        else if(name=="toolbar-barriers")ToolbarUnreadyNativeBarriers();
        else if(name=="toolbar-failures")ToolbarNativeFailureAndReentry();
        else if(name=="toolbar-captions")ToolbarCaptionsOnTheExeRoute();
        else if(name=="panel-images")PanelNativeImageAndMirroring();
        else if(name=="panel-scope")PanelExactClassAndMessageScope();
        else if(name=="panel-isolation")PanelDcIsolationAndClientBuffers();
        else if(name=="panel-leases")PanelLeasesNativeBarriersAndShutdown();
        else if(name=="panel-failures")PanelFailureAndReentry();
        else if(name=="scroll-pictures")ScrollPicturesAndTheWindowColour();
        else if(name=="scroll-picture-scope")ScrollPictureScopeAndIsolation();
        else if(name=="scroll-icons")ScrollIconsWithTheirTransparency();
        else if(name=="scroll-icon-scope")ScrollIconScopeAndIsolation();
        else if(name=="scroll-icon-failures")ScrollIconLeasesAndFailures();
        else if(name=="toolbar-readout")ToolbarWindowReadout();
        else if(name=="native-extents")NativeExtentsAndSelfCopies();
        else return 2;
        std::cout<<"PASS "<<name<<'\n'; return 0;
    }
    catch(const std::exception& e) { std::cerr<<(argc>1?argv[1]:"unknown")<<": "<<e.what()<<'\n'; return 1; }
}
