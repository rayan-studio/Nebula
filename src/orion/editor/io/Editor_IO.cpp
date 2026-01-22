#include "orion/editor/Editor.h"

#include <fstream>
#include <filesystem>
#include <string>

#include <dwrite_1.h>

// Ensure Windows min/max macros don't interfere with std::min/std::max
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

namespace Orion
{
    void Editor::LoadFile(const std::wstring &filePath)
    {
        state_.filePath = filePath;
        state_.lines.clear();
        state_.caret = {0, 0};
        state_.scrollOffsetX = 0.0f;
        state_.scrollOffsetY = 0.0f;
        state_.encoding = L"Unknown";

        int size_needed = WideCharToMultiByte(CP_UTF8, 0,
                                              filePath.c_str(), (int)filePath.size(),
                                              nullptr, 0, nullptr, nullptr);
        std::string pathUtf8(size_needed, '\0');
        WideCharToMultiByte(CP_UTF8, 0,
                            filePath.c_str(), (int)filePath.size(),
                            pathUtf8.data(), size_needed, nullptr, nullptr);

        std::ifstream file(pathUtf8, std::ios::binary);
        if (!file.is_open())
        {
            state_.lines.push_back(L"// Could not open file");
            state_.encoding = L"";
            return;
        }

        std::string data;
        file.seekg(0, std::ios::end);
        std::streamoff fsize = file.tellg();
        if (fsize > 0)
        {
            file.seekg(0, std::ios::beg);
            data.resize((size_t)fsize);
            file.read(&data[0], fsize);
        }
        file.close();

        auto split_wlines = [&](const std::wstring &w)
        {
            state_.lines.clear();
            size_t start = 0;
            size_t i = 0;
            const size_t n = w.size();

            while (i < n)
            {
                wchar_t c = w[i];
                if (c == L'\r' || c == L'\n')
                {
                    state_.lines.emplace_back(w.substr(start, i - start));

                    if (c == L'\r' && (i + 1) < n && w[i + 1] == L'\n')
                        i++;

                    i++;
                    start = i;
                }
                else
                {
                    i++;
                }
            }

            state_.lines.emplace_back(w.substr(start));
            if (state_.lines.empty())
                state_.lines.push_back(L"");
        };

        auto decode_utf8_strict = [&](const char *src, int len, std::wstring &out) -> bool
        {
            out.clear();
            if (len <= 0)
                return true;

            int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, len, nullptr, 0);
            if (wlen <= 0)
                return false;

            out.resize((size_t)wlen);
            int wlen2 = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, len, out.data(), wlen);
            return wlen2 == wlen;
        };

        auto decode_ansi = [&](const char *src, int len, std::wstring &out)
        {
            out.clear();
            if (len <= 0)
                return;

            int wlen = MultiByteToWideChar(CP_ACP, 0, src, len, nullptr, 0);
            if (wlen <= 0)
                return;

            out.resize((size_t)wlen);
            MultiByteToWideChar(CP_ACP, 0, src, len, out.data(), wlen);
        };

        const unsigned char *p = reinterpret_cast<const unsigned char *>(data.data());
        const size_t len = data.size();

        std::wstring decoded;

        if (len >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF)
        {
            state_.encoding = L"UTF-8";
            const char *src = data.data() + 3;
            const int slen = (int)(len - 3);

            if (!decode_utf8_strict(src, slen, decoded))
            {
                int wlen = MultiByteToWideChar(CP_UTF8, 0, src, slen, nullptr, 0);
                decoded.resize((size_t)wlen);
                MultiByteToWideChar(CP_UTF8, 0, src, slen, decoded.data(), wlen);
            }

            split_wlines(decoded);
        }
        else if (len >= 2 && p[0] == 0xFF && p[1] == 0xFE)
        {
            state_.encoding = L"UTF-16 LE";
            const size_t byteCount = len - 2;
            const size_t wcharCount = byteCount / 2;

            std::wstring w;
            w.resize(wcharCount);
            for (size_t i = 0; i < wcharCount; ++i)
            {
                unsigned char lo = p[2 + i * 2];
                unsigned char hi = p[2 + i * 2 + 1];
                w[i] = (wchar_t)((hi << 8) | lo);
            }

            split_wlines(w);
        }
        else if (len >= 2 && p[0] == 0xFE && p[1] == 0xFF)
        {
            state_.encoding = L"UTF-16 BE";
            const size_t byteCount = len - 2;
            const size_t wcharCount = byteCount / 2;

            std::wstring w;
            w.resize(wcharCount);
            for (size_t i = 0; i < wcharCount; ++i)
            {
                unsigned char hi = p[2 + i * 2];
                unsigned char lo = p[2 + i * 2 + 1];
                w[i] = (wchar_t)((hi << 8) | lo);
            }

            split_wlines(w);
        }
        else
        {
            if (decode_utf8_strict(data.data(), (int)len, decoded))
            {
                state_.encoding = L"UTF-8";
                split_wlines(decoded);
            }
            else
            {
                state_.encoding = L"ANSI";
                decode_ansi(data.data(), (int)len, decoded);
                split_wlines(decoded);
            }
        }

        if (state_.lines.empty())
            state_.lines.push_back(L"");
    }

    void Editor::CreateEmpty()
    {
        ResetPreview();
        state_.lines.clear();
        state_.lines.push_back(L"");
        state_.caret = {0, 0};
        state_.filePath.clear();
        state_.scrollOffsetX = 0.0f;
        state_.scrollOffsetY = 0.0f;
    }

    bool Editor::SaveToFile(const std::wstring &filePath)
    {
        int needed = WideCharToMultiByte(CP_UTF8, 0, filePath.c_str(), (int)filePath.size(), NULL, 0, NULL, NULL);
        if (needed <= 0)
            return false;

        std::string pathUtf8(needed, '\0');
        WideCharToMultiByte(CP_UTF8, 0, filePath.c_str(), (int)filePath.size(), pathUtf8.data(), needed, NULL, NULL);

        std::ofstream ofs(pathUtf8, std::ios::binary);
        if (!ofs.is_open())
            return false;

        for (size_t i = 0; i < state_.lines.size(); ++i)
        {
            const std::wstring &wline = state_.lines[i];
            int sz = WideCharToMultiByte(CP_UTF8, 0, wline.c_str(), (int)wline.size(), NULL, 0, NULL, NULL);
            std::string lineUtf8(sz, '\0');
            if (sz > 0)
                WideCharToMultiByte(CP_UTF8, 0, wline.c_str(), (int)wline.size(), lineUtf8.data(), sz, NULL, NULL);

            ofs << lineUtf8;
            if (i + 1 < state_.lines.size())
                ofs << "\n";
        }

        ofs.close();
        if (!ofs)
            return false;

        state_.filePath = filePath;
        // Clear dirty flag after successful save
        ClearDirty();
        RunClangdDiagnosticsAsync();
        return true;
    }

    bool Editor::LoadCustomFont(IDWriteFactory *dwrite, const std::wstring &fontPath)
    {
        IDWriteFactory1 *factory1 = nullptr;
        HRESULT hr = dwrite->QueryInterface(__uuidof(IDWriteFactory1), (void **)&factory1);

        fontLoader_ = new CustomFontCollectionLoader();

        HRESULT regHr = dwrite->RegisterFontCollectionLoader(fontLoader_);
        if (FAILED(regHr))
        {
            fontLoader_->Release();
            fontLoader_ = nullptr;
            if (factory1)
                factory1->Release();
            return false;
        }

        fontCollectionRegisteredFactory_ = dwrite;
        fontCollectionRegisteredFactory_->AddRef();

        const void *collectionKey = fontPath.c_str();
        UINT32 collectionKeySize = (UINT32)((fontPath.size() + 1) * sizeof(wchar_t));

        hr = factory1->CreateCustomFontCollection(
            fontLoader_,
            collectionKey,
            collectionKeySize,
            &customFontCollection_);

        if (FAILED(hr) || !customFontCollection_)
        {
            fontCollectionRegisteredFactory_->UnregisterFontCollectionLoader(fontLoader_);
            fontLoader_->Release();
            fontLoader_ = nullptr;
            fontCollectionRegisteredFactory_->Release();
            fontCollectionRegisteredFactory_ = nullptr;
            factory1->Release();
            return false;
        }

        factory1->Release();

        UINT32 index = 0;
        BOOL exists = FALSE;
        hr = customFontCollection_->FindFamilyName(L"JetBrains Mono", &index, &exists);
        if (FAILED(hr))
            return false;

        return exists == TRUE;
    }
}
