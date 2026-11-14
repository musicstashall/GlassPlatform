"""Actual portable Inno/LZMA reader on synthetic fixtures, not an upstream EXE.
Does not prove Windows Authenticode, real 6.7 layout or product servicing.
"""
from pathlib import Path
import hashlib,lzma,struct,subprocess,tempfile,zlib
root=Path(__file__).resolve().parents[2]
def crc(b):return struct.pack('<I',zlib.crc32(b))
def packed(b):
    props=b'\x5d'+struct.pack('<I',1<<20)
    return props+lzma.compress(b,format=lzma.FORMAT_RAW,filters=[{'id':lzma.FILTER_LZMA1,'dict_size':1<<20,'lc':3,'lp':0,'pb':2}])
def block(b,compressed):
    stream=packed(b) if compressed else b
    stored=b''.join(crc(stream[i:i+4096])+stream[i:i+4096] for i in range(0,len(stream),4096))
    header=struct.pack('<QB',len(stored),int(compressed))
    return crc(header)+header+stored
payloads=[b'first PE placeholder',b'second PE placeholder',b'host placeholder',b'gui placeholder']
def fixture(compressed=True):
    plain=b''.join(payloads);chunk=b'zlb\x1a'+(packed(plain) if compressed else plain)
    records=b'';at=0
    for p in payloads:
        records+=struct.pack('<IIQQQQ',0,0,0,at,len(p),len(chunk))+hashlib.sha256(p).digest()+struct.pack('<QIIB',0,0x30000,3749,0x10 if compressed else 0)
        at+=len(p)
    header=b'Inno Setup Setup Data (6.7.0)'.ljust(64,b'\0')+bytes(53)+block(b'string table',compressed)+block(records,compressed)
    body_at=128;header_at=body_at+len(chunk)+16
    data=bytearray(header_at+len(header));data[body_at:body_at+len(chunk)]=chunk;data[header_at:]=header
    table=b'rDlPtS\xcd\xe6\xd7\x7b\x0b\x2a'+struct.pack('<IQQIiQQI',2,len(data),0,0,0,header_at,body_at,0)
    return bytes(data),table+crc(table)
cpp=r'''
#include "GlassSetup/src/InstallSupport/GlassInnoExtract.h"
#include <fstream>
#include <iterator>
#include <cassert>
using namespace GlassInnoExtract;
Bytes read(const char* p){std::ifstream f(p,std::ios::binary);return Bytes(std::istreambuf_iterator<char>(f),{});}
int main(int argc,char** argv){
 if(argc<3)return 3;
 try{auto in=read(argv[1]),table=read(argv[2]);
 auto files=Extract(View(in),View(table),[&]{return argc>3;});assert(files.size()==4);
 assert(std::string(files[0].bytes.begin(),files[0].bytes.end())=="first PE placeholder");
 assert(std::string(files[3].bytes.begin(),files[3].bytes.end())=="gui placeholder");
 Bytes filter(65542,0);filter[65532]=0xe8;filter[65533]=1; // tail opcode must not be touched
 filter[65536]=0xe9;filter[65537]=5;auto tail=filter;UndoCalls(filter);
 assert(filter[65533]==tail[65533]);assert(filter[65537]==0);
 return 0;}catch(const std::exception&){return 2;}
}
'''
with tempfile.TemporaryDirectory() as tmp:
 d=Path(tmp);(d/'test.cpp').write_text(cpp)
 subprocess.run(['gcc','-std=c99','-c',str(root/'GlassSetup/third_party/lzma/LzmaDec.c'),'-o',str(d/'lzma.o')],check=True)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(root),str(d/'test.cpp'),str(d/'lzma.o'),'-o',str(d/'test')],check=True)
 def run(data,table,ok=False,cancel=False):
    (d/'input').write_bytes(data);(d/'table').write_bytes(table)
    result=subprocess.run([str(d/'test'),str(d/'input'),str(d/'table')]+(['cancel'] if cancel else []))
    assert result.returncode==(0 if ok else 2),result.returncode
 for compressed in [False,True]:
    data,table=fixture(compressed);run(data,table,True)
    run(data,table,cancel=True)
    run(data,table[:-1]);run(data[:-1],table)
    broken=bytearray(table);broken[60]^=1;run(data,broken)
    broken=bytearray(data);broken[-1]^=1;run(broken,table)
    swapped=bytearray(table);swapped[40:48],swapped[48:56]=table[48:56],table[40:48];swapped[60:]=crc(swapped[:60]);run(data,swapped,True)
print('PASS: actual bounded Inno reader/LZMA decoder: raw/compressed headers and chunks, CRC/range rejection, cancellation, reversed offsets, per-file 64K CALL/JMP boundaries')

# Separate integration boundaries: portable parser never publishes/install files.
provider=(root/'GlassSetup/src/InstallSupport/GlassOpenGlassPackage.cpp').read_text()
for token in ['LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_IMAGE_RESOURCE','MAKEINTRESOURCEW(11111)',
              'status==ERROR_SUCCESS','Open Source Developer, Zhe Yang',
              'VerifyPublisher(path,verified.h)==publisher','file.sha256','windowsBuild>=28000',
              'GlassMaintenanceCache::TrustedDirectory','GlassReleaseClient::VerifyHash',
              'OriginalFilename','FileDescription','OpenGlassGUI.exe','OpenGlassHost.exe']:
    assert token in provider,token
assert 'CreateProcess' not in provider and 'ShellExecute' not in provider
assert provider.index('VerifyPublisher(setup,pinned.h)') < provider.index('LoadLibraryExW')
policy=(root/'GlassSetup/src/GlassProducts.h').read_text()
a=policy.index('constexpr bool DefaultSelectedFor(');b=policy.index('constexpr Selection NormalizeInstall',a)
assert 'return false;' in policy[a:b]
print('PASS: provider source gates and explicit-choice default; Windows provider is not executed by this test')

queue=(root/'GlassSetup/src/GlassProductDownloads.h').read_text()
assert 'else if(i==4)' in queue and 'GlassOpenGlassPackage::PrepareForCurrentSystem' in queue
assert 'else if(i<4)' in queue
engine=(root/'GlassSetup/src/InstallEngine.cpp').read_text()
assert 'p.product==Product::OpenGlass?' in engine
assert 'GlassOpenGlassPackage::PrepareForCurrentSystem(tempDir_' in engine
assert 'ReadMetadata(prepared+L".metadata.json")' in provider
assert 'TrustedPath(prepared,false)' in provider
assert 'Prepared OpenGlass hash changed' in provider
assert provider.index('Prepared OpenGlass hash changed') < provider.index('const auto publisher=VerifyPublisher')
print('PASS: production source routing: OpenGlass Next/engine preparation bypasses MSI acquisition; prepared reuse rechecks protected metadata/hash/signature')
