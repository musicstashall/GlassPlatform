#include "restorator_checkerboard_test_shim.h"
#include "../../GlassCore/src/Hooks/delphi/RestoratorCheckerboard.cpp"

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)

namespace
{
    namespace Board = GlassCore::RestoratorCheckerboard;
    namespace T = BoardTest;
    using T::Route;
    using FillFn = int (*)(HDC, const RECT*, HBRUSH);
    using DibFn = HBITMAP (*)(HDC, const BITMAPINFO*, UINT, void**, HANDLE, DWORD);
    using Numbers = std::vector<long long>;
    using Strings = std::vector<std::wstring>;
    using Windows = std::vector<HWND>;
    constexpr COLORREF kFace = RGB(240, 240, 240), kWhite = RGB(255, 255, 255);
    const std::wstring kFillLine =
        L"[GlassCore][RestoratorCheckerboard] fill dc=0x%p rect=[%ld,%ld,%ld,%ld] dib=[%ld,%ld] brush=0x%p style=%d colour=0x%06X bitmap=0x%p bpp=%u size=[%ld,%ld] section=%d root=0x%p board=%d reason=%s result=%d";
    const std::wstring kArmLine =
        L"[GlassCore][RestoratorCheckerboard] arm exe=0x%p root=0x%p FillRect=%d CreateDIBSection=%d held=%d";

    long long Number(const void* p) { return static_cast<long long>(reinterpret_cast<std::intptr_t>(p)); }

    // The checkerboard, stated here and not taken from the unit: squares of four
    // pixels from the origin of the bitmap, RGB(205,205,205) first, then white.
    BYTE BoardLevel(int x, int y) { return (x / 4 + y / 4) % 2 == 0 ? 205 : 255; }
    const BYTE* At(const TestBitmap& b, int x, int y)
    { return b.pixels.data() + (static_cast<size_t>(y) * b.width + x) * 4; }
    template <typename Expected> bool Area(const TestBitmap& b, int x0, int y0, int x1, int y1, Expected expected)
    {
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
            {
                const BYTE* p = At(b, x, y);
                const std::array<BYTE, 4> want = expected(x, y);
                if (p[0] == want[0] && p[1] == want[1] && p[2] == want[2] && p[3] == want[3]) continue;
                std::cerr << "pixel " << x << "," << y << " is " << int(p[2]) << "," << int(p[1]) << "," << int(p[0])
                    << "," << int(p[3]) << ", expected " << int(want[2]) << "," << int(want[1]) << "," << int(want[0])
                    << "," << int(want[3]) << "\n";
                return false;
            }
        return true;
    }
    // GDI writes nothing into the fourth byte, and the unit does not either.
    bool IsBoard(const TestBitmap& b, int x0, int y0, int x1, int y1)
    {
        return Area(b, x0, y0, x1, y1, [](int x, int y) {
            const BYTE level = BoardLevel(x, y); return std::array<BYTE, 4>{level, level, level, 0}; });
    }
    bool IsColour(const TestBitmap& b, int x0, int y0, int x1, int y1, COLORREF c)
    {
        return Area(b, x0, y0, x1, y1, [c](int, int) {
            return std::array<BYTE, 4>{BYTE(c >> 16), BYTE(c >> 8), BYTE(c), 0}; });
    }
    bool Untouched(const TestBitmap& b, int x0, int y0, int x1, int y1)
    { return Area(b, x0, y0, x1, y1, [](int, int) { return std::array<BYTE, 4>{0x11, 0x11, 0x11, 0x11}; }); }

    // A VCL application that shows pictures: a form with a tree and a viewer in
    // it, a dialog with a viewer of its own, and the memory DC with the bitmap
    // the EXE composes a picture on. Its calls go through its import slots.
    struct Fixture
    {
        TestModule exe;
        TestWindow form, viewer, tree, dialog, dialogViewer;
        TestBitmap target, surface;
        TestDC memory, windowDc, screen;
        TestBrush face, white, hatched, hollow;
        int width, height;
        static constexpr size_t kOwnObjects = 6;    // two bitmaps and four brushes

        explicit Fixture(int w = 16, int h = 12, bool start = true)
            : target(w, h), surface(32, 32, 32, false), width(w), height(h)
        {
            CHECK(T::live.size() == kOwnObjects && !T::exclusive && !T::shared);
            T::error = 0; T::clock += 1000000; T::full = true; T::rendering = true;
            T::colours = {{COLOR_BTNFACE, kFace}, {COLOR_WINDOW, kWhite}};
            T::typeQueries = T::colourQueries = T::brushQueries = T::bitmapQueries = T::ancestors = 0;
            T::glassQueries.clear(); T::exclusiveLocks = 0;
            T::lastPattern.clear(); T::lastUsage = 99; T::patternBrushes = T::brushDeletes = 0; T::failPattern = false;
            T::fills.clear(); T::failFills.clear(); T::fillResult = 1; T::duringFill = {};
            T::dibs.clear(); T::dibResult = nullptr;
            T::logs.clear(); T::registrationFailures.clear(); T::enables = T::disables = T::disableAlls = 0;
            T::beforeExclusive = {};
            exe.imports = Imports();
            viewer.parent = tree.parent = &form; dialogViewer.parent = &dialog;
            memory.bitmap = &target; windowDc.type = OBJ_DC; windowDc.bitmap = &surface; screen.type = OBJ_DC;
            face.data = {BS_SOLID, kFace, 0}; white.data = {BS_SOLID, kWhite, 0};
            hatched.data = {BS_HATCHED, kFace, 4}; hollow.data = {BS_NULL, 0, 0};
            GlassCore::ModuleHandler::g_entryPointModule = &exe;
            if (start) Board::Startup();
        }
        ~Fixture()
        {
            T::duringFill = {}; T::beforeExclusive = {};
            Board::Shutdown();
            Board::g_dispatcher.moduleAddress = nullptr;    // this module is about to go
            GlassCore::ModuleHandler::g_entryPointModule = nullptr;
            assert(!T::exclusive && !T::shared && exe.hooks.empty());
            assert(T::live.size() == kOwnObjects);          // every pattern brush was deleted
        }
        static std::unordered_map<std::string, TestModule::Import> Imports()
        {
            return {{"FillRect", {"user32.dll", reinterpret_cast<void*>(ImportedFillRect)}},
                    {"CreateDIBSection", {"gdi32.dll", reinterpret_cast<void*>(ImportedCreateDIBSection)}}};
        }
        template <typename Fn> Fn Import(const char* name)
        {
            const auto hook = exe.hooks.find(name);
            return reinterpret_cast<Fn>(hook != exe.hooks.end() ? hook->second : exe.imports.at(name).target);
        }
        int Fill(HDC dc, const RECT* rect, HBRUSH brush) { return Import<FillFn>("FillRect")(dc, rect, brush); }
        // The whole of the fixture's bitmap.
        int Fill(HBRUSH brush) { const RECT rect{0, 0, width, height}; return Fill(&memory, &rect, brush); }
        HBITMAP Dib(HDC dc, const BITMAPINFO* info, UINT usage = DIB_RGB_COLORS, void** bits = nullptr,
            HANDLE section = nullptr, DWORD offset = 0)
        { return Import<DibFn>("CreateDIBSection")(dc, info, usage, bits, section, offset); }
        // The EXE asks for a DIB section of that size, as VCL and a decoder do.
        void Section(LONG w, LONG h)
        {
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(info.bmiHeader);
            info.bmiHeader.biWidth = w; info.bmiHeader.biHeight = h;
            info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 24;
            Dib(&screen, &info);
        }
        void Arm() { Board::Arm(&viewer); }
        bool Held()
        {
            return exe.hooks.size() == 2 && exe.refs["FillRect"] == 1 && exe.refs["CreateDIBSection"] == 1
                && exe.hooks["FillRect"] == reinterpret_cast<void*>(Board::BoardFill)
                && exe.hooks["CreateDIBSection"] == reinterpret_cast<void*>(Board::NoteDib);
        }
        bool Free()
        {
            return exe.hooks.empty() && exe.refs["FillRect"] == 0 && exe.refs["CreateDIBSection"] == 0
                && Board::g_dispatcher.GetRef(0) == 0 && Board::g_dispatcher.GetRef(1) == 0;
        }
    };
    // The EXE's own fill, and nothing else: one call of its import with what it gave.
    bool Own(const T::Fill& fill, HDC dc, HBRUSH brush)
    { return fill.route == Route::Import && fill.dc == dc && fill.brush == brush && !fill.pattern; }

