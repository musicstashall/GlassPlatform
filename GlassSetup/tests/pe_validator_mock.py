#!/usr/bin/env python3
"""Run the actual PE/export inspector against synthetic 32/64-bit files.

This models Win32 file mapping; it does not execute a DLL or use Windows.
"""
from pathlib import Path
import os
import struct
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "GlassSetup/src/InstallSupport/GlassInstallUtil.cpp").read_text(encoding="utf-8")

def function(token):
    start = source.index(token)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{": depth += 1
        if source[pos] == "}":
            depth -= 1
            if depth == 0: return source[start:pos + 1]
    raise ValueError(token)

inspector = function("bool PeMatches(")
prefix = r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
using BYTE=std::uint8_t; using WORD=std::uint16_t; using DWORD=std::uint32_t; using BOOL=int;
using HANDLE=void*;
#define INVALID_HANDLE_VALUE reinterpret_cast<HANDLE>(-1)
constexpr DWORD GENERIC_READ=1, FILE_SHARE_READ=2, FILE_SHARE_WRITE=4, FILE_SHARE_DELETE=8,
 OPEN_EXISTING=3, FILE_ATTRIBUTE_NORMAL=0, PAGE_READONLY=1, FILE_MAP_READ=1;
constexpr WORD IMAGE_DOS_SIGNATURE=0x5a4d,IMAGE_NT_OPTIONAL_HDR32_MAGIC=0x10b,IMAGE_NT_OPTIONAL_HDR64_MAGIC=0x20b;
constexpr DWORD IMAGE_NT_SIGNATURE=0x4550, IMAGE_DIRECTORY_ENTRY_EXPORT=0;
struct LARGE_INTEGER{std::int64_t QuadPart=0;};
#pragma pack(push,1)
struct IMAGE_DOS_HEADER{WORD e_magic;BYTE unused[58];std::int32_t e_lfanew;};
struct IMAGE_FILE_HEADER{WORD Machine,NumberOfSections;DWORD TimeDateStamp,PointerToSymbolTable,NumberOfSymbols;WORD SizeOfOptionalHeader,Characteristics;};
struct IMAGE_DATA_DIRECTORY{DWORD VirtualAddress,Size;};
struct IMAGE_OPTIONAL_HEADER32{BYTE unused[96];IMAGE_DATA_DIRECTORY DataDirectory[16];};
struct IMAGE_OPTIONAL_HEADER64{BYTE unused[112];IMAGE_DATA_DIRECTORY DataDirectory[16];};
struct IMAGE_SECTION_HEADER{BYTE Name[8];DWORD VirtualSize,VirtualAddress,SizeOfRawData,PointerToRawData,
 PointerToRelocations,PointerToLinenumbers;WORD NumberOfRelocations,NumberOfLinenumbers;DWORD Characteristics;};
struct IMAGE_EXPORT_DIRECTORY{DWORD Characteristics,TimeDateStamp;WORD MajorVersion,MinorVersion;DWORD Name,Base,
 NumberOfFunctions,NumberOfNames,AddressOfFunctions,AddressOfNames,AddressOfNameOrdinals;};
#pragma pack(pop)
static_assert(sizeof(IMAGE_DOS_HEADER)==64 && sizeof(IMAGE_FILE_HEADER)==20 && sizeof(IMAGE_SECTION_HEADER)==40);
static_assert(offsetof(IMAGE_OPTIONAL_HEADER32,DataDirectory)==96 && offsetof(IMAGE_OPTIONAL_HEADER64,DataDirectory)==112);
static std::vector<BYTE> bytes;
HANDLE CreateFileW(const wchar_t*,DWORD,DWORD,void*,DWORD,DWORD,void*){return reinterpret_cast<HANDLE>(1);}
BOOL GetFileSizeEx(HANDLE,LARGE_INTEGER* size){size->QuadPart=static_cast<std::int64_t>(bytes.size());return 1;}
HANDLE CreateFileMappingW(HANDLE,void*,DWORD,DWORD,DWORD,void*){return reinterpret_cast<HANDLE>(2);}
BOOL CloseHandle(HANDLE){return 1;}
void* MapViewOfFile(HANDLE,DWORD,DWORD,DWORD,std::size_t){return bytes.data();}
BOOL UnmapViewOfFile(const void*){return 1;}
'''
main = r'''
int main(int argc,char** argv){
 assert(argc==4);std::ifstream file(argv[1],std::ios::binary);
 bytes.assign(std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>());
 const WORD machine=static_cast<WORD>(std::stoul(argv[2],nullptr,16));
 const bool expected=std::string(argv[3])=="1";
 assert(PeMatches(L"fixture",machine,"GlassSettings_InstallerScanProtocolV1")==expected);
 std::cout<<"PASS: PE fixture "<<argv[1]<<"\n";
}
'''

def fixture(machine, marker=True, malformed=False):
    data = bytearray(0x600)
    struct.pack_into("<H", data, 0, 0x5A4D)
    struct.pack_into("<I", data, 0x3C, 0x80)
    struct.pack_into("<I", data, 0x80, 0x4550)
    is64 = machine == 0x8664
    opt_size, magic, directory_offset = (240, 0x20B, 112) if is64 else (224, 0x10B, 96)
    struct.pack_into("<HHIIIHH", data, 0x84, machine, 1, 0, 0, 0, opt_size, 0)
    optional = 0x98
    struct.pack_into("<H", data, optional, magic)
    struct.pack_into("<II", data, optional + directory_offset, 0x1000, 0x100)
    section = optional + opt_size
    struct.pack_into("<IIII", data, section + 8, 0x400, 0x1000, 0x400, 0x200)
    # IMAGE_EXPORT_DIRECTORY: name count and pointer to table of names.
    struct.pack_into("<I", data, 0x200 + 24, 2 if not malformed else 9000)
    struct.pack_into("<I", data, 0x200 + 32, 0x1040)
    struct.pack_into("<II", data, 0x240, 0x1080, 0x10C0)
    data[0x280:0x280+len(b"GlassSettings_UnlockerW\0")] = b"GlassSettings_UnlockerW\0"
    name = b"GlassSettings_InstallerScanProtocolV1\0" if marker else b"GlassSettings_ScanGlassModules\0"
    data[0x2C0:0x2C0+len(name)] = name
    return data

with tempfile.TemporaryDirectory(prefix="glass-pe-validator-") as directory:
    directory = Path(directory)
    cpp, exe = directory / "test.cpp", directory / "test"
    cpp.write_text(prefix + inspector + "\n" + main, encoding="utf-8")
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-g", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-no-pie", str(cpp), "-o", str(exe)], check=True)
    cases = [
        ("x86-valid", fixture(0x14C), "14c", True),
        ("x64-valid", fixture(0x8664), "8664", True),
        ("wrong-arch", fixture(0x14C), "8664", False),
        ("old-x86", fixture(0x14C, marker=False), "14c", False),
        ("malformed", fixture(0x14C, malformed=True), "14c", False),
        ("truncated", fixture(0x14C)[:200], "14c", False),
    ]
    for name, data, machine, expected in cases:
        path = directory / (name + ".bin")
        path.write_bytes(data)
        subprocess.run([str(exe), str(path), machine, "1" if expected else "0"], check=True)
