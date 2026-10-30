#!/usr/bin/env python3
"""Exercise snapshot/restore destination checks using a fake filesystem."""
from pathlib import Path
import os
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
source = (repo / "GlassSetup/src/InstallSupport/GlassInstallRollback.cpp").read_text(encoding="utf-8")

def function(token):
    start = source.index(token); brace = source.index("{", start); depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{": depth += 1
        if source[pos] == "}":
            depth -= 1
            if depth == 0: return source[start:pos + 1]
    raise ValueError(token)

file_copy = function("bool CopyFileLogged(")
tree = function("bool CopyTree(")
prefix = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cwchar>
#include <functional>
#include <iostream>
#include <map>
#include <string>
using DWORD=std::uint32_t;using uint64_t=std::uint64_t;using HANDLE=void*;using BOOL=int;
constexpr DWORD INVALID_FILE_ATTRIBUTES=0xffffffff,FILE_ATTRIBUTE_DIRECTORY=0x10,FILE_ATTRIBUTE_REPARSE_POINT=0x400,
 ERROR_FILE_NOT_FOUND=2,ERROR_PATH_NOT_FOUND=3,ERROR_GEN_FAILURE=31,ERROR_ACCESS_DENIED=5,
 ERROR_ALREADY_EXISTS=183,ERROR_NOT_SUPPORTED=50,ERROR_DISK_FULL=112,ERROR_CANCELLED=1223,ERROR_NO_MORE_FILES=18;
#define INVALID_HANDLE_VALUE reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-1))
struct WIN32_FIND_DATAW {wchar_t cFileName[260]{};DWORD dwFileAttributes=0,nFileSizeHigh=0,nFileSizeLow=0;};
struct Fake {std::map<std::wstring,DWORD> attributes;DWORD error=0;int copies=0,enumerations=0;bool denyTarget=false,unsafeAncestor=false;} fake;
namespace GlassInstallUtil {bool IsSafeInstallPath(const std::wstring&){return !fake.unsafeAncestor;}}
DWORD GetLastError(){return fake.error;}
DWORD GetFileAttributesW(const wchar_t* path){
 if(fake.denyTarget && std::wstring(path).find(L"dest")==0){fake.error=ERROR_ACCESS_DENIED;return INVALID_FILE_ATTRIBUTES;}
 auto it=fake.attributes.find(path);if(it==fake.attributes.end()){fake.error=ERROR_FILE_NOT_FOUND;return INVALID_FILE_ATTRIBUTES;}
 return it->second;
}
BOOL CreateDirectoryW(const wchar_t* path,void*){if(fake.attributes.count(path)){fake.error=ERROR_ALREADY_EXISTS;return 0;}
 fake.attributes[path]=FILE_ATTRIBUTE_DIRECTORY;return 1;}
int CopyProgress=0;
BOOL CopyFileExW(const wchar_t*,const wchar_t*,int,void*,void*,int){++fake.copies;return 1;}
HANDLE FindFirstFileW(const wchar_t*,WIN32_FIND_DATAW*){++fake.enumerations;fake.error=ERROR_FILE_NOT_FOUND;return INVALID_HANDLE_VALUE;}
BOOL FindNextFileW(HANDLE,WIN32_FIND_DATAW*){fake.error=ERROR_NO_MORE_FILES;return 0;}
BOOL FindClose(HANDLE){return 1;}
bool SamePathInsensitive(const std::wstring& a,const std::wstring& b){return a==b;}
namespace GlassInstallLog {template<class... T>void LineFormat(const wchar_t*,T...) {}}
namespace GlassInstallRollback {using CopyProgressFn=std::function<void(int)>;}
'''
main = r'''
int main(){unsigned error=0;uint64_t copied=0;
 auto reset=[&](){fake={};error=0;copied=0;};
 reset();fake.attributes[L"dest-file"]=FILE_ATTRIBUTE_REPARSE_POINT;
 assert(!CopyFileLogged(L"backup-file",L"dest-file",error,nullptr));assert(error==ERROR_NOT_SUPPORTED && fake.copies==0);
 reset();fake.denyTarget=true;assert(!CopyFileLogged(L"backup-file",L"dest-file",error,nullptr));
 assert(error==ERROR_ACCESS_DENIED && fake.copies==0);
 reset();assert(CopyFileLogged(L"backup-file",L"dest-file",error,nullptr) && fake.copies==1);
 reset();fake.unsafeAncestor=true;assert(!CopyFileLogged(L"backup-file",L"dest-file",error,nullptr));
 assert(error==ERROR_NOT_SUPPORTED && fake.copies==0);
 reset();fake.attributes={{L"backup",FILE_ATTRIBUTE_DIRECTORY},{L"dest",FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT}};
 assert(!CopyTree(L"backup",L"dest",{},copied,1,nullptr,error,nullptr));
 assert(error==ERROR_NOT_SUPPORTED && fake.enumerations==0 && fake.copies==0);
 reset();fake.attributes={{L"backup",FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT}};
 assert(!CopyTree(L"backup",L"dest",{},copied,1,nullptr,error,nullptr) && error==ERROR_NOT_SUPPORTED);
 reset();fake.attributes={{L"backup",FILE_ATTRIBUTE_DIRECTORY},{L"dest",FILE_ATTRIBUTE_DIRECTORY}};
 assert(CopyTree(L"backup",L"dest",{},copied,1,nullptr,error,nullptr) && fake.enumerations==1);
 std::cout<<"PASS: rollback copy rejects destination/source reparse points and unreadable targets\n";
}
'''
with tempfile.TemporaryDirectory(prefix="glass-rollback-dest-") as directory:
    cpp = Path(directory) / "test.cpp"; exe = Path(directory) / "test"
    cpp.write_text(prefix + file_copy + "\n" + tree + "\n" + main, encoding="utf-8")
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-g", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-no-pie", str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
