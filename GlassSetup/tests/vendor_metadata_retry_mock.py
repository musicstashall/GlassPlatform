"""Compile the actual metadata retry worker and UI-thread completion, fake vendor HTTP."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/main.cpp').read_text()
a=s.index('    void RetryVendorMetadata(');b=s.index('    bool BeginProductInspection()',a)
a2=s.index('        case kVendorMetadataReady:')+len('        case kVendorMetadataReady:')
b2=s.index('        case kInspectReady:',a2)
h=r'''
#include <atomic>
#include <thread>
#include <mutex>
#include <string>
#include <cassert>
using UINT=unsigned;
constexpr UINT kVendorMetadataReady=10;
int window=1;unsigned posted=0;bool requestOk=false;int calls=0;unsigned result=0;int product=0;
void PostMessageW(int,UINT,unsigned w,int l){++posted;result=w;product=l;}
void EnableWindow(int,bool){}
namespace InstallLog{template<class... T>void LineFormat(const wchar_t*,T...) {}}
namespace GlassSecureUxPackage{
struct Offer{std::wstring releaseVersion;};
bool Describe(const std::atomic_bool&,Offer& o,std::wstring& error){++calls;if(requestOk)o.releaseVersion=L"4.0.0";else error=L"GitHub HTTP error (403)";return requestOk;}
}
namespace GlassOpenGlassPackage{
bool Describe(const std::atomic_bool&,std::wstring& version,std::uint64_t& size,std::wstring& error){++calls;size=1000;if(requestOk)version=L"3.0.2.3749";else error=L"GitHub HTTP error (403)";return requestOk;}
}
struct Wizard{
 std::thread vendorMetadataThread_;std::mutex inspectMutex_;
 std::atomic_bool inspectReady_{true},inspectStop_{false};
 bool closing_=false,ogMetadataBlocked_=true,sutMetadataBlocked_=true,failurePending_=false;
 std::wstring vendorMetadataRetry_;int next_=1,syncs=0;
 struct{std::wstring openGlassDiscoveryError,secureUxDiscoveryError;struct{std::wstring remotePayloadVersion;}og;GlassSecureUxPackage::Offer secureUxOffer;}inspect_;
 bool ComponentsReady(){return true;}void SyncWebViewState(){++syncs;}
 // WORKER
 int Complete(){auto w=result;auto l=product;
 // COMPLETE
 }
 void Wait(){if(vendorMetadataThread_.joinable())vendorMetadataThread_.join();}
 ~Wizard(){Wait();}
};
int main(){
 for(const auto* id:{L"openglass",L"secureUxTheme"}){
  Wizard w;calls=posted=0;requestOk=false;
  w.RetryVendorMetadata(id);w.RetryVendorMetadata(id);w.Wait();assert(calls==1&&posted==1);
  assert(!w.vendorMetadataRetry_.empty());w.Complete();assert(w.vendorMetadataRetry_.empty());
  assert(w.ogMetadataBlocked_&&w.sutMetadataBlocked_);
  requestOk=true;w.RetryVendorMetadata(id);w.Wait();w.Complete();assert(calls==2);
  const bool og=std::wstring(id)==L"openglass";
  assert(w.ogMetadataBlocked_==!og&&w.sutMetadataBlocked_==og);
  assert(og?w.inspect_.og.remotePayloadVersion==L"3.0.2.3749":w.inspect_.secureUxOffer.releaseVersion==L"4.0.0");
  w.RetryVendorMetadata(id);w.Wait();assert(calls==2); // no retry on healthy card
 }
}
'''.replace('// WORKER',s[a:b]).replace('// COMPLETE',s[a2:b2])
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(h);exe=Path(d)/'test'
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-pthread',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: actual vendor retry worker: scoped request, duplicate suppression, failed retry stays blocked, success updates only its vendor and unlocks')
