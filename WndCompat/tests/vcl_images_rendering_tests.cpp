#include "vcl_images_test_shim.h"
#include "../../GlassShared/src/BitmapUtils.cpp"
#include "../../GlassCore/src/Subsystems/OpaqueBitmapSource.cpp"
#include "../../GlassCore/src/Hooks/ToolbarHooks.cpp"
#include "../../GlassCore/src/Hooks/delphi/TPanelDraw.cpp"
#include <thread>

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)

namespace
{
    inline int chainMasks{}, chainStretches{};
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
        Fixture()
        {
            using namespace ImageTest;
            CHECK(bitmaps.empty());
            creates=deletes=reads=flushes=selects=0;
            nativeMasks=nativeStretches=nativeBits=alphaBlends=keys=queries=0;
            failPlaceholder=failDib=nullBits=failRead=partialRead=failFlush=failMetadata=failNative=false;
            failSelectAt=-1; selectReturnsNull=glow=reentered=false;
            duringNative={}; calls.clear(); removal={}; registrationFailures.clear(); chainMasks=chainStretches=0;
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
            CHECK(source.background==RGB(50,60,70) && source.bkMode==OPAQUE && GetMapMode(&source)==MM_TEXT);
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
            case 4:f.panel.cls=L"TFlowPanel";break;case 5:f.panel.cls=L"TScrollBox";break;
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
        else if(name=="native-extents")NativeExtentsAndSelfCopies();
        else return 2;
        std::cout<<"PASS "<<name<<'\n'; return 0;
    }
    catch(const std::exception& e) { std::cerr<<(argc>1?argv[1]:"unknown")<<": "<<e.what()<<'\n'; return 1; }
}
