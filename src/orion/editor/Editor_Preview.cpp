#include "orion/editor/Editor.h"
#include "core/window/Window.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <shobjidl.h>
#include <thumbcache.h>
#include <wincodec.h>
#include <dxgiformat.h>
#include "nanosvg.h"
#include "nanosvgrast.h"

namespace
{
    std::wstring ToLower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](wchar_t c)
                       { return (wchar_t)std::towlower(c); });
        return value;
    }

    std::string WideToUtf8(const std::wstring &w)
    {
        if (w.empty())
            return std::string();
        int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (len <= 0)
            return std::string();
        std::string out;
        out.resize((size_t)len);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
        if (!out.empty() && out.back() == '\0')
            out.pop_back();
        return out;
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
    using MarkdownSpan = Orion::MarkdownSpan;
    using MarkdownBlock = Orion::MarkdownBlock;
    using MarkdownSvgText = Orion::MarkdownSvgText;
    using MarkdownInlineImage = Orion::MarkdownInlineImage;
    using MarkdownImageAlign = Orion::MarkdownImageAlign;

    std::wstring Trim(const std::wstring &s)
    {
        size_t a = s.find_first_not_of(L" \t\r\n");
        size_t b = s.find_last_not_of(L" \t\r\n");
        if (a == std::wstring::npos || b == std::wstring::npos)
            return L"";
        return s.substr(a, b - a + 1);
    }


    std::wstring StripMarkdownLinks(const std::wstring &line)
    {
        std::wstring out;
        out.reserve(line.size());
        size_t i = 0;
        while (i < line.size())
        {
            if (line[i] == L'[')
            {
                size_t close = line.find(L']', i + 1);
                size_t openParen = (close == std::wstring::npos) ? std::wstring::npos : line.find(L'(', close + 1);
                size_t closeParen = (openParen == std::wstring::npos) ? std::wstring::npos : line.find(L')', openParen + 1);
                if (close != std::wstring::npos && openParen == close + 1 && closeParen != std::wstring::npos)
                {
                    out.append(line.substr(i + 1, close - i - 1));
                    i = closeParen + 1;
                    continue;
                }
            }
            out.push_back(line[i]);
            i++;
        }
        return out;
    }

    bool IsHorizontalRule(const std::wstring &trimmed)
    {
        if (trimmed.size() < 3)
            return false;
        wchar_t c = trimmed[0];
        if (c != L'-' && c != L'*')
            return false;
        for (wchar_t ch : trimmed)
        {
            if (ch != c)
                return false;
        }
        return true;
    }

    bool ParseMarkdownImageLine(const std::wstring &line, std::wstring &outPath, float &outW, float &outH, MarkdownImageAlign &outAlign)
    {
        outPath.clear();
        outW = 0.0f;
        outH = 0.0f;
        outAlign = MarkdownImageAlign::Left;
        std::wstring t = Trim(line);
        if (t.empty())
            return false;

        if (t.rfind(L"![", 0) == 0)
        {
            size_t open = t.find(L"](");
            size_t close = t.find(L")", open == std::wstring::npos ? 0 : open + 2);
            if (open != std::wstring::npos && close != std::wstring::npos && close > open + 2)
            {
                outPath = t.substr(open + 2, close - (open + 2));
                return !outPath.empty();
            }
        }

        if (t.find(L"<img") != std::wstring::npos)
        {
            auto findAttr = [&](const std::wstring &name) -> std::wstring
            {
                size_t pos = t.find(name);
                if (pos == std::wstring::npos)
                    return L"";
                pos = t.find(L'=', pos + name.size());
                if (pos == std::wstring::npos)
                    return L"";
                pos++;
                while (pos < t.size() && iswspace(t[pos]))
                    pos++;
                if (pos >= t.size())
                    return L"";
                wchar_t quote = t[pos];
                if (quote == L'"' || quote == L'\'')
                {
                    size_t end = t.find(quote, pos + 1);
                    if (end != std::wstring::npos)
                        return t.substr(pos + 1, end - pos - 1);
                }
                else
                {
                    size_t end = t.find_first_of(L" \t\r\n>", pos);
                    return t.substr(pos, end == std::wstring::npos ? t.size() - pos : end - pos);
                }
                return L"";
            };

            outPath = findAttr(L"src");
            std::wstring w = findAttr(L"width");
            std::wstring h = findAttr(L"height");
            std::wstring align = ToLower(findAttr(L"align"));
            if (align == L"right")
                outAlign = MarkdownImageAlign::Right;
            else if (align == L"center")
                outAlign = MarkdownImageAlign::Center;
            else if (align == L"left")
                outAlign = MarkdownImageAlign::Left;
            if (!w.empty())
                outW = (float)_wtoi(w.c_str());
            if (!h.empty())
                outH = (float)_wtoi(h.c_str());
            return !outPath.empty();
        }

        return false;
    }


    bool ExtractInlineMarkdownImage(std::wstring &line, std::wstring &outPath, float &outW, float &outH, MarkdownImageAlign &outAlign)
    {
        outPath.clear();
        outW = 0.0f;
        outH = 0.0f;
        outAlign = MarkdownImageAlign::Left;

        size_t htmlPos = line.find(L"<img");
        if (htmlPos != std::wstring::npos)
        {
            std::wstring chunk = line.substr(htmlPos);
            if (ParseMarkdownImageLine(chunk, outPath, outW, outH, outAlign))
            {
                line.erase(htmlPos, line.size() - htmlPos);
                return !outPath.empty();
            }
        }

        size_t imgPos = line.find(L"![");
        if (imgPos == std::wstring::npos)
        {
            size_t badgePos = line.find(L"[![");
            if (badgePos != std::wstring::npos)
                imgPos = badgePos + 1;
        }

        if (imgPos == std::wstring::npos)
            return false;

        size_t open = line.find(L"](", imgPos);
        size_t close = (open == std::wstring::npos) ? std::wstring::npos : line.find(L")", open + 2);
        if (open == std::wstring::npos || close == std::wstring::npos || close <= open + 2)
            return false;

        outPath = line.substr(open + 2, close - (open + 2));

        size_t removeStart = imgPos;
        size_t removeEnd = close + 1;
        if (imgPos > 0 && line[imgPos - 1] == L'[')
        {
            size_t bracketClose = line.find(L']', close + 1);
            size_t parenOpen = (bracketClose == std::wstring::npos) ? std::wstring::npos : line.find(L'(', bracketClose + 1);
            size_t parenClose = (parenOpen == std::wstring::npos) ? std::wstring::npos : line.find(L')', parenOpen + 1);
            if (bracketClose != std::wstring::npos && parenOpen == bracketClose + 1 && parenClose != std::wstring::npos)
            {
                removeStart = imgPos - 1;
                removeEnd = parenClose + 1;
            }
        }

        line.erase(removeStart, removeEnd - removeStart);
        return !outPath.empty();
    }

    std::wstring ResolveImagePath(const std::wstring &filePath, const std::wstring &imgPath)
    {
        if (imgPath.empty())
            return L"";
        std::filesystem::path p(imgPath);
        if (p.is_absolute())
            return p.wstring();
        if (filePath.empty())
            return p.wstring();
        std::filesystem::path base(filePath);
        base = base.parent_path();
        return (base / p).wstring();
    }


    ID2D1Bitmap *CreateBitmapFromRGBA(ID2D1RenderTarget *ctx, const unsigned char *data, int w, int h, float dpi)
    {
        if (!ctx || !data || w <= 0 || h <= 0)
            return nullptr;

        std::vector<unsigned char> premultiplied(w * h * 4);
        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                int idx = (y * w + x) * 4;
                unsigned char r = data[idx + 0];
                unsigned char g = data[idx + 1];
                unsigned char b = data[idx + 2];
                unsigned char a = data[idx + 3];
                float af = a / 255.0f;

                premultiplied[idx + 0] = (unsigned char)(r * af + 0.5f);
                premultiplied[idx + 1] = (unsigned char)(g * af + 0.5f);
                premultiplied[idx + 2] = (unsigned char)(b * af + 0.5f);
                premultiplied[idx + 3] = a;
            }
        }

        D2D1_SIZE_U size = D2D1::SizeU(w, h);
        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);

        ID2D1Bitmap *bmp = nullptr;
        ctx->CreateBitmap(size, premultiplied.data(), w * 4, props, &bmp);
        return bmp;
    }

    ID2D1Bitmap *LoadSvgToD2DBitmap(ID2D1RenderTarget *ctx, const std::wstring &path, float desiredW, float desiredH)
    {
        if (!ctx || path.empty())
            return nullptr;

        std::string u8 = WideToUtf8(path);
        if (u8.empty())
            return nullptr;

        NSVGimage *image = nsvgParseFromFile(u8.c_str(), "px", 96.0f);
        if (!image)
            return nullptr;

        float targetW = (desiredW > 0.0f) ? desiredW : image->width;
        float targetH = (desiredH > 0.0f) ? desiredH : image->height;
        if (targetW <= 0.0f || targetH <= 0.0f)
        {
            nsvgDelete(image);
            return nullptr;
        }

        const int oversample = 2;
        int scaledW = (int)std::ceil(targetW * (float)oversample);
        int scaledH = (int)std::ceil(targetH * (float)oversample);
        if (scaledW <= 0 || scaledH <= 0)
        {
            nsvgDelete(image);
            return nullptr;
        }

        std::vector<unsigned char> buffer((size_t)scaledW * (size_t)scaledH * 4);
        NSVGrasterizer *rast = nsvgCreateRasterizer();

        float scaleW = (float)scaledW / image->width;
        float scaleH = (float)scaledH / image->height;
        float scale = (scaleW < scaleH) ? scaleW : scaleH;
        float tx = (scaledW - image->width * scale) * 0.5f;
        float ty = (scaledH - image->height * scale) * 0.5f;

        nsvgRasterize(rast, image, tx, ty, scale, buffer.data(), scaledW, scaledH, scaledW * 4);

        ID2D1Bitmap *bmp = CreateBitmapFromRGBA(ctx, buffer.data(), scaledW, scaledH, 96.0f * (float)oversample);

        nsvgDelete(image);
        nsvgDeleteRasterizer(rast);
        return bmp;
    }


    bool ReadFileUtf8(const std::wstring &path, std::string &out)
    {
        std::string u8 = WideToUtf8(path);
        if (u8.empty())
            return false;
        std::ifstream file(u8, std::ios::binary);
        if (!file)
            return false;
        out.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        return true;
    }

    std::wstring Utf8ToWide(const std::string &s)
    {
        if (s.empty())
            return std::wstring();
        int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
        if (len <= 0)
            return std::wstring();
        std::wstring out;
        out.resize((size_t)len);
        MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), len);
        return out;
    }

    std::string TrimAscii(const std::string &s)
    {
        size_t a = 0;
        while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
            a++;
        size_t b = s.size();
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n'))
            b--;
        return s.substr(a, b - a);
    }

    bool ParseSvgFloat(const std::string &s, float &out)
    {
        if (s.empty())
            return false;
        size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '	'))
            i++;
        if (i >= s.size())
            return false;
        size_t j = i;
        while (j < s.size() && (isdigit((unsigned char)s[j]) || s[j] == '.' || s[j] == '-' || s[j] == '+'))
            j++;
        if (j == i)
            return false;
        try
        {
            out = std::stof(s.substr(i, j - i));
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    std::string ExtractAttr(const std::string &tag, const char *name)
    {
        std::string needle = std::string(name) + "=";
        size_t pos = tag.find(needle);
        if (pos == std::string::npos)
            return std::string();
        pos += needle.size();
        if (pos >= tag.size())
            return std::string();
        char quote = tag[pos];
        if (quote == '"' || quote == '\'')
        {
            size_t end = tag.find(quote, pos + 1);
            if (end != std::string::npos)
                return tag.substr(pos + 1, end - pos - 1);
        }
        else
        {
            size_t end = tag.find_first_of(" \t\r\n>", pos);
            return tag.substr(pos, end == std::string::npos ? tag.size() - pos : end - pos);
        }
        return std::string();
    }


    bool ParseSvgColor(const std::string &s, D2D1_COLOR_F &out)
    {
        if (s.empty())
            return false;
        if (s == "white")
        {
            out = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
            return true;
        }
        if (s == "black")
        {
            out = D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f);
            return true;
        }
        if (s[0] != '#')
            return false;
        std::string hex = s.substr(1);
        if (hex.size() == 3)
        {
            std::string expanded;
            expanded.push_back(hex[0]); expanded.push_back(hex[0]);
            expanded.push_back(hex[1]); expanded.push_back(hex[1]);
            expanded.push_back(hex[2]); expanded.push_back(hex[2]);
            hex = expanded;
        }
        if (hex.size() != 6)
            return false;
        unsigned int value = 0;
        try
        {
            value = std::stoul(hex, nullptr, 16);
        }
        catch (...)
        {
            return false;
        }
        out = Orion::ColorFromHex(value, 1.0f, false);
        return true;
    }

    bool ParseSvgTextOverlays(const std::wstring &path, std::vector<MarkdownSvgText> &out, float &svgW, float &svgH)
    {
        out.clear();
        svgW = 0.0f;
        svgH = 0.0f;
        std::string data;
        if (!ReadFileUtf8(path, data))
            return false;

        size_t svgPos = data.find("<svg");
        if (svgPos != std::string::npos)
        {
            size_t end = data.find('>', svgPos);
            if (end != std::string::npos)
            {
                std::string tag = data.substr(svgPos, end - svgPos + 1);
                std::string width = ExtractAttr(tag, "width");
                std::string height = ExtractAttr(tag, "height");
                ParseSvgFloat(width, svgW);
                ParseSvgFloat(height, svgH);
                std::string viewBox = ExtractAttr(tag, "viewBox");
                if (!viewBox.empty())
                {
                    float x=0, y=0, w=0, h=0;
                    if (sscanf_s(viewBox.c_str(), "%f %f %f %f", &x, &y, &w, &h) == 4)
                    {
                        if (w > 0) svgW = w;
                        if (h > 0) svgH = h;
                    }
                }
            }
        }

        size_t pos = 0;
        while (true)
        {
            size_t tpos = data.find("<text", pos);
            if (tpos == std::string::npos)
                break;
            size_t tagEnd = data.find('>', tpos);
            if (tagEnd == std::string::npos)
                break;
            size_t close = data.find("</text>", tagEnd);
            if (close == std::string::npos)
                break;

            std::string tag = data.substr(tpos, tagEnd - tpos + 1);
            std::string textContent = data.substr(tagEnd + 1, close - tagEnd - 1);
            textContent = TrimAscii(textContent);
            if (!textContent.empty())
            {
                // remove newlines inside text
                std::string cleaned;
                cleaned.reserve(textContent.size());
                for (char ch : textContent)
                {
                    if (ch == '\r' || ch == '\n' || ch == '\t')
                        continue;
                    cleaned.push_back(ch);
                }
                textContent = TrimAscii(cleaned);
            }

            MarkdownSvgText txt;
            txt.text = Utf8ToWide(textContent);

            std::string xStr = ExtractAttr(tag, "x");
            std::string yStr = ExtractAttr(tag, "y");
            std::string sizeStr = ExtractAttr(tag, "font-size");
            std::string weightStr = ExtractAttr(tag, "font-weight");
            std::string fillStr = ExtractAttr(tag, "fill");
            std::string anchorStr = ExtractAttr(tag, "text-anchor");

            ParseSvgFloat(xStr, txt.x);
            ParseSvgFloat(yStr, txt.y);
            if (ParseSvgFloat(sizeStr, txt.fontSize) == false)
                txt.fontSize = 12.0f;

            if (!weightStr.empty())
            {
                if (weightStr == "bold")
                {
                    txt.weight = DWRITE_FONT_WEIGHT_BOLD;
                }
                else
                {
                    try
                    {
                        int w = std::stoi(weightStr);
                        if (w < 100) w = 100;
                        if (w > 900) w = 900;
                        txt.weight = (DWRITE_FONT_WEIGHT)w;
                    }
                    catch (...)
                    {
                        txt.weight = DWRITE_FONT_WEIGHT_NORMAL;
                    }
                }
            }

            D2D1_COLOR_F col;
            if (ParseSvgColor(fillStr, col))
                txt.color = col;

            if (!anchorStr.empty())
            {
                if (anchorStr == "middle")
                    txt.anchor = 1;
                else if (anchorStr == "end")
                    txt.anchor = 2;
            }

            if (!txt.text.empty())
                out.push_back(txt);

            pos = close + 7;
        }

        return !out.empty();
    }

    ID2D1Bitmap *LoadImageToD2DBitmap(ID2D1RenderTarget *ctx, const std::wstring &path, float desiredW, float desiredH)
    {
        if (!ctx || path.empty())
            return nullptr;

        std::wstring ext = ToLower(std::filesystem::path(path).extension().wstring());
        if (ext == L".svg")
        {
            if (ID2D1Bitmap *svgBmp = LoadSvgToD2DBitmap(ctx, path, desiredW, desiredH))
                return svgBmp;
        }

        IWICImagingFactory *wicFactory = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory))))
        {
            IWICBitmapDecoder *decoder = nullptr;
            HRESULT hr = wicFactory->CreateDecoderFromFilename(
                path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
            if (SUCCEEDED(hr) && decoder)
            {
                IWICBitmapFrameDecode *frame = nullptr;
                hr = decoder->GetFrame(0, &frame);
                if (SUCCEEDED(hr) && frame)
                {
                    IWICFormatConverter *converter = nullptr;
                    hr = wicFactory->CreateFormatConverter(&converter);
                    if (SUCCEEDED(hr) && converter)
                    {
                        hr = converter->Initialize(
                            frame,
                            GUID_WICPixelFormat32bppPBGRA,
                            WICBitmapDitherTypeNone,
                            nullptr,
                            0.0,
                            WICBitmapPaletteTypeCustom);

                        ID2D1Bitmap *bmp = nullptr;
                        if (SUCCEEDED(hr))
                            ctx->CreateBitmapFromWicBitmap(converter, nullptr, &bmp);

                        converter->Release();
                        frame->Release();
                        decoder->Release();
                        wicFactory->Release();
                        if (bmp)
                            return bmp;
                    }
                    else
                    {
                        frame->Release();
                        decoder->Release();
                        wicFactory->Release();
                    }
                }
                else
                {
                    decoder->Release();
                    wicFactory->Release();
                }
            }
            else
            {
                wicFactory->Release();
            }
        }

        HBITMAP hBitmap = nullptr;
        SIZE size = {0, 0};
        bool loaded = LoadPreviewWithShellThumbnail(path, hBitmap, size);
        if (!loaded)
            loaded = LoadPreviewWithThumbnailCache(path, hBitmap, size);
        if (!loaded || !hBitmap)
            return nullptr;

        ID2D1Bitmap *bmp = nullptr;
        IWICImagingFactory *wicFactory2 = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory2))))
        {
            IWICBitmap *wicBitmap = nullptr;
            if (SUCCEEDED(wicFactory2->CreateBitmapFromHBITMAP(hBitmap, nullptr, WICBitmapUseAlpha, &wicBitmap)) && wicBitmap)
            {
                ctx->CreateBitmapFromWicBitmap(wicBitmap, nullptr, &bmp);
                wicBitmap->Release();
            }
            wicFactory2->Release();
        }
        DeleteObject(hBitmap);
        return bmp;
    }

    void AddSpan(std::vector<MarkdownSpan> &spans, UINT32 start, UINT32 length,
                 float fontSize, DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style, bool code)
    {
        if (length == 0)
            return;
        MarkdownSpan s;
        s.start = start;
        s.length = length;
        s.fontSize = fontSize;
        s.weight = weight;
        s.style = style;
        s.code = code;
        spans.push_back(s);
    }

    void ParseInline(const std::wstring &line, std::wstring &out, std::vector<MarkdownSpan> &spans,
                     float baseSize, bool heading)
    {
        size_t i = 0;
        while (i < line.size())
        {
            if (line[i] == L'`')
            {
                size_t end = line.find(L'`', i + 1);
                if (end != std::wstring::npos)
                {
                    UINT32 start = (UINT32)out.size();
                    out.append(line.substr(i + 1, end - i - 1));
                    AddSpan(spans, start, (UINT32)(out.size() - start),
                            baseSize, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, true);
                    i = end + 1;
                    continue;
                }
            }
            if (i + 1 < line.size() && ((line[i] == L'*' && line[i + 1] == L'*') || (line[i] == L'_' && line[i + 1] == L'_')))
            {
                std::wstring token = (line[i] == L'*') ? L"**" : L"__";
                size_t end = line.find(token, i + 2);
                if (end != std::wstring::npos)
                {
                    UINT32 start = (UINT32)out.size();
                    out.append(line.substr(i + 2, end - i - 2));
                    AddSpan(spans, start, (UINT32)(out.size() - start),
                            baseSize, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, false);
                    i = end + 2;
                    continue;
                }
            }
            if (line[i] == L'*' || line[i] == L'_')
            {
                wchar_t token = line[i];
                size_t end = line.find(token, i + 1);
                if (end != std::wstring::npos)
                {
                    UINT32 start = (UINT32)out.size();
                    out.append(line.substr(i + 1, end - i - 1));
                    AddSpan(spans, start, (UINT32)(out.size() - start),
                            baseSize, heading ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_ITALIC, false);
                    i = end + 1;
                    continue;
                }
            }

            out.push_back(line[i]);
            i++;
        }
    }

    std::vector<std::wstring> SplitTableRow(const std::wstring &line)
    {
        std::vector<std::wstring> cells;
        std::wstring cur;
        for (wchar_t ch : line)
        {
            if (ch == L'|')
            {
                cells.push_back(Trim(cur));
                cur.clear();
            }
            else
            {
                cur.push_back(ch);
            }
        }
        cells.push_back(Trim(cur));
        // Drop leading/trailing empty cells caused by leading/trailing pipes
        if (!cells.empty() && cells.front().empty())
            cells.erase(cells.begin());
        if (!cells.empty() && cells.back().empty())
            cells.pop_back();
        return cells;
    }

    void BuildMarkdownBlocks(const std::vector<std::wstring> &lines, const std::wstring &filePath,
                             std::vector<MarkdownBlock> &outBlocks)
    {
        outBlocks.clear();
        bool inCodeBlock = false;

        MarkdownBlock current;
        current.type = MarkdownBlock::Type::Text;

        auto flushText = [&]()
        {
            if (!current.text.empty())
            {
                outBlocks.push_back(current);
                current = MarkdownBlock();
                current.type = MarkdownBlock::Type::Text;
            }
        };

        for (size_t li = 0; li < lines.size(); ++li)
        {
            std::wstring raw = lines[li];
            std::wstring trimmed = Trim(raw);

            std::wstring imgPath;
            float imgW = 0.0f;
            float imgH = 0.0f;
            MarkdownImageAlign imgAlign = MarkdownImageAlign::Left;
            if (ParseMarkdownImageLine(raw, imgPath, imgW, imgH, imgAlign))
            {
                flushText();
                MarkdownBlock img;
                img.type = MarkdownBlock::Type::Image;
                img.imagePath = ResolveImagePath(filePath, imgPath);
                img.imageWidth = imgW;
                img.imageHeight = imgH;
                img.imageAlign = imgAlign;
                outBlocks.push_back(std::move(img));
                continue;
            }

            std::vector<MarkdownInlineImage> inlineImages;
            while (ExtractInlineMarkdownImage(raw, imgPath, imgW, imgH, imgAlign))
            {
                MarkdownInlineImage img;
                img.path = ResolveImagePath(filePath, imgPath);
                img.width = imgW;
                img.height = imgH;
                img.align = imgAlign;
                inlineImages.push_back(std::move(img));
                raw = Trim(raw);
                if (raw.empty())
                    break;
            }
            if (raw.empty() && !inlineImages.empty())
                continue;

            trimmed = Trim(raw);

            if (trimmed.rfind(L"```", 0) == 0)
            {
                inCodeBlock = !inCodeBlock;
                continue;
            }

            if (!inCodeBlock && IsHorizontalRule(trimmed))
            {
                flushText();
                MarkdownBlock rule;
                rule.type = MarkdownBlock::Type::Rule;
                outBlocks.push_back(rule);
                continue;
            }

            if (!inCodeBlock && raw.find(L'|') != std::wstring::npos && li + 1 < lines.size())
            {
                std::wstring nextTrim = Trim(lines[li + 1]);
                bool isSep = !nextTrim.empty();
                for (wchar_t ch : nextTrim)
                {
                    if (ch != L'|' && ch != L'-' && ch != L':' && !iswspace(ch))
                    {
                        isSep = false;
                        break;
                    }
                }
                if (isSep)
                {
                    flushText();
                    MarkdownBlock table;
                    table.type = MarkdownBlock::Type::Table;
                    std::vector<std::vector<std::wstring>> rows;
                    // header row
                    rows.push_back(SplitTableRow(raw));
                    // skip separator line
                    li += 1;
                    // data rows
                    for (size_t r = li + 1; r < lines.size(); ++r)
                    {
                        std::wstring line = lines[r];
                        if (Trim(line).empty())
                            break;
                        if (line.find(L'|') == std::wstring::npos)
                            break;
                        rows.push_back(SplitTableRow(line));
                        li = r;
                    }
                    table.tableRows = std::move(rows);
                    outBlocks.push_back(std::move(table));
                    continue;
                }
            }

            float baseSize = 14.0f;
            bool isHeading = false;
            if (!inCodeBlock)
            {
                size_t hashCount = 0;
                while (hashCount < raw.size() && raw[hashCount] == L'#')
                    hashCount++;
                if (hashCount > 0 && hashCount <= 6 && hashCount < raw.size() && raw[hashCount] == L' ')
                {
                    isHeading = true;
                    raw = raw.substr(hashCount + 1);
                    switch (hashCount)
                    {
                    case 1: baseSize = 24.0f; break;
                    case 2: baseSize = 20.0f; break;
                    case 3: baseSize = 18.0f; break;
                    case 4: baseSize = 16.0f; break;
                    case 5: baseSize = 14.5f; break;
                    case 6: baseSize = 13.5f; break;
                    default: break;
                    }
                }
            }

            if (!inCodeBlock)
            {
                raw = StripMarkdownLinks(raw);
                std::wstring t = Trim(raw);
                if (!t.empty() && t[0] == L'>')
                {
                    size_t pos = t.find_first_not_of(L"> ");
                    std::wstring quoteText = (pos == std::wstring::npos ? L"" : t.substr(pos));
                    flushText();
                    MarkdownBlock quote;
                    quote.type = MarkdownBlock::Type::Text;
                    quote.isQuote = true;
                    ParseInline(quoteText, quote.text, quote.spans, baseSize, isHeading);
                    AddSpan(quote.spans, 0, (UINT32)quote.text.size(),
                            baseSize, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_ITALIC, false);
                    outBlocks.push_back(std::move(quote));
                    continue;
                }

                if (t.rfind(L"- ", 0) == 0 || t.rfind(L"* ", 0) == 0 || t.rfind(L"+ ", 0) == 0)
                {
                    raw = L"\u2022 " + t.substr(2);
                }
            }

            if (inCodeBlock)
            {
                UINT32 start = (UINT32)current.text.size();
                current.text.append(raw);
                AddSpan(current.spans, start, (UINT32)raw.size(),
                        12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, true);
                current.text.push_back(L'\n');
            }
            else
            {
                if (!inlineImages.empty())
                {
                    flushText();
                    MarkdownBlock lineBlock;
                    lineBlock.type = MarkdownBlock::Type::Text;
                    UINT32 start = 0;
                    ParseInline(raw, lineBlock.text, lineBlock.spans, baseSize, isHeading);
                    if (isHeading)
                    {
                        AddSpan(lineBlock.spans, start, (UINT32)lineBlock.text.size(),
                                baseSize, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, false);
                    }
                    lineBlock.inlineImages = std::move(inlineImages);
                    outBlocks.push_back(std::move(lineBlock));
                }
                else
                {
                    UINT32 start = (UINT32)current.text.size();
                    ParseInline(raw, current.text, current.spans, baseSize, isHeading);
                    if (isHeading)
                    {
                        AddSpan(current.spans, start, (UINT32)(current.text.size() - start),
                                baseSize, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, false);
                    }
                    current.text.push_back(L'\n');
                }
            }
        }

        flushText();
    }
}

