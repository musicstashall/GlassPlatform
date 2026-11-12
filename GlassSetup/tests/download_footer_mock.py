"""Actual native footer summary: concise retry/cancel copy, no reselection advice."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/main.cpp').read_text()
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
#define PRIMARYLANGID(x) (x)
constexpr unsigned LANG_RUSSIAN=25;unsigned gLanguage=25;
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
'''.replace('// FN',fn)
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';exe=Path(d)/'test';p.write_text(h)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: actual RU/EN footer: concise failure/retry/cancel, ready, cancelled/empty states')
