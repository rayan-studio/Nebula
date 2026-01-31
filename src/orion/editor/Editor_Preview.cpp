#include "orion/editor/Editor.h"
#include "core/window/Window.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <thread>
#include <shobjidl.h>
#include <thumbcache.h>
#include <wincodec.h>

namespace
{
    std::wstring ToLower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](wchar_t c)
                       { return (wchar_t)std::towlower(c); });
        return value;
    }

    bool IsImageExtension(const std::wstring &extLower)
    {
        return extLower == L".png" || extLower == L".jpg" || extLower == L".jpeg" ||
               extLower == L".gif" || extLower == L".bmp" || extLower == L".tiff" ||
               extLower == L".tif" || extLower == L".webp" || extLower == L".ico";
    }

    HBITMAP CreateHBitmapFromWicSource(IWICBitmapSource *source, SIZE &sizeOut)
    {
        if (!source)
            return nullptr;

        UINT width = 0;
        UINT height = 0;
        if (FAILED(source->GetSize(&width, &height)) || width == 0 || height == 0)
            return nullptr;

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = static_cast<LONG>(width);
        bmi.bmiHeader.biHeight = -static_cast<LONG>(height);
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void *bits = nullptr;
        HBITMAP hBitmap = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!hBitmap || !bits)
        {
            if (hBitmap)
                DeleteObject(hBitmap);
            return nullptr;
        }

        const UINT stride = width * 4;
        const UINT bufferSize = stride * height;
        if (FAILED(source->CopyPixels(nullptr, stride, bufferSize, reinterpret_cast<BYTE *>(bits))))
        {
            DeleteObject(hBitmap);
            return nullptr;
        }

        sizeOut.cx = static_cast<LONG>(width);
        sizeOut.cy = static_cast<LONG>(height);
        return hBitmap;
    }

    bool LoadPreviewWithWic(const std::wstring &filePath, HBITMAP &outBitmap, SIZE &outSize)
    {
        IWICImagingFactory *wicFactory = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory))))
            return false;

        IWICBitmapDecoder *decoder = nullptr;
        HRESULT hr = wicFactory->CreateDecoderFromFilename(
            filePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
        if (FAILED(hr) || !decoder)
        {
            if (wicFactory)
                wicFactory->Release();
            return false;
        }

        IWICBitmapFrameDecode *frame = nullptr;
        hr = decoder->GetFrame(0, &frame);
        if (FAILED(hr) || !frame)
        {
            decoder->Release();
            wicFactory->Release();
            return false;
        }

        IWICFormatConverter *converter = nullptr;
        hr = wicFactory->CreateFormatConverter(&converter);
        if (FAILED(hr) || !converter)
        {
            frame->Release();
            decoder->Release();
            wicFactory->Release();
            return false;
        }

        hr = converter->Initialize(
            frame,
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom);

        bool success = false;
        if (SUCCEEDED(hr))
        {
            HBITMAP hBitmap = CreateHBitmapFromWicSource(converter, outSize);
            if (hBitmap)
            {
                outBitmap = hBitmap;
                success = true;
            }
        }

        converter->Release();
        frame->Release();
        decoder->Release();
        wicFactory->Release();
        return success;
    }

    bool LoadPreviewWithShellThumbnail(const std::wstring &filePath, HBITMAP &outBitmap, SIZE &outSize)
    {
        IShellItem *item = nullptr;
        HRESULT hr = SHCreateItemFromParsingName(filePath.c_str(), nullptr, IID_PPV_ARGS(&item));
        if (FAILED(hr) || !item)
            return false;

        IShellItemImageFactory *factory = nullptr;
        hr = item->QueryInterface(IID_PPV_ARGS(&factory));
        if (FAILED(hr) || !factory)
        {
            item->Release();
            return false;
        }

        SIZE size = {2048, 2048};
        HBITMAP hBitmap = nullptr;
        hr = factory->GetImage(size, SIIGBF_RESIZETOFIT | SIIGBF_BIGGERSIZEOK, &hBitmap);

        factory->Release();
        item->Release();

        if (FAILED(hr) || !hBitmap)
            return false;

        BITMAP bmp = {};
        if (GetObject(hBitmap, sizeof(BITMAP), &bmp) > 0)
        {
            outSize.cx = bmp.bmWidth;
            outSize.cy = bmp.bmHeight;
        }
        outBitmap = hBitmap;
        return true;
    }

    bool LoadPreviewWithThumbnailCache(const std::wstring &filePath, HBITMAP &outBitmap, SIZE &outSize)
    {
        IShellItem *item = nullptr;
        HRESULT hr = SHCreateItemFromParsingName(filePath.c_str(), nullptr, IID_PPV_ARGS(&item));
        if (FAILED(hr) || !item)
            return false;

        IThumbnailCache *cache = nullptr;
        hr = CoCreateInstance(CLSID_LocalThumbnailCache, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&cache));
        if (FAILED(hr) || !cache)
        {
            item->Release();
            return false;
        }

        ISharedBitmap *shared = nullptr;
        WTS_CACHEFLAGS cacheFlags = (WTS_CACHEFLAGS)0;
        hr = cache->GetThumbnail(item, 2048, WTS_EXTRACT | WTS_SCALETOREQUESTEDSIZE, &shared, &cacheFlags, nullptr);

        cache->Release();
        item->Release();

        if (FAILED(hr) || !shared)
            return false;

        HBITMAP hBitmap = nullptr;
        hr = shared->GetSharedBitmap(&hBitmap);
        shared->Release();
        if (FAILED(hr) || !hBitmap)
            return false;

        BITMAP bmp = {};
        if (GetObject(hBitmap, sizeof(BITMAP), &bmp) > 0)
        {
            outSize.cx = bmp.bmWidth;
            outSize.cy = bmp.bmHeight;
        }
        outBitmap = hBitmap;
        return true;
    }
}

