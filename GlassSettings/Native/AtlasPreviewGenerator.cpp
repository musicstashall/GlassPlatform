#include <Windows.h>
#include <wincodec.h>
#include <uxtheme.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <stdio.h>
#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include "AtlasPreviewGenerator.h"

#pragma comment(lib, "uxtheme.lib")

namespace GlassSettingsNative::AtlasPreview
{
	void LogNative(LPCWSTR message) noexcept
	{
		if (!message)
			return;
		OutputDebugStringW(L"[AtlasPreview.Native] ");
		OutputDebugStringW(message);
		OutputDebugStringW(L"\n");
	}

	void LogNativeHr(LPCWSTR step, HRESULT hr) noexcept
	{
		WCHAR buffer[256]{};
		swprintf_s(buffer, L"%s HRESULT=0x%08X", step, static_cast<unsigned>(hr));
		LogNative(buffer);
	}

	namespace
	{
		int ClampInt(int value, int minValue, int maxValue) noexcept
		{
			if (value < minValue) return minValue;
			if (value > maxValue) return maxValue;
			return value;
		}

		ThemeMargins DefaultMargins() noexcept
		{
			return { 8, 8, 8, 8 };
		}

		std::optional<RECT> GetRect(const RectMap& rects, int partId, int stateId) noexcept
		{
			const auto key = MakeLayoutKey(partId, stateId, TMT_ATLASRECT);
			const auto it = rects.find(key);
			if (it == rects.end())
				return std::nullopt;
			return it->second;
		}

		[[maybe_unused]] ThemeMargins GetMargins(const MarginsMap& margins, int partId, int stateId) noexcept
		{
			const auto key = MakeLayoutKey(partId, stateId, TMT_SIZINGMARGINS);
			const auto it = margins.find(key);
			if (it == margins.end())
				return DefaultMargins();
			return it->second;
		}

		int RectWidth(const RECT& rect) noexcept
		{
			return rect.right - rect.left;
		}

		int RectHeight(const RECT& rect) noexcept
		{
			return rect.bottom - rect.top;
		}

		// Source-over в premultiplied PBGRA (WIC 32bppPBGRA).
		void BlendPremultipliedPixel(BYTE* dst, const BYTE* src) noexcept
		{
			const UINT srcA = src[3];
			if (srcA == 0)
				return;
			if (srcA == 255)
			{
				dst[0] = src[0];
				dst[1] = src[1];
				dst[2] = src[2];
				dst[3] = src[3];
				return;
			}

			const UINT invA = 255 - srcA;
			dst[0] = static_cast<BYTE>(src[0] + ((dst[0] * invA + 127) / 255));
			dst[1] = static_cast<BYTE>(src[1] + ((dst[1] * invA + 127) / 255));
			dst[2] = static_cast<BYTE>(src[2] + ((dst[2] * invA + 127) / 255));
			dst[3] = static_cast<BYTE>(srcA + ((dst[3] * invA + 127) / 255));
		}

		std::optional<RECT> GetRectFirst(const RectMap& rects, std::initializer_list<int> partIds) noexcept
		{
			for (const int partId : partIds)
			{
				if (const auto rect = GetRect(rects, partId, STATE_NORMAL))
					return rect;
			}
			return std::nullopt;
		}

		[[maybe_unused]] int ResolveFirstPartId(const RectMap& rects, std::initializer_list<int> partIds) noexcept
		{
			for (const int partId : partIds)
			{
				if (GetRect(rects, partId, STATE_NORMAL))
					return partId;
			}
			return *partIds.begin();
		}

		// Части кнопок/глифов/рамок в атласе DWM — вертикальные мультистейт-полосы
		// (Normal/Hot/Pushed/Disabled). Число полос определяем по равной высоте бэндов.
		// Рамка берёт NORMAL (active = верхний срез); кнопки/глифы — самое непрозрачное
		// состояние (HOT), т.к. у части тем NORMAL прозрачный.
		constexpr BYTE kAlphaThreshold = 16;

		// Число равновысоких состояний в вертикальной полосе части.
		int DetectStateCount(const RECT& part) noexcept
		{
			const int h = RectHeight(part);
			constexpr int candidates[] = { 4, 3, 2, 5, 6 };
			for (const int n : candidates)
			{
				if (n > 1 && h % n == 0 && h / n >= 4)
					return n;
			}
			return 1;
		}

		// Прямоугольник одного состояния (stateIndex: 0 = NORMAL).
		RECT StateSliceRect(const RECT& part, int stateCount, int stateIndex) noexcept
		{
			if (stateCount < 1)
				stateCount = 1;
			if (stateIndex < 0)
				stateIndex = 0;
			if (stateIndex >= stateCount)
				stateIndex = stateCount - 1;

			const int h = RectHeight(part);
			const int sliceH = (stateCount > 1) ? (h / stateCount) : h;
			const int top = part.top + stateIndex * sliceH;
			const int bottom = (stateIndex == stateCount - 1) ? part.bottom : top + sliceH;
			return RECT{ part.left, top, part.right, bottom };
		}

		// NORMAL-состояние части (верхний срез мультистейт-полосы).
		RECT NormalStateRect(const RECT& part, int stateCountCap = 0) noexcept
		{
			int stateCount = DetectStateCount(part);
			if (stateCountCap > 0 && stateCount > stateCountCap)
				stateCount = stateCountCap;
			return StateSliceRect(part, stateCount, 0);
		}

		// Средняя альфа прямоугольника части атласа.
		int MeanAlpha(
			const BYTE* atlas,
			UINT stride,
			UINT atlasW,
			UINT atlasH,
			const RECT& r) noexcept
		{
			const int l = r.left < 0 ? 0 : r.left;
			const int t = r.top < 0 ? 0 : r.top;
			const int rr = r.right > static_cast<int>(atlasW) ? static_cast<int>(atlasW) : r.right;
			const int bb = r.bottom > static_cast<int>(atlasH) ? static_cast<int>(atlasH) : r.bottom;
			if (rr <= l || bb <= t)
				return 0;
			unsigned long long sum = 0;
			unsigned long long count = 0;
			for (int y = t; y < bb; ++y)
			{
				const BYTE* row = atlas + static_cast<size_t>(y) * stride;
				for (int x = l; x < rr; ++x)
				{
					sum += row[static_cast<size_t>(x) * 4 + 3];
					++count;
				}
			}
			return count ? static_cast<int>(sum / count) : 0;
		}

		// Состояние с максимальной средней альфой (HOT/PUSHED). Нужно для тем, где
		// NORMAL (верхний срез) прозрачный/бледный (The Red Theme, win8.1, Vista/win7
		// glass): кнопки рисуются «нормального» вида только при наведении. При равенстве
		// состояний (winxp — все одинаковы) выбирается NORMAL, поэтому регрессии нет.
		RECT MostOpaqueStateRect(
			const BYTE* atlas,
			UINT stride,
			UINT atlasW,
			UINT atlasH,
			const RECT& part) noexcept
		{
			const int stateCount = DetectStateCount(part);
			if (stateCount <= 1)
				return part;
			int bestIndex = 0;
			int bestAlpha = -1;
			for (int i = 0; i < stateCount; ++i)
			{
				const RECT slice = StateSliceRect(part, stateCount, i);
				const int a = MeanAlpha(atlas, stride, atlasW, atlasH, slice);
				if (a > bestAlpha)
				{
					bestAlpha = a;
					bestIndex = i;
				}
			}
			return StateSliceRect(part, stateCount, bestIndex);
		}

		// Выбор состояния для отрисовки кнопки/глифа.
		// База — NORMAL (верхний срез): компактная кнопка БЕЗ свечения. Система рисует
		// прямоугольник HOT (со свечением, бóльших габаритов) поверх соседей только при
		// наведении — в статичной миниатюре он давал лишние зазоры и завышал высоту.
		// Если NORMAL пуст (полностью прозрачный: The Red Theme, win8.1 min/max), берём
		// HOT — срез той же геометрии, поэтому кнопка рисуется в нормальных размерах.
		constexpr int kStateEmptyAlpha = 6;
		RECT NormalOrFallbackStateRect(
			const BYTE* atlas,
			UINT stride,
			UINT atlasW,
			UINT atlasH,
			const RECT& part) noexcept
		{
			const RECT normal = NormalStateRect(part);
			if (MeanAlpha(atlas, stride, atlasW, atlasH, normal) > kStateEmptyAlpha)
				return normal;
			return MostOpaqueStateRect(atlas, stride, atlasW, atlasH, part);
		}

		// Поджать прямоугольник до непрозрачного контента (для центрирования глифа).
		RECT ContentBBox(
			const BYTE* atlas,
			UINT stride,
			UINT atlasW,
			UINT atlasH,
			const RECT& r) noexcept
		{
			const int l = r.left < 0 ? 0 : r.left;
			const int t = r.top < 0 ? 0 : r.top;
			const int rr = r.right > static_cast<int>(atlasW) ? static_cast<int>(atlasW) : r.right;
			const int bb = r.bottom > static_cast<int>(atlasH) ? static_cast<int>(atlasH) : r.bottom;
			int lx = -1, rx = -1, ty = -1, by = -1;
			for (int y = t; y < bb; ++y)
			{
				const size_t row = static_cast<size_t>(y) * stride;
				for (int x = l; x < rr; ++x)
				{
					if (atlas[row + static_cast<size_t>(x) * 4 + 3] > kAlphaThreshold)
					{
						if (lx < 0 || x < lx) lx = x;
						if (x > rx) rx = x;
						if (ty < 0) ty = y;
						by = y;
					}
				}
			}
			if (lx < 0)
				return r;
			return RECT{ lx, ty, rx + 1, by + 1 };
		}

