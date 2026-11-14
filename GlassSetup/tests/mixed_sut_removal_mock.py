"""Actual irreversible SUT gate and worker order/partial completion contracts."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallEngine.cpp').read_text()
a=s.index('bool InstallEngine::CommitEarlySecureUxRemoval()')
method=s[a:s.index('bool InstallEngine::InstallOriginalSecureUxTheme()',a)]
h=r'''
#include <atomic>
#include <cassert>
#include <initializer_list>
using DWORD=unsigned;
constexpr unsigned INFINITE=~0u,WAIT_OBJECT_0=0;
int locks=0,removals=0;bool removeOk=true;
unsigned WaitForSingleObject(int,unsigned){return 0;}
void AcquireSRWLockExclusive(int*){++locks;}
void ReleaseSRWLockExclusive(int*){--locks;}
struct InstallEngine {
 int decisionGate_=0,statusLock_=0;std::atomic<bool> cancelRequested_{false},launchingPhase_{false};bool secureUxRemovalCommitted_=false;
 bool RemoveOriginalSecureUxTheme(){assert(launchingPhase_.load()&&locks==0);++removals;return removeOk;}
 bool CommitEarlySecureUxRemoval() noexcept;
};
// METHOD
int main(){
 for(bool gate:{false,true})for(bool success:{false,true}){InstallEngine e;e.decisionGate_=gate;removeOk=success;removals=0;
  assert(e.CommitEarlySecureUxRemoval()==success);assert(removals==1&&locks==0&&e.secureUxRemovalCommitted_==success);
 }
 InstallEngine e;e.cancelRequested_=true;removals=0;assert(!e.CommitEarlySecureUxRemoval());assert(removals==0&&!e.launchingPhase_.load()&&locks==0);
}
'''.replace('// METHOD',method)
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';exe=Path(d)/'test';p.write_text(h)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
worker=s[s.index('DWORD WINAPI InstallEngine::RunGlassWorker('):s.index('bool InstallEngine::Start(')]
assert worker.index('BuildExecutionPlan()')<worker.index('PrepareManagedRemoval()')<worker.index('StopRunningHosts()')
assert worker.index('ReleaseLockedModules()')<worker.index('CommitEarlySecureUxRemoval()')<worker.index('RunMsiSequence()')
assert 'if(ok && self->selection_.secureUxThemeRemove && self->selection_.removeMask)' in worker
assert 'if(!ok && self->secureUxRemovalCommitted_)' in worker
assert 'SecureUxTheme was removed and was not restored' in worker
assert 'if(secureUxRemovalCommitted_)return true;' in s
assert 'if(selection_.removeMask && selection_.secureUxTheme)' in s
ui=(root/'GlassSetup/src/main.cpp').read_text()
assert '!(selection_.removeMask && selection_.secureUxTheme)' in ui
assert '!(selection_.removeMask && (selection_.secureUxTheme||selection_.secureUxThemeRemove))' not in ui
print('PASS: actual early vendor cancel gate, SUT-before-Settings order, once-only vendor commit, partial recovery contract and native Apply gate')