namespace Orion
{
    void Editor::ResetPreview()
    {
        isPreview_ = false;
        previewMode_ = PreviewMode::None;
        previewMessage_.clear();
        ResetMarkdownPreviewLayout();

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

    void Editor::ResetMarkdownPreviewLayout()
    {
        for (auto *layout : previewMarkdownLayouts_)
        {
            if (layout)
                layout->Release();
        }
        previewMarkdownLayouts_.clear();
        previewMarkdownMetrics_.clear();
        if (previewMarkdownLayout_)
        {
            previewMarkdownLayout_->Release();
            previewMarkdownLayout_ = nullptr;
        }
        for (auto &pair : previewImageCache_)
        {
            if (pair.second)
                pair.second->Release();
        }
        previewImageCache_.clear();
        previewSvgTextCache_.clear();
        previewSvgSizeCache_.clear();
        previewMarkdownText_.clear();
        previewMarkdownSpans_.clear();
        previewMarkdownBlocks_.clear();
        previewMarkdownLayoutWidth_ = 0.0f;
        previewMarkdownLayoutHeight_ = 0.0f;
    }

    void Editor::LoadPreviewAsync(HWND hwnd, const std::wstring &filePath, int tabIndex)
    {
        ResetPreview();
        isPreview_ = true;
        previewMode_ = PreviewMode::Image;
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
        previewMode_ = PreviewMode::Image;
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

    void Editor::SetMarkdownPreviewEnabled(bool enabled)
    {
        if (enabled)
        {
            ResetPreview();
            isPreview_ = true;
            previewMode_ = PreviewMode::Markdown;
            BuildMarkdownBlocks(state_.lines, state_.filePath, previewMarkdownBlocks_);
            previewMessage_.clear();
            state_.scrollOffsetY = 0.0f;
        }
        else
        {
            if (previewMode_ == PreviewMode::Markdown)
            {
                ResetMarkdownPreviewLayout();
                isPreview_ = false;
                previewMode_ = PreviewMode::None;
                state_.scrollOffsetY = 0.0f;
            }
        }
    }

    bool Editor::IsMarkdownPreviewEnabled() const
    {
        return isPreview_ && previewMode_ == PreviewMode::Markdown;
    }

    void Editor::DrawPreview(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite)
    {
        if (!ctx || !dwrite)
            return;

        if (previewMode_ == PreviewMode::Markdown)
        {
            D2D1_RECT_F contentRect = D2D1::RectF(
                state_.leftEdge + 16.0f,
                state_.topEdge + 12.0f,
                state_.rightEdge - 16.0f,
                state_.bottomEdge - 12.0f);

            float availableW = contentRect.right - contentRect.left;
            float availableH = contentRect.bottom - contentRect.top;
            if (availableW < 10.0f || availableH < 10.0f)
                return;

            if (previewMarkdownBlocks_.empty())
                BuildMarkdownBlocks(state_.lines, state_.filePath, previewMarkdownBlocks_);

            bool rebuildLayouts = previewMarkdownLayouts_.size() != previewMarkdownBlocks_.size() ||
                                  std::fabs(previewMarkdownLayoutWidth_ - availableW) > 1.0f;
            if (rebuildLayouts)
            {
                for (auto *layout : previewMarkdownLayouts_)
                {
                    if (layout)
                        layout->Release();
                }
                previewMarkdownLayouts_.assign(previewMarkdownBlocks_.size(), nullptr);
                previewMarkdownMetrics_.assign(previewMarkdownBlocks_.size(), DWRITE_TEXT_METRICS{});

                for (size_t i = 0; i < previewMarkdownBlocks_.size(); ++i)
                {
                    const auto &blk = previewMarkdownBlocks_[i];
                    if (blk.type != MarkdownBlock::Type::Text)
                        continue;

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
                        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                        format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);

                        IDWriteTextLayout *layout = nullptr;
                        dwrite->CreateTextLayout(
                            blk.text.c_str(),
                            (UINT32)blk.text.size(),
                            format,
                            availableW,
                            100000.0f,
                            &layout);

                        if (layout)
                        {
                            for (const auto &span : blk.spans)
                            {
                                DWRITE_TEXT_RANGE range = {span.start, span.length};
                                layout->SetFontSize(span.fontSize, range);
                                layout->SetFontWeight(span.weight, range);
                                layout->SetFontStyle(span.style, range);
                                if (span.code)
                                    layout->SetFontFamilyName(L"Consolas", range);
                            }
                            DWRITE_TEXT_METRICS metrics = {};
                            layout->GetMetrics(&metrics);
                            previewMarkdownLayouts_[i] = layout;
                            previewMarkdownMetrics_[i] = metrics;
                        }

                        format->Release();
                    }
                }

                previewMarkdownLayoutWidth_ = availableW;
                previewMarkdownLayoutHeight_ = availableH;
            }

            float totalHeight = 0.0f;
            for (size_t i = 0; i < previewMarkdownBlocks_.size(); ++i)
            {
                const auto &blk = previewMarkdownBlocks_[i];
                if (blk.type == MarkdownBlock::Type::Text)
                {
                    float h = previewMarkdownMetrics_[i].height;
                    if (!blk.inlineImages.empty())
                    {
                        float maxImgH = 0.0f;
                        for (const auto &img : blk.inlineImages)
                        {
                            ID2D1Bitmap *bmp = nullptr;
                            auto it = previewImageCache_.find(img.path);
                            if (it != previewImageCache_.end())
                                bmp = it->second;
                            else
                            {
                                bmp = LoadImageToD2DBitmap(ctx, img.path, img.width, img.height);
                                if (bmp)
                                    previewImageCache_[img.path] = bmp;
                            }
                            if (bmp)
                            {
                                D2D1_SIZE_F sz = bmp->GetSize();
                                float iw = img.width > 0.0f ? img.width : sz.width;
                                float ih = img.height > 0.0f ? img.height : sz.height;
                                if (iw > availableW)
                                {
                                    float scale = availableW / iw;
                                    iw *= scale;
                                    ih *= scale;
                                }
                                if (ih > maxImgH)
                                    maxImgH = ih;
                            }
                        }
                        h = (std::max)(h, maxImgH);
                    }
                    totalHeight += h + 8.0f;
                }
                else if (blk.type == MarkdownBlock::Type::Rule)
                {
                    totalHeight += 12.0f;
                }
                else if (blk.type == MarkdownBlock::Type::Table)
                {
                    float rowH = 24.0f;
                    float rows = (float)blk.tableRows.size();
                    totalHeight += rows * rowH + 16.0f;
                }
                else if (blk.type == MarkdownBlock::Type::Image)
                {
                    float drawH = 0.0f;
                    ID2D1Bitmap *bmp = nullptr;
                    auto it = previewImageCache_.find(blk.imagePath);
                    if (it != previewImageCache_.end())
                        bmp = it->second;
                    else
                    {
                        bmp = LoadImageToD2DBitmap(ctx, blk.imagePath, blk.imageWidth, blk.imageHeight);
                        if (bmp)
                            previewImageCache_[blk.imagePath] = bmp;
                    }

                    if (bmp)
                    {
                        D2D1_SIZE_F sz = bmp->GetSize();
                        float w = blk.imageWidth > 0.0f ? blk.imageWidth : sz.width;
                        float h = blk.imageHeight > 0.0f ? blk.imageHeight : sz.height;
                        if (w > availableW)
                        {
                            float scale = availableW / w;
                            w *= scale;
                            h *= scale;
                        }
                        drawH = h;
                    }
                    totalHeight += drawH + 12.0f;
                }
            }

            float contentHeight = (std::max)(totalHeight + 8.0f, availableH);
            scrollbar_.UpdateLayout(state_.leftEdge, state_.topEdge, state_.rightEdge - state_.leftEdge, state_.bottomEdge - state_.topEdge, contentHeight);
            state_.scrollOffsetY = scrollbar_.GetScrollOffset();

            ID2D1SolidColorBrush *textBrush = nullptr;
            ctx->CreateSolidColorBrush(theme_.text, &textBrush);
            if (!textBrush)
                return;

            ctx->PushAxisAlignedClip(contentRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            float y = contentRect.top - state_.scrollOffsetY;
            for (size_t i = 0; i < previewMarkdownBlocks_.size(); ++i)
            {
                const auto &blk = previewMarkdownBlocks_[i];
                if (blk.type == MarkdownBlock::Type::Text)
                {
                    if (previewMarkdownLayouts_[i])
                    {
                        if (blk.isQuote)
                        {
                            D2D1_RECT_F quoteRect = D2D1::RectF(contentRect.left, y, contentRect.right, y + previewMarkdownMetrics_[i].height + 10.0f);
                            ID2D1SolidColorBrush *quoteBg = nullptr;
                            ID2D1SolidColorBrush *quoteBar = nullptr;
                            ctx->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.10f, 0.12f, 0.8f), &quoteBg);
                            ctx->CreateSolidColorBrush(D2D1::ColorF(0.35f, 0.55f, 1.0f, 0.9f), &quoteBar);
                            if (quoteBg)
                            {
                                ctx->FillRectangle(quoteRect, quoteBg);
                                quoteBg->Release();
                            }
                            if (quoteBar)
                            {
                                D2D1_RECT_F bar = D2D1::RectF(quoteRect.left + 2.0f, quoteRect.top + 4.0f, quoteRect.left + 5.0f, quoteRect.bottom - 4.0f);
                                ctx->FillRectangle(bar, quoteBar);
                                quoteBar->Release();
                            }
                            ctx->DrawTextLayout(D2D1::Point2F(contentRect.left + 10.0f, y + 5.0f), previewMarkdownLayouts_[i], textBrush);
                            y += previewMarkdownMetrics_[i].height + 12.0f;
                        }
                        else
                        {
                            ctx->DrawTextLayout(D2D1::Point2F(contentRect.left, y), previewMarkdownLayouts_[i], textBrush);
                            float blockH = previewMarkdownMetrics_[i].height;
                            if (!blk.inlineImages.empty())
                            {
                                float maxImgH = 0.0f;
                                float xRight = contentRect.right;
                                float xLeft = contentRect.left;
                                for (const auto &img : blk.inlineImages)
                                {
                                    ID2D1Bitmap *bmp = nullptr;
                                    auto it = previewImageCache_.find(img.path);
                                    if (it != previewImageCache_.end())
                                        bmp = it->second;
                                    else
                                    {
                                        bmp = LoadImageToD2DBitmap(ctx, img.path, img.width, img.height);
                                        if (bmp)
                                            previewImageCache_[img.path] = bmp;
                                    }

                                    if (bmp)
                                    {
                                        D2D1_SIZE_F sz = bmp->GetSize();
                                        float iw = img.width > 0.0f ? img.width : sz.width;
                                        float ih = img.height > 0.0f ? img.height : sz.height;
                                        if (iw > availableW)
                                        {
                                            float scale = availableW / iw;
                                            iw *= scale;
                                            ih *= scale;
                                        }
                                        float ix = xLeft;
                                        if (img.align == MarkdownImageAlign::Right)
                                            ix = xRight - iw;
                                        else if (img.align == MarkdownImageAlign::Center)
                                            ix = xLeft + (availableW - iw) * 0.5f;
                                        D2D1_RECT_F rect = D2D1::RectF(ix, y, ix + iw, y + ih);
                                        ctx->DrawBitmap(bmp, rect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                                        if (ih > maxImgH)
                                            maxImgH = ih;
                                    }
                                }
                                blockH = (std::max)(blockH, maxImgH);
                            }
                            y += blockH + 8.0f;
                        }
                    }
                }
                else if (blk.type == MarkdownBlock::Type::Rule)
                {
                    float lineY = y + 6.0f;
                    ctx->DrawLine(D2D1::Point2F(contentRect.left, lineY),
                                  D2D1::Point2F(contentRect.right, lineY),
                                  textBrush, 1.0f);
                    y += 12.0f;
                }
                else if (blk.type == MarkdownBlock::Type::Table)
                {
                    if (!dwrite || blk.tableRows.empty())
                    {
                        y += 12.0f;
                        continue;
                    }

                    const float paddingX = 8.0f;
                    const float paddingY = 6.0f;
                    const size_t rowCount = blk.tableRows.size();
                    size_t colCount = 0;
                    for (const auto &row : blk.tableRows)
                        colCount = (std::max)(colCount, row.size());
                    if (colCount == 0)
                    {
                        y += 12.0f;
                        continue;
                    }

                    std::vector<float> colWidths(colCount, 0.0f);
                    std::vector<float> rowHeights(rowCount, 0.0f);

                    for (size_t r = 0; r < rowCount; ++r)
                    {
                        const auto &row = blk.tableRows[r];
                        for (size_t c = 0; c < colCount; ++c)
                        {
                            std::wstring cell = (c < row.size()) ? row[c] : L"";
                            IDWriteTextFormat *fmt = nullptr;
                            DWRITE_FONT_WEIGHT weight = (r == 0) ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
                            if (SUCCEEDED(dwrite->CreateTextFormat(
                                    L"Segoe UI",
                                    nullptr,
                                    weight,
                                    DWRITE_FONT_STYLE_NORMAL,
                                    DWRITE_FONT_STRETCH_NORMAL,
                                    13.5f,
                                    L"",
                                    &fmt)))
                            {
                                IDWriteTextLayout *layout = nullptr;
                                if (SUCCEEDED(dwrite->CreateTextLayout(cell.c_str(), (UINT32)cell.size(), fmt, 10000.0f, 10000.0f, &layout)))
                                {
                                    DWRITE_TEXT_METRICS metrics = {};
                                    layout->GetMetrics(&metrics);
                                    colWidths[c] = (std::max)(colWidths[c], metrics.width + paddingX * 2.0f);
                                    rowHeights[r] = (std::max)(rowHeights[r], metrics.height + paddingY * 2.0f);
                                    layout->Release();
                                }
                                fmt->Release();
                            }
                        }
                    }

                    float tableWidth = 0.0f;
                    for (float w : colWidths)
                        tableWidth += w;

                    float x0 = contentRect.left;
                    float y0 = y;

                    ID2D1SolidColorBrush *gridBrush = nullptr;
                    ID2D1SolidColorBrush *headerBg = nullptr;
                    ctx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f), &gridBrush);
                    ctx->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.12f, 0.14f, 0.9f), &headerBg);

