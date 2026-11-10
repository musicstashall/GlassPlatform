"""Production Wide conversion: missing optional version isn't malformed UTF-8."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassReleaseClient.cpp').read_text()
a=s.index('std::wstring Wide(');b=s.index('\nbool Fail(',a)
cpp=r'''
#include <string>
#include <cassert>
#include <codecvt>
#include <locale>
#include <algorithm>
constexpr int CP_UTF8=65001,MB_ERR_INVALID_CHARS=8;
int calls=0;
int MultiByteToWideChar(int page,int flags,const char* input,int size,wchar_t* out,int capacity){
 ++calls;assert(page==CP_UTF8&&flags==MB_ERR_INVALID_CHARS);
 if(!size)return 0;
 try{std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
 auto wide=converter.from_bytes(input,input+size);
 if(out){assert(capacity>=int(wide.size()));std::copy(wide.begin(),wide.end(),out);}
 return int(wide.size());}catch(...){return 0;}
}
// PRODUCTION
int main(){assert(Wide("").empty()&&calls==0);
 assert(Wide("2.0.1.0")==L"2.0.1.0");
 assert(Wide("Версия")==L"Версия");
 assert(Wide(std::string(1,char(0xff)))==L"Invalid UTF-8 response");
}
'''.replace('// PRODUCTION',s[a:b])
with tempfile.TemporaryDirectory() as temp:
 p=Path(temp)/'test.cpp';p.write_text(cpp);exe=Path(temp)/'test'
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: production release UTF-8 conversion: absent metadata, numeric version, Cyrillic and invalid bytes')