    void BoardCase()
    {
        // The EXE has read a picture, asked for a bitmap of its size and lays the
        // colour of its window over all of it. That fill becomes the checkerboard.
        struct Size { int w, h; };
        for (const Size s : {Size{190, 190}, Size{256, 256}, Size{8, 8}, Size{4, 4}, Size{1, 1}, Size{13, 7}, Size{5, 64}})
        {
            Fixture f(s.w, s.h); f.Arm();
            f.Section(s.w, s.h);
            const RECT rect{0, 0, s.w, s.h};
            SetLastError(0x51);
            CHECK(f.Fill(&f.memory, &rect, &f.face) == 1);
            // One call of the import the EXE meant: its DC, its RECT, and a
            // pattern brush where its brush was.
            CHECK(T::fills.size() == 1);
            const auto& fill = T::fills[0];
            CHECK(fill.route == Route::Import && fill.dc == &f.memory && fill.rectPointer == &rect);
            CHECK(fill.pattern && fill.brush != &f.face && fill.entryError == 0x51);
            CHECK(GetLastError() == 0xc001);    // what that call left: not a query's, the logger's or DeleteObject's
            CHECK(T::patternBrushes == 1 && T::brushDeletes == 1 && T::live.size() == Fixture::kOwnObjects);
            CHECK(IsBoard(f.target, 0, 0, s.w, s.h));
            // The EXE's RECT and brush are the EXE's.
            CHECK(rect.left == 0 && rect.top == 0 && rect.right == s.w && rect.bottom == s.h);
            CHECK(f.face.data.lbStyle == BS_SOLID && f.face.data.lbColor == kFace && T::live.count(&f.face));
        }
        {
            // The tile: a packed DIB of 8 by 8 at 24 bits, bottom-up - the first
            // row in memory is the lowest one shown - and nothing after it.
            Fixture f; f.Arm(); f.Section(16, 12);
            CHECK(f.Fill(&f.face) == 1);
            const auto& packed = T::lastPattern;
            CHECK(packed.size() == 40 + 8 * 8 * 3 && T::lastUsage == DIB_RGB_COLORS);
            BITMAPINFOHEADER header{};
            std::memcpy(&header, packed.data(), sizeof(header));
            CHECK(header.biSize == 40 && header.biWidth == 8 && header.biHeight == 8 && header.biPlanes == 1);
            CHECK(header.biBitCount == 24 && header.biCompression == BI_RGB && header.biSizeImage == 0);
            CHECK(!header.biXPelsPerMeter && !header.biYPelsPerMeter && !header.biClrUsed && !header.biClrImportant);
            for (int row = 0; row < 8; ++row)
                for (int x = 0; x < 8; ++x)
                {
                    const BYTE level = BoardLevel(x, 7 - row);
                    const BYTE* p = packed.data() + 40 + (row * 8 + x) * 3;
                    CHECK(p[0] == level && p[1] == level && p[2] == level);
                }
        }
        {
            // Only the rectangle is filled, whatever the size of the bitmap in
            // the DC, and the result is the fill's own.
            Fixture f(40, 30); f.Arm(); f.Section(24, 16);
            T::fillResult = 7;
            const RECT rect{0, 0, 24, 16};
            CHECK(f.Fill(&f.memory, &rect, &f.face) == 7);
            CHECK(IsBoard(f.target, 0, 0, 24, 16));
            CHECK(Untouched(f.target, 24, 0, 40, 30) && Untouched(f.target, 0, 16, 24, 30));
        }
        {
            // The size stays the last section's. The same bitmap filled again is
            // a board again: VCL fills in CopyBitmap, a viewer may fill once more.
            Fixture f; f.Arm(); f.Section(16, 12);
            CHECK(f.Fill(&f.face) == 1 && f.Fill(&f.face) == 1);
            CHECK(T::fills.size() == 2 && T::fills[0].pattern && T::fills[1].pattern);
            CHECK(T::patternBrushes == 2 && T::brushDeletes == 2);
            // Nothing is remembered about a DC or its bitmap: another one of that size too.
            TestBitmap second(16, 12); TestDC secondDc; secondDc.bitmap = &second;
            const RECT rect{0, 0, 16, 12};
            CHECK(f.Fill(&secondDc, &rect, &f.face) == 1 && IsBoard(second, 0, 0, 16, 12));
            // A section of another size ends it: that fill is the window's colour again.
            f.Section(10, 10);
            CHECK(f.Fill(&f.face) == 1);
            CHECK(T::fills.size() == 4 && Own(T::fills[3], &f.memory, &f.face));
            CHECK(IsColour(f.target, 0, 0, 16, 12, kFace));
        }
        {
            // A solid brush of that colour is one, whoever made it and whatever
            // its fourth byte says (a palette-relative colour).
            Fixture f; f.Arm(); f.Section(16, 12);
            TestBrush flagged; flagged.data = {BS_SOLID, kFace | 0x02000000u, 0};
            CHECK(f.Fill(&flagged) == 1 && T::fills[0].pattern && IsBoard(f.target, 0, 0, 16, 12));
        }
        {
            // The colour of the window is what the system says at the fill.
            Fixture f; f.Arm(); f.Section(16, 12);
            T::colours[COLOR_BTNFACE] = RGB(212, 208, 200) | 0xff000000u;
            TestBrush classic; classic.data = {BS_SOLID, RGB(212, 208, 200), 0};
            CHECK(f.Fill(&classic) == 1 && T::fills[0].pattern);
            CHECK(f.Fill(&f.face) == 1 && Own(T::fills[1], &f.memory, &f.face));   // 240,240,240 is then a colour like any other
            // ...and the button face it is, not the colour of a window.
            T::colours[COLOR_BTNFACE] = kFace; T::colours[COLOR_WINDOW] = RGB(212, 208, 200);
            CHECK(f.Fill(&classic) == 1 && Own(T::fills[2], &f.memory, &classic));
            CHECK(f.Fill(&f.white) == 1 && Own(T::fills[3], &f.memory, &f.white));
        }
        {
            // The width and the height are the header's, each in full.
            Fixture f; f.Arm();
            const RECT wide{0, 0, 0x7fffffff, 12}, tall{0, 0, 16, 0x7fffffff};
            f.Section(0x7fffffff, 12);
            CHECK(f.Fill(&f.memory, &wide, &f.face) == 1 && T::fills[0].pattern);
            CHECK(f.Fill(&f.face) == 1 && !T::fills[1].pattern);
            f.Section(16, 0x7fffffff);
            CHECK(f.Fill(&f.memory, &tall, &f.face) == 1 && T::fills[2].pattern);
            CHECK(f.Fill(&f.face) == 1 && !T::fills[3].pattern);
            // A size that only agrees in its low bits is another size.
            f.Section(0x10010, 12);
            CHECK(f.Fill(&f.face) == 1 && !T::fills[4].pattern);
            f.Section(16, 0x1000c);
            CHECK(f.Fill(&f.face) == 1 && !T::fills[5].pattern);
        }
    }

