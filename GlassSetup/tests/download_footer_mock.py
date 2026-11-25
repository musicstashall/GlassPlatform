"""Actual native footer summary: concise retry/cancel copy, no reselection advice."""
from pathlib import Path
import re, subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/main.cpp').read_text(encoding='utf-8')
ru,en=(root/'GlassSetup/res/strings/Setup.rcinc').read_text(encoding='utf-8').split('LANGUAGE LANG_ENGLISH, SUBLANG_ENGLISH_US')
keys=('IDS_DOWNLOAD_PREPARE_FAILED','IDS_DOWNLOAD_PACKAGES_READY','IDS_DOWNLOAD_PROGRESS_PREFIX')
def literal(table,key):
 return re.search(r'^\s*'+key+r' (".*")$',table,re.MULTILINE)[1]
strings='\n'.join(f' case {key}:return gLanguage==25?L{literal(ru,key)}:L{literal(en,key)};' for key in keys)
a=s.index('    std::wstring NativeDownloadSummary() const {')
fn=s[a:s.index('    std::wstring DownloadsJson() const {',a)]
h=r'''
#include <string>
#include <array>
#include <memory>
#include <atomic>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include "resource.h"
unsigned gLanguage=25;
std::wstring Text(unsigned id){switch(id){
 // STRINGS
 default:assert(false);return {};
}}
struct Job{std::atomic<bool> cancel{false},done{false};bool ok=false;std::atomic<std::uint64_t> total{100},bytes{0};};
struct UI{struct {std::array<std::shared_ptr<Job>,7> jobs;} downloads_;bool SimulationBuild()const{return false;}
// FN
};
int main(){UI ui;assert(ui.NativeDownloadSummary().empty());auto j=std::make_shared<Job>();ui.downloads_.jobs[0]=j;
 j->done=true;j->ok=false;
 assert(ui.NativeDownloadSummary()==L"Ошибка подготовки. Повторите загрузку или отмените.");
 assert(ui.NativeDownloadSummary().size()<=52);
 gLanguage=9;assert(ui.NativeDownloadSummary()==L"Preparation failed. Retry the download or cancel.");
 assert(ui.NativeDownloadSummary().size()<=52);
 j->ok=true;assert(ui.NativeDownloadSummary()==L"Selected packages are verified and ready.");
 j->cancel=true;assert(ui.NativeDownloadSummary().empty());
}
'''.replace('// FN',fn).replace('// STRINGS',strings)
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';exe=Path(d)/'test';p.write_text(h,encoding='utf-8')
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(root/'GlassSetup/res'),str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: actual RU/EN footer: concise failure/retry/cancel, ready, cancelled/empty states')
