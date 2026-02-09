#include "SearchPanel.h"
#include "ui/panels/PanelManager.h"
#include "helpers/window_helpers.h"
#include "core/explorer/Explorer.h"
#include "ui/components/input/InputTheme.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>
#include <unordered_set>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

// ============================================================================
// SearchPanel Implementation
// ============================================================================

SearchPanel::SearchPanel()
    : Panel(PanelId::Search)
{
    config_ = PanelConfig(
        PanelId::Search,
        L"\uE721",
        L"Search",
        true,
        false,
        1
    );
    title_ = L"SEARCH";

    // Configure search input avec le style SearchBox
    searchInput_.SetPlaceholder(L"Search files...");
    searchInput_.SetIcon(L"\uE721");
    
    // Use the same visual style as `Orion::SearchBox` (outer box + inner input)
    auto &style = searchInput_.GetStyle();
    style.useSearchBoxStyle = false;
    style.backgroundColor = UI::InputTheme::Background();
    style.borderColor = UI::InputTheme::Border();
    style.focusBorderColor = UI::InputTheme::FocusBorder();
    style.textColor = UI::InputTheme::Text();
    style.placeholderColor = UI::InputTheme::Placeholder();
    style.selectionColor = UI::InputTheme::Selection();
    style.cursorColor = UI::InputTheme::Caret();
    style.cornerRadius = UI::InputTheme::kCornerRadius;
    style.fontFamily = UI::InputTheme::kFontFamily;
    style.fontSize = UI::InputTheme::kFontSize;
    style.padding = UI::InputTheme::kHorizontalPadding;
    
    searchInput_.onTextChanged = [this](const std::wstring &text) {
        UpdateSearchResults();
    };
    searchInput_.onSubmit = [this]() {
        HWND hwnd = GetActiveWindow();
        if (hwnd) OpenSelectedResult(hwnd);
    };
    searchInput_.onEscape = [this]() {
        if (!searchInput_.GetText().empty()) {
            ClearSearch();
        }
    };

    // Start background worker thread for searches
    workerStarted_ = true;
    searchThread_ = std::thread([this]() {
        while (!terminateWorker_.load())
        {
            std::wstring query;
            std::wstring rootPath;

            {
                std::unique_lock<std::mutex> lk(searchMutex_);
                searchCv_.wait(lk, [this]() { return !latestQuery_.empty() || terminateWorker_.load(); });
                if (terminateWorker_.load()) break;
                query = latestQuery_;
                latestQuery_.clear();
                rootPath = searchRoot_.empty() ? GetExplorerManager().GetState().rootPath : searchRoot_;
            }

            if (query.empty() || rootPath.empty())
                continue;

            std::vector<Panels::SearchResult> localResults;
            try
            {
                std::wstring queryLower = query;
                std::transform(queryLower.begin(), queryLower.end(), queryLower.begin(), ::towlower);

                std::function<void(const std::filesystem::path &, int, int&, int)> searchDir;
                int maxResults = 100;
                int foundCount = 0;

                static const std::unordered_set<std::wstring> textExts = {
                    L".txt", L".cpp", L".h", L".hpp", L".c", L".cc",
                    L".py", L".js", L".ts", L".jsx", L".tsx",
                    L".java", L".cs", L".go", L".rs", L".rb",
                    L".html", L".css", L".scss", L".less",
                    L".json", L".xml", L".yaml", L".yml", L".toml",
                    L".md", L".markdown", L".rst", L".svg",
                    L".sh", L".bash", L".ps1", L".bat", L".cmd",
                    L".cmake", L".make", L".ini", L".cfg", L".conf"};

                searchDir = [&](const std::filesystem::path &dir, int depth, int &foundCount, int maxDepth)
                {
                    if (terminateWorker_.load()) return;
                    if (depth > 5 || foundCount >= maxResults) return;

                    try
                    {
                        for (const auto &entry : std::filesystem::directory_iterator(dir))
                        {
                            if (terminateWorker_.load()) return;
                            if (foundCount >= maxResults) break;

                            if (entry.is_directory())
                            {
                                std::wstring dirName = entry.path().filename().wstring();
                                if (dirName[0] != L'.' && dirName != L"node_modules" && dirName != L"build" && dirName != L".git")
                                {
                                    searchDir(entry.path(), depth + 1, foundCount, maxDepth);
                                }
                            }
                            else
                            {
                                std::wstring ext = entry.path().extension().wstring();
                                std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
                                if (textExts.count(ext) == 0) continue;

                                try
                                {
                                    std::wifstream file(entry.path());
                                    if (!file) continue;

                                    std::wstring line;
                                    int lineNum = 0;
                                    while (std::getline(file, line) && foundCount < maxResults)
                                    {
                                        if (terminateWorker_.load()) return;
                                        std::wstring lineLower = line;
                                        std::transform(lineLower.begin(), lineLower.end(), lineLower.begin(), ::towlower);

                                        size_t matchPos = lineLower.find(queryLower);
                                        if (matchPos != std::wstring::npos)
                                        {
                                            Panels::SearchResult result;
                                            result.filePath = entry.path().wstring();
                                            result.fileName = entry.path().filename().wstring();
                                            result.lineNumber = lineNum;
                                            result.matchStart = (int)matchPos;
                                            result.matchLength = (int)query.length();

                                            std::wstring excerpt = line;
                                            size_t start = excerpt.find_first_not_of(L" \t");
                                            if (start != std::wstring::npos) excerpt = excerpt.substr(start);
                                            if (excerpt.length() > 80) excerpt = excerpt.substr(0, 80) + L"...";
                                            result.lineExcerpt = excerpt;

                                            localResults.push_back(result);
                                            foundCount++;
                                        }
                                        lineNum++;
                                    }
                                }
                                catch (...) { }
                            }
                        }
                    }
                    catch (...) { }
                };

                searchDir(rootPath, 0, foundCount, 5);

                std::sort(localResults.begin(), localResults.end(), [](const Panels::SearchResult &a, const Panels::SearchResult &b) {
                    if (a.fileName != b.fileName) return a.fileName < b.fileName;
                    return a.lineNumber < b.lineNumber;
                });

                {
                    std::lock_guard<std::mutex> lg(searchMutex_);
                    searchResults_.swap(localResults);
                    selectedResultIndex_ = searchResults_.empty() ? -1 : 0;
                }

                InvalidateRect(GetActiveWindow(), NULL, FALSE);
            }
            catch (...) { }
        }
    });
}