    void Gates()
    {
        // Each of these leaves the EXE's own fill: one call of its import with
        // its DC, its RECT and its brush, and no brush of ours.
        for (int which = 0; which < 39; ++which)
        {
            Fixture f; f.Arm(); f.Section(16, 12);
            T::logs.clear();
            RECT rect{0, 0, 16, 12};
            const RECT* given = &rect; HDC dc = &f.memory; HBRUSH brush = &f.face;
            TestBrush almost; almost.data = {BS_SOLID, RGB(240, 240, 241), 0};
            TestBrush tinted; tinted.data = {BS_SOLID, RGB(239, 240, 240), 0};
            TestBrush bitmapBrush; bitmapBrush.data = {BS_PATTERN, kFace, 0};
            TestBrush black; black.data = {BS_SOLID, 0, 0};
            TestBrush stale; stale.data = {BS_SOLID, kFace, 0}; T::live.erase(&stale);  // a handle that is no object any more
            TestDC meta, enhanced; meta.type = OBJ_METADC; enhanced.type = OBJ_ENHMETADC;
            const wchar_t* reason = nullptr;        // null: not a word about it
            switch (which)
            {
            case 0: given = nullptr; break;                             // nothing to compare
            case 1: T::full = false; break;                             // another injection mode
            case 2: dc = &f.windowDc; break;                            // a window's DC: the paint itself
            case 3: dc = &meta; break;
            case 4: dc = &enhanced; break;
            case 5: dc = nullptr; break;
            case 6: brush = &f.white; rect.right = 15; break;           // neither that rectangle nor the window's colour
            case 7: brush = &f.hatched; rect = RECT{4, 4, 8, 8}; break;
            // The window's colour, but not over the whole of the last section from its origin.
            case 8: rect.right = 15; reason = L"size"; break;
            case 9: rect.bottom = 13; reason = L"size"; break;
            case 10: rect.left = 1; reason = L"size"; break;            // all but the first column
            case 11: rect.top = -1; reason = L"size"; break;
            case 12: rect = RECT{1, 0, 17, 12}; reason = L"size"; break;        // the same extent, moved
            case 13: rect = RECT{0, 2, 16, 14}; reason = L"size"; break;
            case 14: rect = RECT{0, 0, 12, 16}; reason = L"size"; break;        // width and height changed places
            // A section that is no rectangle from the origin: top-down (the
            // donor's rectangle has a negative bottom), mirrored, or empty.
            case 15: f.Section(16, -12); reason = L"size"; break;
            case 16: f.Section(16, -12); rect.bottom = -12; reason = L"size"; break;
            case 17: f.Section(-16, 12); reason = L"size"; break;
            case 18: f.Section(-16, 12); rect.right = -16; reason = L"size"; break;
            case 19: f.Section(0, 0); rect = RECT{}; reason = L"size"; break;
            case 20: f.Section(16, 0); rect.bottom = 0; reason = L"size"; break;
            case 21: f.Section(0, 12); rect.right = 0; reason = L"size"; break;
            // The whole of the last section, but not a solid brush of the window's colour.
            case 22: brush = &f.white; reason = L"colour"; break;
            case 23: brush = &almost; reason = L"colour"; break;        // one step away in one channel
            case 24: brush = &tinted; reason = L"colour"; break;
            case 25: brush = &f.hatched; reason = L"colour"; break;     // that colour, hatched
            case 26: brush = &f.hollow; reason = L"colour"; break;
            case 27: brush = &bitmapBrush; reason = L"colour"; break;
            case 28: brush = reinterpret_cast<HBRUSH>(ULONG_PTR(COLOR_BTNFACE + 1)); reason = L"colour"; break; // a system colour index, not a brush
            case 29: brush = reinterpret_cast<HBRUSH>(ULONG_PTR(COLOR_MENUBAR + 1)); reason = L"colour"; break; // the last of them
            case 30: brush = nullptr; reason = L"colour"; break;
            case 31: brush = &stale; reason = L"colour"; break;
            // Where the window is black, what is no brush is still not a black brush.
            case 32: T::colours[COLOR_BTNFACE] = 0; brush = reinterpret_cast<HBRUSH>(ULONG_PTR(COLOR_BTNFACE + 1)); reason = L"colour"; break;
            case 33: T::colours[COLOR_BTNFACE] = 0; brush = &stale; reason = L"colour"; break;
            case 34: T::colours[COLOR_BTNFACE] = 0; brush = &f.face; reason = L"colour"; break;
            // More than the section, or less of it, by a single pixel.
            case 35: rect.right = 17; reason = L"size"; break;
            case 36: rect.bottom = 11; reason = L"size"; break;
            case 37: rect.left = -1; reason = L"size"; break;
            case 38: rect.top = 1; reason = L"size"; break;
            }
            T::fillResult = 5;
            SetLastError(0x60 + which);
            CHECK(f.Fill(dc, given, brush) == 5);
            CHECK(T::fills.size() == 1 && Own(T::fills[0], dc, brush));
            CHECK(T::fills[0].rectPointer == given && T::fills[0].entryError == 0x60u + which);
            CHECK(GetLastError() == 0xc001 && T::patternBrushes == 0 && T::brushDeletes == 0);
            CHECK(T::glassQueries.empty());                             // nobody is asked about glass for it
            if (which < 6) CHECK(T::brushQueries == 0 && T::colourQueries == 0);    // not even looked at
            if (reason) CHECK(T::logs.size() == 1 && T::logs[0].strings == Strings{reason});
            else CHECK(T::logs.empty());
            if (brush == &f.face && dc == &f.memory && given)
                CHECK(IsColour(f.target, (std::max)(rect.left, LONG(0)), (std::max)(rect.top, LONG(0)),
                    (std::min)(rect.right, LONG(16)), (std::min)(rect.bottom, LONG(12)), kFace));
            if (which == 34)
            {
                // ...and a black one is the window's colour there.
                CHECK(f.Fill(&black) == 5 && T::fills[1].pattern);
            }
        }
        {
            // Before any section nothing is the picture's rectangle, an empty fill included.
            Fixture f; f.Arm();
            const RECT empty{};
            CHECK(f.Fill(&f.memory, &empty, &f.face) == 1 && Own(T::fills[0], &f.memory, &f.face));
            CHECK(f.Fill(&f.face) == 1 && Own(T::fills[1], &f.memory, &f.face));
            CHECK(T::patternBrushes == 0);
        }
    }

