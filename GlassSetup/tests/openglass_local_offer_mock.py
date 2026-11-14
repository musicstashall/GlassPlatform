"""Compile actual read-only adjacent OG discovery with fake signer and file APIs."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassOpenGlassPackage.cpp').read_text()
a=s.index('bool DescribeLocal(');b=s.index('bool Describe(const',a)
h=r'''
#include <string>
#include <stdexcept>
#include <cassert>
namespace GlassSetupSourceLayout{std::wstring directory=L"source";}
bool signedOk=true,exists=true,versionOk=true;
struct File{int h=1;};void Pin(File&,const std::wstring&){if(!exists)throw std::runtime_error("absent");}
int VerifyPublisher(const std::wstring&,int){if(!signedOk)throw std::runtime_error("bad signer");return 1;}
std::wstring SetupFileVersion(const std::wstring&){if(!versionOk)throw std::runtime_error("bad version");return L"3.0.2.3749";}
// METHOD
int main(){std::wstring v,p;assert(DescribeLocal(v,p)&&v==L"3.0.2.3749"&&p==L"source\\OpenGlassSetup.Signed.exe");
 signedOk=false;assert(!DescribeLocal(v,p)&&v.empty()&&p.empty());signedOk=true;
 exists=false;assert(!DescribeLocal(v,p));exists=true;
 versionOk=false;assert(!DescribeLocal(v,p));versionOk=true;
 GlassSetupSourceLayout::directory.clear();assert(!DescribeLocal(v,p));}
'''.replace('// METHOD',s[a:b])
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(h);exe=Path(d)/'test'
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: actual adjacent OG offer: pinned read-only signature/version check, missing/unsafe offers never authorize local apply')
