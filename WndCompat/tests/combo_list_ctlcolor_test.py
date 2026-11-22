#!/usr/bin/env python3
"""Compile the complete production ComboBoxSubclass.cpp with Win32 doubles.

Exercise the real subclass proc/attach/detach, not a copied CTLCOLOR policy.
These argument/state/lifecycle tests do not validate Windows GDI/DWM or MSVC ABI.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CPP = r'''
#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#define WINAPI
#define CALLBACK
using BOOL=int; using UINT=unsigned; using DWORD=std::uint32_t; using COLORREF=DWORD;
using UINT_PTR=std::uintptr_t; using DWORD_PTR=std::uintptr_t; using WPARAM=std::uintptr_t;
using LPARAM=std::intptr_t; using LRESULT=std::intptr_t; using ULONGLONG=std::uint64_t;
using LPCWSTR=const wchar_t*; using HGDIOBJ=void*;
constexpr BOOL TRUE=1,FALSE=0;
constexpr UINT WM_CTLCOLOREDIT=0x133,WM_CTLCOLORLISTBOX=0x134,WM_CTLCOLORSTATIC=0x138;
constexpr UINT WM_LBUTTONDOWN=0x201,WM_NCDESTROY=0x82,CB_GETDROPPEDSTATE=0x157;
constexpr int COLOR_WINDOW=5,BLACK_BRUSH=4,BS_SOLID=0,BS_NULL=1,BS_PATTERN=3,DBG_NORMAL=1;
constexpr COLORREF CLR_INVALID=0xffffffffu;
struct RECT { int left{},top{},right{},bottom{}; bool operator==(const RECT&) const=default; };
struct LOGBRUSH { UINT lbStyle{}; COLORREF lbColor{}; std::uintptr_t lbHatch{}; };
using HBRUSH=LOGBRUSH*;
struct Window; struct DC;
using HWND=Window*; using HDC=DC*;
using SUBCLASSPROC=LRESULT(CALLBACK*)(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
namespace GlassShared { enum class GlassControlKind { None,ComboBox,ComboLBox,Edit }; }
struct Window {
    std::wstring cls=L"TComboBox";
    HWND parent{}; bool valid=true,backdrop=true,attached=false;
    GlassShared::GlassControlKind kind=GlassShared::GlassControlKind::None;
    SUBCLASSPROC proc{};
};
struct DC {
    HWND owner{}; COLORREF bk=0xffffff,text=0; int mode=2,font=81;
    RECT clip{2,3,91,38}; std::vector<RECT> holes{{12,8,17,13}};
};
struct COMBOBOXINFO {
    DWORD cbSize{}; RECT rcItem{},rcButton{}; DWORD stateButton{};
    HWND hwndCombo{},hwndItem{},hwndList{};
};
namespace Shared { enum class InjectionMode { None,PopupOnly,Full }; }
namespace Mock {
    inline Shared::InjectionMode mode=Shared::InjectionMode::Full;
    inline bool shutdown=false,failInfo=false,failObject=false,failStock=false,failBkRead=false,failBkSet=false;
    inline bool dropped=false;
    inline COLORREF windowColor=0xffffff,nativeBk=0xffffff,nativeText=0x123456;
    inline thread_local DWORD error=0;
    inline DWORD originalError=67,entryError=0x1234;
    inline LRESULT originalResult{};
    inline ULONGLONG clock=100;
    inline int sets=0,removes=0,invalidates=0,signals=0,notices=0;
    inline int setBkCalls=0,objectCalls=0,stockCalls=0;
    inline LOGBRUSH black{BS_SOLID,0,0};
    inline std::set<void*> knownBrushes{&black};
    inline std::map<HWND,COMBOBOXINFO> infos;
    struct Call { HWND hwnd; UINT msg; WPARAM wp; LPARAM lp; DWORD entry; };
    inline std::vector<Call> calls;
    inline std::vector<std::wstring> logs;
    inline std::function<void(HWND,UINT)> duringNative;
}
DWORD GetLastError() { return Mock::error; }
void SetLastError(DWORD value) { Mock::error=value; }
BOOL IsWindow(HWND w) { SetLastError(101); return w&&w->valid; }
BOOL GetComboBoxInfo(HWND w,COMBOBOXINFO* out) {
    assert(out&&out->cbSize==sizeof(COMBOBOXINFO)); SetLastError(102);
    if (Mock::failInfo||!Mock::infos.count(w)) return FALSE;
    *out=Mock::infos.at(w); return TRUE;
}
COLORREF GetBkColor(HDC dc) { SetLastError(103); return dc&&!Mock::failBkRead ? dc->bk : CLR_INVALID; }
COLORREF SetBkColor(HDC dc,COLORREF color) {
    ++Mock::setBkCalls; SetLastError(104);
    return dc&&!Mock::failBkSet ? std::exchange(dc->bk,color) : CLR_INVALID;
}
COLORREF GetSysColor(int index) { assert(index==COLOR_WINDOW); SetLastError(105); return Mock::windowColor; }
int GetObjectW(HGDIOBJ object,int bytes,void* out) {
    ++Mock::objectCalls; SetLastError(106);
    if (Mock::failObject||!Mock::knownBrushes.count(object)) return 0;
    assert(bytes==sizeof(LOGBRUSH)); *static_cast<LOGBRUSH*>(out)=*static_cast<HBRUSH>(object); return bytes;
}
HGDIOBJ GetStockObject(int index) {
    assert(index==BLACK_BRUSH); ++Mock::stockCalls; SetLastError(107);
    return Mock::failStock ? nullptr : &Mock::black;
}
HWND GetFocus() { return nullptr; }
ULONGLONG GetTickCount64() { SetLastError(108); return Mock::clock; }
HWND GetParent(HWND w) { return w ? w->parent : nullptr; }
DWORD GetCurrentThreadId() { return 9; }
DWORD GetWindowThreadProcessId(HWND,DWORD*) { return 9; }
LRESULT SendMessageW(HWND,UINT msg,WPARAM,LPARAM) { assert(msg==CB_GETDROPPEDSTATE); return Mock::dropped; }
BOOL PostMessageW(HWND,UINT,WPARAM,LPARAM) { return TRUE; }
BOOL InvalidateRect(HWND,const RECT*,BOOL) { ++Mock::invalidates; return TRUE; }
BOOL SetWindowSubclass(HWND w,SUBCLASSPROC proc,UINT_PTR id,DWORD_PTR) {
    assert(id==2); w->proc=proc; ++Mock::sets; return TRUE;
}
BOOL RemoveWindowSubclass(HWND w,SUBCLASSPROC,UINT_PTR id) {
    assert(id==2); w->proc=nullptr; ++Mock::removes; return TRUE;
}
LRESULT DefSubclassProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    Mock::calls.push_back({hwnd,msg,wp,lp,GetLastError()});
    const LRESULT result=Mock::originalResult;
    if ((msg==WM_CTLCOLORLISTBOX||msg==WM_CTLCOLOREDIT||msg==WM_CTLCOLORSTATIC)&&wp) {
        auto* dc=reinterpret_cast<HDC>(wp); dc->bk=Mock::nativeBk; dc->text=Mock::nativeText; dc->mode=2;
    }
    if (Mock::duringNative) Mock::duringNative(hwnd,msg);
    SetLastError(Mock::originalError); return result;
}
void DebugLog(LPCWSTR,...) { SetLastError(109); }
void DebugLog(int,LPCWSTR format,...) {
    wchar_t line[2048]{}; va_list args; va_start(args,format);
    const int length=std::vswprintf(line,2048,format,args); va_end(args);
    assert(length>=0); Mock::logs.emplace_back(line); SetLastError(110);
}
namespace GlassShared {
    bool GlassIsBackdropAllowedForHwnd(HWND w) { SetLastError(111); return w&&w->valid&&w->backdrop; }
    GlassControlKind GlassGetWindowKind(HWND w) { return w ? w->kind : GlassControlKind::None; }
    void GlassMarkWindow(HWND w,GlassControlKind kind) { w->kind=kind; }
    void GlassUnmarkWindow(HWND w) { w->kind=GlassControlKind::None; }
}
namespace WndCompat {
    constexpr UINT WM_GLASS_DETACH_COMBOBOX=0x8204;
    bool IsShuttingDown() { return Mock::shutdown; }
    Shared::InjectionMode WinEventHook_GetMode() { return Mock::mode; }
    bool IsWindowClass(HWND w,LPCWSTR cls) {
        SetLastError(112); return w&&w->valid&&::wcscasecmp(w->cls.c_str(),cls)==0;
    }
    bool ComboLBox_IsAttached(HWND w) { SetLastError(113); return w&&w->attached; }
    void RegisterThemedChromeClientWithGlassCore(HWND) {}
    void UnregisterThemedChromeClientWithGlassCore(HWND) {}
    void WinEventHook_SignalDetachComplete() { ++Mock::signals; }
    void WinEventHook_RegisterPendingDetach() {}
    namespace DropDown {
        struct Provider { void(*OnComboBoxDropDown)(HWND); };
        void Notice(HWND) { ++Mock::notices; }
        Provider* GetDropDownEventProvider() { static Provider p{Notice}; return &p; }
    }
}
#include "WndCompat/Popup/ComboLBox/ComboBoxSubclass.cpp"

#define CHECK(x) do { if(!(x)) throw std::runtime_error(std::string(__func__)+": "+#x); } while(false)
namespace {
    struct Context {
        Window combo,list,edit,other;
        DC dc,memory;
        LOGBRUSH brush{BS_SOLID,0xffffff,0};
        Context() {
            Mock::mode=Shared::InjectionMode::Full; Mock::shutdown=false;
            Mock::failInfo=Mock::failObject=Mock::failStock=Mock::failBkRead=Mock::failBkSet=false;
            Mock::nativeBk=Mock::windowColor=0xffffff; Mock::nativeText=0x123456;
            Mock::originalError=67; Mock::duringNative={}; Mock::dropped=false;
            Mock::clock=100; Mock::infos.clear(); Mock::knownBrushes={&Mock::black,&brush};
            list.cls=L"ComboLBox"; list.attached=true; list.kind=GlassShared::GlassControlKind::ComboLBox;
            edit.cls=L"Edit"; edit.parent=&combo; dc.owner=&list;
            Mock::infos[&combo]={sizeof(COMBOBOXINFO),{},{},0,&combo,&edit,&list};
            Mock::originalResult=reinterpret_cast<LRESULT>(&brush);
            WndCompat::ComboBox_Attach(&combo);
            CHECK(combo.proc==ComboBoxSubclassProc && combo.kind==GlassShared::GlassControlKind::ComboBox);
            ResetTrace();
        }
        ~Context() {
            Mock::duringNative={}; Mock::shutdown=false; WndCompat::ComboBox_DetachAll();
            assert(g_comboBoxWindows.empty()); Mock::knownBrushes.erase(&brush);
        }
        void ResetTrace() {
            Mock::calls.clear(); Mock::logs.clear(); Mock::setBkCalls=Mock::objectCalls=Mock::stockCalls=0;
            Mock::removes=Mock::signals=Mock::notices=0;
        }
        LRESULT Call(UINT msg=WM_CTLCOLORLISTBOX,HDC target=nullptr,HWND child=nullptr) {
            SetLastError(Mock::entryError);
            return ComboBoxSubclassProc(&combo,msg,reinterpret_cast<WPARAM>(target ? target : &dc),
                reinterpret_cast<LPARAM>(child ? child : &list),2,0);
        }
        void NativeResult(LRESULT result) {
            CHECK(result==Mock::originalResult && dc.bk==Mock::nativeBk && Mock::setBkCalls==0);
            CHECK(Mock::calls.size()==1 && Mock::calls[0].entry==Mock::entryError);
            CHECK(GetLastError()==Mock::originalError);
        }
        void GlassResult(LRESULT result,HDC target=nullptr) {
            HDC output=target ? target : &dc;
            CHECK(result==reinterpret_cast<LRESULT>(&Mock::black) && output->bk==0);
            CHECK(output->text==Mock::nativeText && output->mode==2 && output->font==81);
            CHECK(Mock::setBkCalls==1 && Mock::calls.size()==1);
            CHECK(Mock::calls[0].entry==Mock::entryError && GetLastError()==Mock::originalError);
            CHECK(Mock::calls[0].hwnd==&combo && Mock::calls[0].msg==WM_CTLCOLORLISTBOX);
            CHECK(Mock::calls[0].wp==reinterpret_cast<WPARAM>(output));
            CHECK(Mock::calls[0].lp==reinterpret_cast<LPARAM>(&list));
        }
    };
    void DefaultList() {
        Context c; const RECT clip=c.dc.clip; const auto holes=c.dc.holes;
        c.GlassResult(c.Call()); CHECK(c.dc.clip==clip && c.dc.holes==holes);
    }
    void MemoryAndCurrentSystemColour() {
        Context c; c.brush.lbColor=Mock::nativeBk=Mock::windowColor=0x443322;
        c.combo.cls=L"tcombobox"; c.GlassResult(c.Call(WM_CTLCOLORLISTBOX,&c.memory),&c.memory);
        c.ResetTrace(); c.brush.lbColor=Mock::nativeBk=0xffffff; c.NativeResult(c.Call());
    }
    void ExactClasses() {
        Context c;
        for (LPCWSTR cls:{L"ComboBox",L"TColorBox",L"TDriveComboBox",L"TFilterComboBox",L"TComboBox2",
                         L"MyTComboBox",L"TComboBoxEx",L"WindowsForms10.COMBOBOX.app.0"}) {
            c.combo.cls=cls; c.ResetTrace(); c.NativeResult(c.Call());
        }
    }
    void ModeAndBackdrop() {
        Context c;
        for (auto mode:{Shared::InjectionMode::None,Shared::InjectionMode::PopupOnly}) {
            Mock::mode=mode; c.ResetTrace(); c.NativeResult(c.Call());
        }
        Mock::mode=Shared::InjectionMode::Full;
        c.combo.backdrop=false; c.ResetTrace(); c.NativeResult(c.Call()); c.combo.backdrop=true;
        c.list.backdrop=false; c.ResetTrace(); c.NativeResult(c.Call());
    }
    void ActualAttachedList() {
        Context c;
        for (int variant=0;variant<7;++variant) {
            auto saved=Mock::infos.at(&c.combo); c.ResetTrace();
            if (variant==0) Mock::failInfo=true;
            if (variant==1) Mock::infos[&c.combo].hwndCombo=&c.other;
            if (variant==2) Mock::infos[&c.combo].hwndList=&c.other;
            if (variant==3) c.list.attached=false;
            if (variant==4) c.list.cls=L"ListBox";
            if (variant==5) c.list.valid=false;
            if (variant==6) Mock::infos[&c.combo].hwndList=nullptr;
            c.NativeResult(c.Call());
            Mock::infos[&c.combo]=saved; Mock::failInfo=false; c.list.attached=true;
            c.list.cls=L"ComboLBox"; c.list.valid=true;
        }
        c.ResetTrace(); c.NativeResult(c.Call(WM_CTLCOLORLISTBOX,&c.dc,&c.other));
        c.ResetTrace(); SetLastError(Mock::entryError);
        c.NativeResult(ComboBoxSubclassProc(&c.combo,WM_CTLCOLORLISTBOX,reinterpret_cast<WPARAM>(&c.dc),0,2,0));
    }
    void BrushAndBkProof() {
        Context c;
        for (int variant=0;variant<7;++variant) {
            c.ResetTrace(); c.brush={BS_SOLID,0xffffff,0}; Mock::nativeBk=0xffffff;
            Mock::originalResult=reinterpret_cast<LRESULT>(&c.brush);
            if (variant==0) c.brush.lbStyle=BS_PATTERN;
            if (variant==1) c.brush.lbStyle=BS_NULL;
            if (variant==2) c.brush.lbColor=0x123456;
            if (variant==3) Mock::nativeBk=0x123456;
            if (variant==4) Mock::originalResult=0;
            if (variant==5) Mock::failObject=true;
            if (variant==6) Mock::originalResult=COLOR_WINDOW+1; // no invented solid-brush metadata
            c.NativeResult(c.Call()); Mock::failObject=false;
        }
    }
    void InvalidDcAndGdiFailures() {
        Context c;
        SetLastError(Mock::entryError);
        CHECK(ComboBoxSubclassProc(&c.combo,WM_CTLCOLORLISTBOX,0,reinterpret_cast<LPARAM>(&c.list),2,0)==Mock::originalResult);
        CHECK(Mock::calls.size()==1 && Mock::setBkCalls==0 && GetLastError()==67);
        c.ResetTrace(); Mock::failBkRead=true; c.NativeResult(c.Call()); Mock::failBkRead=false;
        c.ResetTrace(); Mock::failStock=true; c.NativeResult(c.Call()); Mock::failStock=false;
        c.ResetTrace(); Mock::failBkSet=true;
        CHECK(c.Call()==Mock::originalResult && c.dc.bk==Mock::nativeBk && Mock::setBkCalls==1);
        CHECK(Mock::calls.size()==1 && GetLastError()==67);
    }
    void NativeDecidesBeforeProof() {
        Context c; c.dc.bk=0x123456; c.GlassResult(c.Call());
        c.ResetTrace(); c.dc.bk=0xffffff; Mock::nativeBk=0x123456; c.NativeResult(c.Call());
        c.ResetTrace(); Mock::nativeBk=0xffffff;
        Mock::duringNative=[&](HWND,UINT) { c.brush.lbColor=0x123456; };
        c.NativeResult(c.Call());
    }
    void RevalidateAfterNative() {
        Context c;
        for (int variant=0;variant<9;++variant) {
            c.ResetTrace();
            Mock::duringNative=[&](HWND,UINT) {
                if (variant==0) c.list.attached=false;
                if (variant==1) c.list.valid=false;
                if (variant==2) Mock::infos[&c.combo].hwndList=&c.other;
                if (variant==3) c.combo.backdrop=false;
                if (variant==4) Mock::mode=Shared::InjectionMode::PopupOnly;
                if (variant==5) c.list.backdrop=false;
                if (variant==6) Mock::shutdown=true;
                if (variant==7) c.combo.cls=L"ComboBox";
                if (variant==8) Mock::infos[&c.combo].hwndCombo=&c.other;
            };
            c.NativeResult(c.Call());
            c.list.attached=true; c.list.valid=true; Mock::infos[&c.combo].hwndList=&c.list;
            c.combo.backdrop=c.list.backdrop=true; Mock::mode=Shared::InjectionMode::Full;
            Mock::shutdown=false; c.combo.cls=L"TComboBox"; Mock::infos[&c.combo].hwndCombo=&c.combo;
        }
    }
    void ExistingEditAndOtherMessages() {
        Context c;
        for (LPCWSTR cls:{L"TComboBox",L"TColorBox",L"TDriveComboBox",L"TFilterComboBox"}) {
            c.combo.cls=cls; c.ResetTrace();
            CHECK(c.Call(WM_CTLCOLOREDIT,&c.dc,&c.edit)==reinterpret_cast<LRESULT>(&Mock::black));
            CHECK(Mock::calls.size()==1 && c.dc.bk==0 && c.dc.text==Mock::nativeText);
        }
        c.combo.cls=L"TComboBox";
        c.ResetTrace(); c.NativeResult(c.Call(WM_CTLCOLORSTATIC));
        c.ResetTrace(); c.NativeResult(c.Call(WM_CTLCOLORLISTBOX,&c.dc,&c.edit));
    }
    void ShutdownDetachAndAnimation() {
        Context c; Mock::shutdown=true; c.NativeResult(c.Call()); Mock::shutdown=false;
        c.ResetTrace();
        CHECK(ComboBoxSubclassProc(&c.combo,WndCompat::WM_GLASS_DETACH_COMBOBOX,0,0,2,0)==0);
        CHECK(Mock::removes==1 && Mock::signals==1 && Mock::calls.empty());
        c.other.cls=L"FloatNotifySink"; c.combo.parent=&c.other;
        c.ResetTrace(); c.Call(WM_LBUTTONDOWN); CHECK(Mock::notices==1 && Mock::calls.size()==1);
        c.ResetTrace(); Mock::dropped=true; c.Call(WM_LBUTTONDOWN); CHECK(Mock::notices==0);
    }
    void NestedMessagesStayLocal() {
        Window otherCombo,otherList,otherEdit; DC otherDc;
        Context c; otherList.cls=L"ComboLBox"; otherList.attached=true; otherDc.owner=&otherList;
        otherEdit.cls=L"Edit";
        Mock::infos[&otherCombo]={sizeof(COMBOBOXINFO),{},{},0,&otherCombo,&otherEdit,&otherList};
        WndCompat::ComboBox_Attach(&otherCombo); c.ResetTrace(); bool reentered=false;
        Mock::duringNative=[&](HWND target,UINT msg) {
            if (target!=&c.combo||msg!=WM_CTLCOLORLISTBOX||reentered) return;
            reentered=true;
            CHECK(ComboBoxSubclassProc(&otherCombo,WM_CTLCOLORLISTBOX,reinterpret_cast<WPARAM>(&otherDc),
                reinterpret_cast<LPARAM>(&otherList),2,0)==reinterpret_cast<LRESULT>(&Mock::black));
        };
        CHECK(c.Call()==reinterpret_cast<LRESULT>(&Mock::black));
        CHECK(reentered && Mock::calls.size()==2 && c.dc.bk==0 && otherDc.bk==0 && GetLastError()==67);
        Mock::duringNative={}; WndCompat::ComboBox_Detach(&otherCombo);
    }
    void ProbeIsLocalAndBounded() {
        Context c;
        for (int i=0;i<40;++i) CHECK(c.Call()==reinterpret_cast<LRESULT>(&Mock::black));
        size_t count=0;
        for (const auto& line:Mock::logs) if (line.find(L"[WndCompat][DelphiComboList]")!=std::wstring::npos) {
            ++count; CHECK(line.find(L"CTLCOLOR")!=std::wstring::npos);
            CHECK(line.find(L"replaced=1")!=std::wstring::npos);
            CHECK(line.find(L"brush=")!=std::wstring::npos && line.find(L"bk=0x00FFFFFF")!=std::wstring::npos);
            CHECK(line.find(L"list=")!=std::wstring::npos && line.find(L"dc=")!=std::wstring::npos);
        }
#if defined(_DEBUG)
        CHECK(count==16); Mock::clock=1201; c.Call();
        CHECK(Mock::logs.size()==17);
#else
        CHECK(count==0);
#endif
        CHECK(GetLastError()==67);
    }
}
int main(int argc,char** argv) {
    try {
        if (argc!=2) throw std::runtime_error("one case required");
        const std::string name=argv[1];
        if (name=="default") DefaultList();
        else if (name=="memory-system") MemoryAndCurrentSystemColour();
        else if (name=="classes") ExactClasses();
        else if (name=="mode-backdrop") ModeAndBackdrop();
        else if (name=="identity") ActualAttachedList();
        else if (name=="brush-bk") BrushAndBkProof();
        else if (name=="gdi-failures") InvalidDcAndGdiFailures();
        else if (name=="native-first") NativeDecidesBeforeProof();
        else if (name=="revalidate") RevalidateAfterNative();
        else if (name=="edit-messages") ExistingEditAndOtherMessages();
        else if (name=="lifecycle") ShutdownDetachAndAnimation();
        else if (name=="nested") NestedMessagesStayLocal();
        else if (name=="probe") ProbeIsLocalAndBounded();
        else throw std::runtime_error("unknown case");
        std::cout<<"PASS "<<name<<'\n';
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
'''


class ComboListCtlColorTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CXX', 'g++'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A GCC-compatible C++20 compiler is required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='combo-list-ctlcolor-')
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        stubs = tmp / 'include'
        for header in ('WndPch.h', 'WndCompat.h', 'GlassControlKind.h', 'GlassBackdropMark.h',
                       'GlassMessages.h', 'InjectionMode.h', 'DropDownEventProvider.h',
                       'Tracker/WindowClassUtils.h', 'Tracker/WinEventHandler.h'):
            path = stubs / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#pragma once\n', encoding='utf-8')
        source = tmp / 'combo_list_ctlcolor.cpp'
        source.write_text(CPP, encoding='utf-8')
        cls.binaries = {}
        for mode, flags in (
            ('Release', ['-O2']),
            ('Debug', ['-O0', '-g', '-D_DEBUG']),
            ('DebugProbe', ['-O0', '-g', '-D_DEBUG', '-DGLASS_DELPHI_DRAW_PROBE']),
        ):
            binary = tmp / f'combo-list-ctlcolor-{mode}'
            result = subprocess.run(compiler + [
                '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
                *flags, '-I', str(stubs), '-I', str(ROOT), str(source), '-o', str(binary),
            ], capture_output=True, text=True, timeout=120)
            if result.returncode:
                raise AssertionError(f'{mode} combo list build failed:\n{result.stdout}{result.stderr}')
            cls.binaries[mode] = binary

    def run_case(self, name):
        for mode, binary in self.binaries.items():
            with self.subTest(mode=mode):
                result = subprocess.run([str(binary), name], capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_default_list_changes_only_background_and_preserves_native_state(self): self.run_case('default')
    def test_memory_dc_case_insensitive_class_and_current_system_colour(self): self.run_case('memory-system')
    def test_native_combo_other_vcl_combos_and_similar_classes_stay_native(self): self.run_case('classes')
    def test_full_mode_and_both_backdrop_gates(self): self.run_case('mode-backdrop')
    def test_actual_valid_own_attached_combolbox_is_required(self): self.run_case('identity')
    def test_solid_brush_and_dc_background_must_both_prove_system_colour(self): self.run_case('brush-bk')
    def test_null_invalid_dc_brush_metadata_stock_and_setbk_failure(self): self.run_case('gdi-failures')
    def test_background_is_inspected_after_exactly_one_native_call(self): self.run_case('native-first')
    def test_identity_mode_and_backdrop_are_rechecked_after_native_reentry(self): self.run_case('revalidate')
    def test_accepted_child_edit_and_other_messages_are_unchanged(self): self.run_case('edit-messages')
    def test_shutdown_detach_and_existing_dropdown_animation_are_unchanged(self): self.run_case('lifecycle')
    def test_nested_other_combo_message_keeps_results_and_dcs_separate(self): self.run_case('nested')
    def test_debug_probe_reports_original_brush_decision_and_has_a_budget(self): self.run_case('probe')


if __name__ == '__main__':
    unittest.main()
