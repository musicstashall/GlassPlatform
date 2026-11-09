"""Actual native click observer: child/footer/nonclient clicks, never hover/key/drag."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/main.cpp').read_text()
a=s.index('    void ObserveComponentNoticeClick(');b=s.index('\nprivate:',a);method=s[a:b]
h=r'''
#include <cassert>
#include <string>
#include <iostream>
using UINT=unsigned;using LONG=long;using HWND=int;
struct POINT{LONG x=0,y=0;};struct MSG{HWND hwnd;UINT message;POINT pt;};
constexpr UINT WM_LBUTTONDOWN=10,WM_LBUTTONUP=11,WM_RBUTTONDOWN=12,WM_RBUTTONUP=13,WM_MBUTTONDOWN=14,WM_MBUTTONUP=15,WM_XBUTTONDOWN=16,WM_XBUTTONUP=17,
 WM_NCLBUTTONDOWN=20,WM_NCLBUTTONUP=21,WM_NCRBUTTONDOWN=22,WM_NCRBUTTONUP=23,WM_NCMBUTTONDOWN=24,WM_NCMBUTTONUP=25,WM_NCXBUTTONDOWN=26,WM_NCXBUTTONUP=27,
 WM_MOUSEMOVE=30,WM_NCMOUSEMOVE=31,WM_KEYDOWN=32,WM_COMMAND=33,WM_MOUSEWHEEL=34,WM_MOUSEHWHEEL=35,WM_VSCROLL=36,WM_HSCROLL=37;
constexpr int SM_CXDRAG=1,SM_CYDRAG=2;
bool IsChild(HWND parent,HWND child){return parent==1&&(child==2||child==3);}
int GetSystemMetrics(int){return 4;}
struct Web{int clicks=0;int PostWebMessageAsJson(const wchar_t* text){assert(std::wstring(text)==L"{\"type\":\"component-notice-click\"}"||std::wstring(text)==L"{\"type\":\"component-notice-scroll\"}");++clicks;return 0;}};
struct Observer{
 HWND window=1,webHost_=3;POINT nativeNoticePress_{};UINT nativeNoticeButton_=0;
 bool nativeComponentNoticeActivity_=false,webReady_=true;
 std::wstring nativeComponentNotice_=L"notice";Web web;Web* web_=&web;
// METHOD
};
int main(){
 for(HWND hwnd:{1,2,3,4})for(UINT msg:{WM_MOUSEWHEEL,WM_MOUSEHWHEEL,WM_VSCROLL,WM_HSCROLL}){
  Observer o;o.ObserveComponentNoticeClick({hwnd,msg,{0,0}});assert(o.nativeComponentNoticeActivity_==(hwnd!=4));assert(o.web.clicks==((hwnd==1||hwnd==2)?1:0));
 }

 for(UINT type:{WM_MOUSEMOVE,WM_NCMOUSEMOVE,WM_KEYDOWN,WM_COMMAND,WM_LBUTTONUP}){
  Observer o;o.ObserveComponentNoticeClick({1,type,{0,0}});assert(!o.nativeComponentNoticeActivity_&&o.web.clicks==0);
 }
 for(HWND hwnd:{1,2,3,4})for(UINT down:{WM_LBUTTONDOWN,WM_RBUTTONDOWN,WM_MBUTTONDOWN,WM_XBUTTONDOWN,WM_NCLBUTTONDOWN,WM_NCRBUTTONDOWN,WM_NCMBUTTONDOWN,WM_NCXBUTTONDOWN}){
  Observer o;o.ObserveComponentNoticeClick({hwnd,down,{10,10}});assert(!o.nativeComponentNoticeActivity_);
  o.ObserveComponentNoticeClick({hwnd,down+1,{10,10}});assert(o.nativeComponentNoticeActivity_==(hwnd!=4));assert(o.web.clicks==((hwnd==1||hwnd==2)?1:0));
 }
 {Observer o;o.ObserveComponentNoticeClick({1,WM_LBUTTONDOWN,{0,0}});o.ObserveComponentNoticeClick({1,WM_MOUSEMOVE,{20,0}});o.ObserveComponentNoticeClick({1,WM_LBUTTONUP,{0,0}});assert(!o.nativeComponentNoticeActivity_&&o.web.clicks==0);}
 {Observer o;o.ObserveComponentNoticeClick({1,WM_LBUTTONDOWN,{0,0}});o.ObserveComponentNoticeClick({1,WM_RBUTTONUP,{0,0}});assert(!o.nativeComponentNoticeActivity_&&o.web.clicks==0);}
 std::cout<<"PASS: actual native notice observer: completed client/child/nonclient clicks + scrolling, footer bridge, no WebView duplicate, no hover/keyboard/commands/drag/foreign-window\n";
}
'''.replace('// METHOD',method)
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'click.cpp';p.write_text(h);exe=Path(d)/'click'
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