    void Sizes()
    {
        {
            // Every DIB section the EXE asks for is made by the call it made:
            // the six arguments, the result and the error are its own.
            Fixture f; f.Arm(); T::logs.clear();
            BITMAPINFO info{};
            info.bmiHeader.biWidth = 16; info.bmiHeader.biHeight = 12;
            void* bits = nullptr; int mapping = 0; TestBitmap made(16, 12);
            T::dibResult = &made;
            SetLastError(0x52);
            CHECK(f.Dib(&f.screen, &info, 77, &bits, &mapping, 0x40) == &made);
            CHECK(T::dibs.size() == 1);
            const auto& dib = T::dibs[0];
            CHECK(dib.route == Route::Import && dib.dc == &f.screen && dib.info == &info && dib.usage == 77);
            CHECK(dib.bits == &bits && dib.section == &mapping && dib.offset == 0x40 && dib.entryError == 0x52);
            CHECK(GetLastError() == 0xd001);
            // Nothing is asked, drawn or said for it, and the header is the EXE's.
            CHECK(T::fills.empty() && T::logs.empty() && T::typeQueries == 0 && T::bitmapQueries == 0);
            CHECK(info.bmiHeader.biWidth == 16 && info.bmiHeader.biHeight == 12 && !bits);
            // A section that was refused is passed on as it is.
            T::dibResult = nullptr;
            CHECK(f.Dib(&f.screen, &info) == nullptr && GetLastError() == 0xd002);
        }
        {
            // The last section counts, made or refused: the donor notes it before the call.
            Fixture f; f.Arm();
            f.Section(30, 20); f.Section(16, 12);
            CHECK(f.Fill(&f.face) == 1 && T::fills[0].pattern);
            const RECT earlier{0, 0, 30, 20};
            CHECK(f.Fill(&f.memory, &earlier, &f.face) == 1 && Own(T::fills[1], &f.memory, &f.face));
        }
        {
            // A call without a header changes nothing and is passed on.
            Fixture f; f.Arm(); f.Section(16, 12);
            SetLastError(0x57);
            CHECK(f.Dib(&f.screen, nullptr, 3, nullptr, nullptr, 9) == nullptr);
            CHECK(T::dibs.size() == 2 && T::dibs[1].route == Route::Import && !T::dibs[1].info);
            CHECK(T::dibs[1].usage == 3 && T::dibs[1].offset == 9 && T::dibs[1].entryError == 0x57);
            CHECK(f.Fill(&f.face) == 1 && T::fills[0].pattern);
        }
        {
            // Whatever the bit count, the planes or the DC of the section: only its size is kept.
            Fixture f; f.Arm();
            BITMAPINFO info{};
            info.bmiHeader.biWidth = 16; info.bmiHeader.biHeight = 12; info.bmiHeader.biBitCount = 32;
            f.Dib(nullptr, &info);
            CHECK(f.Fill(&f.face) == 1 && T::fills[0].pattern);
        }
    }

    void Failures()
    {
        {
            // No pattern brush: the EXE's own fill, once.
            Fixture f; f.Arm(); f.Section(16, 12); T::logs.clear();
            T::failPattern = true; T::fillResult = 3;
            SetLastError(0x53);
            CHECK(f.Fill(&f.face) == 3);
            CHECK(T::fills.size() == 1 && Own(T::fills[0], &f.memory, &f.face) && T::fills[0].entryError == 0x53);
            CHECK(GetLastError() == 0xc001 && T::patternBrushes == 1 && T::brushDeletes == 0);
            CHECK(IsColour(f.target, 0, 0, 16, 12, kFace));
            CHECK(T::logs.size() == 1 && T::logs[0].strings == Strings{L"pattern"});
        }
        {
            // The fill with the board fails: its brush is deleted all the same,
            // and the EXE's own fill follows with its own result and error.
            Fixture f; f.Arm(); f.Section(16, 12); T::logs.clear();
            T::failFills = {1}; T::fillResult = 3;
            SetLastError(0x54);
            CHECK(f.Fill(&f.face) == 3);
            CHECK(T::fills.size() == 2 && T::fills[0].pattern && T::fills[0].dc == &f.memory);
            CHECK(Own(T::fills[1], &f.memory, &f.face));
            CHECK(T::fills[0].entryError == 0x54 && T::fills[1].entryError == 0x54);
            CHECK(GetLastError() == 0xc002 && T::patternBrushes == 1 && T::brushDeletes == 1);
            CHECK(IsColour(f.target, 0, 0, 16, 12, kFace));
            CHECK(T::logs.size() == 1 && T::logs[0].strings == Strings{L"fill"} && T::logs[0].numbers.back() == 3);
        }
        {
            // Both fail: the zero and the error are those of the EXE's own fill.
            Fixture f; f.Arm(); f.Section(16, 12);
            T::failFills = {1, 2};
            SetLastError(0x58);
            CHECK(f.Fill(&f.face) == 0);
            CHECK(T::fills.size() == 2 && T::fills[0].pattern && Own(T::fills[1], &f.memory, &f.face));
            CHECK(GetLastError() == 0xc002 && T::brushDeletes == 1);
            CHECK(Untouched(f.target, 0, 0, 16, 12));
        }
        {
            // A refused fill that fails is not made twice.
            Fixture f; f.Arm(); f.Section(16, 12);
            T::failFills = {1};
            CHECK(f.Fill(&f.white) == 0 && T::fills.size() == 1 && GetLastError() == 0xc001);
            T::failFills = {2};
            CHECK(f.Fill(&f.windowDc, nullptr, &f.face) == 0 && T::fills.size() == 2 && GetLastError() == 0xc002);
        }
    }