                    float cy = y0;
                    for (size_t r = 0; r < rowCount; ++r)
                    {
                        float rowH = (rowHeights[r] > 0.0f) ? rowHeights[r] : 22.0f;
                        if (r == 0 && headerBg)
                        {
                            D2D1_RECT_F headerRect = D2D1::RectF(x0, cy, x0 + tableWidth, cy + rowH);
                            ctx->FillRectangle(headerRect, headerBg);
                        }

                        float cx = x0;
                        const auto &row = blk.tableRows[r];
                        for (size_t c = 0; c < colCount; ++c)
                        {
                            std::wstring cell = (c < row.size()) ? row[c] : L"";
                            IDWriteTextFormat *fmt = nullptr;
                            DWRITE_FONT_WEIGHT weight = (r == 0) ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
                            if (SUCCEEDED(dwrite->CreateTextFormat(
                                    L"Segoe UI",
                                    nullptr,
                                    weight,
                                    DWRITE_FONT_STYLE_NORMAL,
                                    DWRITE_FONT_STRETCH_NORMAL,
                                    13.5f,
                                    L"",
                                    &fmt)))
                            {
                                IDWriteTextLayout *layout = nullptr;
                                if (SUCCEEDED(dwrite->CreateTextLayout(cell.c_str(), (UINT32)cell.size(), fmt, colWidths[c] - paddingX * 2.0f, rowH - paddingY * 2.0f, &layout)))
                                {
                                    ctx->DrawTextLayout(D2D1::Point2F(cx + paddingX, cy + paddingY), layout, textBrush);
                                    layout->Release();
                                }
                                fmt->Release();
                            }

                            if (gridBrush)
                            {
                                D2D1_RECT_F cellRect = D2D1::RectF(cx, cy, cx + colWidths[c], cy + rowH);
                                ctx->DrawRectangle(cellRect, gridBrush, 1.0f);
                            }
                            cx += colWidths[c];
                        }
                        cy += rowH;
                    }