		HRESULT BlitStretch(
			IWICBitmap* preview,
			const BYTE* atlasBuffer,
			UINT atlasWidth,
			UINT atlasHeight,
			UINT atlasStride,
			RECT srcRect,
			int dstX,
			int dstY,
			int dstWidth,
			int dstHeight) noexcept
		{
			if (dstWidth <= 0 || dstHeight <= 0)
				return S_OK;

			const int srcWidth = RectWidth(srcRect);
			const int srcHeight = RectHeight(srcRect);
			if (srcWidth <= 0 || srcHeight <= 0)
				return S_OK;

			if (dstX < 0 || dstY < 0)
				return S_OK;

			UINT previewWidth{}, previewHeight{};
			RETURN_IF_FAILED(preview->GetSize(&previewWidth, &previewHeight));
			if (dstX + dstWidth > static_cast<int>(previewWidth) ||
				dstY + dstHeight > static_cast<int>(previewHeight))
			{
				return S_OK;
			}

			WICRect lockRect{ dstX, dstY, dstWidth, dstHeight };
			wil::com_ptr<IWICBitmapLock> lock;
			RETURN_IF_FAILED(preview->Lock(&lockRect, WICBitmapLockWrite, lock.put()));

			UINT dstStride{};
			UINT dstBufferSize{};
			BYTE* dstBuffer{};
			RETURN_IF_FAILED(lock->GetStride(&dstStride));
			RETURN_IF_FAILED(lock->GetDataPointer(&dstBufferSize, &dstBuffer));

			const auto atlasBufferSize = static_cast<size_t>(atlasStride) * atlasHeight;

			for (int y = 0; y < dstHeight; ++y)
			{
				const int srcY = srcRect.top + (y * srcHeight / dstHeight);
				if (srcY < 0 || srcY >= static_cast<int>(atlasHeight))
					continue;

				for (int x = 0; x < dstWidth; ++x)
				{
					const int srcX = srcRect.left + (x * srcWidth / dstWidth);
					if (srcX < 0 || srcX >= static_cast<int>(atlasWidth))
						continue;

					const auto srcOffset = static_cast<size_t>(srcY) * atlasStride + static_cast<size_t>(srcX) * 4;
					const auto dstOffset = static_cast<size_t>(y) * dstStride + static_cast<size_t>(x) * 4;

					if (srcOffset + 3 < atlasBufferSize && dstOffset + 3 < dstBufferSize)
					{
						BlendPremultipliedPixel(
							dstBuffer + dstOffset,
							atlasBuffer + srcOffset);
					}
				}
			}

			return S_OK;
		}

		// Source-over в premultiplied PBGRA; alphaPercent — 100 = без изменений.
		void BlendPremultipliedPixelScaled(BYTE* dst, const BYTE* src, UINT alphaPercent) noexcept
		{
			if (alphaPercent == 0)
				return;

			UINT srcA = src[3];
			if (alphaPercent != 100)
			{
				srcA = static_cast<UINT>((srcA * alphaPercent + 50) / 100);
				if (srcA > 255)
					srcA = 255;
			}

			if (srcA == 0)
				return;

			BYTE scaled[4];
			if (srcA == src[3])
			{
				scaled[0] = src[0];
				scaled[1] = src[1];
				scaled[2] = src[2];
				scaled[3] = static_cast<BYTE>(srcA);
			}
			else
			{
				const UINT origA = src[3];
				if (origA == 0)
					return;
				scaled[0] = static_cast<BYTE>((src[0] * srcA + origA - 1) / origA);
				scaled[1] = static_cast<BYTE>((src[1] * srcA + origA - 1) / origA);
				scaled[2] = static_cast<BYTE>((src[2] * srcA + origA - 1) / origA);
				scaled[3] = static_cast<BYTE>(srcA);
			}

			BlendPremultipliedPixel(dst, scaled);
		}

		HRESULT BlitStretchAlphaScaled(
			IWICBitmap* preview,
			const BYTE* atlasBuffer,
			UINT atlasWidth,
			UINT atlasHeight,
			UINT atlasStride,
			RECT srcRect,
			int dstX,
			int dstY,
			int dstWidth,
			int dstHeight,
			UINT alphaPercent) noexcept
		{
			if (alphaPercent == 0)
				return S_OK;

			if (dstWidth <= 0 || dstHeight <= 0)
				return S_OK;

			const int srcWidth = RectWidth(srcRect);
			const int srcHeight = RectHeight(srcRect);
			if (srcWidth <= 0 || srcHeight <= 0)
				return S_OK;

			if (dstX < 0 || dstY < 0)
				return S_OK;

			UINT previewWidth{}, previewHeight{};
			RETURN_IF_FAILED(preview->GetSize(&previewWidth, &previewHeight));
			if (dstX + dstWidth > static_cast<int>(previewWidth) ||
				dstY + dstHeight > static_cast<int>(previewHeight))
			{
				return S_OK;
			}

			WICRect lockRect{ dstX, dstY, dstWidth, dstHeight };
			wil::com_ptr<IWICBitmapLock> lock;
			RETURN_IF_FAILED(preview->Lock(&lockRect, WICBitmapLockWrite, lock.put()));

			UINT dstStride{};
			UINT dstBufferSize{};
			BYTE* dstBuffer{};
			RETURN_IF_FAILED(lock->GetStride(&dstStride));
			RETURN_IF_FAILED(lock->GetDataPointer(&dstBufferSize, &dstBuffer));

			const auto atlasBufferSize = static_cast<size_t>(atlasStride) * atlasHeight;

			for (int y = 0; y < dstHeight; ++y)
			{
				const int srcY = srcRect.top + (y * srcHeight / dstHeight);
				if (srcY < 0 || srcY >= static_cast<int>(atlasHeight))
					continue;

				for (int x = 0; x < dstWidth; ++x)
				{
					const int srcX = srcRect.left + (x * srcWidth / dstWidth);
					if (srcX < 0 || srcX >= static_cast<int>(atlasWidth))
						continue;

					const auto srcOffset = static_cast<size_t>(srcY) * atlasStride + static_cast<size_t>(srcX) * 4;
					const auto dstOffset = static_cast<size_t>(y) * dstStride + static_cast<size_t>(x) * 4;

					if (srcOffset + 3 < atlasBufferSize && dstOffset + 3 < dstBufferSize)
					{
						BlendPremultipliedPixelScaled(
							dstBuffer + dstOffset,
							atlasBuffer + srcOffset,
							alphaPercent);
					}
				}
			}

			return S_OK;
		}

