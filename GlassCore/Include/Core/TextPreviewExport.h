#pragma once
#include <Windows.h>
#include <cstdint>

#include <GlassInkAlphaCapsule.h>

// Плоские параметры hero-превью текста (GlassSettings) — без GlassInkAlphaCapsule blob.
#pragma pack(push, 1)
struct GlassCoreTextPreviewGlow
{
    uint32_t enabled;
    uint32_t radiusThousandths;
    uint32_t colorArgb;
    uint32_t spreadMode;
};

struct GlassCoreTextPreviewInk
{
    uint8_t  renderMode;       // 0=auto, 1=single, 2=dual
    uint8_t  targetBlendAlpha;
    uint8_t  pseudoGlowEnabled;
    uint8_t  reserved0;
    float    coverageGamma;
    uint32_t pseudoGlowRadiusThousandths;
    uint32_t pseudoGlowIntensity;
};

struct GlassCoreTextPreviewRequest
{
    const WCHAR* sampleText;
    uint32_t     sampleCch;
    LOGFONTW     font;
    const GlassCoreTextPreviewGlow* glow;
    const GlassCoreTextPreviewInk*  ink; // nullptr → текущие engine defaults
    const Shared::InkCapsule::ProfilePayload* inkProfile; // nullptr → subset из ink
    RECT         paintRect;
    BOOL         isDarkTheme;
};
#pragma pack(pop)

// GPA по короткому имени: см. GlassCore.def (x86 stdcall иначе _Name@N).
extern "C" __declspec(dllexport)
HRESULT WINAPI GlassCore_PaintTextPreviewToBuffer(
    BYTE* bgraPremul,
    int   width,
    int   height,
    int   stride,
    const GlassCoreTextPreviewRequest* request,
    BOOL  interactiveDuringGesture) noexcept;
