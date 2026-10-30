#!/usr/bin/env python3
"""Test real installed-version detection and preflight with fake Win32 APIs.

The fakes provide file attributes, versions and MSI registration; no Windows
Installer transaction or real Windows file-system behavior is exercised.
"""
from pathlib import Path
import os
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
setup = (repo / "GlassSetup/src/main.cpp").read_text(encoding="utf-8")


def function(text, token):
    start = text.index(token)
    brace = text.index("{", start)
    depth = 0
    for pos in range(brace, len(text)):
        if text[pos] == "{":
            depth += 1
        if text[pos] == "}":
            depth -= 1
            if depth == 0:
                return text[start:pos + 1]
    raise ValueError(token)


def structure(text, token):
    start = text.index(token)
    end = text.index("\n};", start) + len("\n};")
    return text[start:end]


preflight = function(setup, "bool RunPreflightChecks()")
page = function(setup, "void SetPage(Page page)")
assert 'if (!installed.rootsResolved || (installed.detected && !installed.valid))' in preflight
assert 'SetPage(Page::VersionInvalid)' in preflight and 'return false;' in preflight
assert 'page_==Page::Downgrade || page_==Page::VersionInvalid' in page
assert 'page_==Page::VersionInvalid) return 0;' in setup
assert 'forceCleanInstall_' not in setup and 'offerCleanInstall_' not in setup

pieces = [structure(setup, "struct ParsedVersion"),
          function(setup, "bool TryParseVersion("), function(setup, "int CompareVersions("),
          structure(setup, "struct InstallRootCandidate"), structure(setup, "struct InstalledGlassVersionInfo"),
          function(setup, "void AddInstallRoot("), function(setup, "bool InstalledGlassRoots("),
          function(setup, "bool GlassInstallRegistrationExists()"),
          function(setup, "bool InspectInstalledMarker("), function(setup, "InstalledGlassVersionInfo FindInstalledGlassVersion()")]

prefix = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
using DWORD=uint32_t;using ULONGLONG=uint64_t;
constexpr DWORD ERROR_FILE_NOT_FOUND=2,ERROR_PATH_NOT_FOUND=3,ERROR_ACCESS_DENIED=5,
 FILE_ATTRIBUTE_DIRECTORY=16,INVALID_FILE_ATTRIBUTES=0xffffffffu;
struct Fake {
 bool roots=true,reg64=true,reg86=false,legacy64=false,legacy86=false;
 std::wstring x64=L"C:\\Program Files\\GlassPlatform",x86=L"C:\\Program Files (x86)\\GlassPlatform";
 std::wstring self=L"2.0.0.51";
 std::map<std::wstring,DWORD> files,errors;std::map<std::wstring,std::wstring> versions;
 std::set<std::wstring> unsafe;DWORD error=0;
} fake;
void reset(){fake={};}
DWORD GetLastError(){return fake.error;}
DWORD GetFileAttributesW(const wchar_t* name){std::wstring p=name;auto error=fake.errors.find(p);
 if(error!=fake.errors.end()){fake.error=error->second;return INVALID_FILE_ATTRIBUTES;}
 auto it=fake.files.find(p);if(it!=fake.files.end())return it->second;fake.error=ERROR_FILE_NOT_FOUND;return INVALID_FILE_ATTRIBUTES;}
int _wcsicmp(const wchar_t* a,const wchar_t* b){while(*a&&*b){wchar_t x=std::towlower(*a++),y=std::towlower(*b++);if(x!=y)return x<y?-1:1;}return *a?1:*b?-1:0;}
namespace InstallLog {void Line(const wchar_t*){} template<class... T>void LineFormat(const wchar_t*,T...) {}}
namespace GlassInstallUtil {
 struct InstallRoots {std::wstring x64,x86,openGlass,sourceX64,sourceX86,sourceOpenGlass;};
 bool ResolveInstallRoots(InstallRoots& roots){
  if(!fake.roots){roots={};fake.error=ERROR_ACCESS_DENIED;return false;}
  roots.x64=fake.x64;roots.x86=fake.x86;roots.openGlass=fake.x64.empty()?L"":fake.x64+L"\\OpenGlass";
  roots.sourceX64=L"registry";roots.sourceX86=L"registry";roots.sourceOpenGlass=L"default";return true;}
 bool IsSafeInstallPath(const std::wstring& path){return !fake.unsafe.count(path);}
}
namespace GlassInstallMsi {
 constexpr wchar_t kProductPlatformX64[]=L"64",kProductPlatformX86[]=L"86",kLegacyPlatformX64[]=L"old64",kLegacyPlatformX86[]=L"old86";
 bool IsProductInstalled(const wchar_t* code){std::wstring c=code;
  return c==L"64"?fake.reg64:c==L"86"?fake.reg86:c==L"old64"?fake.legacy64:c==L"old86"?fake.legacy86:false;}
}
std::wstring ReadProductVersionString(const std::wstring& path,ULONGLONG& key){key=0;auto it=fake.versions.find(path);return it==fake.versions.end()?L"":it->second;}
'''

wizard = r'''
enum class Page { Welcome, Conflicts, VersionInvalid, Downgrade };
constexpr int IDS_VERSION_INVALID_BODY=1,IDS_ERR_INSTALL_PATH=2,IDS_VERSION_INVALID_NOTE=3,
 IDS_SETUP_VERSION_INVALID_BODY=4,IDS_SETUP_VERSION_INVALID_NOTE=5,IDS_DOWNGRADE_BODY=6;