		// Полный nine-grid 3x3 (оставлен для возможного будущего использования).
		[[maybe_unused]] HRESULT DrawNineGrid(
			IWICBitmap* preview,
			const BYTE* atlasBuffer,
			UINT atlasWidth,
			UINT atlasHeight,
			UINT atlasStride,
			RECT srcRect,
			ThemeMargins margins,
			int dstX,
			int dstY,
			int dstWidth,
			int dstHeight) noexcept
		{
			const int srcWidth = RectWidth(srcRect);
			const int srcHeight = RectHeight(srcRect);
			if (srcWidth <= 0 || srcHeight <= 0 || dstWidth <= 0 || dstHeight <= 0)
				return S_OK;

			int ml = ClampInt(margins.cxLeftWidth, 0, srcWidth / 2);
			int mr = ClampInt(margins.cxRightWidth, 0, srcWidth / 2);
			int mt = ClampInt(margins.cyTopHeight, 0, srcHeight / 2);
			int mb = ClampInt(margins.cyBottomHeight, 0, srcHeight / 2);

			if (ml + mr > srcWidth)
			{
				ml = srcWidth / 2;
				mr = srcWidth - ml;
			}
			if (mt + mb > srcHeight)
			{
				mt = srcHeight / 2;
				mb = srcHeight - mt;
			}

			const int centerSrcW = srcWidth - ml - mr > 0 ? srcWidth - ml - mr : 1;
			const int centerSrcH = srcHeight - mt - mb > 0 ? srcHeight - mt - mb : 1;

			const int dstMl = dstWidth * ml / srcWidth > 0 ? dstWidth * ml / srcWidth : 1;
			const int dstMr = dstWidth * mr / srcWidth > 0 ? dstWidth * mr / srcWidth : 1;
			const int dstMt = dstHeight * mt / srcHeight > 0 ? dstHeight * mt / srcHeight : 1;
			const int dstMb = dstHeight * mb / srcHeight > 0 ? dstHeight * mb / srcHeight : 1;

			int dstCenterW = dstWidth - dstMl - dstMr;
			int dstCenterH = dstHeight - dstMt - dstMb;
			if (dstCenterW < 1)
			{
				dstCenterW = 1;
				ml = 0;
				mr = 0;
			}
			if (dstCenterH < 1)
			{
				dstCenterH = 1;
				mt = 0;
				mb = 0;
			}

			const RECT srcTopLeft{ srcRect.left, srcRect.top, srcRect.left + ml, srcRect.top + mt };
			const RECT srcTop{ srcRect.left + ml, srcRect.top, srcRect.right - mr, srcRect.top + mt };
			const RECT srcTopRight{ srcRect.right - mr, srcRect.top, srcRect.right, srcRect.top + mt };
			const RECT srcLeft{ srcRect.left, srcRect.top + mt, srcRect.left + ml, srcRect.bottom - mb };
			const RECT srcCenter{
				srcRect.left + ml,
				srcRect.top + mt,
				srcRect.left + ml + centerSrcW,
				srcRect.top + mt + centerSrcH
			};
			const RECT srcRight{ srcRect.right - mr, srcRect.top + mt, srcRect.right, srcRect.bottom - mb };
			const RECT srcBottomLeft{ srcRect.left, srcRect.bottom - mb, srcRect.left + ml, srcRect.bottom };
			const RECT srcBottom{ srcRect.left + ml, srcRect.bottom - mb, srcRect.right - mr, srcRect.bottom };
			const RECT srcBottomRight{ srcRect.right - mr, srcRect.bottom - mb, srcRect.right, srcRect.bottom };

			const int x0 = dstX;
			const int x1 = dstX + dstMl;
			const int x2 = dstX + dstMl + dstCenterW;
			const int y0 = dstY;
			const int y1 = dstY + dstMt;
			const int y2 = dstY + dstMt + dstCenterH;

			RETURN_IF_FAILED(BlitStretch(preview, atlasBuffer, atlasWidth, atlasHeight, atlasStride, srcTopLeft, x0, y0, dstMl, dstMt));
			RETURN_IF_FAILED(BlitStretch(preview, atlasBuffer, atlasWidth, atlasHeight, atlasStride, srcTop, x1, y0, dstCenterW, dstMt));
			RETURN_IF_FAILED(BlitStretch(preview, atlasBuffer, atlasWidth, atlasHeight, atlasStride, srcTopRight, x2, y0, dstMr, dstMt));
			RETURN_IF_FAILED(BlitStretch(preview, atlasBuffer, atlasWidth, atlasHeight, atlasStride, srcLeft, x0, y1, dstMl, dstCenterH));
			RETURN_IF_FAILED(BlitStretch(preview, atlasBuffer, atlasWidth, atlasHeight, atlasStride, srcCenter, x1, y1, dstCenterW, dstCenterH));
			RETURN_IF_FAILED(BlitStretch(preview, atlasBuffer, atlasWidth, atlasHeight, atlasStride, srcRight, x2, y1, dstMr, dstCenterH));
			RETURN_IF_FAILED(BlitStretch(preview, atlasBuffer, atlasWidth, atlasHeight, atlasStride, srcBottomLeft, x0, y2, dstMl, dstMb));
			RETURN_IF_FAILED(BlitStretch(preview, atlasBuffer, atlasWidth, atlasHeight, atlasStride, srcBottom, x1, y2, dstCenterW, dstMb));
			RETURN_IF_FAILED(BlitStretch(preview, atlasBuffer, atlasWidth, atlasHeight, atlasStride, srcBottomRight, x2, y2, dstMr, dstMb));

			return S_OK;
		}

		// Заливка заголовка из FRAMETOP (active 46 / inactive 44).
		// ВЕРТИКАЛЬ — 3-срез по SIZINGMARGINS: cyTop/cyBottom (тонкие 3D-границы)
		//   рисуются 1:1 и не размываются, середина тянется на высоту полосы.
		// ГОРИЗОНТАЛЬ — поля cxLeft/cxRight для caption-частей ненадёжны, поэтому
		//   ширину НЕ растягиваем по полям:
		//  • широкая шапка (sw > outWidth, VistaAero/Win7 = 827 px) — 3-срез: левый и
		//    правый углы (≈ высота среза) 1:1, центр — кусок из СЕРЕДИНЫ источника 1:1
		//    (обрезка); углы окна сохраняются, блики/3D-границы чёткие;
		//  • узкая шапка — левая/правая половины 1:1, между ними тянем один центральный
		//    пиксель (упрощённый вариант): арт по краям/углам не деградирует.
		HRESULT DrawCaptionFill(
			IWICBitmap* preview,
			const BYTE* atlasBuffer,
			UINT atlasWidth,
			UINT atlasHeight,
			UINT atlasStride,
			const RectMap& rects,
			const MarginsMap& margins,
			int outWidth,
			int yOffset,
			int bandHeight) noexcept
		{
			int framePart = 0;
			std::optional<RECT> frameRect;
			for (const int id : { DWMW_FRAMETOP_ACTIVE, DWMW_FRAMETOP_INACTIVE })
			{
				if (const auto r = GetRect(rects, id, STATE_NORMAL))
				{
					framePart = id;
					frameRect = r;
					break;
				}
			}
			if (!frameRect)
				return S_OK;

			// Активное состояние = верхний срез (число состояний — строго по геометрии).
			const RECT st = NormalStateRect(*frameRect);
			const int sw = RectWidth(st);
			const int sh = RectHeight(st);
			if (sw <= 0 || sh <= 0 || outWidth <= 0)
				return S_OK;

			int destH = bandHeight;
			if (destH < 1)
				destH = 1;

			const ThemeMargins m = GetMargins(margins, framePart, STATE_NORMAL);

			// ВЕРТИКАЛЬ — 3-срез по 3D-границам (cyTop/cyBottom рисуем 1:1, чтобы тонкие
			// оттеняющие линии не размывались; середину тянем на высоту полосы).
			int mt = ClampInt(m.cyTopHeight, 0, sh);
			int mb = ClampInt(m.cyBottomHeight, 0, sh);
			if (mt + mb > sh) { mt = sh / 2; mb = sh - mt; }
			int dmt = ClampInt(mt, 0, destH);
			int dmb = ClampInt(mb, 0, destH);
			if (dmt + dmb > destH) { dmt = destH / 2; dmb = destH - dmt; }

			// ГОРИЗОНТАЛЬ — два режима (поля cxLeft/cxRight ненадёжны для caption-частей,
			// поэтому ширину НЕ растягиваем «как попало»):
			//  • Широкая шапка (sw > outWidth, напр. VistaAero/Win7 = 827 px) — берём кусок
			//    по ЦЕНТРУ 1:1 (обрезка): чёткие блики и 3D-границы, без углов окна по краям.
			//  • Узкая шапка — левую и правую половины копируем 1:1, между ними тянем один
			//    центральный пиксель (как упрощённый вариант): арт по краям не деградирует.
			int keepL, keepR, midX, midSrcW, dstMidW;
			if (sw > outWidth)
			{
				// Широкая цельная шапка (VistaAero/Win7 = 827 px): сохраняем СКРУГЛЁННЫЕ
				// УГЛЫ по краям 1:1 (ширина ≈ высота среза — столько занимает скругление),
				// а центр берём куском из СЕРЕДИНЫ источника 1:1 (обрезка). Так и углы на
				// месте, и блики/3D-границы чёткие.
				int corner = ClampInt(sh, 8, outWidth / 3);
				keepL = corner;
				keepR = corner;
				dstMidW = outWidth - keepL - keepR;
				const int srcMidAvail = sw - keepL - keepR;
				midX = st.left + keepL + (srcMidAvail - dstMidW) / 2;
				midSrcW = dstMidW;
			}
			else
			{
				keepL = (sw - 1) / 2;
				keepR = sw - 1 - keepL;
				midX = st.left + keepL;
				midSrcW = sw - keepL - keepR;   // = 1
				dstMidW = outWidth - keepL - keepR;
			}

			const int colSrcX[3] = { st.left, midX, st.right - keepR };
			const int colSrcW[3] = { keepL, midSrcW, keepR };
			const int colDstX[3] = { 0, keepL, outWidth - keepR };
			const int colDstW[3] = { keepL, dstMidW, keepR };

			const int rowSrcY[3] = { st.top, st.top + mt, st.top + sh - mb };
			const int rowSrcH[3] = { mt, sh - mt - mb, mb };
			const int rowDstY[3] = { yOffset, yOffset + dmt, yOffset + destH - dmb };
			const int rowDstH[3] = { dmt, destH - dmt - dmb, dmb };

			for (int r = 0; r < 3; ++r)
			{
				if (rowDstH[r] <= 0 || rowSrcH[r] <= 0)
					continue;
				for (int c = 0; c < 3; ++c)
				{
					if (colDstW[c] <= 0 || colSrcW[c] <= 0)
						continue;

					const RECT src{
						colSrcX[c], rowSrcY[r],
						colSrcX[c] + colSrcW[c], rowSrcY[r] + rowSrcH[r] };
					RETURN_IF_FAILED(BlitStretch(
						preview, atlasBuffer, atlasWidth, atlasHeight, atlasStride,
						src, colDstX[c], rowDstY[r], colDstW[c], rowDstH[r]));
				}
			}

			return S_OK;
		}

