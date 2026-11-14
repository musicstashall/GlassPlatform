"""Execute actual saved-theme parser/batch on fake Win32 APIs; not a Windows test."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassSavedThemeCleanup.h').read_text()
body='\n'.join(line for line in s.splitlines() if not line.startswith('#'))
prefix=r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <map>
#include <string>
#include <vector>
#include <utility>
using DWORD=std::uint32_t;using LONG=int;using BYTE=unsigned char;using HANDLE=int;using HKEY=void*;
constexpr HANDLE INVALID_HANDLE_VALUE=-1;
constexpr DWORD FILE_ATTRIBUTE_READONLY=1,FILE_ATTRIBUTE_DIRECTORY=16,FILE_ATTRIBUTE_REPARSE_POINT=1024,INVALID_FILE_ATTRIBUTES=0xffffffff,
 ERROR_FILE_NOT_FOUND=2,ERROR_PATH_NOT_FOUND=3,ERROR_SUCCESS=0,ERROR_NO_MORE_FILES=18,
 GENERIC_READ=1,DELETE=2,FILE_READ_ATTRIBUTES=4,GENERIC_WRITE=256,FILE_SHARE_READ=8,FILE_SHARE_WRITE=16,FILE_SHARE_DELETE=32,OPEN_EXISTING=3,
 FILE_FLAG_OPEN_REPARSE_POINT=64,FILE_FLAG_SEQUENTIAL_SCAN=128,FILE_NAME_NORMALIZED=0,VOLUME_NAME_DOS=0,
 RRF_RT_REG_SZ=1,RRF_RT_REG_EXPAND_SZ=2,RRF_NOEXPAND=4,REG_SZ=1,KEY_SET_VALUE=2,KEY_CREATE_SUB_KEY=4,FileDispositionInfo=4;
constexpr int TRUE=1;
#define SUCCEEDED(x) ((x)>=0)
struct BY_HANDLE_FILE_INFORMATION{DWORD dwFileAttributes=0,dwVolumeSerialNumber=1,nFileIndexHigh=0,nFileIndexLow=0,nNumberOfLinks=1,nFileSizeHigh=0,nFileSizeLow=0;};
struct WIN32_FIND_DATAW{DWORD dwFileAttributes=0;wchar_t cFileName[512]{};};
struct FILE_DISPOSITION_INFO{int DeleteFile=0;};
struct Entry {std::vector<unsigned char> bytes;DWORD attributes=128,links=1;unsigned id=0;bool exists=true,pending=false;};
struct Open {std::wstring path;DWORD access,share;};
std::map<std::wstring,Entry> fs;std::map<HANDLE,Open> handles;std::map<HKEY,std::wstring> keys;std::map<HKEY,HKEY> owners;
std::map<std::wstring,DWORD> dirs;std::map<HKEY,std::wstring> current;
std::wstring localOverride,actualStyle,finalOverride;std::vector<std::wstring> searchNames;std::size_t searchIndex=0;
int serial=10,searches=0,liveCalls=0,writes=0,deleteCalls=0,failDeleteAt=0;DWORD error=0;
bool denyOpen=false,writeFail=false,deleteFail=false,identityFail=false,liveFail=false;
HKEY user=reinterpret_cast<HKEY>(1);
DWORD GetLastError(){return error;}
int _wcsicmp(const wchar_t*a,const wchar_t*b){return wcscasecmp(a,b);}int _wcsnicmp(const wchar_t*a,const wchar_t*b,std::size_t n){return wcsncasecmp(a,b,n);}
DWORD copy(const std::wstring& text,wchar_t* out,DWORD capacity){if(text.size()+1>capacity)return text.size()+1;std::wcscpy(out,text.c_str());return text.size();}
DWORD ExpandEnvironmentStringsW(const wchar_t* s,wchar_t* out,DWORD n){return copy(s,out,n)+1;}
DWORD GetFullPathNameW(const wchar_t* s,DWORD n,wchar_t* out,void*){return copy(s,out,n);}
DWORD GetWindowsDirectoryW(wchar_t* out,DWORD n){return copy(L"C:\\Windows",out,n);}
DWORD GetFileAttributesW(const wchar_t* raw){std::wstring path=raw;auto it=fs.find(path);if(it!=fs.end()){if(!it->second.exists){error=2;return INVALID_FILE_ATTRIBUTES;}return it->second.attributes;}auto d=dirs.find(path);if(d!=dirs.end()){if(d->second==INVALID_FILE_ATTRIBUTES)error=2;return d->second;}return FILE_ATTRIBUTE_DIRECTORY;}
bool safe(const std::wstring& path){for(auto& d:dirs)if(path.compare(0,d.first.size(),d.first)==0&&(d.second&FILE_ATTRIBUTE_REPARSE_POINT))return false;return true;}
bool allows(DWORD access,DWORD share){return (!(access&(GENERIC_READ|FILE_READ_ATTRIBUTES))||(share&FILE_SHARE_READ))&&(!(access&GENERIC_WRITE)||(share&FILE_SHARE_WRITE))&&(!(access&DELETE)||(share&FILE_SHARE_DELETE));}
HANDLE CreateFileW(const wchar_t* raw,DWORD access,DWORD share,void*,DWORD,DWORD,void*){std::wstring path=raw;auto it=fs.find(path);if(denyOpen){error=5;return -1;}if(it==fs.end()||!it->second.exists){error=2;return -1;}for(auto& h:handles)if(h.second.path==path&&(!allows(access,h.second.share)||!allows(h.second.access,share))){error=32;return -1;}HANDLE h=++serial;handles[h]={path,access,share};return h;}
int CloseHandle(HANDLE h){auto it=handles.find(h);assert(it!=handles.end());auto path=it->second.path;handles.erase(it);bool remaining=false;for(auto& o:handles)if(o.second.path==path)remaining=true;auto& e=fs.at(path);if(e.pending&&!remaining)e.exists=false;return 1;}
int GetFileInformationByHandle(HANDLE h,BY_HANDLE_FILE_INFORMATION* out){auto& o=handles.at(h);if(identityFail&&o.access==FILE_READ_ATTRIBUTES){error=5;return 0;}auto& e=fs.at(o.path);*out={};out->dwVolumeSerialNumber=1;out->dwFileAttributes=e.attributes;out->nNumberOfLinks=e.links;out->nFileIndexLow=e.id;out->nFileSizeLow=e.bytes.size();return 1;}
DWORD GetFinalPathNameByHandleW(HANDLE h,wchar_t* out,DWORD n,DWORD){return copy(finalOverride.empty()?L"\\\\?\\"+handles.at(h).path:finalOverride,out,n);}
int ReadFile(HANDLE h,void* out,DWORD n,DWORD* read,void*){auto& bytes=fs.at(handles.at(h).path).bytes;assert(n==bytes.size());if(n)std::memcpy(out,bytes.data(),n);*read=n;return 1;}
void found(WIN32_FIND_DATAW* out){auto& path=searchNames[searchIndex];out->dwFileAttributes=fs.at(path).attributes;auto at=path.find_last_of(L'\\');std::wcscpy(out->cFileName,path.substr(at+1).c_str());}
HANDLE FindFirstFileW(const wchar_t* pattern,WIN32_FIND_DATAW* out){std::wstring prefix=pattern;prefix.resize(prefix.size()-7);searchNames.clear();searchIndex=0;for(auto& e:fs)if(e.second.exists&&e.first.compare(0,prefix.size(),prefix)==0&&e.first.find(L'\\',prefix.size())==std::wstring::npos&&e.first.size()>=6&&e.first.substr(e.first.size()-6)==L".theme")searchNames.push_back(e.first);if(searchNames.empty()){error=2;return -1;}++searches;found(out);return 10000;}
int FindNextFileW(HANDLE,WIN32_FIND_DATAW* out){if(++searchIndex>=searchNames.size()){error=ERROR_NO_MORE_FILES;return 0;}found(out);return 1;}
int FindClose(HANDLE){--searches;return 1;}
LONG RegGetValueW(HKEY hive,const wchar_t*,const wchar_t* name,DWORD,void*,void* out,DWORD* size){std::wstring value;if(std::wstring(name)==L"Local AppData"){if(localOverride.empty())return 2;value=localOverride;}else{auto it=current.find(hive);if(it==current.end())return 2;value=it->second;}const DWORD needed=(value.size()+1)*sizeof(wchar_t);if(needed>*size)return 234;std::memcpy(out,value.c_str(),needed);*size=needed;return 0;}
LONG RegOpenKeyExW(HKEY hive,const wchar_t* sub,DWORD,DWORD,HKEY* out){if(writeFail)return 5;*out=reinterpret_cast<HKEY>(++serial);keys[*out]=sub;owners[*out]=hive;return 0;}
LONG RegCreateKeyExW(HKEY hive,const wchar_t* sub,DWORD,void*,DWORD,DWORD,void*,HKEY* out,void*){if(writeFail)return 5;*out=reinterpret_cast<HKEY>(++serial);keys[*out]=sub;owners[*out]=hive;return 0;}
LONG RegSetValueExW(HKEY key,const wchar_t* name,DWORD,DWORD,const BYTE* bytes,DWORD){if(writeFail)return 5;++writes;if(std::wstring(name)==L"CurrentTheme")current[owners.at(key)]=reinterpret_cast<const wchar_t*>(bytes);return 0;}
LONG RegCloseKey(HKEY h){assert(keys.erase(h)==1);owners.erase(h);return 0;}
int SetFileInformationByHandle(HANDLE h,DWORD,void*,DWORD){++deleteCalls;if(deleteFail||deleteCalls==failDeleteAt){error=5;return 0;}assert(current[user]!=handles.at(h).path);fs.at(handles.at(h).path).pending=true;return 1;}
namespace Shared::ThemeHelper {int SetSystemVisualStyle(const wchar_t* style,const wchar_t*,const wchar_t*,int){++liveCalls;if(liveFail)return -1;actualStyle=style;return 0;}}
int GetCurrentThemeName(wchar_t* path,int n,wchar_t*,int,wchar_t*,int){copy(actualStyle,path,n);return 0;}
'''
main=r'''
using GlassSavedThemeCleanup::Batch;
std::wstring rootPath=L"C:\\Users\\A",themeDir=rootPath+L"\\AppData\\Local\\Microsoft\\Windows\\Themes",owned=themeDir+L"\\Glass.theme",personal=themeDir+L"\\Personal.theme";
void add(const std::wstring& path,const std::string& text){Entry e;e.id=fs.size()+1;e.bytes.assign(text.begin(),text.end());fs[path]=e;}
void reset(){assert(handles.empty()&&keys.empty()&&searches==0);fs.clear();dirs.clear();current.clear();localOverride.clear();actualStyle.clear();finalOverride.clear();denyOpen=writeFail=deleteFail=identityFail=liveFail=false;liveCalls=writes=deleteCalls=failDeleteAt=0;add(owned,"[Theme]\nName=Glass\n[Glass.OpenGlass]\nBlur=1\n");add(personal,"[Theme]\nName=Personal\n");add(L"C:\\Windows\\Resources\\Themes\\aero.theme","[Theme]\n");add(L"C:\\Windows\\Resources\\Themes\\Aero\\aero.msstyles","stock");}
int main(){std::wstring detail;
 // Encoding/parser rules: headers only, never comments, values or substrings.
 for(const auto& text:{"[Glass.OpenGlass]"," [ gLaSs.Platform ] ; note","[Glass]"}){bool valid=false;std::vector<unsigned char> b(text,text+std::strlen(text));assert(GlassSavedThemeCleanup::HasGlassSection(b,valid)&&valid);}
 for(const auto& text:{";[Glass.OpenGlass]","Name=[Glass.OpenGlass]","[NotGlass.OpenGlass]","[Glass.]","[Glass.OpenGlass]junk","[Theme]"}){bool valid=false;std::vector<unsigned char> b(text,text+std::strlen(text));assert(!GlassSavedThemeCleanup::HasGlassSection(b,valid)&&valid);}
 for(bool be:{false,true}){std::vector<unsigned char>b=be?std::vector<unsigned char>{254,255}:std::vector<unsigned char>{255,254};for(unsigned char c:std::string("[Glass.OpenGlass]")){b.push_back(be?0:c);b.push_back(be?c:0);}bool valid=false;assert(GlassSavedThemeCleanup::HasGlassSection(b,valid)&&valid);b.push_back(0);assert(!GlassSavedThemeCleanup::HasGlassSection(b,valid)&&!valid);}
 {bool valid=false;assert(!GlassSavedThemeCleanup::HasGlassSection({0},valid)&&!valid);std::vector<unsigned char>b(4*1024*1024+1,'x');assert(!GlassSavedThemeCleanup::HasGlassSection(b,valid)&&!valid);}
 reset();{Batch b;bool fallback=false;assert(b.Prepare(rootPath,user,safe,detail));assert(b.Cleanup(user,true,fallback,detail));assert(!fs[owned].exists&&fs[personal].exists&&liveCalls==0&&writes==0&&!fallback);}
 for(bool live:{false,true}){reset();current[user]=owned;{Batch b;bool fallback=false;assert(b.Prepare(rootPath,user,safe,detail));assert(writes==0);assert(b.Cleanup(user,live,fallback,detail));assert(!fs[owned].exists&&fs[personal].exists);assert(current[user]==L"C:\\Windows\\Resources\\Themes\\aero.theme");assert(fallback==!live);assert(liveCalls==int(live));}}
 reset();current[user]=owned;liveFail=true;{Batch b;bool fallback=false;assert(b.Prepare(rootPath,user,safe,detail));assert(b.Cleanup(user,true,fallback,detail)&&fallback&&liveCalls==1);}
 for(int failure=0;failure<8;++failure){reset();if(failure==0)fs[owned].links=2;if(failure==1)fs[owned].attributes|=FILE_ATTRIBUTE_REPARSE_POINT;if(failure==2)dirs[rootPath+L"\\AppData"]=FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT;if(failure==3)denyOpen=true;if(failure==4)finalOverride=L"\\\\?\\C:\\Other\\Glass.theme";if(failure==5)localOverride=L"C:\\Windows";if(failure==6)fs[owned].bytes={255,254,1};if(failure==7)fs[owned].attributes|=FILE_ATTRIBUTE_READONLY;{Batch b;assert(!b.Prepare(rootPath,user,safe,detail));assert(deleteCalls==0&&writes==0);}assert(fs[owned].exists&&fs[personal].exists);}
 for(int failure=0;failure<3;++failure){reset();current[user]=owned;{Batch b;bool fallback=false;assert(b.Prepare(rootPath,user,safe,detail));if(failure==0)writeFail=true;if(failure==1)identityFail=true;if(failure==2)deleteFail=true;assert(!b.Cleanup(user,true,fallback,detail));assert(fs[owned].exists&&fs[personal].exists);assert(!b.Cleanup(user,true,fallback,detail));}}
 // A pre-existing writer may finish style preparation, but prevents deletion
 // until an exclusive read seal can be acquired. Never terminate that writer.
 reset();{HANDLE writer=CreateFileW(owned.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);Batch b;bool fallback=false;assert(b.Prepare(rootPath,user,safe,detail));assert(!b.Cleanup(user,true,fallback,detail));assert(deleteCalls==0&&writes==0&&fs[owned].exists);CloseHandle(writer);}
 // If Windows rewrites the file without Glass sections, it becomes retained.
 reset();current[user]=owned;{Batch b;bool fallback=false;assert(b.Prepare(rootPath,user,safe,detail));std::string plain="[Theme]\nName=Personal\n";fs[owned].bytes.assign(plain.begin(),plain.end());assert(b.Cleanup(user,true,fallback,detail));assert(fs[owned].exists&&liveCalls==0&&writes==0);}
 // Every profile is addressed by its own hive, never by process HKCU.
 reset();{HKEY second=reinterpret_cast<HKEY>(2);const std::wstring secondRoot=L"C:\\Users\\B",secondTheme=secondRoot+L"\\AppData\\Local\\Microsoft\\Windows\\Themes\\Saved.theme";add(secondTheme,"[Glass.OpenGlass]\n");current[user]=owned;current[second]=secondTheme;Batch a,b;bool fallback=false;assert(a.Prepare(rootPath,user,safe,detail));assert(b.Prepare(secondRoot,second,safe,detail));assert(a.Cleanup(user,false,fallback,detail));assert(b.Cleanup(second,false,fallback,detail));assert(!fs[owned].exists&&!fs[secondTheme].exists&&fs[personal].exists);assert(current[user]==current[second]&&fallback);}
 // Active links may use aliases: compare file identity, not just path spelling.
 reset();{const std::wstring alias=L"C:\\Users\\A\\Alias.theme";add(alias,"[Glass.OpenGlass]\n");fs[alias].id=fs[owned].id;current[user]=alias;Batch b;bool fallback=false;assert(b.Prepare(rootPath,user,safe,detail));assert(b.Cleanup(user,false,fallback,detail)&&fallback);assert(current[user]==L"C:\\Windows\\Resources\\Themes\\aero.theme");}
 // A late second-file failure is partial, not success or an implicit retry.
 reset();{const std::wstring second=themeDir+L"\\Second.theme";add(second,"[Glass.OpenGlass]\n");Batch b;bool fallback=false;assert(b.Prepare(rootPath,user,safe,detail));failDeleteAt=2;assert(!b.Cleanup(user,false,fallback,detail));assert(!fs[owned].exists&&fs[second].exists&&fs[personal].exists);assert(!b.Cleanup(user,false,fallback,detail));}
 // Re-read active link at commit, not only preflight.
 reset();{Batch b;bool fallback=false;assert(b.Prepare(rootPath,user,safe,detail));current[user]=owned;assert(b.Cleanup(user,false,fallback,detail)&&fallback);}
 reset();{dirs[themeDir]=INVALID_FILE_ATTRIBUTES;Batch b;assert(b.Prepare(rootPath,user,safe,detail));assert(deleteCalls==0);}
 assert(handles.empty()&&keys.empty()&&searches==0);
}
'''
with tempfile.TemporaryDirectory() as d:
    source=Path(d)/'themes.cpp';source.write_text(prefix+body+main)
    binary=Path(d)/'themes'
    subprocess.run(['g++','-std=c++17','-fsanitize=address,undefined','-fno-pie','-no-pie',str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS: actual saved-theme parser/batch; encodings, ownership boundaries, pinned files, active-link reset/live fallback, errors and handle cleanup (fake APIs)')
