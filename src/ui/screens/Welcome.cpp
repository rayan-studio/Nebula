#include "Welcome.h"
#include <wincodec.h>
#include <dwrite.h>
#include <vector>
#include <string>
#include <algorithm>
#include "helpers/window_helpers.h"
#include "helpers/path_helpers.h"
#include "ui/theme/Theme.h"
#include <filesystem>
#include <cwctype>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

static ID2D1Bitmap *g_welcomeIcon = nullptr;

static std::wstring ResolveUiAssetPath(const wchar_t *filename)
{
    if (!filename || !*filename)
        return L"";

    std::filesystem::path requested(filename);
    if (requested.is_absolute())
        return requested.wstring();

    std::wstring generic = requested.generic_wstring();
    std::wstring lower = generic;
    for (wchar_t &ch : lower)
        ch = (wchar_t)towlower(ch);

    if (lower.rfind(L"assets/", 0) == 0)
    {
        std::filesystem::path rel = std::filesystem::path(generic).lexically_relative(std::filesystem::path(L"assets"));
        return NebulaAssetPath(rel).wstring();
    }

    return (NebulaExeDir() / requested).wstring();
}

static ID2D1Bitmap *LoadBitmapFromFile(ID2D1RenderTarget *ctx, const wchar_t *filename)
{
    if (g_welcomeIcon)
        return g_welcomeIcon;

    IWICImagingFactory *wicFactory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory))))
        return nullptr;

    std::wstring resolvedPath = ResolveUiAssetPath(filename);
    IWICBitmapDecoder *decoder = nullptr;
    if (FAILED(wicFactory->CreateDecoderFromFilename(resolvedPath.c_str(), NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)))
    {
        wicFactory->Release();
        return nullptr;
    }

    IWICBitmapFrameDecode *frame = nullptr;
    decoder->GetFrame(0, &frame);
    if (!frame)
    {
        decoder->Release();
        wicFactory->Release();
        return nullptr;
    }

    IWICFormatConverter *converter = nullptr;
    if (FAILED(wicFactory->CreateFormatConverter(&converter)) || !converter)
    {
        frame->Release();
        decoder->Release();
        wicFactory->Release();
        return nullptr;
    }

    if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom)))
    {
        converter->Release();
        frame->Release();
        decoder->Release();
        wicFactory->Release();
        return nullptr;
    }

    UINT w = 0, h = 0;
    converter->GetSize(&w, &h);
    if (w == 0 || h == 0)
    {
        converter->Release();
        frame->Release();
        decoder->Release();
        wicFactory->Release();
        return nullptr;
    }

    UINT stride = w * 4;
    std::vector<BYTE> pixels(stride * h);
    HRESULT hr = converter->CopyPixels(NULL, stride, (UINT)pixels.size(), pixels.data());
    if (FAILED(hr))
    {
        converter->Release();
        frame->Release();
        decoder->Release();
        wicFactory->Release();
        return nullptr;
    }

    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    D2D1_SIZE_U size = D2D1::SizeU(w, h);
    hr = ctx->CreateBitmap(size, pixels.data(), stride, props, &g_welcomeIcon);

    converter->Release();
    frame->Release();
    decoder->Release();
    wicFactory->Release();

    if (FAILED(hr))
    {
        if (g_welcomeIcon)
        {
            g_welcomeIcon->Release();
            g_welcomeIcon = nullptr;
        }
        return nullptr;
    }

    return g_welcomeIcon;
}

