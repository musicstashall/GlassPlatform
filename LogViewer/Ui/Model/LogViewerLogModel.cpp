#include "SharedPch.h"

#include "framework.h"
#include "Ui/Model/LogViewerLogModel.h"

namespace
{
    [[nodiscard]] bool IsDigit(wchar_t c) noexcept
    {
        return c >= L'0' && c <= L'9';
    }

    // Строго "[HH:MM:SS.mmm] " как в GlassLogServer::WriteLine (дальше — тело из DebugLog).
    [[nodiscard]] bool TryConsumeGlassServerTimestampPrefix(const std::wstring& text, size_t& pos) noexcept
    {
        if (pos >= text.size() || text[pos] != L'[')
            return false;
        const size_t close = text.find(L']', pos + 1);
        if (close == std::wstring::npos || close + 1 >= text.size() || text[close + 1] != L' ')
            return false;

        const size_t innerLen = close - pos - 1;
        if (innerLen != 12)
            return false;

        const wchar_t* p = text.data() + pos + 1;
        if (!IsDigit(p[0]) || !IsDigit(p[1]) || p[2] != L':' ||
            !IsDigit(p[3]) || !IsDigit(p[4]) || p[5] != L':' ||
            !IsDigit(p[6]) || !IsDigit(p[7]) || p[8] != L'.' ||
            !IsDigit(p[9]) || !IsDigit(p[10]) || !IsDigit(p[11]))
            return false;

        pos = close + 2;
        return true;
    }

    [[nodiscard]] bool ReadBracketToken(const std::wstring& text, size_t& pos, std::wstring& out)
    {
        out.clear();
        if (pos >= text.size() || text[pos] != L'[')
            return false;
        const size_t close = text.find(L']', pos + 1);
        if (close == std::wstring::npos)
            return false;
        out.assign(text, pos + 1, close - pos - 1);
        pos = close + 1;
        return true;
    }
}

void ParseLogTags(const std::wstring& text, std::wstring& module, std::wstring& subsystem)
{
    module.clear();
    subsystem.clear();

    size_t pos = 0;
    (void)TryConsumeGlassServerTimestampPrefix(text, pos);

    // Ровно два тега `[Модуль][Подсистема]` в начале тела; дальше в тексте могут быть `[load]` и т.п.
    if (!ReadBracketToken(text, pos, module))
        return;
    (void)ReadBracketToken(text, pos, subsystem);
}
