#include "ui/marketplace/MarketplaceVisuals.h"

#include <windows.h>
#include <winhttp.h>
#include <wincodec.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
    struct AvatarEntry
    {
        std::vector<unsigned char> bytes;
        bool loading = false;
        bool failed = false;
        ID2D1Bitmap* bitmap = nullptr;
        ID2D1RenderTarget* bitmapTarget = nullptr;
    };

    std::mutex g_avatarMutex;
    std::map<std::wstring, AvatarEntry> g_avatarCache;

    void ReleaseAvatarBitmap(AvatarEntry& entry)
    {
        if (entry.bitmap)
        {
            entry.bitmap->Release();
            entry.bitmap = nullptr;
        }
        entry.bitmapTarget = nullptr;
    }

    bool DownloadUrlBytes(const std::wstring& url, std::vector<unsigned char>& outBytes)
    {
        outBytes.clear();

        URL_COMPONENTS components{};
        components.dwStructSize = sizeof(components);
        components.dwHostNameLength = static_cast<DWORD>(-1);
        components.dwUrlPathLength = static_cast<DWORD>(-1);
        components.dwExtraInfoLength = static_cast<DWORD>(-1);
        components.dwSchemeLength = static_cast<DWORD>(-1);

        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components))
            return false;

        std::wstring host(components.lpszHostName, components.dwHostNameLength);
        std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
        if (components.dwExtraInfoLength > 0 && components.lpszExtraInfo)
            path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
        if (path.empty())
            path = L"/";

        const bool isSecure = components.nScheme == INTERNET_SCHEME_HTTPS;

        HINTERNET session = WinHttpOpen(
            L"Nebula/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);
        if (!session)
            return false;

        HINTERNET connection = WinHttpConnect(session, host.c_str(), components.nPort, 0);
        if (!connection)
        {
            WinHttpCloseHandle(session);
            return false;
        }

        HINTERNET request = WinHttpOpenRequest(
            connection,
            L"GET",
            path.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            isSecure ? WINHTTP_FLAG_SECURE : 0);
        if (!request)
        {
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return false;
        }

        const wchar_t* headers = L"Accept: image/*,*/*;q=0.8\r\n";
        BOOL ok = WinHttpSendRequest(
            request,
            headers,
            static_cast<DWORD>(-1L),
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0);
        if (ok)
            ok = WinHttpReceiveResponse(request, nullptr);

        bool success = false;
        if (ok)
        {
            DWORD statusCode = 0;
            DWORD statusSize = sizeof(statusCode);
            if (!WinHttpQueryHeaders(
                    request,
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX,
                    &statusCode,
                    &statusSize,
                    WINHTTP_NO_HEADER_INDEX))
            {
                statusCode = 0;
            }

            if (statusCode >= 200 && statusCode < 300)
            {
                DWORD available = 0;
                while (WinHttpQueryDataAvailable(request, &available) && available > 0)
                {
                    const size_t oldSize = outBytes.size();
                    outBytes.resize(oldSize + available);

                    DWORD downloaded = 0;
                    if (!WinHttpReadData(request, outBytes.data() + oldSize, available, &downloaded))
                    {
                        outBytes.clear();
                        break;
                    }

                    outBytes.resize(oldSize + downloaded);
                }

                success = !outBytes.empty();
            }
        }

        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return success;
    }

    ID2D1Bitmap* CreateBitmapFromMemory(ID2D1RenderTarget* ctx, std::vector<unsigned char>& bytes)
    {
        if (!ctx || bytes.empty())
            return nullptr;

        IWICImagingFactory* wicFactory = nullptr;
        if (FAILED(CoCreateInstance(
                CLSID_WICImagingFactory,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&wicFactory))) || !wicFactory)
        {
            return nullptr;
        }

        IWICStream* stream = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* converter = nullptr;
        ID2D1Bitmap* bitmap = nullptr;

        if (SUCCEEDED(wicFactory->CreateStream(&stream)) && stream &&
            SUCCEEDED(stream->InitializeFromMemory(bytes.data(), static_cast<DWORD>(bytes.size()))) &&
            SUCCEEDED(wicFactory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) && decoder &&
            SUCCEEDED(decoder->GetFrame(0, &frame)) && frame &&
            SUCCEEDED(wicFactory->CreateFormatConverter(&converter)) && converter &&
            SUCCEEDED(converter->Initialize(
                frame,
                GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone,
                nullptr,
                0.0,
                WICBitmapPaletteTypeCustom)))
        {
            ctx->CreateBitmapFromWicBitmap(converter, nullptr, &bitmap);
        }

        if (converter) converter->Release();
        if (frame) frame->Release();
        if (decoder) decoder->Release();
        if (stream) stream->Release();
        wicFactory->Release();
        return bitmap;
    }
}

