#!/usr/bin/env python3
"""Exercise the real native scan TSV/frame parser with mocked UTF-8 conversion."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "GlassSetup/src/InstallSupport/GlassInstallWatch.cpp").read_text(encoding="utf-8")

def extract(begin, end):
    start = source.index(begin)
    return source[start:source.index(end, start)]

merge_helpers = extract("std::wstring TrimToken(", "std::wstring ModuleNamesFromBits(")
consume = extract("bool ConsumeNextPipeEvent(", "} // namespace")
parser = extract("void MergeModuleTokens(", "bool CollectInProcessHolders(")
prefix = r'''
#include <cassert>
#include <codecvt>
#include <cwchar>
#include <cstdint>
#include <iostream>
#include <locale>
#include <string>
#include <vector>
using DWORD=std::uint32_t;
#define _wcsicmp wcscasecmp
constexpr int CP_UTF8=65001,MB_ERR_INVALID_CHARS=8;
DWORD GetCurrentProcessId(){return 1234;}
int MultiByteToWideChar(int,int,const char* text,int size,wchar_t* out,int outSize){
 try{
  std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
  const auto wide=converter.from_bytes(text,text+size);
  if(!out)return static_cast<int>(wide.size());
  if(outSize<static_cast<int>(wide.size()))return 0;
  std::copy(wide.begin(),wide.end(),out);return static_cast<int>(wide.size());
 }catch(...){return 0;}
}
namespace GlassInstallWatch {
 struct ModuleHolder{DWORD pid;std::wstring exe,modules;};
 void MergeModuleTokens(std::wstring&,const std::wstring&);
 bool ParseNativePipeUtf8(const std::string&,std::vector<ModuleHolder>&);
}
'''
main = r'''
using namespace GlassInstallWatch;
int main(){
 std::vector<ModuleHolder> rows;
 assert(ParseNativePipeUtf8("",rows));assert(rows.empty());
 assert(ParseNativePipeUtf8("42\texplorer.exe\tGlassCore.dll\r\n42\texplorer.exe\tGlassCore.dll, WndCompat.dll\n",rows));
 assert(rows.size()==1 && rows[0].pid==42 && rows[0].modules==L"GlassCore.dll, WndCompat.dll");
 rows.clear();assert(ParseNativePipeUtf8("1234\tGlassSetup.exe\tGlassCore.dll\n",rows));assert(rows.size()==1 && rows[0].pid==1234);
 assert(ParseNativePipeUtf8("88\tGlassSetup.exe\tGlassCore.dll\n",rows));assert(rows.size()==2 && rows[1].pid==88);
 for(const auto& invalid:std::vector<std::string>{
   "42\texe\tmodule", "0\texe\tmodule\n", "4294967296\texe\tmodule\n",
   "-2\texe\tmodule\n", "2\t\tmodule\n", "2\texe\t\n", "2\texe\tmodule\textra\n",
   std::string("2\texe\t\xff\n",8),"#empty\n",std::string(1024*1024+1,'x')}){
   std::vector<ModuleHolder> ignored;assert(!ParseNativePipeUtf8(invalid,ignored));
 }
 bool empty=false,end=false,malformed=false;std::string buffer="#emp";rows.clear();
 assert(!ConsumeNextPipeEvent(buffer,rows,empty,end,malformed));buffer+="ty\n";
 assert(ConsumeNextPipeEvent(buffer,rows,empty,end,malformed));assert(empty && !end && !malformed);
 buffer="42\texplorer.exe\tGlassCore.dll\n43\tapp.exe\tWndCompat.dll\n#end\n#empty\n";
 assert(ConsumeNextPipeEvent(buffer,rows,empty,end,malformed));assert(end && !empty && !malformed && rows.size()==2);
 rows.clear();assert(ConsumeNextPipeEvent(buffer,rows,empty,end,malformed));assert(empty && !end && !malformed);
 buffer="#end\n";rows.clear();
 assert(ConsumeNextPipeEvent(buffer,rows,empty,end,malformed));assert(malformed && !end);
 buffer="1234\tGlassSetup.exe\tGlassCore.dll\n#end\n";rows.clear();
 assert(ConsumeNextPipeEvent(buffer,rows,empty,end,malformed));assert(end && !malformed && rows.size()==1);
 buffer="42\texplorer.exe\tGlassCore.dll\n#empty\n";rows.clear();
 assert(ConsumeNextPipeEvent(buffer,rows,empty,end,malformed));assert(malformed && !empty);
 buffer="42\texplorer.exe\tGlassCore.dll\npartial";rows.clear();
 assert(ConsumeNextPipeEvent(buffer,rows,empty,end,malformed));assert(!end && !empty && !malformed && rows.size()==1);
 assert(!ConsumeNextPipeEvent(buffer,rows,empty,end,malformed));buffer="\n#end\n";
 assert(ConsumeNextPipeEvent(buffer,rows,empty,end,malformed));assert(malformed);
 std::cout<<"PASS: real helper parser: UTF-8, rows, identity, partial and malformed frames\n";
}
'''
with tempfile.TemporaryDirectory(prefix="glass-protocol-test-") as directory:
    cpp = Path(directory) / "protocol.cpp"
    exe = Path(directory) / "protocol-test"
    cpp.write_text(prefix + merge_helpers + consume + "namespace GlassInstallWatch {\n" + parser + "\n}\n" + main,
                   encoding="utf-8")
    compiler = os.environ.get("CXX", "g++")
    subprocess.run([compiler, "-std=c++17", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