    void Arming()
    {
        {
            // Before Startup and after Shutdown a paint arms nothing. This block
            // stays first: nothing has started the unit in this process yet.
            Fixture f(16, 12, false);
            f.Arm();
            CHECK(f.Free() && T::enables == 0 && T::ancestors == 0 && T::exclusiveLocks == 0);
            Board::Startup(); f.Arm(); CHECK(f.Held());
            f.Section(16, 12);
            Board::Shutdown();
            CHECK(f.Free() && T::disableAlls == 1);
            f.Arm(); CHECK(f.Free() && T::enables == 2);
            // A restart takes both again and remembers nothing: not the size...
            Board::Startup(); f.Arm(); CHECK(f.Held() && T::enables == 4);
            CHECK(f.Fill(&f.face) == 1 && Own(T::fills[0], &f.memory, &f.face));
            f.Section(16, 12);
            CHECK(f.Fill(&f.face) == 1 && T::fills[1].pattern);
        }
        {
            // Nothing is held until a viewer paints on glass: the EXE's calls go
            // straight to user32 and gdi32, and nobody is asked anything.
            Fixture f;
            CHECK(f.Free() && T::enables == 0);
            f.Section(16, 12);
            CHECK(f.Fill(&f.face) == 1);
            CHECK(T::fills.size() == 1 && Own(T::fills[0], &f.memory, &f.face));
            CHECK(T::typeQueries == 0 && T::logs.empty() && T::ancestors == 0);
            f.Arm();
            // Both imports of the EXE now lead to the unit, one reference each.
            CHECK(f.Held() && T::enables == 2 && T::disables == 0 && T::ancestors == 1);
            // The section the EXE asked for before that is not known...
            CHECK(f.Fill(&f.face) == 1 && Own(T::fills[1], &f.memory, &f.face));
            // ...the next one is.
            f.Section(16, 12);
            CHECK(f.Fill(&f.face) == 1 && T::fills[2].pattern);
            // Once: later paints take nothing more, and do not wait for the lock.
            const int locks = T::exclusiveLocks;
            f.Arm(); Board::Arm(&f.tree); Board::Arm(&f.dialogViewer);
            CHECK(f.Held() && T::enables == 2 && T::exclusiveLocks == locks);
        }
        // Without both imports nothing is held - one without the other has
        // nothing to compare - and nothing is asked for again in this start.
        for (int which = 0; which < 7; ++which)
        {
            Fixture f;
            switch (which)
            {
            case 0: f.exe.imports.erase("FillRect"); break;
            case 1: f.exe.imports.erase("CreateDIBSection"); break;
            case 2: f.exe.imports.clear(); break;
            case 3: T::registrationFailures[&f.exe].insert("FillRect"); break;
            case 4: T::registrationFailures[&f.exe].insert("CreateDIBSection"); break;
            case 5: f.exe.imports["FillRect"].dll = "gdi32.dll"; break;             // not where FillRect lives
            case 6: f.exe.imports["CreateDIBSection"].dll = "user32.dll"; break;
            }
            f.Arm();
            CHECK(f.Free());
            // Both were asked for; only the one that was taken is given back.
            CHECK(T::enables == 2 && T::disables == (which == 2 ? 0 : 1));
            const int enables = T::enables, disables = T::disables, locks = T::exclusiveLocks;
            // Even if the import were there by then.
            f.exe.imports = Fixture::Imports(); T::registrationFailures.clear();
            f.Arm(); Board::Arm(&f.tree);
            CHECK(f.Free() && T::enables == enables && T::disables == disables && T::exclusiveLocks == locks);
            // The EXE's fills and sections are its own.
            f.Section(16, 12);
            CHECK(f.Fill(&f.face) == 1 && Own(T::fills[0], &f.memory, &f.face) && T::typeQueries == 0);
            // A new start asks again.
            Board::Shutdown(); Board::Startup(); f.Arm();
            CHECK(f.Held());
        }
        {
            // The DLL names an import may carry: user32 and gdi32 and their API sets.
            for (const char* dll : {"user32.dll", "ext-ms-win-ntuser-draw-l1-1-0.dll", "ext-ms-win-ntuser-misc-l1-1-0.dll"})
            {
                Fixture f; f.exe.imports["FillRect"].dll = dll;
                f.Arm(); CHECK(f.Held());
            }
            for (const char* dll : {"gdi32.dll", "gdi32full.dll", "ext-ms-win-gdi-desktop-l1-1-0.dll"})
            {
                Fixture f; f.exe.imports["CreateDIBSection"].dll = dll;
                f.Arm(); CHECK(f.Held());
            }
        }
        {
            // ...and not the window: armed again by a viewer without one, nobody answers for the glass.
            Fixture f; Board::Arm(&f.dialogViewer);
            Board::Shutdown(); Board::Startup();
            Board::Arm(nullptr); CHECK(f.Held());
            f.Section(16, 12);
            CHECK(f.Fill(&f.face) == 1 && Own(T::fills[0], &f.memory, &f.face));
            CHECK(T::glassQueries == Windows{nullptr});
        }
        {
            // Two paints at once: the one that waited for the lock finds the
            // imports already asked for and takes nothing more.
            Fixture f;
            T::beforeExclusive = [&] { Board::Arm(&f.tree); };
            f.Arm();
            CHECK(f.Held() && T::enables == 2 && T::logs.size() == 1);
        }
        {
            // A paint that waited for the lock while the unit was shut down takes nothing.
            Fixture f;
            T::beforeExclusive = [] { Board::Shutdown(); };
            f.Arm();
            CHECK(f.Free() && T::enables == 0 && T::logs.empty());
            // ...and leaves the next start free to arm.
            Board::Startup(); f.Arm();
            CHECK(f.Held());
        }
        {
            // Whatever a paint that raced with Shutdown left behind, a start begins with nothing.
            Fixture f; f.Arm(); f.Section(16, 12);
            Board::Shutdown();
            Board::g_root.store(&f.dialog);
            Board::g_lastDib.store((std::uint64_t(12) << 32) | 16u);
            Board::Startup();
            Board::Arm(nullptr);
            CHECK(f.Held() && f.Fill(&f.face) == 1 && Own(T::fills[0], &f.memory, &f.face));
            CHECK(T::glassQueries.empty() && T::logs.back().strings == Strings{L"size"});
            f.Section(16, 12);
            CHECK(f.Fill(&f.face) == 1 && Own(T::fills[1], &f.memory, &f.face) && T::glassQueries == Windows{nullptr});
        }
        {
            // Without the module of the EXE there is nothing to hold, and the next paint may still arm.
            Fixture f; GlassCore::ModuleHandler::g_entryPointModule = nullptr;
            f.Arm();
            CHECK(T::enables == 0 && T::logs.empty());
            GlassCore::ModuleHandler::g_entryPointModule = &f.exe;
            f.Arm(); CHECK(f.Held());
        }
        {
            // Startup is idempotent: a second one forgets nothing of a running start.
            Fixture f; f.Arm(); f.Section(16, 12);
            Board::Startup();
            CHECK(f.Held() && T::enables == 2);
            CHECK(f.Fill(&f.face) == 1 && T::fills[0].pattern && T::glassQueries == Windows{&f.form});
            // So is Shutdown, armed or not.
            Board::Shutdown(); Board::Shutdown();
            CHECK(f.Free() && T::disables == 0);
            Board::Startup(); Board::Shutdown();
            CHECK(f.Free());
        }
        {
            // References the dispatcher already had are not ours to take or give back.
            Fixture f;
            Board::g_dispatcher.moduleAddress = &f.exe;
            { auto lock = Board::g_lock.lock_exclusive(); Board::g_dispatcher.EnableHook(0, true); }
            f.exe.imports.erase("CreateDIBSection");
            f.Arm();
            // FillRect got a second reference and gave it back; the first one stays.
            CHECK(Board::g_dispatcher.GetRef(0) == 1 && Board::g_dispatcher.GetRef(1) == 0 && f.exe.refs["FillRect"] == 1);
        }
    }