namespace Orion
{
    void Editor::ResetPreview()
    {
        isPreview_ = false;
        previewMessage_.clear();

        if (previewD2DBitmap_)
        {
            previewD2DBitmap_->Release();
            previewD2DBitmap_ = nullptr;
        }

        if (previewBitmap_)
        {
            DeleteObject(previewBitmap_);
            previewBitmap_ = nullptr;
        }

        previewBitmapSize_ = {0, 0};
    }

    void Editor::LoadPreviewAsync(HWND hwnd, const std::wstring &filePath, int tabIndex)
    {
        ResetPreview();
        isPreview_ = true;
        previewMessage_ = L"Loading preview...";

        state_.filePath = filePath;
        state_.lines.clear();
        state_.lines.push_back(L"");
        state_.encoding = L"Preview";
        state_.caret = {0, 0};
        state_.scrollOffsetX = 0.0f;
        state_.scrollOffsetY = 0.0f;
        state_.hasSelection = false;
        searchBox_.Hide();
        ClearDiagnostics();
        ClearDirty();

        std::wstring filePathCopy = filePath;

        std::thread([hwnd, tabIndex, filePathCopy]()
                    {
            auto* result = new Window::EditorFileLoadResult();
            result->tabIndex = tabIndex;
            result->filePath = filePathCopy;
            result->isPreview = true;

            HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            bool comInitialized = SUCCEEDED(hrCo);

            std::wstring extension = ToLower(std::filesystem::path(filePathCopy).extension().wstring());
            HBITMAP previewBitmap = nullptr;
            SIZE previewSize = {0, 0};
            bool loaded = false;

            if (IsImageExtension(extension))
                loaded = LoadPreviewWithWic(filePathCopy, previewBitmap, previewSize);

            if (!loaded)
                loaded = LoadPreviewWithShellThumbnail(filePathCopy, previewBitmap, previewSize);
            if (!loaded)
                loaded = LoadPreviewWithThumbnailCache(filePathCopy, previewBitmap, previewSize);

            result->previewBitmap = previewBitmap;
            result->previewSize = previewSize;
            if (!loaded)
                result->previewMessage = L"Preview not available.";

            if (comInitialized)
                CoUninitialize();

            PostMessageW(hwnd, WM_EDITOR_FILE_LOADED, 0, (LPARAM)result); })
            .detach();
    }

