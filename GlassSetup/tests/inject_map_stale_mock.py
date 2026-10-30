#!/usr/bin/env python3
"""Source checks and a model for stale-PID decisions; not execution of the C# code."""
from pathlib import Path

repo=Path(__file__).resolve().parents[2]
src=(repo/'GlassSettings/Services/InjectRuntimeMonitor.cs').read_text(encoding='utf-8')
def function(token):
    start=src.index(token);brace=src.index('{',start);depth=0
    for i in range(brace,len(src)):
        if src[i]=='{':depth+=1
        if src[i]=='}':
            depth-=1
            if not depth:return src[start:i+1]
    raise ValueError(token)
impl=function('private static bool IsCurrentInjectMapSlot(')
assert 'errorInvalidParameter = 87' in impl
assert 'Marshal.GetLastWin32Error() != errorInvalidParameter' in impl
assert 'WaitForSingleObject(h, 0)' in impl and 'wait == waitObject0' in impl
assert 'GetExitCodeProcess' not in impl and 'wait != waitTimeout' in impl
assert impl.index('wait == waitObject0') < impl.index('ClearInjectMapPidForArch') < impl.index('CloseHandle(h)')
assert 'slot.ExeName.Length == mapExeNameLimit' in impl
assert 'StringComparison.OrdinalIgnoreCase' in impl
assert 'if (IsCurrentInjectMapSlot(s))' in function('private static NativeHostInterop.InjectMapSlot[] PruneDeadInjectMapSlots(')
assert 'PruneDeadInjectMapSlots(slots)' in src

def decision(pid, opened, open_error=None, wait='timeout', recorded='', actual=None):
    """Model visibility and whether a slot can be cleared while its process handle is held."""
    if not pid:return False,False
    if not opened:return open_error!=87,False
    if wait=='signaled':return False,True
    if wait!='timeout' or not recorded or actual is None:return True,False
    matches=(actual[:63].casefold()==recorded.casefold() if len(recorded)>=63
             else actual.casefold()==recorded.casefold())
    return matches,False

assert decision(0,False)==(False,False)
assert decision(123,False,87)==(False,False) # absent; do not clear by PID alone
for error in (5,6,299):assert decision(123,False,error)==(True,False) # unknown
assert decision(123,True,wait='signaled')==(False,True) # even exit code 259
assert decision(123,True,wait='failed')==(True,False)
assert decision(123,True,recorded='Setup.exe',actual='setup.EXE')==(True,False)
assert decision(123,True,recorded='Setup.exe',actual='explorer.exe')==(False,False)
assert decision(123,True,recorded='A'*63,actual='A'*63+'tail')==(True,False)
assert decision(123,True,recorded='A'*63,actual='B'*63+'tail')==(False,False)
assert decision(123,True,recorded='Setup.exe',actual=None)==(True,False)
print('PASS: stale-PID source invariants and visibility model; C# and Windows APIs not executed')