    void Glass()
    {
        {
            // A picture is composed with no window to ask, so the top-level
            // window of the viewer that painted answers for the glass - and only
            // where a fill would become a board.
            Fixture f; f.Arm(); f.Section(16, 12);
            CHECK(T::glassQueries.empty());
            CHECK(f.Fill(&f.face) == 1 && T::fills[0].pattern);
            CHECK(T::glassQueries == Windows{&f.form});
            // Its backdrop was taken away: the EXE's own fill.
            f.form.mark = 1; T::logs.clear();
            SetLastError(0x59);
            CHECK(f.Fill(&f.face) == 1 && Own(T::fills[1], &f.memory, &f.face) && T::fills[1].entryError == 0x59);
            CHECK(T::patternBrushes == 1 && GetLastError() == 0xc002);
            CHECK(T::logs.size() == 1 && T::logs[0].strings == Strings{L"glass"});
            // ...and given back.
            f.form.mark = 2;
            CHECK(f.Fill(&f.face) == 1 && T::fills[2].pattern);
            // Glass is off for the whole process.
            T::rendering = false;
            CHECK(f.Fill(&f.face) == 1 && Own(T::fills[3], &f.memory, &f.face));
            f.form.mark = 0;
            CHECK(f.Fill(&f.face) == 1 && Own(T::fills[4], &f.memory, &f.face));
            T::rendering = true;
            CHECK(f.Fill(&f.face) == 1 && T::fills[5].pattern);     // a root without a mark follows the process
            CHECK((T::glassQueries == Windows{&f.form, &f.form, &f.form, &f.form, &f.form, &f.form}));
        }
        {
            // Every such paint names its window anew, armed long ago or not.
            Fixture f; f.Arm(); f.Section(16, 12);
            Board::Arm(&f.dialogViewer);
            f.dialog.mark = 1;
            CHECK(f.Fill(&f.face) == 1 && Own(T::fills[0], &f.memory, &f.face));
            CHECK(T::glassQueries == Windows{&f.dialog});
            Board::Arm(&f.tree);
            CHECK(f.Fill(&f.face) == 1 && T::fills[1].pattern);
            CHECK((T::glassQueries == Windows{&f.dialog, &f.form}));
            // A top-level viewer is its own window.
            Board::Arm(&f.dialog);
            CHECK(f.Fill(&f.face) == 1 && Own(T::fills[2], &f.memory, &f.face) && T::glassQueries.back() == &f.dialog);
        }
        {
            // A window that is gone has no mark: the process answers alone.
            Fixture f; Board::Arm(&f.dialogViewer); f.Section(16, 12);
            f.dialog.mark = 1; f.dialog.alive = f.dialogViewer.alive = false;
            CHECK(f.Fill(&f.face) == 1 && T::fills[0].pattern);
            T::rendering = false;
            CHECK(f.Fill(&f.face) == 1 && Own(T::fills[1], &f.memory, &f.face));
        }
        {
            // A viewer that is no window any more names nothing: the earlier answer stays.
            Fixture f; f.Arm(); f.Section(16, 12);
            f.dialogViewer.alive = false; f.dialog.mark = 1;
            Board::Arm(&f.dialogViewer); Board::Arm(nullptr);
            CHECK(f.Fill(&f.face) == 1 && T::fills[0].pattern && T::glassQueries == Windows{&f.form});
        }
        {
            // Armed by a viewer without a window: both imports are held, but nobody answers.
            Fixture f; Board::Arm(nullptr);
            CHECK(f.Held());
            f.Section(16, 12); T::logs.clear();
            CHECK(f.Fill(&f.face) == 1 && Own(T::fills[0], &f.memory, &f.face));
            CHECK(T::glassQueries == Windows{nullptr} && T::logs[0].strings == Strings{L"glass"});
            f.Arm();
            CHECK(f.Fill(&f.face) == 1 && T::fills[1].pattern);
        }
        {
            // Unless the unit is running, a paint names nothing.
            Fixture f; f.Arm();
            Board::g_shutdown.store(true);
            Board::Arm(&f.dialogViewer);
            Board::g_shutdown.store(false);
            f.dialog.mark = 1; f.Section(16, 12);
            CHECK(f.Fill(&f.face) == 1 && T::fills[0].pattern && T::glassQueries == Windows{&f.form});
        }
    }