    void Editor::ApplyLoadedPreview(std::wstring filePath,
                                    HBITMAP previewBitmap,
                                    SIZE previewSize,
                                    std::wstring previewMessage)
    {
        ResetPreview();
        isPreview_ = true;
        state_.filePath = std::move(filePath);
        state_.encoding = L"Preview";
        state_.lines.clear();
        state_.lines.push_back(L"");
        state_.caret = {0, 0};
        state_.hasSelection = false;
        state_.scrollOffsetX = 0.0f;
        state_.scrollOffsetY = 0.0f;
        previewBitmap_ = previewBitmap;
        previewBitmapSize_ = previewSize;
        previewMessage_ = std::move(previewMessage);
    }

    void Editor::DrawPreview(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite)
    {
        if (!ctx || !dwrite)
            return;

        if (previewBitmap_ && !previewD2DBitmap_)
        {
            IWICImagingFactory *wicFactory = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory))) && wicFactory)
            {
                IWICBitmap *wicBitmap = nullptr;
                if (SUCCEEDED(wicFactory->CreateBitmapFromHBITMAP(previewBitmap_, nullptr, WICBitmapUseAlpha, &wicBitmap)) && wicBitmap)
                {
                    ctx->CreateBitmapFromWicBitmap(wicBitmap, nullptr, &previewD2DBitmap_);
                    wicBitmap->Release();
                }
                wicFactory->Release();
            }

            DeleteObject(previewBitmap_);
            previewBitmap_ = nullptr;
        }

        D2D1_RECT_F contentRect = D2D1::RectF(
            state_.leftEdge,
            state_.topEdge,
            state_.rightEdge,
            state_.bottomEdge);

        if (previewD2DBitmap_)
        {
            D2D1_SIZE_F bitmapSize = previewD2DBitmap_->GetSize();
            float availableW = contentRect.right - contentRect.left;
            float availableH = contentRect.bottom - contentRect.top;

            if (bitmapSize.width > 0.0f && bitmapSize.height > 0.0f)
            {
                float scale = (std::min)(availableW / bitmapSize.width, availableH / bitmapSize.height);
                scale = (std::min)(scale, 1.0f);

                float drawW = bitmapSize.width * scale;
                float drawH = bitmapSize.height * scale;
                float drawLeft = contentRect.left + (availableW - drawW) * 0.5f;
                float drawTop = contentRect.top + (availableH - drawH) * 0.5f;

                D2D1_RECT_F drawRect = D2D1::RectF(drawLeft, drawTop, drawLeft + drawW, drawTop + drawH);
                ctx->DrawBitmap(previewD2DBitmap_, drawRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                return;
            }
        }

        std::wstring message = previewMessage_.empty() ? L"Preview not available." : previewMessage_;

        ID2D1SolidColorBrush *textBrush = nullptr;
        ctx->CreateSolidColorBrush(theme_.text, &textBrush);

        IDWriteTextFormat *format = nullptr;
        if (SUCCEEDED(dwrite->CreateTextFormat(
                L"Segoe UI",
                nullptr,
                DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                14.0f,
                L"en-us",
                &format)))
        {
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        }

        if (textBrush && format)
        {
            ctx->DrawTextW(message.c_str(),
                           static_cast<UINT32>(message.size()),
                           format,
                           contentRect,
                           textBrush);
        }

        if (format)
            format->Release();
        if (textBrush)
            textBrush->Release();
    }
}