		HRESULT BlitCentered(
			IWICBitmap* preview,
			const BYTE* atlasBuffer,
			UINT atlasWidth,
			UINT atlasHeight,
			UINT atlasStride,
			RECT srcRect,
			int dstX,
			int dstY,
			int dstWidth,
			int dstHeight) noexcept
		{
			const int srcWidth = RectWidth(srcRect);
			const int srcHeight = RectHeight(srcRect);
			if (srcWidth <= 0 || srcHeight <= 0 || dstWidth <= 0 || dstHeight <= 0)
				return S_OK;

			int drawW = srcWidth;
			int drawH = srcHeight;
			if (drawW > dstWidth || drawH > dstHeight)
			{
				const int scaleW = dstWidth * 256 / drawW;
				const int scaleH = dstHeight * 256 / drawH;
				const int scale = scaleW < scaleH ? scaleW : scaleH;
				drawW = drawW * scale / 256;
				drawH = drawH * scale / 256;
				if (drawW < 1) drawW = 1;
				if (drawH < 1) drawH = 1;
			}

			const int offsetX = dstX + (dstWidth - drawW) / 2;
			const int offsetY = dstY + (dstHeight - drawH) / 2;
			return BlitStretch(
				preview,
				atlasBuffer,
				atlasWidth,
				atlasHeight,
				atlasStride,
				srcRect,
				offsetX,
				offsetY,
				drawW,
				drawH);
		}

		// PNG-ресурс отражения «стекла» в OpenGlass.dll (тип "PNG", id 102).
		constexpr int kReflectionResId = 102;

		// Грузим PNG 102 из OpenGlass.dll в 32bpp PBGRA-буфер. Картинка ссылается на
		// память ресурса напрямую (InitializeFromMemory не копирует), поэтому CopyPixels
		// обязан отработать до FreeLibrary — он внутри этой же функции, всё ок.
		HRESULT LoadReflectionPixels(
			IWICImagingFactory* wicFactory,
			LPCWSTR dllPath,
			std::unique_ptr<BYTE[]>& outBuffer,
			UINT& outWidth,
			UINT& outHeight,
			UINT& outStride) noexcept
		{
			if (!wicFactory || !dllPath || !*dllPath)
				return E_INVALIDARG;

			const HMODULE module = LoadLibraryExW(
				dllPath, nullptr,
				LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
			if (!module)
				return HRESULT_FROM_WIN32(GetLastError());
			const auto freeModule = wil::scope_exit([module] { FreeLibrary(module); });

			const HRSRC resInfo = FindResourceW(
				module, MAKEINTRESOURCEW(kReflectionResId), L"PNG");
			if (!resInfo)
				return HRESULT_FROM_WIN32(GetLastError());

			const HGLOBAL resData = LoadResource(module, resInfo);
			if (!resData)
				return HRESULT_FROM_WIN32(GetLastError());

			const DWORD resSize = SizeofResource(module, resInfo);
			void* const resPtr = LockResource(resData);
			if (!resPtr || resSize == 0)
				return E_FAIL;

			wil::com_ptr<IWICStream> stream;
			RETURN_IF_FAILED(wicFactory->CreateStream(stream.put()));
			RETURN_IF_FAILED(stream->InitializeFromMemory(
				static_cast<BYTE*>(resPtr), resSize));

			wil::com_ptr<IWICBitmapDecoder> decoder;
			RETURN_IF_FAILED(wicFactory->CreateDecoderFromStream(
				stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put()));

			wil::com_ptr<IWICBitmapFrameDecode> frame;
			RETURN_IF_FAILED(decoder->GetFrame(0, frame.put()));

			wil::com_ptr<IWICFormatConverter> converter;
			RETURN_IF_FAILED(wicFactory->CreateFormatConverter(converter.put()));
			RETURN_IF_FAILED(converter->Initialize(
				frame.get(), GUID_WICPixelFormat32bppPBGRA,
				WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom));

			UINT w{}, h{};
			RETURN_IF_FAILED(converter->GetSize(&w, &h));
			if (w == 0 || h == 0)
				return E_FAIL;

			const UINT stride = w * 4;
			const auto bufferSize = static_cast<size_t>(stride) * h;
			std::unique_ptr<BYTE[]> buffer(new BYTE[bufferSize]);

			WICRect rc{ 0, 0, static_cast<INT>(w), static_cast<INT>(h) };
			RETURN_IF_FAILED(converter->CopyPixels(
				&rc, stride, static_cast<UINT>(bufferSize), buffer.get()));

			outBuffer = std::move(buffer);
			outWidth = w;
			outHeight = h;
			outStride = stride;
			return S_OK;
		}

		// Декод произвольных байтов изображения (PNG/др. контейнер) через WIC в
		// независимый 32bpp PBGRA IWICBitmap. WICBitmapCacheOnLoad копирует пиксели,
		// поэтому результат безопасен после освобождения исходной памяти (ресурс/стрим темы).
		HRESULT DecodeImageBytes(
			IWICImagingFactory* wicFactory,
			const BYTE* data,
			DWORD size,
			IWICBitmap** outBitmap) noexcept
		{
			if (!wicFactory || !data || size == 0 || !outBitmap)
				return E_INVALIDARG;

			wil::com_ptr<IWICStream> stream;
			RETURN_IF_FAILED(wicFactory->CreateStream(stream.put()));
			RETURN_IF_FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data), size));

			wil::com_ptr<IWICBitmapDecoder> decoder;
			RETURN_IF_FAILED(wicFactory->CreateDecoderFromStream(
				stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put()));

			wil::com_ptr<IWICBitmapFrameDecode> frame;
			RETURN_IF_FAILED(decoder->GetFrame(0, frame.put()));

			wil::com_ptr<IWICFormatConverter> converter;
			RETURN_IF_FAILED(wicFactory->CreateFormatConverter(converter.put()));
			RETURN_IF_FAILED(converter->Initialize(
				frame.get(), GUID_WICPixelFormat32bppPBGRA,
				WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom));

			wil::com_ptr<IWICBitmap> bitmap;
			RETURN_IF_FAILED(wicFactory->CreateBitmapFromSource(
				converter.get(), WICBitmapCacheOnLoad, bitmap.put()));

