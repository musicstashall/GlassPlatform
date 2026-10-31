#!/usr/bin/env python3
"""Run the real GlassUnins finish/close handlers against a simulated UI.

This tests their state transitions and close/cancel gate, not Win32 message
pumping or a real uninstall. Those still require a Windows VM.
"""
from pathlib import Path
import os
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
source = (repo / 'GlassSetup/GlassUnins/main.cpp').read_text(encoding='utf-8')


def definition(token):
    start = source.index(token)
    opening = source.index('{', start)
    depth = 0
    for i in range(opening, len(source)):
        if source[i] == '{':
            depth += 1
        elif source[i] == '}':
            depth -= 1
            if depth == 0:
                return source[start:i + 1]
    raise AssertionError(token)


for handler in ('case kWorkerFinished: FinishRemoval(); return 0;',
                'case WM_CLOSE: RequestClose(); return 0;',
                'page_==Page::Error) RequestClose();'):
    assert handler in source, handler

enums = '\n'.join(source[source.index('enum class ' + name):source.index(';', source.index('enum class ' + name)) + 1]
                  for name in ('Page', 'Action', 'OperationState'))
prefix = r'''
#include <atomic>
#include <cassert>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
constexpr int ERROR_INSTALL_USEREXIT=1602, FALSE=0;
int closed=0, signaled=0, disabled=0;
void DestroyWindow(int){++closed;}
void SetEvent(int){++signaled;}
void EnableWindow(int,int){++disabled;}
void LogLine(const std::wstring&){}
'''
class_start = r'''
struct App {
 int window=1,gate_=2,cancel_=3,resultCode_=0;
 Page page_=Page::Select;
 std::thread worker_;
 std::mutex dataMutex_;
 std::atomic<OperationState> operationState_{OperationState::BeforeRemoval};
 std::atomic_bool cancelRequested_{false};
 std::atomic<Action> action_{Action::None};
 void SetPage(Page p){page_=p;}
'''
main = r'''
};
void reset(){closed=signaled=disabled=0;}
int main(){
 // Irreversible MSI work still cannot be interrupted, even if its UI update
 // has not arrived yet; after the worker exits both completion paths can close.
 for(int result:{0,17}) {
  reset();App a;a.page_=Page::Stopping;a.operationState_=OperationState::Removing;
  a.resultCode_=result;a.worker_=std::thread([]{});
  a.RequestClose();assert(!closed && !signaled && !a.cancelRequested_);
  a.page_=Page::Removing;a.RequestClose();assert(!closed && !signaled);
  a.FinishRemoval();
  assert(a.operationState_==OperationState::Finished);
  assert(a.page_==(result?Page::Error:Page::Complete));
  a.RequestClose();assert(closed==1);
 }
 // Cancellation before MSI signals the worker but cannot destroy it early.
 for(Page hold:{Page::Stopping,Page::UnlockModules,Page::UnlockProcesses}) {
  reset();App a;a.page_=hold;a.RequestClose();
  assert(!closed && signaled==1 && disabled==1 && a.cancelRequested_);
  assert(a.action_==Action::Cancel && a.operationState_==OperationState::CancelRequested);
  a.RequestClose();assert(!closed && signaled==1);
  a.resultCode_=ERROR_INSTALL_USEREXIT;a.worker_=std::thread([]{});
  a.FinishRemoval();assert(a.page_==Page::Cancelled && a.operationState_==OperationState::Finished);
  a.RequestClose();assert(closed==1);
 }
 // Preflight and worker-start errors, and the initial selection, remain closable.
 for(Page page:{Page::Select,Page::Error}) {
  reset();App a;a.page_=page;a.RequestClose();assert(closed==1 && !signaled);
 }
 reset();App a;a.page_=Page::Stopping;a.resultCode_=9;a.worker_=std::thread([]{});
 a.FinishRemoval();assert(a.page_==Page::Error);a.RequestClose();assert(closed==1);
 std::cout<<"PASS: real uninstaller finish and close handlers: active MSI, success, failure, cancellation and early errors\n";
}
'''
code = prefix + enums + '\n' + class_start + definition('void FinishRemoval()') + '\n' + definition('void RequestClose()') + '\n' + main
with tempfile.TemporaryDirectory(prefix='glass-unins-close-') as directory:
    cpp = Path(directory) / 'close.cpp'
    exe = Path(directory) / 'close-test'
    cpp.write_text(code, encoding='utf-8')
    compiler = os.environ.get('CXX', 'g++')
    subprocess.run([compiler, '-std=c++17', '-g', '-pthread', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-no-pie', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