    void Isolation()
    {
        {
            // A fill made inside the EXE's fill - another hook of the chain,
            // GDI calling back - is judged on its own.
            Fixture f; f.Arm(); f.Section(16, 12);
            TestBitmap inner(16, 12); TestDC innerDc; innerDc.bitmap = &inner;
            const RECT rect{0, 0, 16, 12};
            int innerResult = 0;
            T::duringFill = [&] { innerResult = f.Fill(&innerDc, &rect, &f.white); };
            CHECK(f.Fill(&f.face) == 1 && innerResult == 1);
            CHECK(T::fills.size() == 2 && T::fills[0].pattern && Own(T::fills[1], &innerDc, &f.white));
            CHECK(IsBoard(f.target, 0, 0, 16, 12) && IsColour(inner, 0, 0, 16, 12, kWhite));
            CHECK(T::patternBrushes == 1 && T::brushDeletes == 1);
            // ...and so is a board inside a board.
            T::duringFill = [&] { innerResult = f.Fill(&innerDc, &rect, &f.face); };
            CHECK(f.Fill(&f.face) == 1 && innerResult == 1);
            CHECK(T::fills.size() == 4 && T::fills[2].pattern && T::fills[3].pattern && T::fills[2].brush != T::fills[3].brush);
            CHECK(IsBoard(inner, 0, 0, 16, 12) && T::patternBrushes == 3 && T::brushDeletes == 3);
        }
        {
            // A section asked for inside the fill changes the next fill, not this one.
            Fixture f; f.Arm(); f.Section(16, 12);
            T::duringFill = [&] { f.Section(3, 3); };
            CHECK(f.Fill(&f.face) == 1 && T::fills[0].pattern && IsBoard(f.target, 0, 0, 16, 12));
            CHECK(f.Fill(&f.face) == 1 && Own(T::fills[1], &f.memory, &f.face));
        }
        {
            // The gate closed with both imports still in place: the EXE's calls
            // pass through them untouched, and no size is noted.
            Fixture f; f.Arm(); f.Section(16, 12);
            Board::g_shutdown.store(true);
            SetLastError(0x5a);
            CHECK(f.Fill(&f.face) == 1 && Own(T::fills[0], &f.memory, &f.face) && T::fills[0].entryError == 0x5a);
            CHECK(T::typeQueries == 0 && GetLastError() == 0xc001);
            f.Section(20, 20);
            CHECK(T::dibs.size() == 2 && T::dibs[1].route == Route::Import);
            Board::g_shutdown.store(false);
            CHECK(f.Fill(&f.face) == 1 && T::fills[1].pattern);     // 16 by 12 is still the last one known
        }
        {
            // After Shutdown a detour that is still reached - a chain that could
            // not be undone - calls user32 and gdi32 itself: once, as it was called.
            Fixture f; f.Arm(); f.Section(16, 12);
            Board::Shutdown();
            const RECT rect{0, 0, 16, 12};
            T::fillResult = 4;
            SetLastError(0x55);
            CHECK(Board::BoardFill(&f.memory, &rect, &f.face) == 4);
            CHECK(T::fills.size() == 1 && T::fills[0].route == Route::Direct && T::fills[0].dc == &f.memory);
            CHECK(T::fills[0].rectPointer == &rect && T::fills[0].brush == &f.face && !T::fills[0].pattern);
            CHECK(T::fills[0].entryError == 0x55 && GetLastError() == 0xc001 && T::typeQueries == 0);
            BITMAPINFO info{};
            info.bmiHeader.biWidth = 16; info.bmiHeader.biHeight = 12;
            void* bits = nullptr; int mapping = 0; TestBitmap made(16, 12);
            T::dibResult = &made;
            SetLastError(0x56);
            CHECK(Board::NoteDib(&f.screen, &info, 1, &bits, &mapping, 2) == &made);
            CHECK(T::dibs.size() == 2 && T::dibs[1].route == Route::Direct && T::dibs[1].dc == &f.screen);
            CHECK(T::dibs[1].info == &info && T::dibs[1].usage == 1 && T::dibs[1].bits == &bits);
            CHECK(T::dibs[1].section == &mapping && T::dibs[1].offset == 2 && T::dibs[1].entryError == 0x56);
            CHECK(GetLastError() == 0xd002);
            // Nothing was noted: restarted, that size is not known.
            Board::Startup(); f.Arm();
            CHECK(f.Fill(&f.face) == 4 && Own(T::fills[1], &f.memory, &f.face));
        }
        {
            // Restarted but not armed, such a detour has no import to call: it
            // lays no board, whatever it was given.
            Fixture f; f.Arm();
            Board::Shutdown(); Board::Startup();
            BITMAPINFO info{};
            info.bmiHeader.biWidth = 16; info.bmiHeader.biHeight = 12;
            Board::NoteDib(&f.screen, &info, 0, nullptr, nullptr, 0);
            const RECT rect{0, 0, 16, 12};
            SetLastError(0x5b);
            CHECK(Board::BoardFill(&f.memory, &rect, &f.face) == 1);
            CHECK(T::dibs.size() == 1 && T::dibs[0].route == Route::Direct);
            CHECK(T::fills.size() == 1 && T::fills[0].route == Route::Direct && T::fills[0].brush == &f.face);
            CHECK(T::fills[0].entryError == 0x5b && T::patternBrushes == 0 && T::typeQueries == 0);
        }
    }