			*outBitmap = bitmap.detach();
			return S_OK;
		}

		// Извлечение ресурса-изображения из DLL (data-file) в независимый IWICBitmap.
		// resType: строковый тип ресурса ("PNG") либо целочисленный в форме "#10"
		// (RT_RCDATA) — FindResourceW поддерживает '#'-нотацию.
		HRESULT DecodeResourceToBitmap(
			IWICImagingFactory* wicFactory,
			LPCWSTR dllPath,
			UINT resId,
			LPCWSTR resType,
			IWICBitmap** outBitmap) noexcept
		{
			if (!wicFactory || !dllPath || !*dllPath || !resType || !outBitmap)
				return E_INVALIDARG;

			const HMODULE module = LoadLibraryExW(
				dllPath, nullptr,
				LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
			if (!module)
				return HRESULT_FROM_WIN32(GetLastError());
			const auto freeModule = wil::scope_exit([module] { FreeLibrary(module); });

			const HRSRC resInfo = FindResourceW(module, MAKEINTRESOURCEW(resId), resType);
			if (!resInfo)
				return HRESULT_FROM_WIN32(GetLastError());

			const HGLOBAL resData = LoadResource(module, resInfo);
			if (!resData)
				return HRESULT_FROM_WIN32(GetLastError());

			const DWORD resSize = SizeofResource(module, resInfo);
			void* const resPtr = LockResource(resData);
			if (!resPtr || resSize == 0)
				return E_FAIL;

			return DecodeImageBytes(wicFactory, static_cast<const BYTE*>(resPtr), resSize, outBitmap);
		}

		// COM-инициализация, устойчивая к уже поднятому апартаменту. Вызывается из STA
		// (UI-поток C#) и MTA (Task.Run): wil::CoInitializeEx() кидает RPC_E_CHANGED_MODE
		// в STA → весь экспорт падал в E_FAIL. WIC работает в любом апартаменте, поэтому
		// при RPC_E_CHANGED_MODE просто не трогаем чужую инициализацию.
		[[nodiscard]] auto EnsureComInitialized() noexcept
		{
			const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
			const bool shouldUninit = SUCCEEDED(hr);
			return wil::scope_exit([shouldUninit]() noexcept
			{
				if (shouldUninit)
					CoUninitialize();
			});
		}
	}

	HRESULT ParseLayoutFile(LPCWSTR layoutPath, RectMap& outRects, MarginsMap& outMargins) noexcept
	{
		{
			WCHAR pathBuffer[512]{};
			swprintf_s(pathBuffer, L"ParseLayout start: %s", layoutPath ? layoutPath : L"<null>");
			LogNative(pathBuffer);
		}

		outRects.clear();
		outMargins.clear();

		FILE* file{};
		const auto err = _wfopen_s(&file, layoutPath, L"r");
		if (err != 0 || !file)
			return HRESULT_FROM_WIN32(err != 0 ? err : ERROR_FILE_NOT_FOUND);

		const auto closeFile = wil::scope_exit([file] { fclose(file); });

		// RS1-атласы (RS1Compatibility=1) используют сдвинутую нумерацию: part ID > 10
		// больше на 1, чем в схеме win10. Приводим к единой схеме (как OpenGlass).
		bool rs1Compat = false;

		WCHAR lineBuffer[512]{};
		while (!feof(file))
		{
			if (!fgetws(lineBuffer, static_cast<int>(std::size(lineBuffer)), file))
				break;

			if (lineBuffer[0] == L'#' || lineBuffer[0] == L'\n' || lineBuffer[0] == L'\r')
				continue;

			if (_wcsnicmp(lineBuffer, L"RS1Compatibility", 16) == 0)
			{
				int value{};
				if (swscanf_s(lineBuffer, L"RS1Compatibility=%d", &value) == 1 && value != 0)
					rs1Compat = true;
				continue;
			}

			int partId{}, stateId{}, propId{};
			RECT rect{};
			WCHAR keyBuffer[64]{};

			const auto scanned = swscanf_s(
				lineBuffer,
				L"%[^=]=%d,%d,%d,%d",
				keyBuffer,
				static_cast<unsigned int>(std::size(keyBuffer)),
				&rect.left,
				&rect.top,
				&rect.right,
				&rect.bottom
			);

			if (scanned != 5)
				continue;

			if (swscanf_s(keyBuffer, L"%d;%d;%d", &partId, &stateId, &propId) != 3)
				continue;

			if (rs1Compat && partId > 10)
				--partId;

			if (propId == TMT_ATLASRECT)
			{
				const auto key = MakeLayoutKey(partId, stateId, propId);
				outRects[key] = rect;
			}
			else if (propId == TMT_SIZINGMARGINS)
			{
				const auto key = MakeLayoutKey(partId, stateId, propId);
				outMargins[key] = ThemeMargins{
					rect.left,
					rect.top,
					rect.right,
					rect.bottom
				};
			}
		}

		if (outRects.empty())
		{
			LogNative(L"ParseLayout failed: no atlas rects");
			return E_FAIL;
		}

		{
			WCHAR summary[128]{};
			swprintf_s(summary, L"ParseLayout ok: rects=%zu margins=%zu", outRects.size(), outMargins.size());
			LogNative(summary);
		}

		return S_OK;
	}

	HRESULT LoadAtlasBitmap(
		IWICImagingFactory* wicFactory,
		LPCWSTR atlasPath,
		IWICBitmapSource** outBitmap,
		UINT* outWidth,
		UINT* outHeight) noexcept
	{
		if (!wicFactory || !atlasPath || !outBitmap)
			return E_POINTER;

		wil::com_ptr<IWICBitmapDecoder> decoder;
		RETURN_IF_FAILED(wicFactory->CreateDecoderFromFilename(
			atlasPath,
			nullptr,
			GENERIC_READ,
			WICDecodeMetadataCacheOnDemand,
			decoder.put()
		));

		wil::com_ptr<IWICBitmapFrameDecode> frame;
		RETURN_IF_FAILED(decoder->GetFrame(0, frame.put()));

		if (outWidth && outHeight)
			RETURN_IF_FAILED(frame->GetSize(outWidth, outHeight));

		wil::com_ptr<IWICFormatConverter> converter;
		RETURN_IF_FAILED(wicFactory->CreateFormatConverter(converter.put()));

		RETURN_IF_FAILED(converter->Initialize(
			frame.get(),
			GUID_WICPixelFormat32bppPBGRA,
			WICBitmapDitherTypeNone,
			nullptr,
			0.0,
			WICBitmapPaletteTypeCustom
		));

		*outBitmap = converter.detach();
		return S_OK;
	}

	HRESULT CompositePreview(
		IWICImagingFactory* wicFactory,
		IWICBitmapSource* atlasBitmap,
		const RectMap& rects,
		const MarginsMap& margins,
		UINT targetWidth,
		UINT targetHeight,
		LPCWSTR reflectionDllPath,
		UINT reflectionAlphaPercent,
		IWICBitmap** outPreview) noexcept
	{
		if (!wicFactory || !atlasBitmap || !outPreview)
			return E_POINTER;

		static_cast<void>(ActiveFrameOnly);

		UINT atlasWidth{}, atlasHeight{};
		RETURN_IF_FAILED(atlasBitmap->GetSize(&atlasWidth, &atlasHeight));

		const UINT atlasStride = atlasWidth * 4;
		const UINT atlasBufferSize = atlasStride * atlasHeight;
		std::unique_ptr<BYTE[]> atlasBuffer(new BYTE[atlasBufferSize]);

		WICRect atlasRect{ 0, 0, static_cast<INT>(atlasWidth), static_cast<INT>(atlasHeight) };
		RETURN_IF_FAILED(atlasBitmap->CopyPixels(&atlasRect, atlasStride, atlasBufferSize, atlasBuffer.get()));

		// Спецификации кнопок заголовка: фон (мультистейт) + глиф (мультистейт).
		struct CaptionButtonSpec
		{
			std::initializer_list<int> bgPartIds;
			std::initializer_list<int> glyphPartIds;
		};

		// Порядок слева направо: MIN, MAX, CLOSE. ВАЖНО: фоны part 3/5 в атласах оказались
		// «перепутаны» — скруглённый-слева фон (левый торец группы) лежит в part 5, а
		// плоский — в part 3. Поэтому ЛЕВОЙ кнопке (min-глиф) даём фон part 5, а средней
		// (max-глиф) — part 3; иначе скругление вылезает на средней кнопке.
		const CaptionButtonSpec buttonSpecs[] = {
			{ { DWMW_MAXBUTTON, DWMW_MAXBUTTON_ALT, DWMW_MAXBUTTON_INACTIVE },
			  { DWMW_GLYPH_MIN96, DWMW_GLYPH_MIN120, DWMW_GLYPH_MIN144, DWMW_GLYPH_MIN192 } },
			{ { DWMW_MINBUTTON, DWMW_MINBUTTON_ALT, DWMW_MINBUTTON_INACTIVE },
			  { DWMW_GLYPH_MAX96, DWMW_GLYPH_MAX120, DWMW_GLYPH_MAX144, DWMW_GLYPH_MAX192 } },
			{ { DWMW_CLOSEBUTTON, DWMW_CLOSEBUTTON_ALT, DWMW_CLOSEBUTTON_INACTIVE, DWMW_CLOSEBUTTON_MAX },
			  { DWMW_GLYPH_CLOSE96, DWMW_GLYPH_CLOSE120, DWMW_GLYPH_CLOSE144, DWMW_GLYPH_CLOSE192 } },
		};

		const int buttonCount = static_cast<int>(std::size(buttonSpecs));

		// Реальные размеры кнопок берём из НАТИВНОГО размера их NORMAL-состояния в
		// атласе (один срез мультистейт-полосы) — это соответствует тому, как они
		// выглядят в окнах. Высота заголовка = высота кнопки.
		// Семантика частей зависит от темы:
		//  • Стандартные темы (AEROTUNE/winxp): фон кнопки 3/5/7 (видимый) + мелкий
		//    символ-глиф 23/19/11 поверх. Ширина ячейки = ширина фона.
		//  • «Лицевые» темы (Aero Glow): фон 3/5/7 прозрачный, а 23/19/11 — это ВСЯ
		//    кнопка с запечённым символом. Тогда рисуем «лицо» в нативной ширине встык,
		//    ширина ячейки = ширина лица (иначе масштабирование давало бы зазоры).
		struct ResolvedButton
		{
			bool hasBg{};
			bool hasGlyph{};
			RECT glyphState{};
			RECT blitRect{};   // что рисуем встык (фон-срез или обрезанное «лицо»)
			int cellW{};
			int cellH{};
		};

		constexpr int kButtonBgAlphaMin = 8;

		ResolvedButton resolved[3]{};
		int captionHeight = 0;
		const int metricFallbackW = GetSystemMetrics(SM_CXSIZE) > 0 ? GetSystemMetrics(SM_CXSIZE) : 45;
		const int metricFallbackH = GetSystemMetrics(SM_CYSIZE) > 0 ? GetSystemMetrics(SM_CYSIZE) : 30;

		for (int i = 0; i < buttonCount; ++i)
		{
			auto& rb = resolved[i];
			RECT bgState{};
			std::optional<RECT> bgPartRect;

			if (const auto bgRect = GetRectFirst(rects, buttonSpecs[i].bgPartIds))
			{
				bgPartRect = *bgRect;
				bgState = NormalOrFallbackStateRect(
					atlasBuffer.get(), atlasStride, atlasWidth, atlasHeight, *bgRect);
				// Фон считаем присутствующим только если он реально непрозрачен.
				if (MeanAlpha(atlasBuffer.get(), atlasStride, atlasWidth, atlasHeight, bgState) > kButtonBgAlphaMin)
					rb.hasBg = true;
			}

			if (const auto glyphRect = GetRectFirst(rects, buttonSpecs[i].glyphPartIds))
			{
				rb.hasGlyph = true;
				rb.glyphState = NormalOrFallbackStateRect(
					atlasBuffer.get(), atlasStride, atlasWidth, atlasHeight, *glyphRect);
			}

			if (rb.hasBg)
			{
				// Стандартная тема: фон кнопки виден целиком, ширина ячейки = ширина фона.
				rb.blitRect = bgState;
				rb.cellW = RectWidth(bgState);
				rb.cellH = RectHeight(bgState);
			}
			else if (rb.hasGlyph)
			{
				// Только символ (Win10/11: min/max без текстуры фона). Для отрисовки — tight
				// bbox глифа; для раскладки — ширина ATLASRECT части кнопки (3/5/7), иначе
				// cellW схлопывается до ширины штриха и кнопки слипаются с close.
				rb.blitRect = ContentBBox(
					atlasBuffer.get(), atlasStride, atlasWidth, atlasHeight, rb.glyphState);
				if (bgPartRect)
				{
					const RECT bgLayout = NormalStateRect(*bgPartRect);
					rb.cellW = RectWidth(bgLayout);
					rb.cellH = RectHeight(bgLayout);
				}
				else
				{
					rb.cellW = RectWidth(rb.glyphState);
					rb.cellH = RectHeight(rb.glyphState);
				}
				if (rb.cellW <= 0)
					rb.cellW = metricFallbackW;
				if (rb.cellH <= 0)
					rb.cellH = metricFallbackH;
			}
			else
			{
				rb.cellW = metricFallbackW;
				rb.cellH = metricFallbackH;
			}

			if (rb.cellH > captionHeight)
				captionHeight = rb.cellH;
		}
		if (captionHeight <= 0)
			captionHeight = metricFallbackH;

		// «Толстые» рамки (winxp) сами задают высоту заголовка: их FRAMETOP содержит
		// полный градиент caption. Тонкие рамки (Win10/11) ниже кнопок — рисуются
		// сверху в нативной высоте, тело остаётся прозрачным.
		if (const auto frameRect = GetRectFirst(rects, { DWMW_FRAMETOP_ACTIVE, DWMW_FRAMETOP_INACTIVE }))
		{
			const int frameH = RectHeight(NormalStateRect(*frameRect));
			if (frameH > captionHeight)
				captionHeight = frameH;
		}

		int buttonsTotalW = 0;
		for (const auto& rb : resolved)
			buttonsTotalW += rb.cellW;

		// СТАБИЛЬНЫЙ размер: холст фиксирован (targetWidth×targetHeight), кнопки прижаты
		// вправо. Полосу прижимаем к ВЕРХУ холста (yOffset=0): верхняя граница рамки
		// совпадает с верхом изображения, поэтому акриловая подложка в XAML (равная
		// габаритам изображения) начинается ровно там, где атлас рисует рамку. Низ под
		// полосой — «тело» окна (стекло). Так миниатюры не «гуляют» и влезают в карточку.
		const int outWidth = static_cast<int>(targetWidth);
		const int outHeight = static_cast<int>(targetHeight);
		if (captionHeight > outHeight)
			captionHeight = outHeight;
		const int yOffset = 0;

		{
			WCHAR sizeLog[200]{};
			swprintf_s(
				sizeLog,
				L"Composite canvas=%dx%d caption=%d cells=%d/%d/%d",
				outWidth, outHeight, captionHeight,
				resolved[0].cellW, resolved[1].cellW, resolved[2].cellW);
			LogNative(sizeLog);
		}

		wil::com_ptr<IWICBitmap> preview;
		RETURN_IF_FAILED(wicFactory->CreateBitmap(
			static_cast<UINT>(outWidth),
			static_cast<UINT>(outHeight),
			GUID_WICPixelFormat32bppPBGRA,
			WICBitmapCacheOnDemand,
			preview.put()
		));

		{
			WICRect lockRect{ 0, 0, outWidth, outHeight };
			wil::com_ptr<IWICBitmapLock> lock;
			RETURN_IF_FAILED(preview->Lock(&lockRect, WICBitmapLockWrite, lock.put()));

			UINT bufferSize{};
			BYTE* buffer{};
			RETURN_IF_FAILED(lock->GetDataPointer(&bufferSize, &buffer));
			ZeroMemory(buffer, bufferSize);
		}

		// ПЕРВЫЙ СЛОЙ (фон): отражение «стекла» из OpenGlass.dll (PNG 102). Берём кусок
		// от верхнего левого угла БЕЗ масштабирования (1:1) и кладём в (0,0). Поверх
		// смешиваются рамка и кнопки. Отражение полупрозрачно (alpha мал), поэтому
		// сквозь тело окна по-прежнему читается шахматка-индикатор из XAML.
		if (reflectionAlphaPercent > 0 && reflectionDllPath && *reflectionDllPath)
		{
			std::unique_ptr<BYTE[]> reflBuffer;
			UINT reflW{}, reflH{}, reflStride{};
			const HRESULT reflHr = LoadReflectionPixels(
				wicFactory, reflectionDllPath, reflBuffer, reflW, reflH, reflStride);
			if (SUCCEEDED(reflHr) && reflBuffer)
			{
				const int cropW = reflW < static_cast<UINT>(outWidth)
					? static_cast<int>(reflW) : outWidth;
				const int cropH = reflH < static_cast<UINT>(outHeight)
					? static_cast<int>(reflH) : outHeight;
				const RECT reflSrc{ 0, 0, cropW, cropH };
				RETURN_IF_FAILED(BlitStretchAlphaScaled(
					preview.get(), reflBuffer.get(), reflW, reflH, reflStride,
					reflSrc, 0, 0, cropW, cropH, reflectionAlphaPercent));
			}
			else
			{
				LogNativeHr(L"LoadReflectionPixels failed (continue without)", reflHr);
			}
		}

		// Заливка заголовка из FRAMETOP active (46): по вертикали 3-срез (3D-границы
		// 1:1), по ширине — обрезка по центру для широких шапок (VistaAero/Win7) либо
		// «половины 1:1 + центральный пиксель» для узких.
		RETURN_IF_FAILED(DrawCaptionFill(
			preview.get(),
			atlasBuffer.get(),
			atlasWidth,
			atlasHeight,
			atlasStride,
			rects,
			margins,
			outWidth,
			yOffset,
			captionHeight));

		// Кнопки в правом краю полосы, встык. Фон/«лицо» — на высоту полосы,
		// символ (для стандартных тем) — по центру ТЕЛА кнопки (не всей полосы).
		// Небольшой инсет (вниз и влево), чтобы верхняя/правая кромки кнопок не лезли
		// на верхнюю/правую кромки шапки, как в реальном окне.
		constexpr int kButtonInsetRight = 2;
		constexpr int kButtonInsetTop = 2;
		int buttonX = outWidth - buttonsTotalW - kButtonInsetRight;
		if (buttonX < 0)
			buttonX = 0;

		for (int i = 0; i < buttonCount; ++i)
		{
			const auto& rb = resolved[i];
			const int cellH = rb.cellH > 0 ? rb.cellH : captionHeight;
			int cellY = yOffset + (captionHeight - cellH) / 2 + kButtonInsetTop;
			if (cellY + cellH > outHeight)
				cellY = outHeight - cellH;
			if (cellY < 0)
				cellY = 0;

			// Только глиф без текстуры фона: символ по центру ячейки, без растягивания.
			if (!rb.hasBg && rb.hasGlyph)
			{
				const RECT glyphContent = ContentBBox(
					atlasBuffer.get(), atlasStride, atlasWidth, atlasHeight, rb.glyphState);
				RETURN_IF_FAILED(BlitCentered(
					preview.get(),
					atlasBuffer.get(),
					atlasWidth,
					atlasHeight,
					atlasStride,
					glyphContent,
					buttonX,
					cellY,
					rb.cellW,
					cellH
				));
				buttonX += rb.cellW;
				continue;
			}

			// Тело кнопки = непрозрачный контент среза. По горизонтали ставим по ЦЕНТРУ
			// ячейки (шаг = ширина среза), чтобы при несимметричных полях (Version 2)
			// кнопки не слипались и зазоры были равны.
			const RECT body = ContentBBox(
				atlasBuffer.get(), atlasStride, atlasWidth, atlasHeight, rb.blitRect);
			const bool hasBody = RectWidth(body) > 0 && RectHeight(body) > 0;
			const int drawW = hasBody ? RectWidth(body) : rb.cellW;
			const int drawX = buttonX + (rb.cellW - drawW) / 2;

			// КЛЮЧЕВОЕ для высоты: различаем два вида среза.
			//  • «Стеклянная пилюля» (VistaAero, Win7, win8.1): контент прижат к верху
			//    (topOff ≤ 2) и снизу лишь небольшой прозрачный «фартук» (≤ 40% среза) —
			//    это часть кнопки, её нужно ЗАПОЛНИТЬ растяжением тела до высоты среза,
			//    иначе кнопка выходит вдвое ниже нужного.
			//  • «Готовая кнопка» (AEROTUNE, XP, Version 2, Bupa): контент уже нужного
			//    размера (центрирован или с большим намеренным «фартуком») — растяжение
			//    деформирует пиксель-арт, поэтому рисуем в НАТИВНОЙ высоте 1:1.
			bool stretchFill = false;
			int topOff = 0;
			int bodyH = cellH;
			if (rb.hasBg && hasBody)
			{
				const int sliceH = RectHeight(rb.blitRect);
				topOff = body.top - rb.blitRect.top;
				const int botSkirt = rb.blitRect.bottom - body.bottom;
				bodyH = RectHeight(body);
				stretchFill = (topOff <= 2) && (botSkirt * 5 <= sliceH * 2);
			}

			RECT bodySrc{};
			int glyphY = cellY;
			int glyphH = cellH;
			if (stretchFill)
			{
				// Растягиваем сам контент по вертикали на высоту ячейки.
				bodySrc = hasBody ? body : rb.blitRect;
				glyphY = cellY;
				glyphH = cellH;
			}
			else if (rb.hasBg && hasBody)
			{
				// Нативная высота: вертикаль среза не масштабируем (1:1), контент остаётся
				// на своём месте; символ центрируем по реальному телу кнопки.
				bodySrc = RECT{ body.left, rb.blitRect.top, body.right, rb.blitRect.bottom };
				glyphY = cellY + topOff;
				glyphH = bodyH;
			}
			else
			{
				// «Лицевые» темы (Aero Glow): рисуем лицо как есть.
				bodySrc = hasBody ? body : rb.blitRect;
			}

			RETURN_IF_FAILED(BlitStretch(
				preview.get(),
				atlasBuffer.get(),
				atlasWidth,
				atlasHeight,
				atlasStride,
				bodySrc,
				drawX,
				cellY,
				drawW,
				cellH
			));

			if (rb.hasBg && rb.hasGlyph)
			{
				// Символ — по центру тела кнопки, в нативном размере.
				const RECT glyphContent = ContentBBox(
					atlasBuffer.get(), atlasStride, atlasWidth, atlasHeight, rb.glyphState);
				RETURN_IF_FAILED(BlitCentered(
					preview.get(),
					atlasBuffer.get(),
					atlasWidth,
					atlasHeight,
					atlasStride,
					glyphContent,
					drawX,
					glyphY,
					drawW,
					glyphH
				));
			}

			buttonX += rb.cellW;
		}

		*outPreview = preview.detach();
		return S_OK;
	}

	HRESULT SaveBitmapToPNG(
		IWICImagingFactory* wicFactory,
		IWICBitmap* bitmap,
		LPCWSTR outputPath) noexcept
	{
		if (!wicFactory || !bitmap || !outputPath)
			return E_POINTER;

		wil::com_ptr<IWICBitmapEncoder> encoder;
		RETURN_IF_FAILED(wicFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));

		wil::com_ptr<IWICStream> stream;
		RETURN_IF_FAILED(wicFactory->CreateStream(stream.put()));
		RETURN_IF_FAILED(stream->InitializeFromFilename(outputPath, GENERIC_WRITE));

		RETURN_IF_FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));

		wil::com_ptr<IWICBitmapFrameEncode> frame;
		RETURN_IF_FAILED(encoder->CreateNewFrame(frame.put(), nullptr));
		RETURN_IF_FAILED(frame->Initialize(nullptr));

		UINT width{}, height{};
		RETURN_IF_FAILED(bitmap->GetSize(&width, &height));
		RETURN_IF_FAILED(frame->SetSize(width, height));

		WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
		RETURN_IF_FAILED(frame->SetPixelFormat(&format));

		RETURN_IF_FAILED(frame->WriteSource(bitmap, nullptr));

		RETURN_IF_FAILED(frame->Commit());
		RETURN_IF_FAILED(encoder->Commit());

		return S_OK;
	}
}