namespace UI {

void DrawWelcomeD2D(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd, const std::wstring &customFontPath,
                    float editorLeft, float editorTop, float editorRight, float editorBottom)
{
    if (!ctx || !dwrite)
        return;

    float width = editorRight - editorLeft;
    float height = editorBottom - editorTop;
    if (width <= 0 || height <= 0)
        return;

    // Fill welcome area with the current app theme background.
    ID2D1SolidColorBrush *bgBrush = nullptr;
    ctx->CreateSolidColorBrush(UI::Theme::ChromeBackground(), &bgBrush);
    if (bgBrush)
    {
        ctx->FillRectangle(D2D1::RectF(editorLeft, editorTop, editorRight, editorBottom), bgBrush);
        bgBrush->Release();
    }

    ID2D1Bitmap *icon = LoadBitmapFromFile(ctx, L"assets/favicon.ico");

    UINT dpi = win32_get_dpi_for_window(hwnd);
    float iconSize = static_cast<float>(win32_dpi_scale(64, dpi));
    float centerX = editorLeft + width * 0.5f;
    float centerY = editorTop + height * 0.34f;
    float iconX = centerX - iconSize * 0.5f;
    float iconY = centerY - iconSize * 0.5f;

    if (icon)
    {
        D2D1_RECT_F iconRect = D2D1::RectF(iconX, iconY, iconX + iconSize, iconY + iconSize);
        ctx->DrawBitmap(icon, iconRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    // Title
    IDWriteTextFormat *titleFmt = nullptr;
    float titleSize = std::clamp(static_cast<float>(win32_dpi_scale(36, dpi)), static_cast<float>(win32_dpi_scale(24, dpi)), static_cast<float>(win32_dpi_scale(48, dpi)));
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, titleSize, L"en-us", &titleFmt);
    if (titleFmt)
    {
        titleFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        titleFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        ID2D1SolidColorBrush *b = nullptr;
        ctx->CreateSolidColorBrush(UI::Theme::PrimaryText(), &b);

        std::wstring title = L"Nebula";
        D2D1_RECT_F titleRect = D2D1::RectF(editorLeft, iconY + iconSize + 18.0f, editorRight, iconY + iconSize + 18.0f + titleSize * 1.2f);
        ctx->DrawTextW(title.c_str(), (UINT32)title.size(), titleFmt, titleRect, b);

        if (b) b->Release();
        titleFmt->Release();
    }

    // Shortcuts - Style VS Code minimaliste
    struct Shortcut {
        std::wstring label;
        std::wstring keys;
    };
    
    std::vector<Shortcut> shortcuts = {
        {L"Open File", L"Ctrl + O"},
        {L"Open Folder", L"Ctrl + K Ctrl + O"},
        {L"New File", L"Ctrl + N"}
    };

    float startY = iconY + iconSize + 20.0f + titleSize * 1.4f + 20.0f;
    float lineHeight = 28.0f;
    float maxLabelWidth = 140.0f;

    // Formats de texte
    IDWriteTextFormat *labelFmt = nullptr;
    IDWriteTextFormat *keyFmt = nullptr;
    float labelSize = static_cast<float>(win32_dpi_scale(13, dpi));
    float keySize = static_cast<float>(win32_dpi_scale(11, dpi));
    
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, labelSize, L"en-us", &labelFmt);
    dwrite->CreateTextFormat(L"JetBrains Mono", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, keySize, L"en-us", &keyFmt);
    
    if (labelFmt && keyFmt)
    {
        labelFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        labelFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        keyFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        keyFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        // Brushes
        ID2D1SolidColorBrush *labelBrush = nullptr;
        ID2D1SolidColorBrush *keyBrush = nullptr;
        ID2D1SolidColorBrush *keyBgBrush = nullptr;

        ctx->CreateSolidColorBrush(UI::Theme::MutedText(), &labelBrush);
        ctx->CreateSolidColorBrush(UI::Theme::PrimaryText(), &keyBrush);
        D2D1_COLOR_F keyBgColor = UI::Theme::GetPalette().explorerToolbarHover;
        keyBgColor.a = 0.8f;
        ctx->CreateSolidColorBrush(keyBgColor, &keyBgBrush);

        for (size_t i = 0; i < shortcuts.size(); ++i)
        {
            float currentY = startY + i * lineHeight;
            float labelX = centerX - maxLabelWidth;
            
            // Label à gauche
            D2D1_RECT_F labelRect = D2D1::RectF(
                labelX, 
                currentY, 
                labelX + maxLabelWidth - 10.0f, 
                currentY + lineHeight
            );
            
            if (labelBrush)
                ctx->DrawTextW(shortcuts[i].label.c_str(), (UINT32)shortcuts[i].label.size(), labelFmt, labelRect, labelBrush);

            // Mesurer la largeur du texte du raccourci
            IDWriteTextLayout *keyLayout = nullptr;
            dwrite->CreateTextLayout(shortcuts[i].keys.c_str(), (UINT32)shortcuts[i].keys.size(), keyFmt, 300.0f, lineHeight, &keyLayout);
            
            DWRITE_TEXT_METRICS keyMetrics;
            if (keyLayout && SUCCEEDED(keyLayout->GetMetrics(&keyMetrics)))
            {
                float keyWidth = keyMetrics.width + 12.0f; // padding
                float keyHeight = 20.0f;
                float keyX = centerX + 10.0f;
                float keyY = currentY + (lineHeight - keyHeight) * 0.5f;

                // Fond arrondi pour le raccourci
                D2D1_ROUNDED_RECT keyBg = D2D1::RoundedRect(
                    D2D1::RectF(keyX, keyY, keyX + keyWidth, keyY + keyHeight),
                    3.0f, 3.0f
                );
                
                if (keyBgBrush)
                    ctx->FillRoundedRectangle(keyBg, keyBgBrush);

                // Texte du raccourci
                D2D1_RECT_F keyRect = D2D1::RectF(
                    keyX + 6.0f,
                    currentY,
                    keyX + keyWidth,
                    currentY + lineHeight
                );
                
                if (keyBrush)
                    ctx->DrawTextW(shortcuts[i].keys.c_str(), (UINT32)shortcuts[i].keys.size(), keyFmt, keyRect, keyBrush);
            }
            
            if (keyLayout) keyLayout->Release();
        }

        if (labelBrush) labelBrush->Release();
        if (keyBrush) keyBrush->Release();
        if (keyBgBrush) keyBgBrush->Release();
    }

    if (labelFmt) labelFmt->Release();
    if (keyFmt) keyFmt->Release();
}

} // namespace UI