SearchPanel::~SearchPanel()
{
    // Signal worker termination and join thread
    terminateWorker_.store(true);
    searchCv_.notify_one();
    if (searchThread_.joinable())
        searchThread_.join();
}

void SearchPanel::Initialize()
{
    // Nothing special to initialize
}

// Dans la méthode Draw, simplifier le code :
void SearchPanel::Draw(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND /*hwnd*/)
{
    if (!visible_ || state_.physicalWidth <= 0)
        return;

    D2D1_ANTIALIAS_MODE oldAA = ctx->GetAntialiasMode();
    D2D1_TEXT_ANTIALIAS_MODE oldTextAA = ctx->GetTextAntialiasMode();
    ctx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

    D2D1_RECT_F clipRect = D2D1::RectF(
        state_.leftEdge,
        state_.topEdge,
        state_.rightEdge,
        state_.bottomEdge);

    ctx->PushAxisAlignedClip(clipRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    // Background
    Panel::DrawBackground(ctx);

    // Title
    Panel::DrawTitle(ctx, dwrite);

    // ✨ SIMPLIFIÉ : Le TextInput gère maintenant tout le style SearchBox
    searchInput_.Draw(ctx, dwrite);

    // Results
    DrawResults(ctx, dwrite);

    // Right border
    Panel::DrawRightBorder(ctx);

    // Scrollbar
    scrollbar_.Draw(ctx);

    ctx->PopAxisAlignedClip();

    ctx->SetAntialiasMode(oldAA);
    ctx->SetTextAntialiasMode(oldTextAA);
}

void SearchPanel::DrawResults(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite)
{
    if (searchResults_.empty())
    {
        // Draw "No results" or instruction
        IDWriteTextFormat *tf = nullptr;
        dwrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL,
                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                 12.0f, L"en-us", &tf);
        if (tf)
        {
            tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

            ID2D1SolidColorBrush *textBrush = nullptr;
            ctx->CreateSolidColorBrush(D2D1::ColorF(0.5f, 0.5f, 0.5f), &textBrush);

            D2D1_RECT_F rect = D2D1::RectF(
                state_.leftEdge + state_.leftPadding,
                resultsTop_,
                state_.rightEdge - state_.leftPadding,
                resultsTop_ + 60.0f);

            const wchar_t *msg = searchInput_.GetText().empty()
                                     ? L"Type to search in files"
                                     : L"No results found";
            ctx->DrawTextW(msg, (UINT32)wcslen(msg), tf, rect, textBrush);

            if (textBrush)
                textBrush->Release();
            tf->Release();
        }
        return;
    }

    // Create brushes
    ID2D1SolidColorBrush *fileNameBrush = nullptr;
    ID2D1SolidColorBrush *pathBrush = nullptr;
    ID2D1SolidColorBrush *excerptBrush = nullptr;
    ID2D1SolidColorBrush *hoverBgBrush = nullptr;
    ID2D1SolidColorBrush *selectedBgBrush = nullptr;
    ID2D1SolidColorBrush *separatorBrush = nullptr;

    ctx->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.95f, 0.95f), &fileNameBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.55f, 0.55f, 0.55f), &pathBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.75f, 0.75f, 0.75f), &excerptBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.15f, 0.15f, 0.15f), &hoverBgBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.30f, 0.50f), &selectedBgBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.2f, 0.2f), &separatorBrush);

    // Text formats
    IDWriteTextFormat *fileNameFormat = nullptr;
    IDWriteTextFormat *pathFormat = nullptr;
    IDWriteTextFormat *excerptFormat = nullptr;

    dwrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             13.0f, L"en-us", &fileNameFormat);
    dwrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             11.0f, L"en-us", &pathFormat);
    dwrite->CreateTextFormat(L"Consolas", NULL, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             11.0f, L"en-us", &excerptFormat);

    if (fileNameFormat)
        fileNameFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    if (pathFormat)
    {
        pathFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        pathFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    if (excerptFormat)
    {
        excerptFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        excerptFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    // Clip to results area
    D2D1_RECT_F resultsClip = D2D1::RectF(
        state_.leftEdge, resultsTop_,
        state_.rightEdge, state_.bottomEdge);
    ctx->PushAxisAlignedClip(resultsClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    // Draw results
    float y = resultsTop_ - scrollbar_.GetScrollOffset();
    int maxDisplay = 100;
    float padding = state_.leftPadding;

    for (size_t i = 0; i < searchResults_.size() && i < (size_t)maxDisplay; ++i)
    {
        D2D1_RECT_F itemRect = D2D1::RectF(
            state_.leftEdge,
            y,
            state_.rightEdge,
            y + resultItemHeight_);

        // Skip if outside visible area
        if (itemRect.bottom < resultsTop_ || itemRect.top > state_.bottomEdge)
        {
            y += resultItemHeight_;
            continue;
        }

        // Background for hover/selected with rounded corners feel
        D2D1_RECT_F bgRect = D2D1::RectF(
            state_.leftEdge + 4.0f,
            y + 2.0f,
            state_.rightEdge - 4.0f,
            y + resultItemHeight_ - 2.0f);
        D2D1_ROUNDED_RECT roundedBg = D2D1::RoundedRect(bgRect, 4.0f, 4.0f);

        if ((int)i == selectedResultIndex_)
        {
            ctx->FillRoundedRectangle(roundedBg, selectedBgBrush);
        }
        else if ((int)i == hoveredResultIndex_)
        {
            ctx->FillRoundedRectangle(roundedBg, hoverBgBrush);
        }

        const auto &result = searchResults_[i];

        // File name - bigger and clearer
        D2D1_RECT_F nameRect = D2D1::RectF(
            state_.leftEdge + padding,
            y + 8.0f,
            state_.rightEdge - padding,
            y + 24.0f);
        ctx->DrawTextW(result.fileName.c_str(), (UINT32)result.fileName.length(),
                       fileNameFormat, nameRect, fileNameBrush);

        // Path - relative path, smaller
        std::wstring displayPath = result.filePath;
        // Try to make path relative to root
        std::wstring rootPath = GetExplorerManager().GetState().rootPath;
        if (!rootPath.empty() && displayPath.find(rootPath) == 0)
        {
            displayPath = displayPath.substr(rootPath.length());
            if (!displayPath.empty() && (displayPath[0] == L'\\' || displayPath[0] == L'/'))
                displayPath = displayPath.substr(1);
        }

        D2D1_RECT_F pathRect = D2D1::RectF(
            state_.leftEdge + padding,
            y + 24.0f,
            state_.rightEdge - padding,
            y + 38.0f);
        ctx->DrawTextW(displayPath.c_str(), (UINT32)displayPath.length(),
                       pathFormat, pathRect, pathBrush);

        // Excerpt - code preview with monospace font
        if (!result.lineExcerpt.empty())
        {
            D2D1_RECT_F excerptRect = D2D1::RectF(
                state_.leftEdge + padding,
                y + 38.0f,
                state_.rightEdge - padding,
                y + resultItemHeight_ - 6.0f);
            ctx->DrawTextW(result.lineExcerpt.c_str(), (UINT32)result.lineExcerpt.length(),
                           excerptFormat, excerptRect, excerptBrush);
        }

        y += resultItemHeight_;
    }

    // Pop results clip
    ctx->PopAxisAlignedClip();

    // Cleanup
    if (fileNameFormat)
        fileNameFormat->Release();
    if (pathFormat)
        pathFormat->Release();
    if (excerptFormat)
        excerptFormat->Release();
    if (fileNameBrush)
        fileNameBrush->Release();
    if (pathBrush)
        pathBrush->Release();
    if (excerptBrush)
        excerptBrush->Release();
    if (hoverBgBrush)
        hoverBgBrush->Release();
    if (selectedBgBrush)
        selectedBgBrush->Release();
    if (separatorBrush)
        separatorBrush->Release();
}
void SearchPanel::UpdateLayout(HWND hwnd)
{
    RECT client;
    GetClientRect(hwnd, &client);

    UINT dpi = win32_get_dpi_for_window(hwnd);
    RECT tbRect = win32_titlebar_rect(hwnd);
    int sidebarWidth = win32_dpi_scale(52, dpi);
    int footerHeight = win32_dpi_scale(28, dpi);

    float scale = dpi / 96.0f;
    state_.physicalWidth = static_cast<int>(state_.logicalWidth * scale);

    state_.leftEdge = static_cast<float>(client.left + sidebarWidth);
    state_.rightEdge = state_.leftEdge + static_cast<float>(state_.physicalWidth);
    state_.topEdge = static_cast<float>(tbRect.bottom);
    state_.bottomEdge = static_cast<float>(client.bottom - footerHeight);

    if (!visible_) {
        state_.physicalWidth = 0;
        state_.rightEdge = state_.leftEdge;
    }

    // Position de l'input - le TextInput gère maintenant ses propres paddings
    float inputTop = state_.topEdge + state_.titleHeight + 8.0f;
    float inputH = 32.0f;
    float inputLeft = state_.leftEdge + state_.leftPadding;
    float inputRight = state_.rightEdge - state_.leftPadding;

    // Le TextInput ajoutera automatiquement les paddings de la boîte externe
    searchInput_.SetRect(D2D1::RectF(inputLeft, inputTop, inputRight, inputTop + inputH));

    // La zone de résultats commence après l'input + ses paddings externes
    resultsTop_ = inputTop + inputH + 12.0f;

    // Update scrollbar
    float contentHeight = searchResults_.size() * resultItemHeight_;
    float viewportHeight = state_.bottomEdge - resultsTop_;
    if (viewportHeight < 0) viewportHeight = 0;
    scrollbar_.UpdateLayout(state_.leftEdge, resultsTop_,
                            static_cast<float>(state_.physicalWidth),
                            viewportHeight, contentHeight);
}

void SearchPanel::OnMouseMove(HWND hwnd, POINT clientPoint)
{
    // Handle resize (updates hover/drag state)
    HandleResizeMouseMove(hwnd, clientPoint);

    // If actively resizing, don't process other interactions
    if (state_.isResizing)
    {
        return;
    }

    // If hovering resize zone, don't process other interactions (prevents cursor override)
    if (state_.isHoveringResizeZone)
    {
        return;
    }

    int oldHovered = hoveredResultIndex_;
    hoveredResultIndex_ = HitTestResult(clientPoint);

    // Scrollbar
    scrollbar_.OnMouseMove(clientPoint);

    if (oldHovered != hoveredResultIndex_)
    {
        InvalidateRect(hwnd, NULL, FALSE);
    }
}

void SearchPanel::OnLeftButtonDown(HWND hwnd, POINT clientPoint)
{
    // Handle resize first (from Panel base)
    if (HandleResizeLeftButtonDown(hwnd, clientPoint))
    {
        return;
    }

    // Check scrollbar
    if (scrollbar_.OnLeftButtonDown(clientPoint))
    {
        SetCapture(hwnd);
        return;
    }

    // Check input click
    if (searchInput_.HitTest(clientPoint))
    {
        searchInput_.OnLeftButtonDown(hwnd, clientPoint);
        InvalidateRect(hwnd, NULL, FALSE);
        return;
    }
    else
    {
        // Clicked outside input - lose focus
        if (searchInput_.IsFocused())
        {
            searchInput_.SetFocused(false);
            InvalidateRect(hwnd, NULL, FALSE);
        }
    }

    // Check result click
    int hitIndex = HitTestResult(clientPoint);
    if (hitIndex >= 0)
    {
        // If clicking same item, open it
        if (hitIndex == selectedResultIndex_)
        {
            OpenSelectedResult(hwnd);
        }
        else
        {
            selectedResultIndex_ = hitIndex;
        }
        InvalidateRect(hwnd, NULL, FALSE);
    }
}

void SearchPanel::OnLeftButtonUp(HWND hwnd)
{
    // Handle resize first (from Panel base)
    if (HandleResizeLeftButtonUp(hwnd))
    {
        return;
    }

    scrollbar_.OnLeftButtonUp();
    POINT pt = {0, 0};
    GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);
    searchInput_.OnLeftButtonUp(hwnd, pt);
}

void SearchPanel::OnMouseWheel(HWND hwnd, int delta)
{
    scrollbar_.OnMouseWheel(delta);
    InvalidateRect(hwnd, NULL, FALSE);
}

void SearchPanel::OnChar(wchar_t ch)
{
    if (!searchInput_.IsFocused())
        return;
    searchInput_.OnChar(ch);
}

void SearchPanel::OnKeyDown(WPARAM key)
{
    // Let TextInput handle its keys first
    if (searchInput_.IsFocused())
    {
        searchInput_.OnKeyDown(key);
    }

    // Handle result navigation
    if (key == VK_UP)
    {
        if (selectedResultIndex_ > 0)
        {
            selectedResultIndex_--;
        }
    }
    else if (key == VK_DOWN)
    {
        if (selectedResultIndex_ < (int)searchResults_.size() - 1)
        {
            selectedResultIndex_++;
        }
    }
}

void SearchPanel::SetSearchRoot(const std::wstring &rootPath)
{
    searchRoot_ = rootPath;
}

void SearchPanel::ClearSearch()
{
    searchInput_.SetText(L"");
    searchResults_.clear();
    selectedResultIndex_ = -1;
    hoveredResultIndex_ = -1;
}

void SearchPanel::OpenSelectedResult(HWND hwnd)
{
    if (selectedResultIndex_ >= 0 && selectedResultIndex_ < (int)searchResults_.size())
    {
        const auto &result = searchResults_[selectedResultIndex_];
        // Send message to open file with line number in WPARAM
        // WM_USER + 100: wParam = lineNumber (0-based), lParam = filePath
        SendMessageW(hwnd, WM_USER + 100, (WPARAM)result.lineNumber, (LPARAM)result.filePath.c_str());
    }
}

void SearchPanel::UpdateSearchResults()
{
    {
        std::lock_guard<std::mutex> lg(searchMutex_);
        latestQuery_ = searchInput_.GetText();
    }
    searchCv_.notify_one();
}

int SearchPanel::HitTestResult(POINT clientPoint) const
{
    if (clientPoint.y < resultsTop_ || clientPoint.y > state_.bottomEdge)
        return -1;
    if (clientPoint.x < state_.leftEdge || clientPoint.x > state_.rightEdge)
        return -1;

    float y = resultsTop_ - scrollbar_.GetScrollOffset();
    for (size_t i = 0; i < searchResults_.size(); ++i)
    {
        if (clientPoint.y >= y && clientPoint.y < y + resultItemHeight_)
        {
            return static_cast<int>(i);
        }
        y += resultItemHeight_;
    }
    return -1;
}

// Global accessor
static SearchPanel *g_searchPanel = nullptr;

SearchPanel &GetSearchPanel()
{
    if (!g_searchPanel)
    {
        // This should be managed by PanelManager, but provide fallback
        static SearchPanel instance;
        g_searchPanel = &instance;
    }
    return *g_searchPanel;
}