extern "C" __declspec(dllexport) HRESULT WINAPI GlassSettings_GenerateAtlasPreview(
	LPCWSTR atlasPath,
	LPCWSTR layoutPath,
	LPCWSTR outputPath,
	UINT targetWidth,
	UINT targetHeight,
	LPCWSTR reflectionDllPath,
	UINT reflectionAlphaPercent)
{
	try
	{
		using namespace GlassSettingsNative::AtlasPreview;

		if (!atlasPath || !layoutPath || !outputPath)
			return E_POINTER;

		if (targetWidth == 0 || targetHeight == 0)
			return E_INVALIDARG;

		{
			WCHAR entry[768]{};
			swprintf_s(
				entry,
				L"GenerateAtlasPreview start atlas=%s layout=%s output=%s size=%ux%u",
				atlasPath,
				layoutPath,
				outputPath,
				targetWidth,
				targetHeight);
			GlassSettingsNative::AtlasPreview::LogNative(entry);
		}

		const auto coInit = wil::CoInitializeEx();

		wil::com_ptr<IWICImagingFactory> wicFactory;
		HRESULT hr = CoCreateInstance(
			CLSID_WICImagingFactory,
			nullptr,
			CLSCTX_INPROC_SERVER,
			IID_PPV_ARGS(wicFactory.put())
		);
		if (FAILED(hr))
		{
			GlassSettingsNative::AtlasPreview::LogNativeHr(L"CoCreateInstance(WIC)", hr);
			return hr;
		}

		RectMap rects;
		MarginsMap margins;
		hr = ParseLayoutFile(layoutPath, rects, margins);
		if (FAILED(hr))
		{
			GlassSettingsNative::AtlasPreview::LogNativeHr(L"ParseLayoutFile", hr);
			return hr;
		}

		wil::com_ptr<IWICBitmapSource> atlasBitmap;
		hr = LoadAtlasBitmap(wicFactory.get(), atlasPath, atlasBitmap.put(), nullptr, nullptr);
		if (FAILED(hr))
		{
			GlassSettingsNative::AtlasPreview::LogNativeHr(L"LoadAtlasBitmap", hr);
			return hr;
		}

		wil::com_ptr<IWICBitmap> preview;
		hr = CompositePreview(wicFactory.get(), atlasBitmap.get(), rects, margins, targetWidth, targetHeight, reflectionDllPath, reflectionAlphaPercent, preview.put());
		if (FAILED(hr))
		{
			GlassSettingsNative::AtlasPreview::LogNativeHr(L"CompositePreview", hr);
			return hr;
		}

		hr = SaveBitmapToPNG(wicFactory.get(), preview.get(), outputPath);
		if (FAILED(hr))
		{
			GlassSettingsNative::AtlasPreview::LogNativeHr(L"SaveBitmapToPNG", hr);
			return hr;
		}

		GlassSettingsNative::AtlasPreview::LogNative(L"GenerateAtlasPreview success");
		return S_OK;
	}
	catch (...)
	{
		GlassSettingsNative::AtlasPreview::LogNative(L"GenerateAtlasPreview exception");
		return E_FAIL;
	}
}

