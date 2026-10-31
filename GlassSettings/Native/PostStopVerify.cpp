// R9: пост-стопная проверка остатков Glass*-модулей — БЕЗ загрузки GlassCompat.
//
// Зачем отдельный процесс: per-process ссылку на INCONTEXT-хук снимает ОС в момент
// завершения процесса-владельца хуков (rundll32-хост), поэтому `verify` внутри хоста
// видит остатки, которых через 0.5 с уже нет (прогон 9: explorer «держал» compat в
// 13:01:28.9 и отпустил его в 13:01:29.5).
//
// Почему здесь, а не в GlassCompat: прошлая версия запускала
// `rundll32 GlassCompat.dll,Main /verify` — и наблюдатель видел в списке процессов
// НОВЫЙ RunDLL32, который держит GlassCompat (это был наш же верификатор, а не остаток).
// Этот экспорт живёт в GlassSettings.Native.dll: ни compat, ни core, ни wnd в процессе
// нет, поэтому верификатор не попадает ни в список, ни в собственные `hits`.
//
// Запуск: rundll32 GlassSettings.Native.dll,GlassSettings_PostStopVerify

#include <Windows.h>
#include <GlassUlTrace.h>

extern "C" __declspec(dllexport) void CALLBACK GlassSettings_PostStopVerify(
    HWND, HINSTANCE, LPSTR, int) noexcept
{
    GlassUlTrace::Logf(
        "H98", "PostStopVerify.cpp:PostStopVerify", "verify_process",
        "\"stage\":\"start\"");
    Sleep(2000);
    GlassUlTrace::LogVerify(2, "post2");
    Sleep(6000);
    GlassUlTrace::LogVerify(8, "post8");
    GlassUlTrace::Logf(
        "H98", "PostStopVerify.cpp:PostStopVerify", "verify_process",
        "\"stage\":\"done\"");
}
