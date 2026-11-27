"""Чистая логика «сторожа» падений DWM (скользящее окно 30 с, Danger) — реальные ассерты на C++."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
h=r'''
#include "GlassShared/include/OpenGlassDwmGuardPolicy.h"
#include <cassert>
#include <iostream>
#include <vector>
using Shared::OpenGlassDwmGuardPolicy::Action;
using Shared::OpenGlassDwmGuardPolicy::AppendCrash;
using Shared::OpenGlassDwmGuardPolicy::CountWithinWindow;
using Shared::OpenGlassDwmGuardPolicy::DropExpired;
using Shared::OpenGlassDwmGuardPolicy::EvaluateCrash;
using Shared::OpenGlassDwmGuardPolicy::IsQuarantine;
using Shared::OpenGlassDwmGuardPolicy::IsVersionStrictlyGreater;
constexpr unsigned long long FT=10000000ull; // 1 тик = 100 нс
int main(){
 // --- AppendCrash: скользящее окно отрезает старые метки
 {
  std::vector<unsigned long long> t;
  AppendCrash(t, 100*FT); AppendCrash(t, 120*FT);
  assert(CountWithinWindow(t, 125*FT)==2); // оба в окне (окно: 95..125)
  assert(CountWithinWindow(t, 135*FT)==1); // метка 100c вышла из окна (135-100>30)
  assert(CountWithinWindow(t, 155*FT)==0); // 155-120>30 — истекли обе
 }
 // --- AppendCrash: храним не больше kMaxCrashTimesStored
 {
  std::vector<unsigned long long> t;
  for(unsigned long long i=0;i<12;i++) AppendCrash(t,(1000+i)*FT);
  assert(t.size()==8);
  assert(t.front()==1004*FT); // самые старые вытеснены
 }
 // --- DropExpired: после загрузки маркера остаются только свежие метки
 {
  std::vector<unsigned long long> t={900*FT, 1000*FT, 1024*FT};
  DropExpired(t, 1025*FT); // окно: 995..1025 с
  assert(t.size()==2 && t[0]==1000*FT);
 }
 // --- EvaluateCrash: одиночный неспешный краш -> Suspect
 assert(EvaluateCrash(false,1,1,false)==Action::Suspect);
 // --- EvaluateCrash: мгновенный краш -> Danger, без карантина
 assert(EvaluateCrash(false,1,1,true)==Action::EnterDanger);
 // --- EvaluateCrash: второй краш в окне -> карантин (не в Danger)
 assert(EvaluateCrash(false,2,2,false)==Action::QuarantinePair);
 assert(EvaluateCrash(false,3,3,false)==Action::QuarantinePair);
 // --- EvaluateCrash: любой следующий краш в режиме Danger -> карантин
 assert(EvaluateCrash(true,2,2,false)==Action::QuarantineDanger);
 assert(EvaluateCrash(true,2,2,true)==Action::QuarantineDanger);
 // --- EvaluateCrash: абсолютный предохранитель — 3-й краш вне окна -> карантин
 assert(EvaluateCrash(false,1,3,false)==Action::QuarantineFuse);
 assert(EvaluateCrash(false,1,3,true)==Action::QuarantineFuse);
 assert(EvaluateCrash(false,2,3,false)==Action::QuarantinePair); // окно точнее предохранителя
 assert(IsQuarantine(EvaluateCrash(false,1,3,false)));
 assert(IsQuarantine(EvaluateCrash(true,2,2,false)));
 assert(IsQuarantine(EvaluateCrash(false,2,2,true)));
 assert(!IsQuarantine(EvaluateCrash(false,1,1,true)));
 assert(!IsQuarantine(EvaluateCrash(false,1,1,false)));
 assert(!IsQuarantine(EvaluateCrash(false,2,2,false)) != true); // пара — карантин
 static_assert(Shared::OpenGlassDwmGuardPolicy::kFuseCrashCount==3, "fuse=3");
 static_assert(Shared::OpenGlassDwmGuardPolicy::kMaxWatchSeconds==120, "hard cap=120");
 static_assert(Shared::OpenGlassDwmGuardPolicy::kCleanExitSeconds==30, "clean exit=30");
 // --- IsVersionStrictlyGreater: строго больше, числовые токены
 assert(IsVersionStrictlyGreater(L"3.0.2.3750",L"3.0.2.3749"));
 assert(IsVersionStrictlyGreater(L"3.1.0.0",L"3.0.2.3749"));
 assert(!IsVersionStrictlyGreater(L"3.0.2.3749",L"3.0.2.3749"));
 assert(!IsVersionStrictlyGreater(L"3.0.2.3748",L"3.0.2.3749"));
 assert(!IsVersionStrictlyGreater(L"3.0.2",L"3.0.2.0")); // равны (тривиальное)
 assert(!IsVersionStrictlyGreater(L"garbage",L"3.0.2.3749"));
 assert(!IsVersionStrictlyGreater(L"3.0.x",L"3.0.2"));
 assert(!IsVersionStrictlyGreater(L"",L"3.0.2.3749"));
 assert(IsVersionStrictlyGreater(L"3.0.2.3749",L"3.0.2")); // 3.0.2.3749 > 3.0.2.0
 // --- цикл: мгновенный краш -> Danger -> второй краш -> стоп
 {
  std::vector<unsigned long long> t;
  AppendCrash(t, 1000*FT);
  bool stateDanger=false;
  size_t total=1;
  Action a=EvaluateCrash(stateDanger, CountWithinWindow(t,1000*FT), total, true);
  assert(a==Action::EnterDanger); stateDanger=true;
  AppendCrash(t, 1003*FT); ++total;
  a=EvaluateCrash(stateDanger, CountWithinWindow(t,1003*FT), total, false);
  assert(a==Action::QuarantineDanger);
 }
 // --- цикл: неспешный первый краш -> Suspect; второй через 20 с -> карантин
 {
  std::vector<unsigned long long> t;
  AppendCrash(t, 1000*FT);
  assert(EvaluateCrash(false, CountWithinWindow(t,1000*FT), 1, false)==Action::Suspect);
  AppendCrash(t, 1020*FT);
  assert(EvaluateCrash(false, CountWithinWindow(t,1020*FT), 2, false)==Action::QuarantinePair);
 }
 // --- цикл: первый краш в секунде 29, второй после истечения окна — не карантин
 {
  std::vector<unsigned long long> t;
  AppendCrash(t, 1000*FT);
  assert(CountWithinWindow(t, 1031*FT)==0); // окно 30 с истекло
 }
 // --- цикл: медленный ритм 0→35→70 с — пара окна не срабатывает, предохранитель на 3-м
 {
  std::vector<unsigned long long> t;
  AppendCrash(t, 1000*FT);
  assert(EvaluateCrash(false, CountWithinWindow(t,1000*FT), 1, false)==Action::Suspect);
  AppendCrash(t, 1035*FT); // вне окна первого (35>30)
  assert(CountWithinWindow(t, 1035*FT)==1);
  assert(EvaluateCrash(false, CountWithinWindow(t,1035*FT), 2, false)==Action::Suspect);
  AppendCrash(t, 1070*FT); // вне окна второго (35>30); окно пустеет, счётчик цикла — 3
  assert(CountWithinWindow(t, 1070*FT)==1);
  assert(EvaluateCrash(false, CountWithinWindow(t,1070*FT), 3, false)==Action::QuarantineFuse);
 }
 // --- цикл: вторая сессия, краш #2 через 10 с после первого (маркер пережит) -> карантин
 {
  std::vector<unsigned long long> t={1000*FT}; // метка из прошлой сессии
  AppendCrash(t, 1008*FT);
  assert(EvaluateCrash(false, CountWithinWindow(t,1008*FT), 2, true)==Action::QuarantinePair);
 }
 std::cout<<"PASS: OpenGlassDwmGuardPolicy window, danger and version semantics\n";
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(h);exe=Path(d)/'test'
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(root),str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