    void Diagnostics()
    {
        {
            // Armed: one line, with what was taken.
            Fixture f; f.Arm();
            CHECK(T::logs.size() == 1 && T::logs[0].format == kArmLine && T::logs[0].level == DBG_NORMAL);
            CHECK((T::logs[0].numbers == Numbers{Number(&f.exe), Number(&f.form), 1, 1, 1}) && T::logs[0].strings.empty());
            f.Arm(); Board::Arm(&f.tree);
            CHECK(T::logs.size() == 1);
            // A board: the DC, the rectangle, the last section, the brush it replaced, the bitmap it went into.
            f.Section(16, 12);
            T::fillResult = 9;
            CHECK(f.Fill(&f.face) == 9 && GetLastError() == 0xc001);
            CHECK(T::logs.size() == 2 && T::logs[1].format == kFillLine && T::logs[1].level == DBG_NORMAL);
            CHECK(T::logs[1].strings == Strings{L"none"});
            CHECK((T::logs[1].numbers == Numbers{Number(&f.memory), 0, 0, 16, 12, 16, 12, Number(&f.face), 0, 0xF0F0F0,
                Number(&f.target), 24, 16, 12, 1, Number(&f.form), 1, 9}));
        }
        {
            // A refusal says why, and what the brush and the bitmap were.
            Fixture f(20, 14); f.Arm(); f.Section(16, 12); T::logs.clear();
            TestBitmap device(20, 14, 32, false); TestDC deviceDc; deviceDc.bitmap = &device;
            TestDC bare;                                    // a memory DC with nothing we can read selected
            const RECT moved{2, 3, 18, 15}, whole{0, 0, 16, 12};
            CHECK(f.Fill(&deviceDc, &moved, &f.face) == 1);
            CHECK(T::logs.size() == 1 && T::logs[0].format == kFillLine && T::logs[0].strings == Strings{L"size"});
            CHECK((T::logs[0].numbers == Numbers{Number(&deviceDc), 2, 3, 18, 15, 16, 12, Number(&f.face), 0, 0xF0F0F0,
                Number(&device), 32, 20, 14, 0, Number(&f.form), 0, 1}));
            f.hatched.data.lbColor = RGB(1, 2, 3);
            CHECK(f.Fill(&bare, &whole, &f.hatched) == 1);
            CHECK(T::logs.size() == 2 && T::logs[1].strings == Strings{L"colour"});
            CHECK((T::logs[1].numbers == Numbers{Number(&bare), 0, 0, 16, 12, 16, 12, Number(&f.hatched), 2, 0x030201,
                0, 0, 0, 0, 0, Number(&f.form), 0, 1}));
            // A system colour index in the brush's place has no style and no colour to tell.
            const HBRUSH index = reinterpret_cast<HBRUSH>(ULONG_PTR(COLOR_WINDOW + 1));
            CHECK(f.Fill(&f.memory, &whole, index) == 1);
            CHECK(T::logs.size() == 3 && T::logs[2].strings == Strings{L"colour"});
            CHECK((T::logs[2].numbers == Numbers{Number(&f.memory), 0, 0, 16, 12, 16, 12, COLOR_WINDOW + 1, -1, 0,
                Number(&f.target), 24, 20, 14, 1, Number(&f.form), 0, 1}));
            // The colour is told without its fourth byte.
            TestBrush flagged; flagged.data = {BS_SOLID, 0x02112233u, 0};
            CHECK(f.Fill(&f.memory, &whole, &flagged) == 1);
            CHECK(T::logs.size() == 4 && T::logs[3].numbers[8] == 0 && T::logs[3].numbers[9] == 0x112233);
            // A top-down section is told as it was asked for.
            f.Section(16, -12);
            CHECK(f.Fill(&f.memory, &whole, &f.face) == 1);
            CHECK(T::logs.size() == 5 && T::logs[4].strings == Strings{L"size"});
            CHECK(T::logs[4].numbers[5] == 16 && T::logs[4].numbers[6] == -12);
        }
        // Without both imports: a warning, with which one was there.
        for (int which = 0; which < 3; ++which)
        {
            Fixture f;
            if (which != 1) f.exe.imports.erase("FillRect");
            if (which != 0) f.exe.imports.erase("CreateDIBSection");
            Board::Arm(&f.dialogViewer);
            CHECK(T::logs.size() == 1 && T::logs[0].format == kArmLine && T::logs[0].level == DBG_WARNING);
            CHECK((T::logs[0].numbers == Numbers{Number(&f.exe), Number(&f.dialog), which == 1, which == 0, 0}));
            f.Arm();
            CHECK(T::logs.size() == 1);
        }
        {
            // Sixteen lines a second of each kind, counted apart: boards, the
            // window's colour elsewhere, other brushes over the section, and
            // what was refused or failed after both matched.
            Fixture f; f.Arm(); f.Section(16, 12); T::logs.clear();
            const RECT elsewhere{0, 0, 5, 5};
            const auto count = [](const wchar_t* reason) {
                return std::count_if(T::logs.begin(), T::logs.end(),
                    [&](const T::Log& log) { return log.strings == Strings{reason}; });
            };
            for (int i = 0; i < 20; ++i)
            {
                CHECK(f.Fill(&f.face) == 1);
                CHECK(f.Fill(&f.memory, &elsewhere, &f.face) == 1);
                CHECK(f.Fill(&f.white) == 1);
                f.form.mark = 1; CHECK(f.Fill(&f.face) == 1); f.form.mark = 2;
            }
            CHECK(count(L"none") == 16 && count(L"size") == 16 && count(L"colour") == 16 && count(L"glass") == 16);
            CHECK(T::logs.size() == 64 && T::fills.size() == 80);
            // The three ends of the last kind share one budget.
            T::failPattern = true; CHECK(f.Fill(&f.face) == 1); T::failPattern = false;
            T::failFills = {static_cast<int>(T::fills.size()) + 1}; CHECK(f.Fill(&f.face) == 1);
            CHECK(T::logs.size() == 64);
            // A second later each has its budget again, and not a moment before.
            T::clock += 999;
            CHECK(f.Fill(&f.face) == 1 && T::logs.size() == 64);
            T::clock += 1;
            T::failPattern = true; CHECK(f.Fill(&f.face) == 1); T::failPattern = false;
            T::failFills = {static_cast<int>(T::fills.size()) + 1}; CHECK(f.Fill(&f.face) == 1);
            CHECK(f.Fill(&f.face) == 1);
            CHECK(T::logs.size() == 67 && count(L"pattern") == 1 && count(L"fill") == 1 && count(L"none") == 17);
            // Lines that are not written cost no queries either.
            T::clock += 1000; T::logs.clear();
            for (int i = 0; i < 16; ++i) CHECK(f.Fill(&f.face) == 1);
            const int queries = T::bitmapQueries;
            CHECK(f.Fill(&f.face) == 1 && T::bitmapQueries == queries && T::logs.size() == 16);
        }
        {
            // What passes without a word says nothing at all.
            Fixture f; f.Arm(); f.Section(16, 12); T::logs.clear();
            const RECT elsewhere{0, 0, 5, 5};
            CHECK(f.Fill(&f.windowDc, &elsewhere, &f.face) == 1 && f.Fill(&f.memory, &elsewhere, &f.white) == 1);
            CHECK(f.Fill(&f.memory, nullptr, &f.face) == 1);
            f.Section(8, 8);
            CHECK(T::logs.empty());
        }
    }
}

int main(int argc, char** argv)
{
    try
    {
        if (argc != 2) throw std::runtime_error("one case required");
        const std::string name = argv[1];
        if (name == "board") BoardCase();
        else if (name == "gates") Gates();
        else if (name == "sizes") Sizes();
        else if (name == "failures") Failures();
        else if (name == "arming") Arming();
        else if (name == "glass") Glass();
        else if (name == "isolation") Isolation();
        else if (name == "diagnostics") Diagnostics();
        else throw std::runtime_error("unknown case");
        std::cout << "PASS " << name << '\n';
    }
    catch (const std::exception& e) { std::cerr << argv[1] << ": " << e.what() << '\n'; return 1; }
}