std::wstring Text(int id){return std::to_wstring(id);}
std::wstring SelfProductVersion(){return fake.self;}
std::vector<std::wstring> DetectConflictingApps(){return {};}
std::wstring FormatVersionPairText(int,const std::wstring& a,const std::wstring& b){return a+L" / "+b;}
struct Wizard {Page page_=Page::Welcome;std::vector<std::wstring> conflicts_;
 std::wstring installedGlassVersion_,setupProductVersion_,preflightX64Root_,preflightX86Root_,preflightOpenGlassRoot_,blockReason_,versionInvalidNote_,downgradeBody_;
 void SetPage(Page p){page_=p;}
'''+preflight+r'''
};
void addCore(const std::wstring& root,const std::wstring& version){auto p=root+L"\\GlassCore.dll";fake.files[p]=0;fake.versions[p]=version;}
int main(){
 // Reproduce a healthy x64 installation: it is an update, not a clean reinstall.
 reset();addCore(fake.x64,L"2.0.0.50");Wizard normal;assert(normal.RunPreflightChecks());
 assert(normal.installedGlassVersion_==L"2.0.0.50" && normal.page_==Page::Welcome);
 // Resolve and compare both architectures, retaining the highest valid version.
 reset();fake.reg86=true;addCore(fake.x64,L"2.0.0.50");addCore(fake.x86,L"2.0.0.51");
 Wizard both;assert(both.RunPreflightChecks() && both.installedGlassVersion_==L"2.0.0.51");
 // A registered, non-default root must be used for version detection.
 reset();fake.reg86=true;fake.x86=L"D:\\Programs\\GlassPlatform";addCore(fake.x64,L"2.0.0.50");
 addCore(fake.x86,L"2.0.0.51");Wizard custom;assert(custom.RunPreflightChecks());
 assert(custom.preflightX86Root_==fake.x86 && custom.installedGlassVersion_==L"2.0.0.51");
 // A genuinely clean system is distinct from an unreadable registered install.
 reset();fake.reg64=false;Wizard empty;assert(empty.RunPreflightChecks() && empty.installedGlassVersion_.empty());
 reset();Wizard missing;assert(!missing.RunPreflightChecks() && missing.page_==Page::VersionInvalid);
 assert(missing.blockReason_==Text(IDS_VERSION_INVALID_BODY) && missing.versionInvalidNote_==Text(IDS_VERSION_INVALID_NOTE));
 // If system root lookup fails, block even if MSI detection is also empty.
 reset();fake.roots=false;fake.reg64=false;Wizard noroot;assert(!noroot.RunPreflightChecks());
 assert(noroot.page_==Page::VersionInvalid && noroot.blockReason_==Text(IDS_ERR_INSTALL_PATH));
 // Missing or invalid metadata, inaccessible markers and unsafe links do not
 // authorize a destructive clean installation.
 reset();addCore(fake.x64,L"bad");Wizard badVersion;assert(!badVersion.RunPreflightChecks() && badVersion.page_==Page::VersionInvalid);
 reset();fake.errors[fake.x64+L"\\GlassCore.dll"]=ERROR_ACCESS_DENIED;Wizard denied;assert(!denied.RunPreflightChecks());
 reset();addCore(fake.x64,L"2.0.0.50");fake.unsafe.insert(fake.x64+L"\\GlassCore.dll");Wizard unsafe;assert(!unsafe.RunPreflightChecks());
 reset();addCore(fake.x64,L"2.0.0.50");fake.files[fake.x86+L"\\GlassCompat.dll"]=0;Wizard incomplete;assert(!incomplete.RunPreflightChecks());
 // Downgrades and unreadable Setup versions are still blocked.
 reset();addCore(fake.x64,L"2.0.0.60");Wizard newer;assert(!newer.RunPreflightChecks() && newer.page_==Page::Downgrade);
 reset();addCore(fake.x64,L"2.0.0.50");fake.self=L"";Wizard noSelf;assert(!noSelf.RunPreflightChecks());
 assert(noSelf.page_==Page::VersionInvalid && noSelf.versionInvalidNote_==Text(IDS_SETUP_VERSION_INVALID_NOTE));
 std::cout<<"PASS: real preflight: healthy x64/x86 update, clean system, inaccessible paths, versions and downgrade\n";
}
'''
with tempfile.TemporaryDirectory(prefix="glass-version-") as directory:
    cpp = Path(directory) / "test.cpp"
    exe = Path(directory) / "test"
    cpp.write_text(prefix + "\n".join(pieces) + wizard, encoding="utf-8")
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-g", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-no-pie", str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
