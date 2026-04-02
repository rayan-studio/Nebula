#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <map>
#include <mutex>

#include "orion/editor/Editor.h"

class MarketplaceExtensionTabView
{
public:
    enum class ReadmeMode
    {
        Preview,
        Markdown,
    };

    void SetLibraryName(const std::wstring &name);
    void UpdateLayout(HWND hwnd, float left, float top, float right, float bottom);
    void Draw(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd);

    void OnMouseMove(HWND hwnd, POINT clientPoint);
    void OnLeftButtonDown(HWND hwnd, POINT clientPoint);
    void OnLeftButtonUp(HWND hwnd);
    void OnMouseWheel(HWND hwnd, int delta);
    bool OnKeyDown(WPARAM key);
    bool IsPointInView(POINT clientPoint) const;
    bool WantsHandCursor(POINT clientPoint) const;
    LPCWSTR CursorForPoint(POINT clientPoint) const;
    bool UpdateUiAnimation();

private:
    std::wstring currentLibraryName_;
    D2D1_RECT_F bounds_           = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F cardRect_         = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F actionButtonRect_ = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F readmeClipRect_   = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F readmePreviewTabRect_ = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F readmeMarkdownTabRect_ = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F repositoryLinkRect_ = D2D1::RectF(0, 0, 0, 0);
    bool actionButtonHovered_ = false;
    bool readmePreviewTabHovered_ = false;
    bool readmeMarkdownTabHovered_ = false;
    bool repositoryLinkHovered_ = false;
    HWND hwnd_ = nullptr;

    // README async fetch
    std::map<std::wstring, std::wstring> readmeCache_;
    std::mutex cacheMutex_;
    std::wstring lastFetchedFor_;
    std::wstring currentPreviewText_;
    ReadmeMode readmeMode_ = ReadmeMode::Preview;

    // Reuse the existing editor Markdown preview renderer for README content.
    Orion::Editor readmePreviewEditor_;

    void FetchReadmeAsync(HWND hwnd, const std::wstring& libName, const std::wstring& gitUrl);
    void ApplyReadmeEditorMode();

    static bool FetchRawUrl(const std::wstring& host, const std::wstring& path,
                            std::string& outBody);
    static bool ParseGitHubUrl(const std::wstring& gitUrl,
                               std::wstring& owner, std::wstring& repo);
    static std::wstring NormalizeReadmeMarkdown(const std::wstring& md);

    bool IsPointInRect(POINT pt, const D2D1_RECT_F &rect) const;
};