// Извлечение изображения-ресурса из DLL в PNG-файл.
// Дефолт отражения: dllPath=OpenGlass.dll, resId=102, resType="PNG".
// Дефолт материала: dllPath=Windows.UI.Xaml.Controls.dll, resId=2000, resType="#10" (RT_RCDATA).
extern "C" __declspec(dllexport) HRESULT WINAPI GlassSettings_ExtractResourceImage(
	LPCWSTR dllPath,
	UINT resId,
	LPCWSTR resType,
	LPCWSTR outputPath)
{
	try
	{
		using namespace GlassSettingsNative::AtlasPreview;

		if (!dllPath || !resType || !outputPath)
			return E_POINTER;

		const auto coInit = EnsureComInitialized();

		wil::com_ptr<IWICImagingFactory> wicFactory;
		HRESULT hr = CoCreateInstance(
			CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
			IID_PPV_ARGS(wicFactory.put()));
		if (FAILED(hr))
		{
			LogNativeHr(L"CoCreateInstance(WIC)", hr);
			return hr;
		}

		wil::com_ptr<IWICBitmap> bitmap;
		hr = DecodeResourceToBitmap(wicFactory.get(), dllPath, resId, resType, bitmap.put());
		if (FAILED(hr))
		{
			LogNativeHr(L"DecodeResourceToBitmap", hr);
			return hr;
		}

		hr = SaveBitmapToPNG(wicFactory.get(), bitmap.get(), outputPath);
		if (FAILED(hr))
		{
			LogNativeHr(L"SaveBitmapToPNG(resource)", hr);
			return hr;
		}

		LogNative(L"ExtractResourceImage success");
		return S_OK;
	}
	catch (...)
	{
		GlassSettingsNative::AtlasPreview::LogNative(L"ExtractResourceImage exception");
		return E_FAIL;
	}
}

