#!/usr/bin/env python3
"""Exercise real MSI path/context guards with fake Windows Installer APIs.

This checks control flow and ordering, not actual MSI behavior on Windows.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import xml.etree.ElementTree as ET

repo = Path(__file__).resolve().parents[2]
msi_src = (repo / "GlassSetup/src/InstallSupport/GlassInstallMsi.cpp").read_text(encoding="utf-8")
rollback_src = (repo / "GlassSetup/src/InstallSupport/GlassInstallRollback.cpp").read_text(encoding="utf-8")
engine = (repo / "GlassSetup/src/InstallEngine.cpp").read_text(encoding="utf-8")
setup = (repo / "GlassSetup/src/main.cpp").read_text(encoding="utf-8")
unins = (repo / "GlassSetup/GlassUnins/main.cpp").read_text(encoding="utf-8")

def function(text, token):
    start = text.index(token)
    brace = text.index("{", start)
    depth = 0
    for pos in range(brace, len(text)):
        if text[pos] == "{": depth += 1
        if text[pos] == "}":
            depth -= 1
            if depth == 0: return text[start:pos + 1]
    raise ValueError(token)

start_install = function(setup, "void StartInstallation()")
startup = function(setup, "void StartupWorker(")
roots = function(setup, "bool InstalledGlassRoots(")
unins_roots = function(unins, "void ResolveRoots()")
for body in (start_install, startup, roots, unins_roots):
    assert "ResolveInstallRoots(" in body
    assert "TryGetAutorunGlassCompatInstallRoot(" not in body
assert "args_.originalDir" not in unins_roots
assert 'ogDir=roots.openGlass' in start_install
# Workspace paths use one Windows separator at the junction, not a doubled
# literal that depends on Win32 path normalization.
assert r'workspace.directory+L"\\current-' in unins
assert r'workspaceDir+L"\\rollback-prior-' in rollback_src
assert r'workspaceDir+L"\\recheck-' in engine
worker = function(engine, "DWORD WINAPI InstallEngine::WorkerThread(")
assert worker.index("CreateRollbackSnapshot()") < worker.index("StopRunningHosts()") < worker.index("InstallGlassSettingsEarly()")
assert worker.index("CapturePriorHostState()") < worker.index("CreateRollbackSnapshot()")
create = function(rollback_src, "bool CreateSnapshot(")
assert create.index("VerifyProductRegistrations(") < create.index("TrySnapshotProduct(")
operation = function(unins, "int RunOperation(")
assert operation.index("CheckProductFileRoots(") < operation.index("StopAllForUninstall(") < operation.index("UninstallProduct(")
assert "ValidatePlatformTask(" in operation and "ValidateOpenGlassPaths(" in operation
for stage in ("bool InstallEngine::RunOneMsi(", "bool InstallEngine::RunMsiSequence(", "bool InstallEngine::PurgeRemnants("):
    assert "RecheckSnapshottedProduct(" in function(engine, stage), stage
retry = function(engine, "bool InstallEngine::RunOneMsi(").split("exitCode == ERROR_PRODUCT_VERSION", 1)[1]
assert retry.index("RecheckRelatedProducts(") < retry.index("GlassInstallMsi::InstallProduct(")
recheck = function(engine, "bool InstallEngine::RecheckSnapshottedProduct(")
assert recheck.index("CopyLocalPackage(") < recheck.index("CheckProductFileRoots(")

# Keep UpgradeCodes used by the guard synchronized with the actual projects.
family_section = function(rollback_src, "bool VerifyProductRegistrations(")
for filename in ("Glass-x64.wixproj", "Glass-x86.wixproj", "OpenGlass-x64.wixproj",
                 "GlassSettings-x64.wixproj", "GlassSettings-x86.wixproj"):
    root = ET.parse(repo / "GlassSetup/msi" / filename).getroot()
    upgrade = next(node.text for node in root.iter() if node.tag.endswith("GlassUpgradeCode"))
    assert upgrade in family_section, (filename, upgrade)

contexts = function(msi_src, "ProductPathCheck CheckProductContexts(")
file_roots = function(msi_src, "ProductPathCheck CheckProductFileRoots(")
snapshot = function(rollback_src, "bool TrySnapshotProduct(")
safe_uninstall = function(rollback_src, "bool UninstallSessionProductSafely(")
verify_registrations = function(rollback_src, "bool VerifyProductRegistrations(")
assert snapshot.index("CheckProductFileRoots(") < snapshot.index("oldProducts.push_back(")

prefix = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <climits>
#include <cstdint>
#include <cwchar>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>
using DWORD=std::uint32_t;using UINT=unsigned;using MSIHANDLE=unsigned;
#define _wcsicmp wcscasecmp
#define _wcsnicmp wcsncasecmp
constexpr UINT ERROR_SUCCESS=0,ERROR_NO_MORE_ITEMS=259,ERROR_UNKNOWN_PRODUCT=1605,
 ERROR_INVALID_DATA=13,ERROR_CANCELLED=1223,ERROR_FILE_NOT_FOUND=2,ERROR_PATH_NOT_FOUND=3,ERROR_GEN_FAILURE=31,ERROR_MORE_DATA=234;
constexpr DWORD MSIINSTALLCONTEXT_ALL=7;
constexpr int MSIDBOPEN_READONLY=0;
enum INSTALLSTATE {INSTALLSTATE_UNKNOWN=-1,INSTALLSTATE_ABSENT=2,INSTALLSTATE_LOCAL=3,INSTALLSTATE_SOURCE=4,INSTALLSTATE_DEFAULT=5};
enum MSICONDITION {MSICONDITION_FALSE=0,MSICONDITION_TRUE=1,MSICONDITION_NONE=2,MSICONDITION_ERROR=3};
struct Row {std::wstring id,path;INSTALLSTATE state=INSTALLSTATE_LOCAL;bool readOk=true;
 std::wstring folderId=L"directory",componentDir=L"directory";bool keyNull=true;};
struct Fake {
 unsigned all=0,current=0;INSTALLSTATE productState=INSTALLSTATE_UNKNOWN;
 bool contextError=false,openOk=true,viewOk=true,executeOk=true,fetchOk=true,copyOk=true,folderView=false,countView=false,extraUnmatched=false;
 int cancelAtPath=0,pathCalls=0,copyCalls=0,closeCalls=0,uninstallCalls=0;std::atomic_bool* cancelled=nullptr;
 std::size_t next=0;std::vector<Row> rows,folderRows;std::wstring sql,cachedCode=L"p";
 std::map<std::wstring,std::vector<std::wstring>> related;
 std::map<std::wstring,MSICONDITION> tables;
} fake;
DWORD GetLastError(){return ERROR_FILE_NOT_FOUND;}
int DeleteFileW(const wchar_t*){return 1;}
UINT MsiEnumProductsExW(const wchar_t*,const wchar_t* sid,DWORD contexts,DWORD index,void*,void*,void*,void*){
 assert(contexts==MSIINSTALLCONTEXT_ALL);if(fake.contextError)return ERROR_GEN_FAILURE;
 return index<(sid?fake.all:fake.current)?ERROR_SUCCESS:ERROR_NO_MORE_ITEMS;
}
INSTALLSTATE MsiQueryProductStateW(const wchar_t*){return fake.productState;}
UINT MsiEnumRelatedProductsW(const wchar_t* upgrade,DWORD reserved,DWORD index,wchar_t* out){
 assert(reserved==0);const auto it=fake.related.find(upgrade);
 if(it==fake.related.end() || index>=it->second.size())return ERROR_NO_MORE_ITEMS;
 std::wcscpy(out,it->second[index].c_str());return ERROR_SUCCESS;
}
UINT MsiOpenDatabaseW(const wchar_t*,int mode,MSIHANDLE* out){assert(mode==MSIDBOPEN_READONLY);if(!fake.openOk)return ERROR_GEN_FAILURE;*out=1;return ERROR_SUCCESS;}
MSICONDITION MsiDatabaseIsTablePersistentW(MSIHANDLE db,const wchar_t* table){
 assert(db==1);auto it=fake.tables.find(table);return it==fake.tables.end()?MSICONDITION_NONE:it->second;
}
UINT MsiDatabaseOpenViewW(MSIHANDLE db,const wchar_t* sql,MSIHANDLE* out){assert(db==1);fake.sql=sql;fake.folderView=fake.sql.find(L"CreateFolder")!=std::wstring::npos;
 fake.countView=fake.sql.find(L"SELECT `Component_` FROM")==0;
 if(!fake.viewOk)return ERROR_GEN_FAILURE;*out=2;return ERROR_SUCCESS;}
UINT MsiViewExecute(MSIHANDLE view,MSIHANDLE record){assert(view==2 && record==0);fake.next=0;return fake.executeOk?ERROR_SUCCESS:ERROR_GEN_FAILURE;}
UINT MsiViewFetch(MSIHANDLE view,MSIHANDLE* out){assert(view==2);if(!fake.fetchOk)return ERROR_GEN_FAILURE;
 const auto& rows=fake.folderView?fake.folderRows:fake.rows;
 if(fake.next>=rows.size()+(fake.countView && fake.extraUnmatched?1:0))return ERROR_NO_MORE_ITEMS;
 *out=100+static_cast<MSIHANDLE>(fake.next++);return ERROR_SUCCESS;}
UINT MsiRecordGetStringW(MSIHANDLE row,UINT col,wchar_t* out,DWORD* size){assert(row>=100 && col>=1 && col<=3);
 const Row& r=(fake.folderView?fake.folderRows:fake.rows).at(row-100);
 const auto& value=col==1?r.id:(col==2?r.folderId:r.componentDir);
 if(!r.readOk || value.size()>=*size)return ERROR_MORE_DATA;std::wcscpy(out,value.c_str());*size=static_cast<DWORD>(value.size());return ERROR_SUCCESS;}
int MsiRecordIsNull(MSIHANDLE row,UINT col){assert(fake.folderView && col==4);return fake.folderRows.at(row-100).keyNull;}
INSTALLSTATE MsiGetComponentPathW(const wchar_t*,const wchar_t* id,wchar_t* out,DWORD* size){
 ++fake.pathCalls;if(fake.cancelAtPath && fake.pathCalls==fake.cancelAtPath && fake.cancelled)fake.cancelled->store(true);
 const auto& rows=fake.folderView?fake.folderRows:fake.rows;
 for(const auto& row:rows)if(row.id==id){if(row.path.size()>=*size){*size=static_cast<DWORD>(row.path.size());return row.state;}
 std::wcscpy(out,row.path.c_str());*size=static_cast<DWORD>(row.path.size());return row.state;}return INSTALLSTATE_UNKNOWN;}
UINT MsiCloseHandle(MSIHANDLE){++fake.closeCalls;return ERROR_SUCCESS;}
UINT MsiViewClose(MSIHANDLE){return ERROR_SUCCESS;}
namespace GlassInstallLog {template<class... T>void LineFormat(const wchar_t*,T...) {}}
namespace GlassInstallUtil {
 bool IsSafeInstallPath(const std::wstring& path){return path.find(L"linked")==std::wstring::npos;}
 bool IsWithinInstallRoot(const std::wstring& root,const std::wstring& path){
  return !root.empty() && path.size()>=root.size() && _wcsnicmp(root.c_str(),path.c_str(),root.size())==0 &&
   (path.size()==root.size() || path[root.size()]==L'\\');
 }
}
namespace GlassInstallMsi {
 enum class ProductPathCheck {Safe,OutsideRoot,Unverifiable,Cancelled};
 std::wstring ProductCodeFromMsiPath(const std::wstring&){return fake.cachedCode;}
 constexpr wchar_t kProductPlatformX64[]=L"p64",kLegacyPlatformX64[]=L"old64",kProductPlatformX86[]=L"p86",
  kLegacyPlatformX86[]=L"old86",kProductOpenGlass[]=L"og",kLegacyOpenGlass[]=L"oldog",
  kProductSettingsX64[]=L"gs64",kProductSettingsX86[]=L"gs86";
 ProductPathCheck CheckProductContexts(const wchar_t*,std::wstring&,const std::atomic_bool* = nullptr) noexcept;
 ProductPathCheck CheckProductFileRoots(const wchar_t*,const std::wstring&,const std::wstring&,std::wstring&,const std::atomic_bool* = nullptr) noexcept;
 bool CopyLocalPackage(const wchar_t*,const std::wstring&,const std::atomic_bool* = nullptr){++fake.copyCalls;return fake.copyOk;}
 bool IsProductInstalled(const wchar_t*){return fake.productState==INSTALLSTATE_DEFAULT;}
 bool UninstallProduct(const wchar_t*){++fake.uninstallCalls;return true;}
 std::wstring QueryDisplayVersion(const wchar_t*){return L"1.2";}
'''
middle = r'''
}
namespace GlassInstallRollback {
 struct ProductSnapshot{std::wstring productCode,installDir,cachedMsiPath,displayVersion;};
 struct SessionState{
  std::wstring workspaceDir=L"secure",x64Dir=L"root",x86Dir=L"root",openGlassDir=L"root";std::vector<ProductSnapshot> oldProducts;
  unsigned copyError=0;bool hadPriorInstall=false,productPathUnverified=false;std::wstring productPathCode,productPathDetail;
 };
}
'''
main = r'''
int main(){
 using namespace GlassInstallMsi;std::wstring detail;std::atomic_bool cancelled=false;
 auto reset=[&](){fake=Fake{};cancelled=false;};
 reset();assert(CheckProductContexts(L"p",detail,&cancelled)==ProductPathCheck::Safe);
 reset();fake.all=fake.current=1;fake.productState=INSTALLSTATE_DEFAULT;
 assert(CheckProductContexts(L"p",detail,&cancelled)==ProductPathCheck::Safe);
 reset();fake.all=1;fake.current=0;fake.productState=INSTALLSTATE_UNKNOWN;
 assert(CheckProductContexts(L"p",detail,&cancelled)==ProductPathCheck::Unverifiable);
 reset();fake.all=fake.current=2;fake.productState=INSTALLSTATE_DEFAULT;
 assert(CheckProductContexts(L"p",detail,&cancelled)==ProductPathCheck::Unverifiable);
 reset();fake.contextError=true;assert(CheckProductContexts(L"p",detail,&cancelled)==ProductPathCheck::Unverifiable);
 reset();fake.all=fake.current=1;fake.productState=INSTALLSTATE_ABSENT;
 assert(CheckProductContexts(L"p",detail,&cancelled)==ProductPathCheck::Unverifiable);
 reset();cancelled=true;assert(CheckProductContexts(L"p",detail,&cancelled)==ProductPathCheck::Cancelled);
 reset();fake.rows={{L"{one}",L"C:\\fixed\\one.dll"},{L"{one}",L"C:\\fixed\\one.dll"},
  {L"{two}",L"C:\\fixed\\sub\\two.dll"}};
 assert(CheckProductFileRoots(L"p",L"cached",L"C:\\fixed",detail,&cancelled)==ProductPathCheck::Safe);
 assert(fake.pathCalls==2 && fake.sql.find(L"`File`.`Component_` = `Component`.`Component`")!=std::wstring::npos);
 reset();fake.rows={{L"{one}",L"C:\\fixed\\file.dll"}};fake.extraUnmatched=true;
 assert(CheckProductFileRoots(L"p",L"cached",L"C:\\fixed",detail,&cancelled)==ProductPathCheck::Unverifiable);
 assert(detail.find(L"Unmatched File")!=std::wstring::npos);
 reset();fake.rows={{L"{one}",L"C:\\fixed\\file.dll"}};fake.cachedCode=L"different";
 assert(CheckProductFileRoots(L"p",L"cached",L"C:\\fixed",detail,&cancelled)==ProductPathCheck::Unverifiable && fake.pathCalls==0);
 reset();fake.rows={{L"{one}",L"C:\\fixed\\file.dll"}};fake.tables[L"CustomAction"]=MSICONDITION_TRUE;
 assert(CheckProductFileRoots(L"p",L"cached",L"C:\\fixed",detail,&cancelled)==ProductPathCheck::Unverifiable);
 assert(detail.find(L"CustomAction")!=std::wstring::npos && fake.pathCalls==0);
 reset();fake.rows={{L"{one}",L"C:\\fixed\\file.dll"}};fake.tables[L"RemoveFile"]=MSICONDITION_ERROR;
 assert(CheckProductFileRoots(L"p",L"cached",L"C:\\fixed",detail,&cancelled)==ProductPathCheck::Unverifiable);
 reset();fake.rows={{L"{one}",L"C:\\fixed\\file.dll"}};fake.tables[L"CreateFolder"]=MSICONDITION_TRUE;
 fake.folderRows={{L"{folder}",L"C:\\fixed\\empty"}};
 assert(CheckProductFileRoots(L"p",L"cached",L"C:\\fixed",detail,&cancelled)==ProductPathCheck::Safe && fake.pathCalls==2);
 reset();fake.rows={{L"{one}",L"C:\\fixed\\file.dll"}};fake.tables[L"CreateFolder"]=MSICONDITION_TRUE;
 fake.folderRows={{L"{folder}",L"C:\\fixed\\empty"}};fake.folderRows[0].componentDir=L"elsewhere";
 assert(CheckProductFileRoots(L"p",L"cached",L"C:\\fixed",detail,&cancelled)==ProductPathCheck::Unverifiable);
 reset();fake.rows={{L"{one}",L"C:\\fixed\\file.dll"}};fake.tables[L"CreateFolder"]=MSICONDITION_TRUE;
 fake.folderRows={{L"{folder}",L"C:\\fixed\\empty"}};fake.folderRows[0].keyNull=false;
 assert(CheckProductFileRoots(L"p",L"cached",L"C:\\fixed",detail,&cancelled)==ProductPathCheck::Unverifiable);
 reset();fake.rows={{L"{one}",L"C:\\fixed\\file.dll"}};fake.tables[L"CreateFolder"]=MSICONDITION_TRUE;
 fake.folderRows={{L"{folder}",L"D:\\VS_PROJECT\\Debug\\empty"}};
 assert(CheckProductFileRoots(L"p",L"cached",L"C:\\fixed",detail,&cancelled)==ProductPathCheck::OutsideRoot);
 reset();fake.rows={{L"{one}",L"D:\\VS_PROJECT\\Build\\x86\\Debug\\GlassCompat.dll"}};
 assert(CheckProductFileRoots(L"p",L"cached",L"C:\\fixed",detail,&cancelled)==ProductPathCheck::OutsideRoot);
 assert(detail==fake.rows[0].path);
 reset();fake.rows={{L"{one}",L"C:\\fixed\\linked\\file.dll"}};
 assert(CheckProductFileRoots(L"p",L"cached",L"C:\\fixed",detail,&cancelled)==ProductPathCheck::Unverifiable);
 reset();fake.rows={{L"{one}",L"01:\\Software\\Glass"}};
 assert(CheckProductFileRoots(L"p",L"cached",L"C:\\fixed",detail,&cancelled)==ProductPathCheck::OutsideRoot);
 reset();fake.rows={{L"",L"C:\\fixed\\a.dll"}};
 assert(CheckProductFileRoots(L"p",L"cached",L"C:\\fixed",detail,&cancelled)==ProductPathCheck::Unverifiable);
 reset();fake.rows={{L"{one}",L"C:\\fixed\\a.dll",INSTALLSTATE_ABSENT}};
 assert(CheckProductFileRoots(L"p",L"cached",L"C:\\fixed",detail,&cancelled)==ProductPathCheck::Unverifiable);
 reset();fake.rows={{L"{one}",L"C:\\fixed\\a.dll",INSTALLSTATE_SOURCE}};
 assert(CheckProductFileRoots(L"p",L"cached",L"C:\\fixed",detail,&cancelled)==ProductPathCheck::Unverifiable);
 reset();fake.rows={{L"{one}",L"",INSTALLSTATE_LOCAL}};
 assert(CheckProductFileRoots(L"p",L"cached",L"C:\\fixed",detail,&cancelled)==ProductPathCheck::Unverifiable);
 reset();assert(CheckProductFileRoots(L"p",L"cached",L"C:\\fixed",detail,&cancelled)==ProductPathCheck::Unverifiable);
 reset();fake.openOk=false;assert(CheckProductFileRoots(L"p",L"cached",L"C:\\fixed",detail,&cancelled)==ProductPathCheck::Unverifiable);
 reset();fake.viewOk=false;assert(CheckProductFileRoots(L"p",L"cached",L"C:\\fixed",detail,&cancelled)==ProductPathCheck::Unverifiable);
 reset();fake.executeOk=false;assert(CheckProductFileRoots(L"p",L"cached",L"C:\\fixed",detail,&cancelled)==ProductPathCheck::Unverifiable);
 reset();fake.fetchOk=false;assert(CheckProductFileRoots(L"p",L"cached",L"C:\\fixed",detail,&cancelled)==ProductPathCheck::Unverifiable);
 reset();fake.rows={{L"{one}",L"C:\\fixed\\a.dll"},{L"{two}",L"C:\\fixed\\b.dll"}};
 fake.cancelled=&cancelled;fake.cancelAtPath=1;
 assert(CheckProductFileRoots(L"p",L"cached",L"C:\\fixed",detail,&cancelled)==ProductPathCheck::Cancelled);
 reset();{GlassInstallRollback::SessionState registrations;assert(VerifyProductRegistrations(registrations,&cancelled));}
 reset();fake.all=fake.current=1;fake.productState=INSTALLSTATE_DEFAULT;
 fake.related[L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5010}"]={L"p64"};
 {GlassInstallRollback::SessionState registrations;assert(VerifyProductRegistrations(registrations,&cancelled));}
 reset();fake.all=fake.current=1;fake.productState=INSTALLSTATE_DEFAULT;
 fake.related[L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5010}"]={L"unknown-product"};
 {GlassInstallRollback::SessionState registrations;assert(!VerifyProductRegistrations(registrations,&cancelled));
  assert(registrations.productPathUnverified && registrations.productPathCode==L"unknown-product");}
 reset();fake.all=1;fake.current=0;
 {GlassInstallRollback::SessionState registrations;assert(!VerifyProductRegistrations(registrations,&cancelled));
  assert(registrations.productPathUnverified);}
 reset();cancelled=true;
 {GlassInstallRollback::SessionState registrations;assert(!VerifyProductRegistrations(registrations,&cancelled));
  assert(registrations.copyError==ERROR_CANCELLED && !registrations.productPathUnverified);}
 reset();fake.cachedCode=L"product";fake.productState=INSTALLSTATE_DEFAULT;fake.rows={{L"{one}",L"D:\\VS_PROJECT\\Debug\\file.dll"}};
 GlassInstallRollback::SessionState s;
 assert(!TrySnapshotProduct(s,L"product",L"C:\\fixed",&cancelled));
 assert(s.oldProducts.empty() && s.productPathUnverified && s.productPathDetail==fake.rows[0].path && fake.copyCalls==1);
 reset();fake.cachedCode=L"product";fake.productState=INSTALLSTATE_DEFAULT;fake.rows={{L"{one}",L"C:\\fixed\\file.dll"}};
 GlassInstallRollback::SessionState good;
 assert(TrySnapshotProduct(good,L"product",L"C:\\fixed",&cancelled));
 assert(good.oldProducts.size()==1 && good.oldProducts[0].installDir==L"C:\\fixed");
 reset();fake.productState=INSTALLSTATE_DEFAULT;fake.copyOk=false;
 GlassInstallRollback::SessionState missing;
 assert(!TrySnapshotProduct(missing,L"product",L"C:\\fixed",&cancelled) && missing.oldProducts.empty());
 reset();fake.all=fake.current=1;fake.productState=INSTALLSTATE_DEFAULT;fake.cachedCode=L"gs64";
 fake.rows={{L"{one}",L"D:\\VS_PROJECT\\Debug\\file.dll"}};
 {GlassInstallRollback::SessionState rollback;rollback.x64Dir=L"C:\\fixed";
  assert(!GlassInstallRollback::UninstallSessionProductSafely(rollback,L"gs64") && fake.uninstallCalls==0);}
 reset();fake.all=fake.current=1;fake.productState=INSTALLSTATE_DEFAULT;fake.cachedCode=L"gs64";
 fake.rows={{L"{one}",L"C:\\fixed\\GlassSettings\\file.dll"}};
 {GlassInstallRollback::SessionState rollback;rollback.x64Dir=L"C:\\fixed";
  assert(GlassInstallRollback::UninstallSessionProductSafely(rollback,L"gs64") && fake.uninstallCalls==1);}
 reset();fake.all=1;fake.current=0;fake.productState=INSTALLSTATE_UNKNOWN;
 {GlassInstallRollback::SessionState rollback;
  assert(!GlassInstallRollback::UninstallSessionProductSafely(rollback,L"gs64") && fake.uninstallCalls==0);}
 reset();{GlassInstallRollback::SessionState rollback;
  assert(GlassInstallRollback::UninstallSessionProductSafely(rollback,L"gs64") && fake.uninstallCalls==0);}
 std::cout<<"PASS: real MSI guard: fixed roots, components, context, cancellation, snapshot and rollback uninstall\n";
}
'''

with tempfile.TemporaryDirectory(prefix="glass-fixed-msi-") as directory:
    cpp = Path(directory) / "fixed.cpp"
    exe = Path(directory) / "fixed"
    cpp.write_text(prefix + contexts + "\n" + file_roots + middle + verify_registrations + "\n" + snapshot +
                   "\nnamespace GlassInstallRollback {\n" + safe_uninstall + "\n}\n" + main, encoding="utf-8")
    compiler = os.environ.get("CXX", "g++")
    subprocess.run([compiler, "-std=c++17", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
                    str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("PASS: fixed Setup/Unins roots, stage order and WiX UpgradeCode invariants")
