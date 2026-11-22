#include "restorator_images_test_shim.h"
#include "../../GlassShared/src/BitmapUtils.cpp"
#include "../../GlassCore/src/Subsystems/OpaqueBitmapSource.cpp"
#define Detours RestoratorDetours
#include "../../GlassCore/src/Hooks/delphi/RestoratorImages.cpp"
#undef Detours
#include <cstdlib>
#include <new>
#include <thread>

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)

// The shim arms this countdown only while the production pixel policy runs.
// noinline: the replaced pair must stay opaque to GCC's new/delete matching.
[[gnu::noinline]] void* operator new(std::size_t size)
{
    if (RestoratorTest::allocationCountdown >= 0 && RestoratorTest::allocationCountdown-- == 0)
    {
        RestoratorTest::allocationCountdown = -1; RestoratorTest::allocationFailed = true;
        throw std::bad_alloc{};
    }
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc{};
}
[[gnu::noinline]] void operator delete(void* p) noexcept { std::free(p); }
[[gnu::noinline]] void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace
{
    namespace Ri = GlassCore::RestoratorImages;
    constexpr DWORD SRCPAINT = 0x00ee0086, SRCINVERT = 0x00660046;
    const auto kExportBit = reinterpret_cast<decltype(Ri::g_actualBitBlt)>(&RestoratorExportBitBlt);
    const auto kExportStretch = reinterpret_cast<decltype(Ri::g_actualStretchBlt)>(&RestoratorExportStretchBlt);
    const auto kTrampolineBit = reinterpret_cast<decltype(Ri::g_actualBitBlt)>(&RestoratorTrampolineBitBlt);
    const auto kTrampolineStretch = reinterpret_cast<decltype(Ri::g_actualStretchBlt)>(&RestoratorTrampolineStretchBlt);

    void ResetHooks()
    {
        using namespace RestoratorTest;
        failBegin = failCommit = false; failAttachAt = failDetachAt = 0;
        armAllocations = allocationCountdown = -1; copyFault = 0;
    }
    bool Attached() { return RestoratorTest::bitDetour && RestoratorTest::stretchDetour; }
    bool Detached()
    {
        return !RestoratorTest::bitDetour && !RestoratorTest::stretchDetour
            && Ri::g_actualBitBlt == kExportBit && Ri::g_actualStretchBlt == kExportStretch && !Ri::g_attached;
    }

    // What a painted pixel must become. Gone: transparent black. Stroke: its own
    // colour, as opaque as it is bright. Block: its own colour, A=255.
    enum Kind { Gone, Stroke, Block };

    struct Fixture
    {
        TestModule exe, comctl, otherModule;
        TestWindow tree, viewer, other;
        TestDC treeDc, treeCanvasDc, viewerDc, source, memory, foreign;
        TestFont font{23};
        LOGBRUSH brush{BS_SOLID, RGB(9,8,7)};
        HBITMAP picture{}, surface{}, memoryBitmap{};
        std::vector<BYTE> bytes; // the caller's bitmap, as it must remain
        std::vector<BYTE> alpha; // expected A per pixel, stated by each test, never derived from the algorithm
        int width, height;

        explicit Fixture(int w = 64, int h = 40, int bpp = 24, bool dib = true) : width(w), height(h)
        {
            using namespace ImageTest;
            CHECK(bitmaps.empty());
            creates=deletes=reads=flushes=selects=0;
            nativeMasks=nativeStretches=nativeBits=alphaBlends=keys=queries=0;
            failPlaceholder=failDib=nullBits=failRead=partialRead=failFlush=failMetadata=failNative=false;
            failSelectAt=-1; selectReturnsNull=glow=reentered=false;
            duringNative={}; calls.clear(); last={}; removal={}; registrationFailures.clear(); modulePaths.clear();
            Test::full=true; Test::stackOwners.clear(); Test::topWindow=Test::host=nullptr; Test::ux=nullptr;
            PanelTest::parents.clear(); PanelTest::instances.clear(); PanelTest::maps.clear();
            PanelTest::bitmaps.clear(); PanelTest::clients.clear();
            GlassCore::ModuleHandler::g_entryPointModule=&exe;
            GlassCore::ModuleHandler::g_comctl32New=&comctl;
            GlassCore::ModuleHandler::g_comctl32Legacy=nullptr;
            GlassCore::PaintDispatcher::imageProviders.clear();
            ResetHooks();
            RestoratorTest::gdiAvailable=RestoratorTest::bitExport=RestoratorTest::stretchExport=true;
            RestoratorTest::resolutions=RestoratorTest::exportCalls=RestoratorTest::trampolineCalls=0;
            RestoratorTest::writes=RestoratorTest::attachCalls=RestoratorTest::detachCalls=0;
            RestoratorTest::attaches=RestoratorTest::detaches=RestoratorTest::countQueries=0;
            RestoratorTest::pending.clear(); RestoratorTest::items.clear();
            static ULONGLONG epoch=0;
            RestoratorTest::clock=epoch+=100000; // a fresh rate-limit window for every fixture
            RestoratorTest::logFormats.clear(); RestoratorTest::logStrings.clear();
            RestoratorTest::dibQueries=0; RestoratorTest::allocationFailed=false;
            CHECK(!RestoratorTest::bitDetour && !RestoratorTest::stretchDetour);
            tree.cls=L"TResTreeView"; viewer.cls=L"TImgViewBox"; other.cls=L"TButton";
            treeDc.owner=&tree; treeCanvasDc.owner=&tree; viewerDc.owner=&viewer; foreign.owner=&other;
            picture=MakeBitmap(w,h,bpp,dib); surface=MakeBitmap(8,8); memoryBitmap=MakeBitmap(8,8);
            alpha.assign(static_cast<size_t>(w)*h, 0); // black: all background unless a test says otherwise
            bytes=picture->pixels;
            source.brush=&brush; source.font=&font; source.text=RGB(20,30,40);
            source.background=RGB(50,60,70); source.bkMode=OPAQUE;
            PanelTest::bitmaps[&treeDc]=surface; PanelTest::bitmaps[&treeCanvasDc]=surface;
            PanelTest::bitmaps[&viewerDc]=surface;
            PanelTest::bitmaps[&source]=picture; PanelTest::bitmaps[&memory]=memoryBitmap;
            PanelTest::bitmaps[&foreign]=PanelTest::stockBitmap;
            // What BeginPaint returned for each window; other DCs are tracked by the tests.
            Test::stackOwners[&treeDc]=&tree; Test::stackOwners[&viewerDc]=&viewer;
            caller=&exe;
            Ri::Startup();
            CHECK(GlassCore::PaintDispatcher::imageProviders.size()==1 && Detached());
        }
        ~Fixture()
        {
            ImageTest::duringNative={};
            ResetHooks();
            Ri::g_shutdown.store(false); // a test may have left the gate closed; Shutdown below must detach
            Ri::Shutdown();
            assert(Detached() && Ri::g_refs==0 && Ri::g_sessions.empty() && !Ri::g_inside);
            assert(RestoratorTest::attaches==RestoratorTest::detaches);
            PanelTest::bitmaps.clear();
            const auto owned=ImageTest::bitmaps;
            for (auto bmp : owned) ImageDeleteObject(bmp);
            assert(ImageTest::creates==ImageTest::deletes);
        }

        void PutAlpha(int x, int y, BYTE r, BYTE g, BYTE b, BYTE expected)
        {
            assert(x>=0 && y>=0 && x<width && y<height);
            auto* p=picture->pixels.data()+(static_cast<size_t>(y)*width+x)*4;
            p[0]=b; p[1]=g; p[2]=r;
            alpha[static_cast<size_t>(y)*width+x]=expected;
            bytes=picture->pixels;
        }
        void Put(int x, int y, BYTE r, BYTE g, BYTE b, Kind kind)
        { PutAlpha(x,y,r,g,b, kind==Gone?BYTE(0):kind==Block?BYTE(255):(std::max)({r,g,b})); }
        void Grey(int x, int y, BYTE level, Kind kind) { Put(x,y,level,level,level,kind); }
        // [x0,x1) x [y0,y1)
        void Box(int x0, int y0, int x1, int y1, BYTE r, BYTE g, BYTE b, Kind kind)
        { for (int y=y0;y<y1;++y) for (int x=x0;x<x1;++x) Put(x,y,r,g,b,kind); }
        void Alpha(int x, int y, BYTE a)
        { picture->pixels[(static_cast<size_t>(y)*width+x)*4+3]=a; bytes=picture->pixels; }

        auto& Provider() { CHECK(GlassCore::PaintDispatcher::imageProviders.size()==1); return GlassCore::PaintDispatcher::imageProviders.front(); }
        void Begin(TestWindow& w, HMODULE module=nullptr)
        { Test::topWindow=&w; Provider().onBegin(&w, module?module:&exe); }
        void End(TestWindow& w) { Provider().onEnd(&w, &exe); }
        BOOL Bit(HDC target=nullptr, HDC src=nullptr, DWORD rop=SRCCOPY, int w=-1, int h=-1)
        {
            SetLastError(0x29);
            return RestoratorExportBitBlt(target?target:&treeDc,11,12,w<0?width:w,h<0?height:h,src?src:&source,2,1,rop);
        }
        BOOL Stretch(HDC target=nullptr, DWORD rop=SRCCOPY, int w=40, int h=30, int sw=20, int sh=10)
        {
            SetLastError(0x29);
            return RestoratorExportStretchBlt(target?target:&viewerDc,11,12,w,h,&source,2,1,sw,sh,rop);
        }
        void Restored()
        {
            CHECK(PanelTest::bitmaps.at(&source)==picture && picture->pixels==bytes);
            CHECK(source.brush==&brush && source.font==&font && source.text==RGB(20,30,40));
            CHECK(source.background==RGB(50,60,70) && source.bkMode==OPAQUE && GetMapMode(&source)==MM_TEXT);
            CHECK(ImageTest::bitmaps.size()==3 && !Ri::g_inside);
            CHECK(ImageTest::alphaBlends==0 && ImageTest::keys==0 && ImageTest::nativeMasks==0);
        }
        // The native blit read our copy: every pixel is either transparent black
        // or the caller's RGB with the stated alpha (never above it: premultiplied).
        void Prepared()
        {
            const auto& p=ImageTest::last.pixels;
            // `selected` is only an identity here: the copy is gone by now.
            CHECK(ImageTest::last.selected && ImageTest::last.selected!=picture && p.size()==bytes.size());
            for (size_t i=0;i<alpha.size();++i)
            {
                const BYTE* out=p.data()+i*4; const BYTE* in=bytes.data()+i*4;
                const bool ok=alpha[i]
                    ? out[0]==in[0] && out[1]==in[1] && out[2]==in[2] && out[3]==alpha[i]
                        && out[0]<=out[3] && out[1]<=out[3] && out[2]<=out[3]
                    : !out[0] && !out[1] && !out[2] && !out[3];
                if (!ok)
                {
                    std::cerr<<"pixel "<<i%width<<","<<i/width<<" expected A="<<int(alpha[i])
                        <<" in="<<int(in[2])<<","<<int(in[1])<<","<<int(in[0])
                        <<" out="<<int(out[2])<<","<<int(out[1])<<","<<int(out[0])<<","<<int(out[3])<<"\n";
                    CHECK(ok);
                }
            }
            CHECK(ImageTest::last.entryError==0x29 && GetLastError()==0x51);
            CHECK(ImageTest::readDc==&source);
            Restored();
        }
        void Native()
        {
            CHECK(ImageTest::last.selected==picture && ImageTest::last.pixels==bytes);
            CHECK(ImageTest::last.entryError==0x29 && GetLastError()==0x51);
            Restored();
        }
    };

    // A hand-made miniature of the real placeholders (64x40): lossy text on black
    // above a banner that reaches both edges. Each element states its own result.
    void PaintPlaceholder(Fixture& f)
    {
        CHECK(f.width==64 && f.height==40);
        // Ringing: near-black speckle up to the background level, in colour too.
        for (int i=0;i<24;++i) f.Grey((i*11+1)%64, 12+(i%3), BYTE(1+i%16), Gone);
        f.Put(2,11,6,0,3,Gone); f.Put(60,1,0,16,9,Gone); f.Grey(0,0,16,Gone); f.Grey(63,39,16,Gone);
        // Dim specks and a dim blob that touch no content are noise as well.
        f.Grey(40,2,17,Gone); f.Grey(44,6,25,Gone); f.Grey(50,10,32,Gone);
        f.Box(54,3,57,6,30,30,30,Gone);
        // Letter "o": a one-pixel light-grey ring. The black inside a letter is background.
        f.Box(7,3,10,4,200,200,200,Stroke); f.Box(7,9,10,10,200,200,200,Stroke);
        f.Box(6,4,7,9,200,200,200,Stroke); f.Box(10,4,11,9,200,200,200,Stroke);
        f.Grey(8,6,9,Gone);
        // White stem with a lossy edge: bright, just-content, dim-but-chained, detached.
        f.Box(14,3,16,10,255,255,255,Stroke);
        f.Grey(16,4,90,Stroke); f.Grey(16,5,33,Stroke); f.Grey(16,6,32,Stroke); f.Grey(17,6,17,Stroke);
        f.Grey(19,6,17,Gone);
        // Two-pixel strokes meeting, and a 4x4 coloured dot: still text, not a block.
        f.Box(22,3,31,5,255,240,200,Stroke); f.Box(25,5,27,10,180,180,180,Stroke);
        f.Box(34,4,38,8,120,200,90,Stroke);
        // Glow above the banner: its faint part is background; the rest is one row
        // more of the block below it.
        for (int x=0;x<64;++x) f.Put(x,15,4,0,0,Gone);
        for (int x=0;x<20;++x) f.Put(x,16,12,0,0,Gone);
        for (int x=20;x<41;++x) f.Put(x,16,36,0,0,Block);
        for (int x=41;x<64;++x) f.Put(x,16,23,0,0,Block);
        // Banner rows 17..33: grey strip, dim teal block, darker seam, red body.
        f.Box(0,17,12,34,33,33,33,Block);
        f.Box(12,17,24,34,9,31,28,Block);
        f.Box(24,17,25,34,22,22,22,Block);
        f.Box(25,17,64,34,176,31,29,Block);
        // Caption: white strokes inside a dark outline, six banner rows above and
        // below. The outline is enclosed by a solid block, so it stays opaque.
        f.Box(30,23,51,28,4,4,4,Block);
        f.Box(32,24,35,27,255,255,255,Block); f.Box(38,24,40,27,255,255,255,Block);
        f.Box(44,25,49,26,255,255,255,Block);
        f.Put(41,25,0,0,0,Block);
        for (int x=0;x<64;++x) { f.Put(x,34,23,0,0,Block); f.Put(x,35,4,0,0,Gone); }
        for (int i=0;i<12;++i) f.Grey((i*5+2)%64, 36+(i%4), BYTE(2+i), Gone);
    }

    void PolicyPlaceholder()
    {
        for (bool viewer : {false,true})
        {
            Fixture f; PaintPlaceholder(f);
            f.Begin(viewer?f.viewer:f.tree);
            if (viewer) CHECK(f.Stretch()); else CHECK(f.Bit());
            f.Prepared();
            // Exact black inside the banner is opaque black, not a key colour;
            // the light-grey ring of the letter is as opaque as it is bright.
            const auto* p=ImageTest::last.pixels.data()+(25u*64+41)*4;
            CHECK(!p[0] && !p[1] && !p[2] && p[3]==255);
            const auto* ring=ImageTest::last.pixels.data()+(4u*64+6)*4;
            CHECK(ring[0]==200 && ring[1]==200 && ring[2]==200 && ring[3]==200);
            CHECK(ImageTest::reads==1 && ImageTest::creates==5 && ImageTest::deletes==2);
            f.End(viewer?f.viewer:f.tree);
        }
        // Idempotent on the caller's side: a second paint prepares a fresh copy
        // from the untouched bitmap and gives the same bytes.
        Fixture f; PaintPlaceholder(f); f.Begin(f.tree);
        CHECK(f.Bit()); f.Prepared(); const auto first=ImageTest::last.pixels;
        CHECK(f.Bit()); f.Prepared(); CHECK(ImageTest::last.pixels==first && ImageTest::reads==2);
        f.End(f.tree);
    }

    void PolicyLevels()
    {
        struct Probe { BYTE r, g, b; Kind kind; };
        // One pixel on black. The brightest channel decides: 16 is background,
        // 17..32 alone is noise, 33 and above is content with that much alpha.
        for (const Probe p : {Probe{0,0,0,Gone}, Probe{16,16,16,Gone}, Probe{17,17,17,Gone},
            Probe{32,32,32,Gone}, Probe{33,33,33,Stroke}, Probe{255,255,255,Stroke},
            Probe{33,0,0,Stroke}, Probe{0,33,0,Stroke}, Probe{0,0,33,Stroke},
            Probe{32,0,0,Gone}, Probe{0,32,0,Gone}, Probe{0,0,32,Gone}, Probe{16,32,7,Gone},
            Probe{200,100,50,Stroke}, Probe{1,2,254,Stroke}})
        {
            Fixture f(9,9); f.Put(4,4,p.r,p.g,p.b,p.kind);
            f.Begin(f.tree); CHECK(f.Bit()); f.Prepared(); f.End(f.tree);
        }
        {
            // Dim pixels are content only when chained to content, through any of 8 neighbours.
            Fixture f(13,13);
            f.Grey(5,5,33,Stroke);
            f.Grey(6,5,17,Stroke);  // beside
            f.Grey(4,4,32,Stroke);  // diagonal
            f.Grey(7,6,20,Stroke);  // chained through (6,5)
            f.Grey(8,7,17,Stroke);  // and further
            f.Grey(3,3,25,Stroke);  // chained backwards, before the content in scan order
            f.Grey(2,2,17,Stroke);
            f.Grey(10,9,17,Gone);   // one dark pixel short of the chain
            f.Grey(5,9,32,Gone); f.Grey(6,9,32,Gone); f.Grey(5,10,32,Gone); // dim blob, no content
            f.Grey(1,6,16,Gone);    // the background level never chains, even beside...
            f.Grey(4,5,16,Gone);    // ...content itself
            f.Begin(f.tree); CHECK(f.Bit()); f.Prepared(); f.End(f.tree);
        }
        {
            // A long dim line hanging on one content pixel at its far end.
            Fixture f(40,7);
            for (int x=3;x<36;++x) f.Grey(x,3,BYTE(17+x%16),Stroke);
            f.Grey(36,3,200,Stroke);
            f.Begin(f.tree); CHECK(f.Bit()); f.Prepared(); f.End(f.tree);
        }
    }

    // A frame [x0,x1)x[y0,y1) around a dark hole [hx0,hx1)x[hy0,hy1).
    void Frame(Fixture& f, BYTE level, Kind frame, int x0, int y0, int x1, int y1,
        int hx0, int hy0, int hx1, int hy1, Kind hole)
    {
        f.Box(x0,y0,x1,y1,level,level,level,frame);
        f.Box(hx0,hy0,hx1,hy1,0,0,0,hole);
        f.Grey(hx0+1,hy0+1,9,hole);   // ringing inside the hole
        f.Grey(hx1-2,hy1-2,20,hole);  // and a dim speck that touches nothing
    }
    void PolicySolid()
    {
        {
            // Walls five thick on every side: a solid block. It is opaque whatever
            // its brightness, and its dark inside is a detail of it.
            Fixture f(30,30); Frame(f,200,Block,5,5,25,25,10,10,20,20,Block);
            f.Begin(f.tree); CHECK(f.Bit()); f.Prepared(); f.End(f.tree);
        }
        {
            // Walls four thick: strokes, not a block; the inside is background.
            Fixture f(30,30); Frame(f,200,Stroke,5,5,25,25,9,9,21,21,Gone);
            f.Begin(f.tree); CHECK(f.Bit()); f.Prepared(); f.End(f.tree);
        }
        for (int side=0;side<4;++side)
        {
            // One wall of four is enough to let the background in. (White frame:
            // which of its pixels still count as block does not show.)
            Fixture f(30,30);
            Frame(f,255,Stroke,5,5,25,25, side==0?9:10, side==1?9:10, side==2?21:20, side==3?21:20, Gone);
            f.Begin(f.tree); CHECK(f.Bit()); f.Prepared(); f.End(f.tree);
        }
        {
            // A block lying on the picture's edge still encloses its hole.
            Fixture f(30,30); Frame(f,200,Block,0,8,15,23,5,13,10,18,Block);
            f.Begin(f.tree); CHECK(f.Bit()); f.Prepared(); f.End(f.tree);
        }
        {
            // Notches open to each side are background, each entered in one
            // direction only; the sealed pocket between them is not.
            Fixture f(50,50);
            f.Box(10,10,40,40,200,200,200,Block);
            f.Box(15,10,18,19,0,0,0,Gone);   // from above
            f.Box(32,31,35,40,0,0,0,Gone);   // from below
            f.Box(10,24,18,27,0,0,0,Gone);   // from the left
            f.Box(31,15,40,18,0,0,0,Gone);   // from the right
            f.Box(23,23,27,27,3,3,3,Block);  // five block pixels away from every notch
            f.Begin(f.tree); CHECK(f.Bit()); f.Prepared(); f.End(f.tree);
        }
        {
            // Blocks across the whole top and bottom: the background is reached from the sides.
            Fixture f(6,80);
            f.Box(0,0,6,5,200,200,200,Block); f.Box(0,75,6,80,200,200,200,Block);
            f.Box(2,30,4,40,150,150,150,Stroke); f.Grey(1,50,9,Gone);
            f.Begin(f.tree); CHECK(f.Bit()); f.Prepared(); f.End(f.tree);
        }
        {
            // Blocks down both sides: the background is reached from above and below.
            Fixture f(80,6);
            f.Box(0,0,5,6,200,200,200,Block); f.Box(75,0,80,6,200,200,200,Block);
            f.Box(30,2,40,4,150,150,150,Stroke); f.Grey(50,1,9,Gone);
            f.Begin(f.tree); CHECK(f.Bit()); f.Prepared(); f.End(f.tree);
        }
        {
            // Dim content counts for thickness once it is chained: a dark banner end
            // with five dim columns left of its pocket.
            Fixture f(30,30);
            f.Box(5,5,15,25,28,28,28,Block); f.Box(15,5,25,25,200,30,30,Block);
            f.Box(10,12,14,16,2,2,2,Block);
            f.Begin(f.tree); CHECK(f.Bit()); f.Prepared(); f.End(f.tree);
        }
        {
            // With three columns the pocket is open, and that piece of wall is a stroke.
            Fixture f(30,30);
            f.Box(5,5,15,25,28,28,28,Block); f.Box(15,5,25,25,200,30,30,Block);
            f.Box(8,12,12,16,2,2,2,Gone); f.Box(5,12,8,16,28,28,28,Stroke);
            f.Begin(f.tree); CHECK(f.Bit()); f.Prepared(); f.End(f.tree);
        }
        {
            // The same dim slab without anything bright is noise, and so is its pocket.
            Fixture f(30,30);
            f.Box(5,5,25,25,28,28,28,Gone);
            f.Box(8,12,12,16,2,2,2,Gone);
            f.Begin(f.tree); CHECK(f.Bit()); f.Prepared(); f.End(f.tree);
        }
        {
            // Square letters: the ring of an "o" may be up to four thick.
            Fixture f(30,30); Frame(f,200,Stroke,3,3,15,15,7,7,11,11,Gone);
            f.Box(18,18,23,23,200,200,200,Block); // a 5x5 dot is a block, with nothing to enclose
            f.Box(24,3,28,7,200,200,200,Stroke);  // a 4x4 dot is not
            f.Begin(f.tree); CHECK(f.Bit()); f.Prepared(); f.End(f.tree);
        }
    }

    void PolicyOutline()
    {
        // 10x10: 36 outline pixels. Twenty-seven of them dark is exactly three quarters.
        for (int bright : {9,10})
        {
            Fixture f(10,10);
            for (int x=0;x<bright;++x) f.Grey(x,0,210,Stroke);
            f.Grey(5,5,210,Stroke);
            f.Begin(f.tree); CHECK(f.Bit());
            CHECK(ImageTest::reads==1); // the copy was made either way
            if (bright==9) f.Prepared(); else f.Native();
            f.End(f.tree);
        }
        // The outline is judged by the background level alone: 16 is dark, 17 is not.
        for (BYTE level : {BYTE(16),BYTE(17)})
        {
            Fixture f(10,10);
            for (int y=0;y<10;++y) f.Grey(0,y,level,Gone);
            f.Grey(5,5,210,Stroke);
            f.Begin(f.tree); CHECK(f.Bit());
            if (level==16) f.Prepared(); else f.Native();
            f.End(f.tree);
        }
        {
            // An ordinary picture: not on black, not touched, black details included.
            Fixture f(12,12);
            f.Box(0,0,12,12,240,240,240,Block); f.Box(4,4,8,8,0,0,0,Block);
            f.Begin(f.viewer); CHECK(f.Stretch()); f.Native(); f.End(f.viewer);
        }
        // Degenerate sizes: every pixel is outline.
        for (int which=0;which<6;++which)
        {
            Fixture f(which<2?1:which<4?7:1, which<2?1:which<4?1:7);
            const bool white=which%2==1;
            if (white) f.Box(0,0,f.width,f.height,255,255,255,Block);
            f.Begin(f.tree); CHECK(f.Bit());
            if (white) f.Native(); else f.Prepared();
            f.End(f.tree);
        }
        {
            Fixture f(2,2); f.Grey(1,1,210,Stroke); // 3 of 4 dark
            f.Begin(f.tree); CHECK(f.Bit()); f.Prepared(); f.End(f.tree);
        }
        {
            Fixture f(2,2); f.Grey(1,1,210,Stroke); f.Grey(0,0,210,Stroke);
            f.Begin(f.tree); CHECK(f.Bit()); f.Native(); f.End(f.tree);
        }
    }

    void Routes()
    {
        // The donor pairs (tree + BitBlt, viewer + StretchBlt) and the crossed ones.
        for (bool viewer : {false,true}) for (bool stretch : {false,true})
        {
            Fixture f; PaintPlaceholder(f);
            TestWindow& host=viewer?f.viewer:f.tree; HDC dc=viewer?&f.viewerDc:&f.treeDc;
            f.Begin(host);
            CHECK(Attached() && RestoratorTest::writes==1 && RestoratorTest::attaches==2 && Ri::g_refs==1);
            CHECK(Ri::g_actualBitBlt==kTrampolineBit && Ri::g_actualStretchBlt==kTrampolineStretch);
            CHECK(RestoratorTest::countQueries==(viewer?0:1));
            if (stretch) CHECK(f.Stretch(dc)); else CHECK(f.Bit(dc));
            CHECK(RestoratorTest::exportCalls==1 && RestoratorTest::trampolineCalls==1);
            CHECK(ImageTest::nativeBits==(stretch?0:1) && ImageTest::nativeStretches==(stretch?1:0));
            const auto& c=ImageTest::last;
            CHECK(c.dest==dc && c.source==&f.source && c.rop==SRCCOPY && c.x==11 && c.y==12 && c.sx==2 && c.sy==1);
            if (stretch) CHECK(c.w==40 && c.h==30 && c.sw==20 && c.sh==10);
            else CHECK(c.w==64 && c.h==40);
            f.Prepared();
            f.End(host);
            CHECK(Detached() && RestoratorTest::writes==2 && RestoratorTest::detaches==2 && Ri::g_refs==0);
            // Outside the session the export is itself again.
            CHECK(f.Bit(dc)); f.Native();
            CHECK(RestoratorTest::trampolineCalls==1 && ImageTest::reads==1);
        }
        {
            // Scale, mirroring, source offsets and oversized extents are the caller's.
            Fixture f; PaintPlaceholder(f); f.Begin(f.viewer);
            SetLastError(0x29);
            CHECK(RestoratorExportStretchBlt(&f.viewerDc,5,6,-128,80,&f.source,3,4,20,-10,SRCCOPY));
            const auto c=ImageTest::last;
            CHECK(c.x==5 && c.y==6 && c.w==-128 && c.h==80 && c.sx==3 && c.sy==4 && c.sw==20 && c.sh==-10);
            f.Prepared();
            SetLastError(0x29);
            CHECK(RestoratorExportBitBlt(&f.viewerDc,-7,8,500,-300,&f.source,30,-20,SRCCOPY));
            const auto d=ImageTest::last;
            CHECK(d.x==-7 && d.y==8 && d.w==500 && d.h==-300 && d.sx==30 && d.sy==-20);
            f.Prepared();
            f.End(f.viewer);
        }
        {
            // A second DC of the same window that the dispatcher tracks (a VCL canvas
            // that took its own with GetDC) is the same surface; untracked, it is not.
            Fixture f; PaintPlaceholder(f); f.Begin(f.tree);
            CHECK(f.Bit(&f.treeCanvasDc)); CHECK(ImageTest::reads==0); f.Native();
            Test::stackOwners[&f.treeCanvasDc]=&f.tree;
            CHECK(f.Bit(&f.treeCanvasDc)); CHECK(ImageTest::last.dest==&f.treeCanvasDc); f.Prepared();
            f.End(f.tree);
        }
        {
            // Class names are compared like Windows does, and a populated viewer is still a viewer.
            Fixture f; PaintPlaceholder(f);
            f.tree.cls=L"tresTREEview"; f.viewer.cls=L"TIMGVIEWBOX"; RestoratorTest::items[&f.viewer]=7;
            f.Begin(f.tree); CHECK(f.Bit()); f.Prepared(); f.End(f.tree);
            f.Begin(f.viewer); CHECK(f.Stretch()); f.Prepared(); f.End(f.viewer);
            CHECK(RestoratorTest::countQueries==1);
        }
    }

    void Scope()
    {
        // No session is leased: nothing is attached and the blit never reaches us.
        for (int which=0;which<12;++which)
        {
            Fixture f; PaintPlaceholder(f); HMODULE module=&f.exe; bool recorded=true;
            switch (which)
            {
            case 0:f.tree.cls=L"TResTreeViewEx";recorded=false;break;
            case 1:f.tree.cls=L"TTreeView";recorded=false;break;
            case 2:f.tree.cls=L"TScrollBox";recorded=false;break;
            case 3:f.tree.cls=L"TResTreeVie";recorded=false;break;
            case 4:f.tree.cls=L"SysTreeView32";recorded=false;break;
            case 5:module=&f.comctl;break;                                 // comctl32 paints the tree itself
            case 6:PanelTest::instances[&f.tree]=&f.otherModule;break;     // same name, another module's class
            case 7:Test::full=false;break;
            case 8:f.tree.backdrop=false;break;
            case 9:RestoratorTest::items[&f.tree]=1;break;                 // a populated tree
            case 10:GlassCore::ModuleHandler::g_entryPointModule=nullptr;module=nullptr;break;
            case 11:module=&f.otherModule;break;
            }
            Test::topWindow=&f.tree;
            f.Provider().onBegin(&f.tree,module);
            CHECK(Detached() && RestoratorTest::writes==0 && Ri::g_refs==0);
            CHECK(Ri::g_sessions.size()==(recorded?1u:0u));
            CHECK(RestoratorTest::countQueries==(which==9?1:0)); // asked only once everything else fits
            CHECK(f.Bit()); CHECK(ImageTest::reads==0 && RestoratorTest::trampolineCalls==0); f.Native();
            CHECK(f.Stretch(&f.treeDc)); CHECK(ImageTest::reads==0); f.Native();
            f.End(f.tree);
            CHECK(Ri::g_sessions.empty() && RestoratorTest::writes==0);
        }
    }

    void Isolation()
    {
        // A leased session is open, yet this particular call is not its picture.
        for (int which=0;which<18;++which)
        {
            Fixture f; PaintPlaceholder(f); f.Begin(f.tree);
            HDC dest=&f.treeDc; DWORD rop=SRCCOPY; int w=-1, h=-1;
            switch (which)
            {
            case 0:dest=&f.foreign;break;                                        // another window's DC
            case 1:dest=&f.memory;Test::stackOwners[&f.memory]=&f.tree;break;    // a memory DC, even a tracked one
            case 2:dest=&f.viewerDc;break;                                       // the other class's window
            case 3:Test::topWindow=&f.other;break;                               // a drawing session above ours
            case 4:Test::stackOwners.erase(&f.treeDc);break;                     // a DC the dispatcher does not track
            case 5:Test::stackOwners[&f.treeDc]=&f.viewer;break;                 // or tracks for another window
            case 6:rop=SRCAND;break;
            case 7:rop=SRCPAINT;break;
            case 8:rop=SRCINVERT;break;
            case 9:w=0;break;
            case 10:h=0;break;
            case 11:ImageTest::caller=&f.comctl;break;                           // not an EXE call site
            case 12:ImageTest::caller=nullptr;break;
            case 13:Test::full=false;break;
            case 14:f.tree.backdrop=false;break;
            case 15:f.tree.valid=false;break;
            case 16:Ri::g_shutdown.store(true);break;
            case 17:dest=&f.foreign;Test::stackOwners[&f.foreign]=&f.tree;break; // tracked for us, owned by another
            }
            CHECK(f.Bit(dest,nullptr,rop,w,h));
            CHECK(ImageTest::reads==0 && ImageTest::creates==3 && RestoratorTest::trampolineCalls==1);
            CHECK(ImageTest::last.dest==dest && ImageTest::last.rop==rop);
            f.Native();
            Ri::g_shutdown.store(false); f.tree.valid=true;
            f.End(f.tree);
        }
        for (int which=0;which<4;++which)
        {
            // StretchBlt: any zero extent, or a ROP that is not a plain copy.
            Fixture f; PaintPlaceholder(f); f.Begin(f.viewer);
            CHECK(f.Stretch(nullptr, which==3?SRCAND:SRCCOPY, which==0?0:40, 30, which==1?0:20, which==2?0:10));
            CHECK(ImageTest::reads==0); f.Native(); f.End(f.viewer);
        }
        {
            // Sources that are not a bitmap in a memory DC.
            Fixture f; PaintPlaceholder(f); f.Begin(f.tree);
            SetLastError(0x29);
            CHECK(RestoratorExportBitBlt(&f.treeDc,1,2,3,4,nullptr,0,0,SRCCOPY));
            CHECK(ImageTest::reads==0 && !ImageTest::last.source && GetLastError()==0x51);
            SetLastError(0x29);
            CHECK(RestoratorExportBitBlt(&f.treeDc,1,2,3,4,&f.treeDc,0,0,SRCCOPY)); // scrolling its own surface
            CHECK(ImageTest::reads==0 && ImageTest::last.selected==f.surface);
            SetLastError(0x29);
            CHECK(RestoratorExportBitBlt(&f.treeDc,1,2,3,4,&f.foreign,0,0,SRCCOPY)); // stock bitmap
            CHECK(ImageTest::reads==0 && !ImageTest::last.selected);
            PanelTest::bitmaps[&f.foreign]=f.picture;                               // a window DC as source
            SetLastError(0x29);
            CHECK(RestoratorExportBitBlt(&f.treeDc,1,2,3,4,&f.foreign,0,0,SRCCOPY));
            CHECK(ImageTest::reads==0 && ImageTest::last.selected==f.picture && ImageTest::last.pixels==f.bytes);
            PanelTest::bitmaps[&f.foreign]=PanelTest::stockBitmap;
            f.Restored(); f.End(f.tree);
        }
        {
            // Sessions are per thread: the same call from another thread is native.
            Fixture f; PaintPlaceholder(f); f.Begin(f.tree);
            std::thread worker([&] { CHECK(f.Bit()); CHECK(ImageTest::reads==0); f.Native(); });
            worker.join();
            CHECK(f.Bit()); f.Prepared(); f.End(f.tree);
        }
        {
            // A blit made from inside our own native call is not prepared again.
            Fixture f; PaintPlaceholder(f); f.Begin(f.tree);
            ImageTest::duringNative=[&] {
                CHECK(Ri::g_inside);
                CHECK(RestoratorExportBitBlt(&f.treeDc,1,2,3,4,&f.source,0,0,SRCCOPY));
                CHECK(RestoratorExportStretchBlt(&f.treeDc,1,2,3,4,&f.memory,0,0,5,6,SRCCOPY));
                CHECK(Ri::g_inside);
            };
            CHECK(f.Bit());
            CHECK(ImageTest::calls.size()==3 && ImageTest::reads==1 && RestoratorTest::trampolineCalls==3);
            CHECK(ImageTest::calls[1].selected==ImageTest::calls[0].selected); // saw the copy that was selected
            CHECK(ImageTest::calls[2].selected==f.memoryBitmap);               // another black picture, as it is
            f.Restored();
            // Outside the native call the same blit is a picture of its own.
            ImageTest::duringNative={};
            SetLastError(0x29);
            CHECK(RestoratorExportStretchBlt(&f.treeDc,1,2,3,4,&f.memory,0,0,5,6,SRCCOPY));
            CHECK(ImageTest::reads==2 && ImageTest::last.selected!=f.memoryBitmap);
            f.End(f.tree);
        }
    }

    void Sources()
    {
        // Colour depths the shared opaque-source helper accepts, DIB and DDB.
        for (int bpp : {8,16,24,32}) for (bool dib : {false,true})
        {
            Fixture f(64,40,bpp,dib); PaintPlaceholder(f);
            f.Begin(f.tree); CHECK(f.Bit()); f.Prepared();
            CHECK(ImageTest::flushes==(bpp==32&&dib?2:1));
            f.End(f.tree);
        }
        for (int bpp : {1,4})
        {
            Fixture f(64,40,bpp); PaintPlaceholder(f);
            f.Begin(f.tree); CHECK(f.Bit()); CHECK(ImageTest::reads==0); f.Native(); f.End(f.tree);
        }
        {
            // A 32bpp DIB that already carries alpha is left to the native copy, alpha included.
            Fixture f(64,40,32); PaintPlaceholder(f); f.Alpha(63,39,1);
            f.Begin(f.tree); CHECK(f.Bit());
            CHECK(ImageTest::reads==0 && ImageTest::flushes==1); f.Native(); f.End(f.tree);
        }
        {
            // The fourth byte of a DDB or of a narrower DIB is not alpha and is ignored.
            for (int which=0;which<2;++which)
            {
                Fixture f(64,40,which?24:32,which!=0); PaintPlaceholder(f);
                for (int i=0;i<64*40;i+=7) f.Alpha(i%64,i/64,BYTE(i));
                f.Begin(f.tree); CHECK(f.Bit()); f.Prepared(); f.End(f.tree);
            }
        }
        {
            // Size: 2^22 pixels is still a candidate (a full-screen buffer), one column more is not.
            Fixture f(2048,2048); f.Box(500,500,520,520,90,120,60,Block); f.Box(100,100,102,160,90,120,60,Stroke);
            f.Begin(f.tree); CHECK(f.Bit()); f.Prepared(); f.End(f.tree);
        }
        for (int which=0;which<4;++which)
        {
            Fixture f; PaintPlaceholder(f);
            switch (which)
            {
            case 0:f.picture->info.bmWidth=2049;f.picture->info.bmHeight=2048;break;
            case 1:f.picture->info.bmWidth=(std::numeric_limits<LONG>::max)();f.picture->info.bmHeight=(std::numeric_limits<LONG>::max)();break;
            case 2:f.picture->info.bmWidth=0;break;
            case 3:f.picture->info.bmHeight=-40;break;
            }
            f.Begin(f.tree); CHECK(f.Bit());
            CHECK(ImageTest::reads==0 && ImageTest::creates==3 && ImageTest::flushes==0);
            f.picture->info.bmWidth=64; f.picture->info.bmHeight=40;
            f.Native(); f.End(f.tree);
        }
        // Every failure of the copy leaves one native blit of the caller's own bitmap.
        for (int which=0;which<12;++which)
        {
            Fixture f(64,40,which==11?32:24); PaintPlaceholder(f);
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
            case 9:ImageTest::failSelectAt=1;ImageTest::selectReturnsNull=true;break;
            case 10:ImageTest::failSelectAt=2;ImageTest::selectReturnsNull=true;break;
            case 11:ImageTest::failFlush=true;break; // 32bpp: the alpha look-up is skipped, the helper refuses
            }
            f.Begin(f.tree); CHECK(f.Bit());
            ImageTest::failMetadata=false;
            CHECK(ImageTest::calls.size()==1); f.Native(); f.End(f.tree);
        }
        for (int fault=1;fault<=7;++fault)
        {
            // The prepared copy is written only if it is what the helper promised:
            // a 32bpp single-plane DIB of the caller's size with accessible bits.
            Fixture f; PaintPlaceholder(f); f.Begin(f.tree);
            RestoratorTest::copyFault=fault;
            CHECK(f.Bit());
            RestoratorTest::copyFault=0;
            CHECK(ImageTest::reads==1 && ImageTest::calls.size()==1); f.Native(); f.End(f.tree);
        }
        {
            // Out of memory anywhere inside the pixel policy: the copy is dropped
            // untouched and the caller's bitmap is drawn natively. No exception escapes.
            int failures=0;
            for (long after=0;;++after)
            {
                Fixture f; PaintPlaceholder(f); f.Begin(f.tree);
                RestoratorTest::armAllocations=after;
                CHECK(f.Bit());
                RestoratorTest::armAllocations=-1;
                CHECK(RestoratorTest::allocationCountdown==-1 && ImageTest::calls.size()==1 && ImageTest::reads==1);
                const bool failed=RestoratorTest::allocationFailed;
                if (failed) { f.Native(); ++failures; } else f.Prepared();
                f.End(f.tree);
                if (!failed) break;
                CHECK(after<64);
            }
            CHECK(failures>=3); // the two planes and the work list, at the least
        }
    }

    void Leases()
    {
        {
            // A second BeginPaint of the same window that is not ours (comctl32) is
            // recorded: it bars the picture path and its End does not end our session.
            // First fixture of the process: the lease generation is still the one
            // an unleased session carries, so only its flag keeps its End harmless.
            Fixture f; PaintPlaceholder(f);
            CHECK(Ri::g_generation==0);
            f.Begin(f.tree); f.Begin(f.tree,&f.comctl);
            CHECK(Ri::g_sessions.size()==2 && Ri::g_refs==1);
            CHECK(f.Bit()); CHECK(ImageTest::reads==0); f.Native();
            f.End(f.tree);
            CHECK(Ri::g_sessions.size()==1 && Attached() && Ri::g_refs==1);
            // Another window painting in between is not ours to record or to end.
            f.Begin(f.other); f.End(f.other);
            CHECK(Ri::g_sessions.size()==1 && Attached() && Ri::g_refs==1);
            Test::topWindow=&f.tree;
            CHECK(f.Bit()); f.Prepared();
            f.End(f.tree); CHECK(Detached() && Ri::g_sessions.empty());
            // Ends without a Begin, or for a window we never recorded, change nothing.
            f.End(f.tree); f.End(f.other); f.Provider().onEnd(nullptr,nullptr);
            CHECK(Detached() && Ri::g_refs==0 && RestoratorTest::writes==2);
            // A release that matches no lease does not underflow, and nothing is
            // leased once Shutdown has begun, whoever asks.
            Ri::Release(Ri::g_generation);
            CHECK(Ri::g_refs==0 && RestoratorTest::writes==2);
            Ri::Shutdown();
            unsigned generation=0;
            CHECK(!Ri::Acquire(generation) && Ri::g_refs==0 && Detached());
            GlassCore::PaintDispatcher::imageProviders.clear(); Ri::Startup();
        }
        {
            // Two windows painting inside one another share one attachment.
            Fixture f; PaintPlaceholder(f);
            f.Begin(f.tree); f.Begin(f.viewer);
            CHECK(Attached() && Ri::g_refs==2 && RestoratorTest::writes==1 && RestoratorTest::attaches==2);
            CHECK(f.Bit()); CHECK(ImageTest::reads==0); f.Native(); // the tree is not the top session now
            CHECK(f.Stretch()); f.Prepared();
            f.End(f.viewer);
            CHECK(Attached() && Ri::g_refs==1 && RestoratorTest::writes==1);
            Test::topWindow=&f.tree; CHECK(f.Bit()); f.Prepared();
            f.End(f.tree); CHECK(Detached() && Ri::g_refs==0 && RestoratorTest::writes==2);
        }
        for (int which=0;which<4;++which)
        {
            // A transaction that fails installs neither detour and leases nothing.
            Fixture f; PaintPlaceholder(f);
            switch (which)
            {
            case 0:RestoratorTest::failBegin=true;break;
            case 1:RestoratorTest::failAttachAt=1;break;
            case 2:RestoratorTest::failAttachAt=2;break;
            case 3:RestoratorTest::failCommit=true;break;
            }
            f.Begin(f.tree);
            CHECK(Detached() && Ri::g_refs==0 && RestoratorTest::attaches==0 && RestoratorTest::writes==1);
            CHECK(Ri::g_sessions.size()==1 && !Ri::g_sessions.back().leased);
            CHECK(f.Bit()); CHECK(ImageTest::reads==0 && RestoratorTest::trampolineCalls==0); f.Native();
            f.End(f.tree);
            CHECK(RestoratorTest::detachCalls==0 && RestoratorTest::writes==1);
            ResetHooks();
            f.Begin(f.tree); CHECK(Attached() && Ri::g_refs==1); // the next paint simply tries again
            CHECK(f.Bit()); f.Prepared(); f.End(f.tree); CHECK(Detached());
        }
        for (int which=0;which<3;++which)
        {
            // A failed last Detach keeps both trampolines; calls pass through them
            // and the next session reuses the attachment.
            Fixture f; PaintPlaceholder(f); f.Begin(f.tree);
            switch (which)
            {
            case 0:RestoratorTest::failDetachAt=1;break;
            case 1:RestoratorTest::failDetachAt=2;break;
            case 2:RestoratorTest::failCommit=true;break;
            }
            f.End(f.tree);
            CHECK(Attached() && Ri::g_attached && Ri::g_refs==0 && Ri::g_sessions.empty());
            CHECK(Ri::g_actualBitBlt==kTrampolineBit && Ri::g_actualStretchBlt==kTrampolineStretch);
            CHECK(f.Bit()); CHECK(ImageTest::reads==0 && RestoratorTest::trampolineCalls==1); f.Native();
            CHECK(f.Stretch()); CHECK(RestoratorTest::trampolineCalls==2); f.Native();
            ResetHooks();
            const int writes=RestoratorTest::writes;
            f.Begin(f.tree); CHECK(Ri::g_refs==1 && RestoratorTest::writes==writes && RestoratorTest::attaches==2);
            CHECK(f.Bit()); f.Prepared();
            f.End(f.tree); CHECK(Detached() && RestoratorTest::detaches==2);
        }
        {
            // Shutdown retries the Detach; a Startup in between must not resolve
            // the exports again over live trampolines.
            Fixture f; PaintPlaceholder(f); f.Begin(f.tree);
            RestoratorTest::failCommit=true; f.End(f.tree);
            Ri::Shutdown(); CHECK(Attached() && Ri::g_attached && Ri::g_refs==0);
            const int resolved=RestoratorTest::resolutions;
            GlassCore::PaintDispatcher::imageProviders.clear();
            Ri::Startup();
            CHECK(RestoratorTest::resolutions==resolved && Ri::g_actualBitBlt==kTrampolineBit);
            ResetHooks();
            const int writes=RestoratorTest::writes;
            f.Begin(f.tree); CHECK(RestoratorTest::writes==writes && Ri::g_refs==1);
            CHECK(f.Bit()); f.Prepared(); f.End(f.tree); CHECK(Detached());
            Ri::Shutdown(); CHECK(Detached());
            GlassCore::PaintDispatcher::imageProviders.clear();
            Ri::Startup(); CHECK(RestoratorTest::resolutions==resolved+2 && Detached());
        }
        for (int which=0;which<3;++which)
        {
            // Without both exports there is nothing to lease.
            Fixture f; PaintPlaceholder(f);
            Ri::Shutdown(); GlassCore::PaintDispatcher::imageProviders.clear();
            if (which==0) RestoratorTest::gdiAvailable=false;
            if (which==1) RestoratorTest::bitExport=false;
            if (which==2) RestoratorTest::stretchExport=false;
            Ri::Startup();
            f.Begin(f.tree);
            CHECK(RestoratorTest::writes==0 && Ri::g_refs==0 && !RestoratorTest::bitDetour && !RestoratorTest::stretchDetour);
            CHECK(f.Bit()); f.Native(); f.End(f.tree);
            Ri::Shutdown(); GlassCore::PaintDispatcher::imageProviders.clear();
            RestoratorTest::gdiAvailable=RestoratorTest::bitExport=RestoratorTest::stretchExport=true;
            Ri::Startup();
        }
        {
            // Startup is idempotent; a provider is registered once per start.
            Fixture f; PaintPlaceholder(f);
            const int resolved=RestoratorTest::resolutions;
            Ri::Startup();
            CHECK(GlassCore::PaintDispatcher::imageProviders.size()==1 && RestoratorTest::resolutions==resolved);
            // Before Startup and after Shutdown the provider records nothing.
            f.Begin(f.tree); Ri::Shutdown();
            CHECK(Detached() && Ri::g_refs==0 && Ri::g_sessions.empty() && RestoratorTest::detaches==2);
            CHECK(f.Bit()); f.Native();
            f.End(f.tree); f.Begin(f.tree);
            CHECK(Ri::g_sessions.empty() && RestoratorTest::writes==2);
            f.End(f.tree);
            GlassCore::PaintDispatcher::imageProviders.clear();
            Ri::Startup();
        }
        for (bool restart : {false,true})
        {
            // A session left open on another thread across Shutdown belongs to the
            // old lease generation: its End releases nothing, restarted or not.
            Fixture f; PaintPlaceholder(f);
            const auto provider=f.Provider();
            std::atomic<int> stage{0};
            std::thread worker([&] {
                provider.onBegin(&f.tree,&f.exe);
                stage=1; while (stage!=2) std::this_thread::yield();
                provider.onEnd(&f.tree,&f.exe);
                CHECK(Ri::g_sessions.empty());
                stage=3;
            });
            while (stage!=1) std::this_thread::yield();
            CHECK(Attached() && Ri::g_refs==1);
            Ri::Shutdown(); CHECK(Detached() && Ri::g_refs==0);
            GlassCore::PaintDispatcher::imageProviders.clear();
            if (restart) { Ri::Startup(); f.Begin(f.viewer); CHECK(Attached() && Ri::g_refs==1); }
            const int writes=RestoratorTest::writes;
            stage=2; worker.join(); CHECK(stage==3);
            CHECK(RestoratorTest::writes==writes && Ri::g_refs==(restart?1u:0u));
            if (restart) { CHECK(f.Stretch()); f.Prepared(); f.End(f.viewer); CHECK(Detached()); }
            else Ri::Startup();
        }
    }

    void Failures()
    {
        {
            // Native FALSE is returned once, with its own LastError; nothing is retried.
            Fixture f; PaintPlaceholder(f); f.Begin(f.tree);
            ImageTest::failNative=true;
            CHECK(!f.Bit()); CHECK(ImageTest::calls.size()==1 && RestoratorTest::trampolineCalls==1); f.Prepared();
            CHECK(!f.Stretch(&f.treeDc)); CHECK(ImageTest::calls.size()==2); f.Prepared();
            CHECK(!f.Bit(nullptr,nullptr,SRCAND)); CHECK(ImageTest::calls.size()==3); f.Native();
            f.End(f.tree);
            CHECK(!f.Bit()); CHECK(ImageTest::calls.size()==4); f.Native();
        }
        {
            // The entry LastError reaches the native call on every path, whatever
            // the preparation and the logger did in between.
            Fixture f; PaintPlaceholder(f); f.Begin(f.tree);
            for (DWORD rop : {SRCCOPY,SRCAND})
            {
                SetLastError(0x1234);
                CHECK(RestoratorExportBitBlt(&f.treeDc,0,0,64,40,&f.source,0,0,rop));
                CHECK(ImageTest::last.entryError==0x1234 && GetLastError()==0x51);
            }
            f.End(f.tree);
        }
    }

    std::wstring LastReason()
    {
        CHECK(RestoratorTest::logStrings.size()>=2);
        return RestoratorTest::logStrings.back();
    }
    int ImageLogs()
    {
        return static_cast<int>(std::count_if(RestoratorTest::logFormats.begin(), RestoratorTest::logFormats.end(),
            [](const std::wstring& s) { return s.find(L"] image api=")!=std::wstring::npos; }));
    }
    void Diagnostics()
    {
        {
            // One metadata line per in-session EXE blit, naming why it stayed native.
            Fixture f; PaintPlaceholder(f); f.Begin(f.tree);
            CHECK(RestoratorTest::logFormats.size()==3); // resolve, detour, Begin
            CHECK(RestoratorTest::logStrings.back()==L"TResTreeView");
            CHECK(f.Bit()); CHECK(LastReason()==L"none");
            CHECK(RestoratorTest::logStrings[RestoratorTest::logStrings.size()-2]==L"BitBlt");
            CHECK(f.Stretch(&f.treeDc)); CHECK(LastReason()==L"none");
            CHECK(RestoratorTest::logStrings[RestoratorTest::logStrings.size()-2]==L"StretchBlt");
            CHECK(f.Bit(nullptr,nullptr,SRCAND)); CHECK(LastReason()==L"rop");
            CHECK(f.Bit(nullptr,nullptr,SRCCOPY,0)); CHECK(LastReason()==L"extents");
            CHECK(f.Bit(&f.memory)); CHECK(LastReason()==L"dc");
            CHECK(f.Bit(nullptr,&f.treeDc)); CHECK(LastReason()==L"source");
            Test::full=false; CHECK(f.Bit()); CHECK(LastReason()==L"mode"); Test::full=true;
            f.picture->info.bmWidth=4096; f.picture->info.bmHeight=4096;
            CHECK(f.Bit()); CHECK(LastReason()==L"size");
            f.picture->info.bmWidth=64; f.picture->info.bmHeight=40;
            f.picture->info.bmBitsPixel=4; CHECK(f.Bit()); CHECK(LastReason()==L"copy");
            f.picture->info.bmBitsPixel=24;
            f.picture->info.bmWidth=0; CHECK(f.Bit()); CHECK(LastReason()==L"size");
            f.picture->info.bmWidth=64;
            ImageTest::failMetadata=true; CHECK(f.Bit()); CHECK(LastReason()==L"source");
            ImageTest::failMetadata=false;
            CHECK(ImageLogs()==11);
            // Calls that are not ours are not logged at all.
            ImageTest::caller=&f.comctl; CHECK(f.Bit()); ImageTest::caller=&f.exe;
            CHECK(ImageLogs()==11);
            f.End(f.tree);
            CHECK(f.Bit()); CHECK(ImageLogs()==11);
        }
        {
            Fixture f(64,40,32); PaintPlaceholder(f); f.Alpha(3,3,200); f.Begin(f.viewer);
            CHECK(RestoratorTest::logStrings.back()==L"TImgViewBox");
            CHECK(f.Stretch()); CHECK(LastReason()==L"alpha"); f.End(f.viewer);
        }
        {
            Fixture f(12,12); f.Box(0,0,12,12,200,200,200,Block); f.Begin(f.viewer);
            CHECK(f.Stretch()); CHECK(LastReason()==L"background"); f.End(f.viewer);
        }
        {
            // Sixteen lines a second per channel; no bitmap or window text is ever logged.
            Fixture f; PaintPlaceholder(f); f.Begin(f.tree);
            for (int i=0;i<40;++i) CHECK(f.Bit(nullptr,nullptr,SRCAND));
            CHECK(ImageLogs()==16);
            RestoratorTest::clock+=999; CHECK(f.Bit()); CHECK(ImageLogs()==16);
            RestoratorTest::clock+=1; CHECK(f.Bit()); CHECK(ImageLogs()==17 && LastReason()==L"none");
            f.End(f.tree);
            for (const auto& s : RestoratorTest::logStrings)
                CHECK(s==L"TResTreeView" || s==L"BitBlt" || s==L"rop" || s==L"none");
        }
    }

    void Buffered()
    {
        {
            // A double-buffered paint: the EXE copies its whole black client buffer.
            // The policy sees the same picture with a wider margin.
            std::vector<BYTE> pixels, expected;
            { Fixture small; PaintPlaceholder(small); pixels=small.bytes; expected=small.alpha; }
            Fixture f(200,120);
            for (int y=0;y<40;++y) for (int x=0;x<64;++x)
            {
                const BYTE* p=pixels.data()+(static_cast<size_t>(y)*64+x)*4;
                f.PutAlpha(70+x,40+y,p[2],p[1],p[0],expected[static_cast<size_t>(y)*64+x]);
            }
            f.Begin(f.tree);
            SetLastError(0x29);
            CHECK(RestoratorExportBitBlt(&f.treeDc,0,0,200,120,&f.source,0,0,SRCCOPY));
            f.Prepared(); f.End(f.tree);
        }
        {
            // A buffer that already went through a prepared blit carries alpha in a
            // 32bpp DIB: the final copy is native and keeps that alpha byte for byte.
            Fixture f(64,40,32); PaintPlaceholder(f);
            for (int i=0;i<64*40;++i) if (f.alpha[i]) f.Alpha(i%64,i/64,f.alpha[i]);
            f.Begin(f.tree); CHECK(f.Bit()); CHECK(ImageTest::reads==0); f.Native(); f.End(f.tree);
        }
    }
}

int main(int argc, char** argv)
{
    const std::string name=argc==2?argv[1]:"";
    const std::pair<const char*, void(*)()> cases[]{
        {"policy-placeholder",PolicyPlaceholder}, {"policy-levels",PolicyLevels},
        {"policy-solid",PolicySolid}, {"policy-outline",PolicyOutline},
        {"routes",Routes}, {"scope",Scope}, {"isolation",Isolation}, {"sources",Sources},
        {"leases",Leases}, {"failures",Failures}, {"diagnostics",Diagnostics}, {"buffered",Buffered},
    };
    for (const auto& [id,run] : cases)
    {
        if (name!=id) continue;
        try { run(); return 0; }
        catch (const std::exception& e) { std::cerr<<id<<": "<<e.what()<<"\n"; return 1; }
    }
    std::cerr<<"unknown case\n";
    return 2;
}