D2D1_COLOR_F MarketplaceCategoryColor(const std::wstring& category)
{
    std::wstring lowered = category;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });

    if (lowered == L"ui") return D2D1::ColorF(0.32f, 0.58f, 0.89f);
    if (lowered == L"graphics") return D2D1::ColorF(0.15f, 0.65f, 0.60f);
    if (lowered == L"math") return D2D1::ColorF(0.49f, 0.34f, 0.76f);
    if (lowered == L"utilities") return D2D1::ColorF(1.00f, 0.44f, 0.26f);
    if (lowered == L"audio") return D2D1::ColorF(0.93f, 0.25f, 0.48f);
    if (lowered == L"networking") return D2D1::ColorF(0.15f, 0.78f, 0.85f);
    if (lowered == L"physics") return D2D1::ColorF(0.94f, 0.33f, 0.31f);
    if (lowered == L"testing") return D2D1::ColorF(0.20f, 0.75f, 0.35f);
    if (lowered == L"serialization") return D2D1::ColorF(0.95f, 0.68f, 0.25f);
    return D2D1::ColorF(0.50f, 0.55f, 0.65f);
}

std::wstring MarketplaceInitials(const std::wstring& name)
{
    std::wstring result;
    bool nextUpper = true;
    for (wchar_t ch : name)
    {
        if (ch == L'/' || ch == L' ' || ch == L'_' || ch == L'-')
        {
            nextUpper = true;
            continue;
        }

        if (nextUpper && std::iswalpha(ch))
        {
            result.push_back(static_cast<wchar_t>(std::towupper(ch)));
            nextUpper = false;
        }
        else if (std::iswupper(ch) && !result.empty())
        {
            result.push_back(ch);
        }

        if (result.size() >= 2)
            break;
    }

    if (result.empty() && !name.empty())
        result.push_back(static_cast<wchar_t>(std::towupper(name.front())));
    if (result.size() == 1 && name.size() > 1)
        result.push_back(static_cast<wchar_t>(std::towupper(name[1])));
    return result;
}

void MarketplaceEnsureAvatarAsync(const std::wstring& url, HWND hwnd)
{
    if (url.empty())
        return;

    {
        std::lock_guard<std::mutex> lock(g_avatarMutex);
        auto& entry = g_avatarCache[url];
        if (entry.loading || entry.failed || !entry.bytes.empty())
            return;
        entry.loading = true;
    }

    std::thread([url, hwnd]() {
        std::vector<unsigned char> bytes;
        const bool ok = DownloadUrlBytes(url, bytes);

        {
            std::lock_guard<std::mutex> lock(g_avatarMutex);
            AvatarEntry& entry = g_avatarCache[url];
            entry.loading = false;
            entry.failed = !ok;
            if (ok)
            {
                entry.bytes = std::move(bytes);
                ReleaseAvatarBitmap(entry);
            }
        }

        if (hwnd)
            InvalidateRect(hwnd, nullptr, FALSE);
    }).detach();
}

ID2D1Bitmap* MarketplaceLoadAvatarBitmap(ID2D1RenderTarget* ctx, const std::wstring& url)
{
    if (!ctx || url.empty())
        return nullptr;

    std::vector<unsigned char> bytes;
    {
        std::lock_guard<std::mutex> lock(g_avatarMutex);
        auto it = g_avatarCache.find(url);
        if (it == g_avatarCache.end())
            return nullptr;

        AvatarEntry& entry = it->second;
        if (entry.bitmap && entry.bitmapTarget == ctx)
            return entry.bitmap;
        if (entry.bytes.empty())
            return nullptr;
        bytes = entry.bytes;
    }

    ID2D1Bitmap* bitmap = CreateBitmapFromMemory(ctx, bytes);
    if (!bitmap)
        return nullptr;

    std::lock_guard<std::mutex> lock(g_avatarMutex);
    AvatarEntry& entry = g_avatarCache[url];
    ReleaseAvatarBitmap(entry);
    entry.bitmap = bitmap;
    entry.bitmapTarget = ctx;
    return entry.bitmap;
}
