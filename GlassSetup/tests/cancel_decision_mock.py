#!/usr/bin/env python3
"""Exercise the real cancel/failure synchronization with threaded event mocks."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "GlassSetup/src/InstallEngine.cpp").read_text(encoding="utf-8")

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

functions = [function(token) for token in (
    "bool InstallEngine::PauseForCancelDecision(",
    "void InstallEngine::ResumeAfterCancelDecision(",
    "void InstallEngine::RequestCancel(",
    "bool InstallEngine::ChooseCancellationOrFailure(")]
worker = source[source.index("DWORD WINAPI InstallEngine::WorkerThread("):source.index("bool InstallEngine::Start(")]
assert worker.index("ChooseCancellationOrFailure(!ok)") < worker.index("errorPageRequested_")
cancel_branch = worker[worker.index("if (cancelled) {"):worker.index("} else {\n                    const bool cleanAttempt")]
for stale in ("error_.clear()", "failureDetail_.clear()", "msiMessage_.clear()", "msiExitCode_ = 0"):
    assert stale in cancel_branch

prefix = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
using DWORD=unsigned;constexpr DWORD INFINITE=~0u,WAIT_OBJECT_0=0,WAIT_TIMEOUT=258;
struct Event {std::mutex mutex;std::condition_variable cv;bool signaled=true;};
using HANDLE=Event*;
DWORD WaitForSingleObject(HANDLE event,DWORD timeout){
 std::unique_lock<std::mutex> lock(event->mutex);
 if(timeout==0)return event->signaled?WAIT_OBJECT_0:WAIT_TIMEOUT;
 event->cv.wait(lock,[&]{return event->signaled;});return WAIT_OBJECT_0;
}
void SetEvent(HANDLE event){{std::lock_guard<std::mutex> lock(event->mutex);event->signaled=true;}event->cv.notify_all();}
void ResetEvent(HANDLE event){std::lock_guard<std::mutex> lock(event->mutex);event->signaled=false;}
void AcquireSRWLockExclusive(std::mutex* mutex){mutex->lock();}void ReleaseSRWLockExclusive(std::mutex* mutex){mutex->unlock();}
namespace InstallLog {void Line(const wchar_t*){}}
struct Activity {void End(){}};
class InstallEngine {
public:
 enum class HoldAction {None,Cancel};enum class HoldKind {None};
 bool started_=true;std::atomic_bool finished_{false},launchingPhase_{false},rollingBack_{false},failureFinalizing_{false},
  cancelRequested_{false},quietClose_{false};
 std::mutex statusLock_;Event decision,hold,error;HANDLE decisionGate_=&decision,holdGate_=&hold,errorPageShown_=&error;
 HoldAction holdAction_=HoldAction::None;HoldKind holdKind_=HoldKind::None;Activity unlockActivity_;
 bool PauseForCancelDecision() noexcept;void ResumeAfterCancelDecision() noexcept;void RequestCancel() noexcept;
 bool ChooseCancellationOrFailure(bool failed) noexcept;
};
'''
main = r'''
int main(){
 using namespace std::chrono_literals;
 {
  InstallEngine e;assert(e.PauseForCancelDecision());std::promise<void> entered;auto ready=entered.get_future();
  auto result=std::async(std::launch::async,[&]{entered.set_value();return e.ChooseCancellationOrFailure(true);});
  ready.get();assert(result.wait_for(50ms)==std::future_status::timeout);e.RequestCancel();
  assert(result.get() && e.cancelRequested_ && !e.failureFinalizing_);
 }
 {
  InstallEngine e;assert(e.PauseForCancelDecision());std::promise<void> entered;auto ready=entered.get_future();
  auto result=std::async(std::launch::async,[&]{entered.set_value();return e.ChooseCancellationOrFailure(true);});
  ready.get();assert(result.wait_for(50ms)==std::future_status::timeout);e.ResumeAfterCancelDecision();
  assert(!result.get() && e.failureFinalizing_);e.RequestCancel();assert(!e.cancelRequested_);
  assert(!e.PauseForCancelDecision());
 }
 {
  InstallEngine e;assert(!e.ChooseCancellationOrFailure(true));assert(!e.PauseForCancelDecision());
  e.RequestCancel();assert(!e.cancelRequested_); // failure claimed first
 }
 {
  InstallEngine e;e.RequestCancel();assert(e.ChooseCancellationOrFailure(true));assert(!e.failureFinalizing_);
 }
 {
  InstallEngine e;e.finished_=true;assert(!e.PauseForCancelDecision());
 }
 std::cout<<"PASS: real failure/cancel gate: confirmation, accepted cancel, continue, failure-first races\n";
}
'''
with tempfile.TemporaryDirectory(prefix="glass-cancel-race-") as directory:
    cpp = Path(directory) / "cancel.cpp"
    exe = Path(directory) / "cancel"
    cpp.write_text(prefix + "\n".join(functions) + "\n" + main, encoding="utf-8")
    compiler = os.environ.get("CXX", "g++")
    subprocess.run([compiler, "-std=c++17", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-pthread", str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