                    if (gridBrush)
                        gridBrush->Release();
                    if (headerBg)
                        headerBg->Release();

                    y = cy + 12.0f;
                }
                else if (blk.type == MarkdownBlock::Type::Image)
                {
                    ID2D1Bitmap *bmp = nullptr;
                    auto it = previewImageCache_.find(blk.imagePath);
                    if (it != previewImageCache_.end())
                        bmp = it->second;
                    else
                    {
                        bmp = LoadImageToD2DBitmap(ctx, blk.imagePath, blk.imageWidth, blk.imageHeight);
                        if (bmp)
                            previewImageCache_[blk.imagePath] = bmp;
                    }

                    if (bmp)
                    {
                        D2D1_SIZE_F sz = bmp->GetSize();
                        float w = blk.imageWidth > 0.0f ? blk.imageWidth : sz.width;
                        float h = blk.imageHeight > 0.0f ? blk.imageHeight : sz.height;
                        if (w > availableW)
                        {
                            float scale = availableW / w;
                            w *= scale;
                            h *= scale;
                        }
                        float x = contentRect.left;
                        if (blk.imageAlign == MarkdownImageAlign::Right)
                            x = contentRect.right - w;
                        else if (blk.imageAlign == MarkdownImageAlign::Center)
                            x = contentRect.left + (availableW - w) * 0.5f;
                        D2D1_RECT_F rect = D2D1::RectF(x, y, x + w, y + h);
                        ctx->DrawBitmap(bmp, rect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);

                        std::wstring ext = ToLower(std::filesystem::path(blk.imagePath).extension().wstring());
                        if (ext == L".svg")
                        {
                            auto itText = previewSvgTextCache_.find(blk.imagePath);
                            auto itSize = previewSvgSizeCache_.find(blk.imagePath);
                            if (itText == previewSvgTextCache_.end())
                            {
                                std::vector<MarkdownSvgText> texts;
                                float svgW = 0.0f;
                                float svgH = 0.0f;
                                ParseSvgTextOverlays(blk.imagePath, texts, svgW, svgH);
                                previewSvgTextCache_[blk.imagePath] = texts;
                                previewSvgSizeCache_[blk.imagePath] = D2D1::SizeF(svgW, svgH);
                                itText = previewSvgTextCache_.find(blk.imagePath);
                                itSize = previewSvgSizeCache_.find(blk.imagePath);
                            }

                            if (itText != previewSvgTextCache_.end() && itSize != previewSvgSizeCache_.end())
                            {
                                float svgW = itSize->second.width > 0.0f ? itSize->second.width : w;
                                float svgH = itSize->second.height > 0.0f ? itSize->second.height : h;
                                float scaleX = w / svgW;
                                float scaleY = h / svgH;
                                for (const auto &txt : itText->second)
                                {
                                    float fontSize = txt.fontSize * scaleY;
                                    IDWriteTextFormat *fmt = nullptr;
                                    if (dwrite && SUCCEEDED(dwrite->CreateTextFormat(
                                            L"Segoe UI",
                                            nullptr,
                                            txt.weight,
                                            DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL,
                                            fontSize,
                                            L"",
                                            &fmt)))
                                    {
                                        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                                        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                                        IDWriteTextLayout *layout = nullptr;
                                        std::wstring textW = txt.text;
                                        float maxW = w;
                                        float maxH = h;
                                        if (SUCCEEDED(dwrite->CreateTextLayout(textW.c_str(), (UINT32)textW.size(), fmt, maxW, maxH, &layout)))
                                        {
                                            DWRITE_TEXT_METRICS metrics = {};
                                            layout->GetMetrics(&metrics);
                                            float tx = x + txt.x * scaleX;
                                            if (txt.anchor == 1)
                                                tx -= metrics.width * 0.5f;
                                            else if (txt.anchor == 2)
                                                tx -= metrics.width;
                                            float ty = y + (txt.y * scaleY) - fontSize;
                                            ID2D1SolidColorBrush *svgBrush = nullptr;
                                            ctx->CreateSolidColorBrush(txt.color, &svgBrush);
                                            if (svgBrush)
                                            {
                                                ctx->DrawTextLayout(D2D1::Point2F(tx, ty), layout, svgBrush);
                                                svgBrush->Release();
                                            }
                                            layout->Release();
                                        }
                                        fmt->Release();
                                    }
                                }
                            }
                        }
                        y += h + 12.0f;
                    }
                }
            }
            ctx->PopAxisAlignedClip();

            textBrush->Release();
            scrollbar_.Draw(ctx);
            return;
        }

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