// Превью окна для текущего визуального стиля: атлас и раскладка берутся
// напрямую из активной .msstyles через theme API (без .layout/.png на диске).
// Повтор логики экспорта атласа из OpenGlassGUI + CustomThemeAtlasLoader.
extern "C" __declspec(dllexport) HRESULT WINAPI GlassSettings_GenerateCurrentThemeAtlasPreview(
	LPCWSTR outputPath,
	UINT targetWidth,
	UINT targetHeight,
	LPCWSTR reflectionDllPath,
	UINT reflectionAlphaPercent)
{
	try
	{
		using namespace GlassSettingsNative::AtlasPreview;

		if (!outputPath)
			return E_POINTER;
		if (targetWidth == 0 || targetHeight == 0)
			return E_INVALIDARG;

		const auto coInit = EnsureComInitialized();

		wil::com_ptr<IWICImagingFactory> wicFactory;
		HRESULT hr = CoCreateInstance(
			CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
			IID_PPV_ARGS(wicFactory.put()));
		if (FAILED(hr))
		{
			LogNativeHr(L"CoCreateInstance(WIC)", hr);
			return hr;
		}

		WCHAR themeFileName[MAX_PATH]{};
		hr = GetCurrentThemeName(themeFileName, ARRAYSIZE(themeFileName), nullptr, 0, nullptr, 0);
		if (FAILED(hr))
		{
			LogNativeHr(L"GetCurrentThemeName", hr);
			return hr;
		}

		// Флаги ровно как в OpenGlass (CustomThemeAtlasLoader): GetThemeStream читает
		// DISKSTREAM именно из data-file модуля .msstyles. AS_IMAGE_RESOURCE здесь даёт
		// ERROR_NOT_FOUND.
		const HMODULE themeModule = LoadLibraryExW(
			themeFileName, nullptr,
			LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (!themeModule)
		{
			hr = HRESULT_FROM_WIN32(GetLastError());
			LogNativeHr(L"LoadLibraryEx(msstyles)", hr);
			return hr;
		}
		const auto freeThemeModule = wil::scope_exit([themeModule] { FreeLibrary(themeModule); });

		const HTHEME hTheme = OpenThemeData(nullptr, L"DWMWindow");
		if (!hTheme)
		{
			LogNative(L"OpenThemeData(DWMWindow) returned null");
			return E_FAIL;
		}
		const auto closeTheme = wil::scope_exit([hTheme] { CloseThemeData(hTheme); });

		// part = DWM_WINDOW_THEME_PART::COMMON (0), prop = TMT_DISKSTREAM — общий атлас темы.
		void* streamAddr = nullptr;
		DWORD streamSize = 0;
		hr = GetThemeStream(hTheme, 0, 0, TMT_DISKSTREAM, &streamAddr, &streamSize, themeModule);
		if (FAILED(hr) || !streamAddr || streamSize == 0)
		{
			LogNativeHr(L"GetThemeStream(TMT_DISKSTREAM)", FAILED(hr) ? hr : E_FAIL);
			return FAILED(hr) ? hr : E_FAIL;
		}

		wil::com_ptr<IWICBitmap> atlasBitmap;
		hr = DecodeImageBytes(wicFactory.get(), static_cast<const BYTE*>(streamAddr), streamSize, atlasBitmap.put());
		if (FAILED(hr))
		{
			LogNativeHr(L"DecodeImageBytes(atlas)", hr);
			return hr;
		}

		RectMap rects;
		MarginsMap margins;
		for (int part = 1; part <= 91; ++part)
		{
			RECT rc{};
			if (SUCCEEDED(GetThemeRect(hTheme, part, STATE_NORMAL, TMT_ATLASRECT, &rc)))
				rects[MakeLayoutKey(part, STATE_NORMAL, TMT_ATLASRECT)] = rc;

			MARGINS mg{};
			if (SUCCEEDED(GetThemeMargins(hTheme, nullptr, part, STATE_NORMAL, TMT_SIZINGMARGINS, nullptr, &mg)))
			{
				margins[MakeLayoutKey(part, STATE_NORMAL, TMT_SIZINGMARGINS)] =
					ThemeMargins{ mg.cxLeftWidth, mg.cxRightWidth, mg.cyTopHeight, mg.cyBottomHeight };
			}
		}

		if (rects.empty())
		{
			LogNative(L"GenerateCurrentThemeAtlasPreview: no atlas rects");
			return E_FAIL;
		}

		wil::com_ptr<IWICBitmap> preview;
		hr = CompositePreview(wicFactory.get(), atlasBitmap.get(), rects, margins, targetWidth, targetHeight, reflectionDllPath, reflectionAlphaPercent, preview.put());
		if (FAILED(hr))
		{
			LogNativeHr(L"CompositePreview(currentTheme)", hr);
			return hr;
		}

		hr = SaveBitmapToPNG(wicFactory.get(), preview.get(), outputPath);
		if (FAILED(hr))
		{
			LogNativeHr(L"SaveBitmapToPNG(currentTheme)", hr);
			return hr;
		}

		LogNative(L"GenerateCurrentThemeAtlasPreview success");
		return S_OK;
	}
	catch (...)
	{
		GlassSettingsNative::AtlasPreview::LogNative(L"GenerateCurrentThemeAtlasPreview exception");
		return E_FAIL;
	}
}

// Экспорт «сырого» атласа активной темы: DISKSTREAM (PNG) пишется как есть в
// outputPngPath, рядом создаётся сопутствующий «<outputPngPath>.layout» с ATLASRECT
// и SIZINGMARGINS/CONTENTMARGINS — формат, совместимый с ParseLayoutFile и
// CustomThemeAtlas из OpenGlass (донор: OpenGlassGUI «Export current theme atlas»).
extern "C" __declspec(dllexport) HRESULT WINAPI GlassSettings_ExportCurrentThemeAtlas(
	LPCWSTR outputPngPath)
{
	try
	{
		using namespace GlassSettingsNative::AtlasPreview;

		if (!outputPngPath || !*outputPngPath)
			return E_POINTER;

		WCHAR themeFileName[MAX_PATH]{};
		HRESULT hr = GetCurrentThemeName(themeFileName, ARRAYSIZE(themeFileName), nullptr, 0, nullptr, 0);
		if (FAILED(hr))
		{
			LogNativeHr(L"GetCurrentThemeName", hr);
			return hr;
		}

		const HMODULE themeModule = LoadLibraryExW(
			themeFileName, nullptr,
			LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (!themeModule)
		{
			hr = HRESULT_FROM_WIN32(GetLastError());
			LogNativeHr(L"LoadLibraryEx(msstyles)", hr);
			return hr;
		}
		const auto freeThemeModule = wil::scope_exit([themeModule] { FreeLibrary(themeModule); });

		const HTHEME hTheme = OpenThemeData(nullptr, L"DWMWindow");
		if (!hTheme)
		{
			LogNative(L"OpenThemeData(DWMWindow) returned null");
			return E_FAIL;
		}
		const auto closeTheme = wil::scope_exit([hTheme] { CloseThemeData(hTheme); });

		void* streamAddr = nullptr;
		DWORD streamSize = 0;
		hr = GetThemeStream(hTheme, 0, 0, TMT_DISKSTREAM, &streamAddr, &streamSize, themeModule);
		if (FAILED(hr) || !streamAddr || streamSize == 0)
		{
			LogNativeHr(L"GetThemeStream(TMT_DISKSTREAM)", FAILED(hr) ? hr : E_FAIL);
			return FAILED(hr) ? hr : E_FAIL;
		}

		// Сам атлас (PNG) — побайтно как в .msstyles.
		{
			wil::unique_hfile file{ CreateFileW(
				outputPngPath, GENERIC_WRITE, 0, nullptr,
				CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) };
			if (!file)
			{
				hr = HRESULT_FROM_WIN32(GetLastError());
				LogNativeHr(L"CreateFile(atlas png)", hr);
				return hr;
			}

			DWORD written = 0;
			if (!WriteFile(file.get(), streamAddr, streamSize, &written, nullptr) || written != streamSize)
			{
				hr = HRESULT_FROM_WIN32(GetLastError());
				LogNativeHr(L"WriteFile(atlas png)", hr);
				return hr;
			}
		}

		// Сопутствующий .layout (best-effort): прямоугольники и поля частей DWMWindow.
		std::wstring layoutPath{ outputPngPath };
		layoutPath += L".layout";
		{
			wil::unique_hfile layoutFile{ CreateFileW(
				layoutPath.c_str(), GENERIC_WRITE, 0, nullptr,
				CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) };
			if (layoutFile)
			{
				std::string content;
				char buffer[256];

				content += "# Rects\n# 8002 = TMT_ATLASRECT\n";
				for (int part = 1; part < 100; ++part)
				{
					RECT rc{};
					if (SUCCEEDED(GetThemeRect(hTheme, part, 0, TMT_ATLASRECT, &rc)))
					{
						const int len = sprintf_s(buffer, "%u;0;8002=%ld,%ld,%ld,%ld\n",
							part, rc.left, rc.top, rc.right, rc.bottom);
						if (len > 0)
							content.append(buffer, len);
					}
				}

				content += "\n# Margins\n# 3601 = TMT_SIZINGMARGINS\n# 3602 = TMT_CONTENTMARGINS\n";
				for (int part = 1; part < 100; ++part)
				{
					MARGINS mg{};
					if (SUCCEEDED(GetThemeMargins(hTheme, nullptr, part, 0, TMT_SIZINGMARGINS, nullptr, &mg)))
					{
						const int len = sprintf_s(buffer, "%u;0;3601=%d,%d,%d,%d\n",
							part, mg.cxLeftWidth, mg.cxRightWidth, mg.cyTopHeight, mg.cyBottomHeight);
						if (len > 0)
							content.append(buffer, len);
					}
					if (SUCCEEDED(GetThemeMargins(hTheme, nullptr, part, 0, TMT_CONTENTMARGINS, nullptr, &mg)))
					{
						const int len = sprintf_s(buffer, "%u;0;3602=%d,%d,%d,%d\n",
							part, mg.cxLeftWidth, mg.cxRightWidth, mg.cyTopHeight, mg.cyBottomHeight);
						if (len > 0)
							content.append(buffer, len);
					}
				}

				if (!content.empty())
				{
					DWORD written = 0;
					WriteFile(layoutFile.get(), content.c_str(),
						static_cast<DWORD>(content.size()), &written, nullptr);
				}
			}
		}

		LogNative(L"ExportCurrentThemeAtlas success");
		return S_OK;
	}
	catch (...)
	{
		GlassSettingsNative::AtlasPreview::LogNative(L"ExportCurrentThemeAtlas exception");
		return E_FAIL;
	}
}
