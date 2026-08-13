#pragma once

#include <Windows.h>
#include <wincodec.h>
#include <cstdint>
#include <unordered_map>

namespace GlassSettingsNative::AtlasPreview
{
	// Ключ: (partId << 48) | (stateId << 32) | propId — всегда uint64_t (на Win32 size_t=32 → UB).
	constexpr uint64_t MakeLayoutKey(int partId, int stateId, int propId) noexcept
	{
		const auto u1 = static_cast<uint64_t>(static_cast<uint16_t>(partId));
		const auto u2 = static_cast<uint64_t>(static_cast<uint16_t>(stateId));
		const auto ui = static_cast<uint64_t>(static_cast<uint32_t>(propId));
		return (u1 << 48) | (u2 << 32) | ui;
	}

	// TMT константы из UxTheme (vssym32.h не подключаем — там глобальные enum-имена,
	// конфликтующие с этими константами под `using namespace`).
	constexpr int TMT_ATLASRECT = 8002;
	constexpr int TMT_SIZINGMARGINS = 3601;
	constexpr int TMT_CONTENTMARGINS = 3602;
	// Диск-стрим атласа темы (PNG) — для GetThemeStream(TMT_DISKSTREAM).
	// Значение из vssym32.h (213). vssym32.h не подключаем (там #define-макросы,
	// конфликтующие с TMT_ATLASRECT/TMT_SIZINGMARGINS из этого заголовка).
	constexpr int TMT_DISKSTREAM = 213;

	// Preview собирается только из частей активной (focused) рамки DWMWindow (Win10/11).
	// Не использовать INACTIVE/SHADOW/DARK/SQUEEGEE/SMALL* part ID.
	constexpr bool ActiveFrameOnly = true;

	// Карта частей класса DWMWindow (схема aerolite / Win10 layout, части 1–91).
	// Подтверждено по layout-файлам атласов и поведению uDWM/OpenGlass.

	// Рамка
	constexpr int DWMW_FRAMEBOTTOM = 1;
	constexpr int DWMW_FRAMELEFT = 31;
	constexpr int DWMW_FRAMERIGHT = 33;
	constexpr int DWMW_FRAMETOP_ACTIVE = 46;
	constexpr int DWMW_FRAMETOP_INACTIVE = 44;

	// Кнопки заголовка. Подтверждено по цвету NORMAL-среза в атласах
	// (winxp_Royale/RoundedCorners): часть 7 — красная = CLOSE; 3/5 — MIN/MAX.
	// Активные состояния: 3=MIN, 5=MAX/RESTORE, 7=CLOSE. Пары (4,6,8) и (88,90,91) —
	// inactive/maximized варианты, используются как fallback.
	constexpr int DWMW_MINBUTTON = 3;
	constexpr int DWMW_MAXBUTTON = 5;
	constexpr int DWMW_CLOSEBUTTON = 7;
	constexpr int DWMW_MINBUTTON_ALT = 88;
	constexpr int DWMW_MAXBUTTON_ALT = 90;
	constexpr int DWMW_CLOSEBUTTON_ALT = 91;
	constexpr int DWMW_MINBUTTON_INACTIVE = 4;
	constexpr int DWMW_MAXBUTTON_INACTIVE = 6;
	constexpr int DWMW_CLOSEBUTTON_INACTIVE = 8;
	constexpr int DWMW_CLOSEBUTTON_MAX = 10;

	// Глифы кнопок (DPI 96 — основной для превью)
	constexpr int DWMW_GLYPH_CLOSE96 = 11;
	constexpr int DWMW_GLYPH_CLOSE120 = 12;
	constexpr int DWMW_GLYPH_CLOSE144 = 13;
	constexpr int DWMW_GLYPH_CLOSE192 = 14;
	constexpr int DWMW_GLYPH_MAX96 = 19;
	constexpr int DWMW_GLYPH_MAX120 = 20;
	constexpr int DWMW_GLYPH_MAX144 = 21;
	constexpr int DWMW_GLYPH_MAX192 = 22;
	constexpr int DWMW_GLYPH_MIN96 = 23;
	constexpr int DWMW_GLYPH_MIN120 = 24;
	constexpr int DWMW_GLYPH_MIN144 = 25;
	constexpr int DWMW_GLYPH_MIN192 = 26;

	// State ID (layout экспортирован с stateId=0; мультистейт — вертикальные полосы в ATLASRECT)
	constexpr int STATE_NORMAL = 0;

	struct ThemeMargins
	{
		int cxLeftWidth{};
		int cxRightWidth{};
		int cyTopHeight{};
		int cyBottomHeight{};
	};

	using RectMap = std::unordered_map<uint64_t, RECT>;
	using MarginsMap = std::unordered_map<uint64_t, ThemeMargins>;

	// Парсинг .Layout файла
	[[nodiscard]] HRESULT ParseLayoutFile(
		LPCWSTR layoutPath,
		RectMap& outRects,
		MarginsMap& outMargins) noexcept;

	// Загрузка PNG атласа через WIC и конвертация в 32bpp PBGRA
	[[nodiscard]] HRESULT LoadAtlasBitmap(
		IWICImagingFactory* wicFactory,
		LPCWSTR atlasPath,
		IWICBitmapSource** outBitmap,
		UINT* outWidth,
		UINT* outHeight) noexcept;

	// Композиция preview изображения из атласа (прозрачный фон).
	// reflectionDllPath — путь к OpenGlass.dll: из него грузится PNG-ресурс 102
	// (отражение «стекла») и кладётся нижним слоем. Может быть nullptr (без отражения).
	[[nodiscard]] HRESULT CompositePreview(
		IWICImagingFactory* wicFactory,
		IWICBitmapSource* atlasBitmap,
		const RectMap& rects,
		const MarginsMap& margins,
		UINT targetWidth,
		UINT targetHeight,
		LPCWSTR reflectionDllPath,
		UINT reflectionAlphaPercent,
		IWICBitmap** outPreview) noexcept;

	// Сохранение WIC bitmap в PNG файл
	[[nodiscard]] HRESULT SaveBitmapToPNG(
		IWICImagingFactory* wicFactory,
		IWICBitmap* bitmap,
		LPCWSTR outputPath) noexcept;
}
