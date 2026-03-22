#include "TerminalPanel.h"
#include "TerminalSession.h"
#include "core/explorer/Explorer.h"
#include "helpers/window_helpers.h"
#include "ui/theme/Theme.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <cwctype>

// -----------------------------------------------------------
// Global accessor
// -----------------------------------------------------------
TerminalPanel& GetTerminalPanel()
{
    static TerminalPanel gTerminal;
    return gTerminal;
}

TerminalPanel::TerminalPanel() {}
TerminalPanel::~TerminalPanel()
{
    CloseAll();
    if (fontCollection_)
    {
        fontCollection_->Release();
        fontCollection_ = nullptr;
    }
}

static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), NULL, 0, NULL, NULL);
    if (len <= 0)
        return {};
    std::string out((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), len, NULL, NULL);
    return out;
}

namespace
{
static std::wstring TrimCopy(const std::wstring &value)
{
    size_t start = 0;
    size_t end = value.size();
    while (start < end && std::iswspace(value[start]))
        ++start;
    while (end > start && std::iswspace(value[end - 1]))
        --end;
    return value.substr(start, end - start);
}

static std::wstring StripWrappers(std::wstring value)
{
    value = TrimCopy(value);
    while (!value.empty() &&
           (value.front() == L'"' || value.front() == L'\'' || value.front() == L'[' || value.front() == L'('))
    {
        value.erase(value.begin());
        value = TrimCopy(value);
    }
    while (!value.empty() &&
           (value.back() == L'"' || value.back() == L'\'' || value.back() == L']' || value.back() == L')' ||
            value.back() == L',' || value.back() == L';'))
    {
        value.pop_back();
        value = TrimCopy(value);
    }
    return value;
}

static bool ParseIntSpan(const std::wstring &s, size_t begin, size_t end, int &out)
{
    if (begin >= end || end > s.size())
        return false;
    for (size_t i = begin; i < end; ++i)
    {
        if (!std::iswdigit(s[i]))
            return false;
    }
    try
    {
        out = std::stoi(s.substr(begin, end - begin));
        return true;
    }
    catch (...)
    {
        return false;
    }
}

static bool ResolvePathCandidate(const std::wstring &raw, std::wstring &resolvedPath)
{
    std::wstring candidate = StripWrappers(raw);
    if (candidate.empty())
        return false;

    std::error_code ec;
    std::filesystem::path p(candidate);
    if (std::filesystem::is_regular_file(p, ec))
    {
        resolvedPath = p.wstring();
        return true;
    }

    if (p.is_relative())
    {
        const auto &rootPath = GetExplorerManager().GetState().rootPath;
        if (!rootPath.empty())
        {
            std::filesystem::path fromRoot = std::filesystem::path(rootPath) / p;
            if (std::filesystem::is_regular_file(fromRoot, ec))
            {
                resolvedPath = fromRoot.wstring();
                return true;
            }
        }

        std::filesystem::path cwd = std::filesystem::current_path(ec);
        if (!ec)
        {
            std::filesystem::path fromCwd = cwd / p;
            if (std::filesystem::is_regular_file(fromCwd, ec))
            {
                resolvedPath = fromCwd.wstring();
                return true;
            }
        }
    }

    return false;
}

static bool ResolvePathFromPrefix(const std::wstring &prefix, std::wstring &resolvedPath)
{
    std::wstring trimmed = TrimCopy(prefix);
    if (trimmed.empty())
        return false;

    std::vector<size_t> starts;
    starts.push_back(0);
    for (size_t i = 0; i < trimmed.size(); ++i)
    {
        const wchar_t c = trimmed[i];
        if (c == L' ' || c == L'\t' || c == L'"' || c == L'\'' || c == L'[' || c == L'(')
        {
            if (i + 1 < trimmed.size())
                starts.push_back(i + 1);
        }
    }

    for (auto it = starts.rbegin(); it != starts.rend(); ++it)
    {
        if (ResolvePathCandidate(trimmed.substr(*it), resolvedPath))
            return true;
    }
    return false;
}

static bool TryParseOutputLocation(const std::wstring &line, std::wstring &resolvedPath, int &lineOut, int &colOut)
{
    // Format: path(line,col)
    for (size_t i = 0; i < line.size(); ++i)
    {
        if (line[i] != L'(')
            continue;
        size_t a = i + 1;
        if (a >= line.size() || !std::iswdigit(line[a]))
            continue;
        size_t b = a;
        while (b < line.size() && std::iswdigit(line[b]))
            ++b;
        if (b >= line.size() || line[b] != L',')
            continue;
        size_t c = b + 1;
        if (c >= line.size() || !std::iswdigit(line[c]))
            continue;
        size_t d = c;
        while (d < line.size() && std::iswdigit(line[d]))
            ++d;
        if (d >= line.size() || line[d] != L')')
            continue;

        int ln = 0, col = 0;
        if (!ParseIntSpan(line, a, b, ln) || !ParseIntSpan(line, c, d, col))
            continue;
        if (!ResolvePathFromPrefix(line.substr(0, i), resolvedPath))
            continue;

        lineOut = (std::max)(0, ln - 1);
        colOut = (std::max)(0, col - 1);
        return true;
    }

    // Format: path:line:col
    for (size_t i = 0; i < line.size(); ++i)
    {
        if (line[i] != L':')
            continue;

        size_t a = i + 1;
        if (a >= line.size() || !std::iswdigit(line[a]))
            continue;
        size_t b = a;
        while (b < line.size() && std::iswdigit(line[b]))
            ++b;
        if (b >= line.size() || line[b] != L':')
            continue;
        size_t c = b + 1;
        if (c >= line.size() || !std::iswdigit(line[c]))
            continue;
        size_t d = c;
        while (d < line.size() && std::iswdigit(line[d]))
            ++d;

        int ln = 0, col = 0;
        if (!ParseIntSpan(line, a, b, ln) || !ParseIntSpan(line, c, d, col))
            continue;
        if (!ResolvePathFromPrefix(line.substr(0, i), resolvedPath))
            continue;

        lineOut = (std::max)(0, ln - 1);
        colOut = (std::max)(0, col - 1);
        return true;
    }

    return false;
}
} // namespace

bool TerminalPanel::IsInitialized() const
{
    for (auto& s : sessions_)
        if (s && s->IsInitialized())
            return true;
    return false;
}

int TerminalPanel::GetTerminalCount() const
{
    return (int)sessions_.size();
}

int TerminalPanel::AllocateSessionId()
{
    return nextSessionId_++;
}

void TerminalPanel::SetActiveIndex(int idx)
{
    if (idx < 0 || idx >= (int)sessions_.size())
        return;
    activeIndex_ = idx;
    tabBar_.SetActiveTab(idx);
}

TerminalSession* TerminalPanel::ActiveSession()
{
    if (activeIndex_ < 0 || activeIndex_ >= (int)sessions_.size()) return nullptr;
    return sessions_[activeIndex_].get();
}
const TerminalSession* TerminalPanel::ActiveSession() const
{
    if (activeIndex_ < 0 || activeIndex_ >= (int)sessions_.size()) return nullptr;
    return sessions_[activeIndex_].get();
}

void TerminalPanel::EnsureAtLeastOneSession(HWND hwnd)
{
    if (!sessions_.empty()) return;

    // Crée une session par défaut (non initialisée tant que pas visible/focus si tu veux)
    sessions_.push_back(std::make_unique<TerminalSession>());
    sessionIds_.push_back(AllocateSessionId());
    activeIndex_ = 0;
    SyncTabBar();

    // On peut init direct quand on affiche
    (void)hwnd;
}

void TerminalPanel::EnsureActiveInitialized(HWND hwnd)
{
    TerminalSession* s = ActiveSession();
    if (!s) return;

    if (!s->IsInitialized())
    {
        // startDir vide => current dir, mais normalement tu appelles NewTerminal(startDir) depuis Window
        s->Initialize(hwnd, L"");
        UpdatePseudoConsoleSizeFromPixelsForActive();
        s->SnapToBottomSoon();
    }
}

void TerminalPanel::NewTerminal(HWND hwnd, const std::wstring& startDir)
{
    if (!hwnd) return;

    // crée
    sessions_.push_back(std::make_unique<TerminalSession>());
    sessionIds_.push_back(AllocateSessionId());
    activeIndex_ = (int)sessions_.size() - 1;
    SyncTabBar();

    // init now
    sessions_[activeIndex_]->Initialize(hwnd, startDir);

    // resize to current panel content area
    UpdatePseudoConsoleSizeFromPixelsForActive();
    sessions_[activeIndex_]->SnapToBottomSoon();

    visible_ = true;
    focused_ = true;
}

void TerminalPanel::EnsureSessionExists(HWND hwnd)
{
    EnsureAtLeastOneSession(hwnd);
}

void TerminalPanel::EnsureActiveInit(HWND hwnd)
{
    EnsureActiveInitialized(hwnd);
}

void TerminalPanel::CloseTerminal(int idx)
{
    if (idx < 0 || idx >= (int)sessions_.size())
        return;

    sessions_[idx]->Shutdown();
    sessions_.erase(sessions_.begin() + idx);
    if (idx < (int)sessionIds_.size())
        sessionIds_.erase(sessionIds_.begin() + idx);
    SyncTabBar();

    if (sessions_.empty())
    {
        activeIndex_ = -1;
        visible_ = false;
        focused_ = false;
        SyncTabBar();
        return;
    }

    if (activeIndex_ >= (int)sessions_.size())
        activeIndex_ = (int)sessions_.size() - 1;
    tabBar_.SetActiveTab(activeIndex_);
}

void TerminalPanel::CloseAll()
{
    for (auto& s : sessions_)
        if (s) s->Shutdown();
    sessions_.clear();
    sessionIds_.clear();
    activeIndex_ = -1;
    visible_ = false;
    focused_ = false;
    if (mouseCaptureOwned_ && GetCapture() != NULL)
        ReleaseCapture();
    mouseCaptureOwned_ = false;
    SyncTabBar();
}

// ---------------- Visible / Focus ----------------
void TerminalPanel::ToggleVisible()
{
    visible_ = !visible_;
    if (!visible_)
    {
        focused_ = false;
        if (mouseCaptureOwned_ && GetCapture() != NULL)
            ReleaseCapture();
        mouseCaptureOwned_ = false;
    }
}

void TerminalPanel::SetVisible(bool v)
{
    visible_ = v;
    if (!visible_)
    {
        focused_ = false;
        if (mouseCaptureOwned_ && GetCapture() != NULL)
            ReleaseCapture();
        mouseCaptureOwned_ = false;
    }
}

void TerminalPanel::Unfocus()
{
    focused_ = false;
}

// ---------------- Font control ----------------
void TerminalPanel::SetFont(const std::wstring& family, float sizePx)
{
    fontFamily_ = family;
    fontSize_ = sizePx;
    UpdatePseudoConsoleSizeFromPixelsForActive();
}

void TerminalPanel::SetFontCollection(IDWriteFontCollection* fc)
{
    if (fc)
        fc->AddRef();
    if (fontCollection_)
        fontCollection_->Release();
    fontCollection_ = fc;
    UpdatePseudoConsoleSizeFromPixelsForActive();
}

// ---------------- Layout ----------------
void TerminalPanel::UpdateLayout(HWND hwnd, float left, float top, float right, float bottom)
{
    (void)hwnd;

    left_ = left; top_ = top; right_ = right; bottom_ = bottom;
    heightPx_ = bottom_ - top_;

    state_.leftEdge = left;
    state_.topEdge = top;
    state_.rightEdge = right;
    state_.bottomEdge = bottom;
    {
        float w = right - left;
        if (w < 0.0f) w = 0.0f;
        state_.physicalWidth = (int)w;
    }

    SyncTabBar();
    tabBar_.UpdateLayout(left_, top_ + resizeZoneH_, TabsBarRightEdge());

    // Session viewport inside the rounded content card
    TerminalSession* s = ActiveSession();
    if (s)
    {
        const float pad = 8.0f;
        float tabsH = TabsBarHeightPx();
        float cardTop    = top_ + resizeZoneH_ + tabsH + pad + 4.0f;
        float cardLeft   = left_  + pad + 4.0f;
        float cardRight  = right_ - pad - 4.0f;
        float cardBottom = bottom_ - pad - 4.0f;
        s->SetViewport(cardLeft, cardTop, cardRight, cardBottom);
        UpdatePseudoConsoleSizeFromPixelsForActive();
    }
}

bool TerminalPanel::IsPointInPanel(POINT pt) const
{
    if (!visible_) return false;
    return pt.x >= (LONG)left_ && pt.x <= (LONG)right_
        && pt.y >= (LONG)top_ && pt.y <= (LONG)bottom_;
}

bool TerminalPanel::IsPointInResizeZone(POINT pt) const
{
    if (!visible_) return false;
    return pt.x >= (LONG)left_ && pt.x <= (LONG)right_
        && pt.y >= (LONG)top_ && pt.y <= (LONG)(top_ + resizeZoneH_);
}

bool TerminalPanel::IsPointInTabsBarArea(POINT pt) const
{
    if (!visible_) return false;
    return IsPointInTabsBar(pt);
}

bool TerminalPanel::IsPointInPlusButton(POINT pt) const
{
    if (!visible_) return false;
    return HitTestPlus(pt);
}

// ---------------- Tabs UI rects ----------------
RECT TerminalPanel::TabsBarRectClient() const
{
    RECT r;
    r.left = (LONG)left_;
    r.top = (LONG)(top_ + resizeZoneH_);
    r.right = (LONG)right_;
    r.bottom = (LONG)(top_ + resizeZoneH_ + TabsBarHeightPx());
    return r;
}

RECT TerminalPanel::SessionTabRectClient(int index) const
{
    RECT t = TabsBarRectClient();
    int x = t.left + 10 + index * 128;
    RECT r;
    r.left = x;
    r.right = x + 116;
    r.top = t.top + 4;
    r.bottom = t.bottom - 4;
    return r;
}

RECT TerminalPanel::SessionCloseRectClient(int index) const
{
    RECT tab = SessionTabRectClient(index);
    RECT r;
    r.right = tab.right - 6;
    r.left = r.right - 14;
    r.top = tab.top + ((tab.bottom - tab.top) - 14) / 2;
    r.bottom = r.top + 14;
    return r;
}

float TerminalPanel::TabsBarRightEdge() const
{
    RECT output = OutputButtonRectClient();
    float rightEdge = (float)output.left - 6.0f;
    if (rightEdge < left_)
        rightEdge = left_;
    return rightEdge;
}

bool TerminalPanel::HitTestSessionTab(int index, POINT pt) const
{
    RECT r = SessionTabRectClient(index);
    return pt.x >= r.left && pt.x <= r.right && pt.y >= r.top && pt.y <= r.bottom;
}

bool TerminalPanel::HitTestSessionClose(int index, POINT pt) const
{
    RECT r = SessionCloseRectClient(index);
    return pt.x >= r.left && pt.x <= r.right && pt.y >= r.top && pt.y <= r.bottom;
}

RECT TerminalPanel::MinimizeButtonRectClient() const
{
    RECT t = TabsBarRectClient();
    int size = (int)TabsBarHeightPx() - 8;
    if (size < 14) size = 14;
    RECT r;
    r.right  = t.right - 8;
    r.left   = r.right - size;
    r.top    = t.top + 4;
    r.bottom = t.bottom - 4;
    return r;
}

bool TerminalPanel::HitTestMinimize(POINT pt) const
{
    RECT r = MinimizeButtonRectClient();
    return (pt.x >= r.left && pt.x <= r.right && pt.y >= r.top && pt.y <= r.bottom);
}

RECT TerminalPanel::PlusButtonRectClient() const
{
    RECT mini = MinimizeButtonRectClient();
    int size = (int)TabsBarHeightPx() - 6;
    if (size < 14) size = 14;
    RECT r;
    r.right  = mini.left - 6;
    r.left   = r.right - size;
    r.top    = mini.top - 2;
    r.bottom = mini.bottom + 2;
    return r;
}

RECT TerminalPanel::ProblemsButtonRectClient() const
{
    RECT plus = PlusButtonRectClient();
    int height = (int)TabsBarHeightPx();
    int width = (int)std::round(height * 4.6f);
    RECT r;
    r.right = plus.left - 6;
    r.left = r.right - width;
    r.top = plus.top + 3;
    r.bottom = plus.bottom - 3;
    return r;
}

RECT TerminalPanel::OutputButtonRectClient() const
{
    RECT problems = ProblemsButtonRectClient();
    int height = (int)TabsBarHeightPx();
    int width = (int)std::round(height * 4.1f);
    RECT r;
    r.right = problems.left - 6;
    r.left = r.right - width;
    r.top = problems.top;
    r.bottom = problems.bottom;
    return r;
}

bool TerminalPanel::HitTestPlus(POINT pt) const
{
    RECT r = PlusButtonRectClient();
    return (pt.x >= r.left && pt.x <= r.right && pt.y >= r.top && pt.y <= r.bottom);
}

bool TerminalPanel::HitTestProblems(POINT pt) const
{
    RECT r = ProblemsButtonRectClient();
    return (pt.x >= r.left && pt.x <= r.right && pt.y >= r.top && pt.y <= r.bottom);
}

bool TerminalPanel::HitTestOutput(POINT pt) const
{
    RECT r = OutputButtonRectClient();
    return (pt.x >= r.left && pt.x <= r.right && pt.y >= r.top && pt.y <= r.bottom);
}

bool TerminalPanel::HitTestOutputCopy(POINT pt) const
{
    return pt.x >= outputCopyRect_.left && pt.x <= outputCopyRect_.right &&
           pt.y >= outputCopyRect_.top && pt.y <= outputCopyRect_.bottom;
}

bool TerminalPanel::IsPointInTabsBar(POINT pt) const
{
    RECT t = TabsBarRectClient();
    return (pt.x >= t.left && pt.x <= t.right && pt.y >= t.top && pt.y <= t.bottom);
}

void TerminalPanel::SyncTabBar()
{
    int desired = (int)sessions_.size();
    int current = tabBar_.GetTabCount();

    while (current < desired)
    {
        tabBar_.AddTab(L"", L"");
        current++;
    }

    while (current > desired)
    {
        tabBar_.CloseTab(current - 1);
        current--;
    }

    for (int i = 0; i < desired; ++i)
    {
        int id = (i < (int)sessionIds_.size()) ? sessionIds_[i] : (i + 1);
        std::wstring title = L"Terminal " + std::to_wstring(id);
        tabBar_.UpdateTabPath(i, L"", title);
    }

    if (activeIndex_ >= 0 && activeIndex_ < desired)
        tabBar_.SetActiveTab(activeIndex_);
}

void TerminalPanel::UpdateHoveredOutputLink(POINT pt)
{
    hoveredOutputLink_ = {};

    if (!showOutput_)
        return;
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) == 0)
        return;
    if (outputLineHeight_ <= 0.0f)
        return;
    if (HitTestOutputCopy(pt))
        return;
    if (pt.x < outputBodyRect_.left || pt.x > outputBodyRect_.right ||
        pt.y < outputBodyRect_.top || pt.y > outputBodyRect_.bottom)
        return;

    float scrollOffset = outputScrollbar_.GetScrollOffset();
    float localY = (float)pt.y + scrollOffset - outputBodyStartY_;
    if (localY < 0.0f)
        return;

    int idx = (int)(localY / outputLineHeight_);
    std::wstring line;
    {
        std::lock_guard<std::mutex> lock(outputMutex_);
        if (idx < 0 || idx >= (int)outputLines_.size())
            return;
        line = outputLines_[idx];
    }

    std::wstring path;
    int lineNo = -1;
    int colNo = -1;
    if (!TryParseOutputLocation(line, path, lineNo, colNo))
        return;

    hoveredOutputLink_.active = true;
    hoveredOutputLink_.filePath = std::move(path);
    hoveredOutputLink_.line = lineNo;
    hoveredOutputLink_.column = colNo;
    hoveredOutputLink_.lineIndex = idx;
}

// ---------------- Mouse ----------------
void TerminalPanel::OnLeftButtonDown(HWND hwnd, POINT pt)
{
    if (!visible_) return;

    if (HitTestMinimize(pt))
    {
        SetVisible(false);
        return;
    }

    if (HitTestOutput(pt))
    {
        showOutput_ = !showOutput_;
        if (showOutput_)
            showProblems_ = false;
        hoveredOutputLink_ = {};
        return;
    }

    if (HitTestProblems(pt))
    {
        showProblems_ = !showProblems_;
        if (showProblems_)
            showOutput_ = false;
        hoveredProblemIndex_ = -1;
        hoveredOutputLink_ = {};
        return;
    }

    // Plus click => NewTerminal (startDir choisi par Window normalement via menu)
    if (HitTestPlus(pt))
    {
        // Ici: fallback current dir
        NewTerminal(hwnd, L"");
        return;
    }

    if (showOutput_ && IsPointInPanel(pt))
    {
        UpdateHoveredOutputLink(pt);
        if (hoveredOutputLink_.active)
        {
            OpenFileRequest *req = new OpenFileRequest();
            req->filePath = hoveredOutputLink_.filePath;
            req->line = hoveredOutputLink_.line;
            req->column = hoveredOutputLink_.column;
            PostMessageW(hwnd, WM_OPEN_FILE_AT, 0, (LPARAM)req);
            return;
        }

        if (HitTestOutputCopy(pt))
        {
            std::wstring all;
            {
                std::lock_guard<std::mutex> lock(outputMutex_);
                for (const auto& l : outputLines_)
                {
                    all.append(l);
                    all.push_back(L'\n');
                }
                if (!outputBuffer_.empty())
                    all.append(outputBuffer_);
            }

            if (!all.empty())
            {
                bool copied = false;
                if (OpenClipboard(hwnd))
                {
                    EmptyClipboard();
                    size_t bytes = (all.size() + 1) * sizeof(wchar_t);
                    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
                    if (mem)
                    {
                        void* dst = GlobalLock(mem);
                        memcpy(dst, all.c_str(), bytes);
                        GlobalUnlock(mem);
                        if (SetClipboardData(CF_UNICODETEXT, mem))
                            copied = true;
                    }
                    CloseClipboard();
                }
                if (copied)
                {
                    outputCopyFeedback_ = true;
                    outputCopyFeedbackUntil_ = GetTickCount() + 1200;
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }

        if (outputScrollbar_.OnLeftButtonDown(pt))
        {
            outputAutoFollow_ = false;
            outputPendingScrollToBottom_ = false;
            SetCapture(hwnd);
            mouseCaptureOwned_ = true;
            return;
        }
    }

    if (showProblems_ && IsPointInPanel(pt))
    {
        if (!IsPointInTabsBar(pt) && !IsPointInResizeZone(pt))
        {
            int idx = -1;
            if (problemsRowHeight_ > 0.0f &&
                pt.x >= problemsListRect_.left && pt.x <= problemsListRect_.right &&
                pt.y >= problemsListRect_.top && pt.y <= problemsListRect_.bottom)
            {
                idx = (int)((pt.y - problemsListRect_.top) / problemsRowHeight_);
            }

            if (idx >= 0 && idx < (int)problems_.size())
            {
                const auto &p = problems_[idx];
                int targetLine = p.line - 1;
                int targetCol = p.column - 1;
                if (targetLine < 0)
                    targetLine = 0;
                if (targetCol < 0)
                    targetCol = 0;

                OpenFileRequest *req = new OpenFileRequest();
                req->filePath = problemsFilePath_;
                req->line = targetLine;
                req->column = targetCol;
                PostMessageW(hwnd, WM_OPEN_FILE_AT, 0, (LPARAM)req);
                return;
            }
        }
    }

    if ((showOutput_ || showProblems_) && IsPointInPanel(pt))
    {
        if (!IsPointInTabsBar(pt) && !IsPointInResizeZone(pt))
        {
            // Keep terminal focused when interacting with output/problems panel
            focused_ = true;
            return;
        }
    }

    if (IsPointInTabsBar(pt))
    {
        for (int i = 0; i < (int)sessions_.size(); ++i)
        {
            if (sessions_.size() > 1 && HitTestSessionClose(i, pt))
            {
                CloseTerminal(i);
                return;
            }
            if (HitTestSessionTab(i, pt))
            {
                SetActiveIndex(i);
                showOutput_ = false;
                showProblems_ = false;
                focused_ = true;
                EnsureActiveInitialized(hwnd);
                return;
            }
        }
    }

    // Resize zone
    if (IsPointInResizeZone(pt))
    {
        resizing_ = true;
        dragStart_ = pt;
        startTop_ = top_;
        SetCapture(hwnd);
        mouseCaptureOwned_ = true;
        return;
    }

    // Scrollbar capture for active session
    if (TerminalSession* s = ActiveSession())
    {
        if (s->OnScrollbarLButtonDown(pt))
        {
            SetCapture(hwnd);
            mouseCaptureOwned_ = true;
            focused_ = true;
            return;
        }
    }

    if (IsPointInPanel(pt))
    {
        focused_ = true;
        if (TerminalSession* s = ActiveSession())
        {
            if (s->OnLeftButtonDown(pt))
            {
                SetCapture(hwnd);
                mouseCaptureOwned_ = true;
            }
        }
    }
}

void TerminalPanel::OnLeftButtonUp(HWND hwnd)
{
    (void)hwnd;

    if (showOutput_)
    {
        if (outputScrollbar_.OnLeftButtonUp())
        {
            ReleaseCapture();
            mouseCaptureOwned_ = false;
            return;
        }
    }

    if (TerminalSession* s = ActiveSession())
    {
        if (s->OnScrollbarLButtonUp())
        {
            ReleaseCapture();
            mouseCaptureOwned_ = false;
            return;
        }
    }

    if (TerminalSession* s = ActiveSession())
    {
        if (s->OnLeftButtonUp())
        {
            ReleaseCapture();
            mouseCaptureOwned_ = false;
            return;
        }
    }

    if (resizing_)
    {
        resizing_ = false;
        ReleaseCapture();
        mouseCaptureOwned_ = false;
        return;
    }

    if (mouseCaptureOwned_)
    {
        if (GetCapture() != NULL)
            ReleaseCapture();
        mouseCaptureOwned_ = false;
    }
}

bool TerminalPanel::OnMouseMove(HWND hwnd, POINT pt)
{
    (void)hwnd;
    if (!visible_) return false;

    bool changed = false;
    bool lmbDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

    // Resizing drag should not forward events to tabs/scrollbar/session
    if (resizing_)
    {
        int dy = (pt.y - dragStart_.y);
        float newTop = startTop_ + (float)dy;

        float maxTop = bottom_ - minHeight_;
        if (newTop < 0.0f) newTop = 0.0f;
        if (newTop > maxTop) newTop = maxTop;

        if (newTop != top_)
        {
            top_ = newTop;
            heightPx_ = bottom_ - top_;

            // update state + viewport
            state_.topEdge = top_;

    if (TerminalSession* s = ActiveSession())
    {
    float tabsH = TabsBarHeightPx();
    s->SetViewport(left_, top_ + resizeZoneH_, right_, bottom_ - tabsH);
                UpdatePseudoConsoleSizeFromPixelsForActive();
            }
            return true;
        }
        return changed;
    }

    bool inResizeZone = IsPointInResizeZone(pt);
    if (inResizeZone && !lmbDown)
    {
        bool prev = resizeHover_;
        resizeHover_ = true;
        if (prev != resizeHover_)
            changed = true;
        if (hoveredPlus_)
        {
            hoveredPlus_ = false;
            changed = true;
        }
        if (tabBar_.ClearHover())
            changed = true;
        return changed;
    }

    // Hover tabs / plus
    bool prevPlus = hoveredPlus_;
    hoveredPlus_ = HitTestPlus(pt);
    if (prevPlus != hoveredPlus_)
        changed = true;

    bool prevProblems = hoveredProblems_;
    hoveredProblems_ = HitTestProblems(pt);
    if (prevProblems != hoveredProblems_)
        changed = true;

    bool prevOutput = hoveredOutput_;
    hoveredOutput_ = HitTestOutput(pt);
    if (prevOutput != hoveredOutput_)
        changed = true;

    bool prevMinimize = hoveredMinimize_;
    hoveredMinimize_ = HitTestMinimize(pt);
    if (prevMinimize != hoveredMinimize_)
        changed = true;

    int prevProblemRow = hoveredProblemIndex_;
    hoveredProblemIndex_ = -1;
    if (showProblems_ && problemsRowHeight_ > 0.0f &&
        pt.x >= problemsListRect_.left && pt.x <= problemsListRect_.right &&
        pt.y >= problemsListRect_.top && pt.y <= problemsListRect_.bottom)
    {
        int idx = (int)((pt.y - problemsListRect_.top) / problemsRowHeight_);
        if (idx >= 0 && idx < (int)problems_.size())
            hoveredProblemIndex_ = idx;
    }
    if (prevProblemRow != hoveredProblemIndex_)
        changed = true;

    if (showOutput_)
    {
        bool prevCopy = hoveredOutputCopy_;
        hoveredOutputCopy_ = HitTestOutputCopy(pt);
        if (prevCopy != hoveredOutputCopy_)
            changed = true;

        TerminalPanel::HoveredOutputLink prevLink = hoveredOutputLink_;
        UpdateHoveredOutputLink(pt);
        if (prevLink.active != hoveredOutputLink_.active ||
            prevLink.lineIndex != hoveredOutputLink_.lineIndex ||
            prevLink.filePath != hoveredOutputLink_.filePath)
        {
            changed = true;
        }
    }
    else if (hoveredOutputLink_.active)
    {
        hoveredOutputLink_ = {};
        changed = true;
    }

    int prevSessionTab = hoveredSessionTab_;
    int prevSessionClose = hoveredSessionClose_;
    hoveredSessionTab_ = -1;
    hoveredSessionClose_ = -1;
    if (IsPointInTabsBar(pt) && !hoveredPlus_ && !hoveredProblems_ && !hoveredOutput_)
    {
        for (int i = 0; i < (int)sessions_.size(); ++i)
        {
            if (sessions_.size() > 1 && HitTestSessionClose(i, pt))
            {
                hoveredSessionTab_ = i;
                hoveredSessionClose_ = i;
                break;
            }
            if (HitTestSessionTab(i, pt))
            {
                hoveredSessionTab_ = i;
                break;
            }
        }
    }
    if (prevSessionTab != hoveredSessionTab_ || prevSessionClose != hoveredSessionClose_)
        changed = true;

    // Scrollbar hover/drag
    if (TerminalSession* s = ActiveSession())
    {
        if (s->OnScrollbarMouseMove(pt))
            changed = true;
    }

    if (showOutput_)
    {
        if (outputScrollbar_.OnMouseMove(pt))
            changed = true;
        if (outputScrollbar_.IsDragging())
            outputAutoFollow_ = false;
    }

    if (TerminalSession* s = ActiveSession())
    {
        if (s->OnMouseMove(pt, lmbDown))
            changed = true;
    }

    bool prev = resizeHover_;
    resizeHover_ = false;
    if (prev != resizeHover_)
        changed = true;

    return changed;
}

void TerminalPanel::OnMouseWheel(HWND hwnd, int wheelDelta)
{
    (void)hwnd;
    if (!visible_) return;

    if (showOutput_)
    {
        if (outputScrollbar_.OnMouseWheel(wheelDelta))
        {
            outputAutoFollow_ = false;
            outputPendingScrollToBottom_ = false;
        }
        return;
    }

    if (TerminalSession* s = ActiveSession())
        s->OnMouseWheel(wheelDelta);
}

// ---------------- Keyboard ----------------
void TerminalPanel::OnChar(wchar_t ch)
{
    if (!visible_ || !focused_) return;
    EnsureActiveInitialized(NULL); // safety if needed

    if (TerminalSession* s = ActiveSession())
        s->OnChar(ch);
}

void TerminalPanel::OnKeyDown(WPARAM vk)
{
    if (!visible_ || !focused_) return;
    EnsureActiveInitialized(NULL); // safety if needed

    if (TerminalSession* s = ActiveSession())
        s->OnKeyDown(vk);
}

// ---------------- ConPTY -> VTerm feed ----------------
void TerminalPanel::HandleConPTYOutput(const char* data, size_t len)
{
    TerminalSession* s = ActiveSession();
    if (!s) return;
    s->HandleConPTYOutput(data, len);
}

// ---------------- Resize pseudo console from panel pixels ----------------
void TerminalPanel::UpdatePseudoConsoleSizeFromPixelsForActive()
{
    TerminalSession* s = ActiveSession();
    if (!s || !s->IsInitialized())
        return;

    float tabsH = TabsBarHeightPx();

    float contentW = (right_ - left_) - 20.0f;
    float contentH = bottom_ - (top_ + resizeZoneH_ + tabsH);

    s->UpdatePseudoConsoleSizeFromPixels(contentW, contentH, fontFamily_, fontSize_, fontCollection_);
}

// ---------------- Render ----------------
void TerminalPanel::Draw(ID2D1RenderTarget* rt, IDWriteFactory* dwrite)
{
    Draw(rt, dwrite, GetActiveWindow());
}

void TerminalPanel::Draw(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, HWND hwnd)
{
    if (!visible_ || !rt || !dwrite) return;
    const UI::Theme::Palette &themePalette = UI::Theme::GetPalette();
    const bool lightMode = (UI::Theme::GetMode() == UI::Theme::Mode::Light);

    EnsureAtLeastOneSession(NULL);
    SyncTabBar();
    tabBar_.UpdateLayout(left_, top_ + resizeZoneH_, TabsBarRightEdge());

    // Brushes
    ID2D1SolidColorBrush* bg = nullptr;
    ID2D1SolidColorBrush* fg = nullptr;
    ID2D1SolidColorBrush* muted = nullptr;
    ID2D1SolidColorBrush* border = nullptr;
    ID2D1SolidColorBrush* accentBrush = nullptr;
    ID2D1SolidColorBrush* resizeLine = nullptr;

    rt->CreateSolidColorBrush(UI::Theme::ChromeBackground(), &bg);
    rt->CreateSolidColorBrush(UI::Theme::PrimaryText(), &fg);
    rt->CreateSolidColorBrush(UI::Theme::MutedText(), &muted);
    D2D1_COLOR_F terminalBorderColor = UI::Theme::ChromeBorder();
    terminalBorderColor.a = lightMode ? 0.72f : 0.46f;
    rt->CreateSolidColorBrush(terminalBorderColor, &border);
    rt->CreateSolidColorBrush(UI::Theme::Accent(), &accentBrush);
    rt->CreateSolidColorBrush(resizeHover_ || resizing_ ? UI::Theme::Accent() : UI::Theme::ChromeBorder(), &resizeLine);

    if (!bg || !fg || !muted || !border || !accentBrush || !resizeLine)
    {
        if (bg) bg->Release();
        if (fg) fg->Release();
        if (muted) muted->Release();
        if (border) border->Release();
        if (accentBrush) accentBrush->Release();
        if (resizeLine) resizeLine->Release();
        return;
    }

    float tabsH = TabsBarHeightPx();
    // Toolbar spans the full top: resize zone (invisible) + session tabs
    float toolbarBottom = top_ + resizeZoneH_ + tabsH;

    // Padding around the content card
    const float pad = 8.0f;
    const float radius = 7.0f;
    float contentLeft   = left_  + pad;
    float contentRight  = right_ - pad;
    float contentTop    = toolbarBottom + pad;
    float contentBottom = bottom_ - pad;
    D2D1_RECT_F contentRect = D2D1::RectF(contentLeft, contentTop, contentRight, contentBottom);

    // Step 1: fill entire panel fully opaque — blocks editor content behind the panel
    {
        D2D1_COLOR_F solidBg = UI::Theme::ChromeBackground();
        solidBg.a = 1.0f;
        ID2D1SolidColorBrush* opaqueBg = nullptr;
        rt->CreateSolidColorBrush(solidBg, &opaqueBg);
        if (opaqueBg)
        {
            rt->FillRectangle(D2D1::RectF(left_, top_, right_, bottom_), opaqueBg);
            opaqueBg->Release();
        }
    }

    // Step 2: keep the toolbar in the same visual family as the terminal body
    {
        ID2D1SolidColorBrush* toolbarBg = nullptr;
        D2D1_COLOR_F tint = UI::Theme::ChromeBackground();
        tint.a = 1.0f;
        rt->CreateSolidColorBrush(tint, &toolbarBg);
        if (toolbarBg)
        {
            rt->FillRectangle(D2D1::RectF(left_, top_, right_, toolbarBottom), toolbarBg);
            toolbarBg->Release();
        }
    }

    // Resize handle: only visible when hovered
    if (resizeHover_ || resizing_)
    {
        float gripY = top_ + resizeZoneH_ * 0.5f;
        rt->DrawLine(D2D1::Point2F(left_ + 40.0f, gripY),
                     D2D1::Point2F(right_ - 40.0f, gripY), resizeLine, 1.5f);
    }

    // Toolbar bottom separator (very thin, same as chrome borders)
    {
        float sepY = std::round(toolbarBottom) - 0.5f;
        rt->DrawLine(D2D1::Point2F(left_, sepY), D2D1::Point2F(right_, sepY), border, 0.8f);
    }

    // Session tabs
    auto drawSessionTab = [&](int index)
    {
        const Tab* tab = tabBar_.GetTab(index);
        if (!tab)
            return;

        RECT rc = SessionTabRectClient(index);
        D2D1_RECT_F r = D2D1::RectF((float)rc.left, (float)rc.top, (float)rc.right, (float)rc.bottom);
        bool active = index == activeIndex_;
        bool hovered = index == hoveredSessionTab_;

        IDWriteTextFormat* tabFmt = nullptr;
        dwrite->CreateTextFormat(
            L"Segoe UI Variable Text",
            NULL,
            active ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            12.0f,
            L"en-us",
            &tabFmt);
        if (tabFmt)
        {
            tabFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            tabFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            ID2D1SolidColorBrush* textBr = (active || hovered) ? fg : muted;
            float textRight = r.right - (sessions_.size() > 1 ? 18.0f : 0.0f);
            rt->DrawTextW(
                tab->displayName.c_str(),
                (UINT32)tab->displayName.size(),
                tabFmt,
                D2D1::RectF(r.left, r.top, textRight, r.bottom),
                textBr);
            tabFmt->Release();
        }

        D2D1_COLOR_F underlineColor = active ? UI::Theme::Accent() : UI::Theme::ChromeBorder();
        underlineColor.a = active ? 0.96f : (hovered ? 0.55f : 0.0f);
        if (underlineColor.a > 0.0f)
        {
            ID2D1SolidColorBrush* underlineBrush = nullptr;
            rt->CreateSolidColorBrush(underlineColor, &underlineBrush);
            if (underlineBrush)
            {
                float y = r.bottom - (active ? 2.0f : 1.0f);
                rt->DrawLine(
                    D2D1::Point2F(r.left + 2.0f, y),
                    D2D1::Point2F(r.right - 2.0f, y),
                    underlineBrush,
                    active ? 2.0f : 1.0f);
                underlineBrush->Release();
            }
        }

        if (sessions_.size() > 1)
        {
            RECT closeRc = SessionCloseRectClient(index);
            D2D1_RECT_F closeRect = D2D1::RectF((float)closeRc.left, (float)closeRc.top, (float)closeRc.right, (float)closeRc.bottom);
            bool closeHovered = index == hoveredSessionClose_;
            D2D1_COLOR_F closeColor = closeHovered ? UI::Theme::PrimaryText() : UI::Theme::MutedText();
            closeColor.a = closeHovered ? 0.95f : 0.62f;
            ID2D1SolidColorBrush* closeBrush = nullptr;
            rt->CreateSolidColorBrush(closeColor, &closeBrush);
            if (closeBrush)
            {
                const float pad = 4.0f;
                rt->DrawLine(
                    D2D1::Point2F(closeRect.left + pad, closeRect.top + pad),
                    D2D1::Point2F(closeRect.right - pad, closeRect.bottom - pad),
                    closeBrush,
                    1.1f);
                rt->DrawLine(
                    D2D1::Point2F(closeRect.left + pad, closeRect.bottom - pad),
                    D2D1::Point2F(closeRect.right - pad, closeRect.top + pad),
                    closeBrush,
                    1.1f);
                closeBrush->Release();
            }
        }
    };

    for (int i = 0; i < (int)sessions_.size(); ++i)
        drawSessionTab(i);

    // Content card: rounded rect — bg already fills behind, just draw the border
    rt->DrawRoundedRectangle(D2D1::RoundedRect(contentRect, radius, radius), border, 0.9f);


    // Helper to draw a modern tab button with accent underline
    auto drawTabButton = [&](const RECT& btnRect, const std::wstring& label, bool active, bool hovered)
    {
        D2D1_RECT_F r = D2D1::RectF((float)btnRect.left, (float)btnRect.top, (float)btnRect.right, (float)btnRect.bottom);

        // Hover fill
        if (hovered && !active)
        {
            ID2D1SolidColorBrush* hoverBg = nullptr;
            rt->CreateSolidColorBrush(themePalette.explorerRowHover, &hoverBg);
            if (hoverBg)
            {
                D2D1_COLOR_F hoverColor = themePalette.explorerRowHover;
                hoverColor.a = 0.45f;
                hoverBg->SetColor(hoverColor);
                rt->FillRoundedRectangle(D2D1::RoundedRect(r, 4.0f, 4.0f), hoverBg);
                hoverBg->Release();
            }
        }

        // Text
        IDWriteTextFormat* tabFmt = nullptr;
        dwrite->CreateTextFormat(
            L"Segoe UI Variable Text",
            NULL,
            active ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            12.0f,
            L"en-us",
            &tabFmt);
        if (tabFmt)
        {
            tabFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            tabFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            ID2D1SolidColorBrush* textBr = (active || hovered) ? fg : muted;
            rt->DrawTextW(label.c_str(), (UINT32)label.size(), tabFmt, r, textBr);
            tabFmt->Release();
        }

        // Accent underline for active tab
        if (active)
        {
            float underlineH = 2.5f;
            D2D1_RECT_F underline = D2D1::RectF(
                r.left + 6.0f, r.bottom - underlineH - 2.0f,
                r.right - 6.0f, r.bottom - 2.0f);
            rt->FillRoundedRectangle(D2D1::RoundedRect(underline, 1.5f, 1.5f), accentBrush);
        }
    };

    // Output tab
    {
        size_t outputCount = 0;
        {
            std::lock_guard<std::mutex> lock(outputMutex_);
            outputCount = outputLines_.size();
        }
        RECT out = OutputButtonRectClient();
        std::wstring outLabel = L"Output (" + std::to_wstring(outputCount) + L")";
        drawTabButton(out, outLabel, showOutput_, hoveredOutput_);
    }

    // Problems tab
    {
        RECT prb = ProblemsButtonRectClient();
        std::wstring prbLabel = L"Probl\u00E8mes (" + std::to_wstring(problems_.size()) + L")";
        drawTabButton(prb, prbLabel, showProblems_, hoveredProblems_);
    }

    // Plus button
    RECT pr = PlusButtonRectClient();
    D2D1_RECT_F plus = D2D1::RectF((float)pr.left, (float)pr.top, (float)pr.right, (float)pr.bottom);

    if (hoveredPlus_)
    {
        ID2D1SolidColorBrush* plusBg = nullptr;
        rt->CreateSolidColorBrush(themePalette.explorerRowHover, &plusBg);
        if (plusBg)
        {
            rt->FillRoundedRectangle(D2D1::RoundedRect(plus, 3.0f, 3.0f), plusBg);
            plusBg->Release();
        }
    }

    // draw "+"
    float cx = (plus.left + plus.right) * 0.5f;
    float cy = (plus.top + plus.bottom) * 0.5f;
    float halfSize = (plus.bottom - plus.top) * 0.28f;

    ID2D1SolidColorBrush* plusStroke = nullptr;
    D2D1_COLOR_F plusStrokeColor = hoveredPlus_
        ? UI::Theme::PrimaryText()
        : UI::Theme::MutedText();
    rt->CreateSolidColorBrush(plusStrokeColor, &plusStroke);
    if (plusStroke)
    {
        rt->DrawLine(D2D1::Point2F(cx - halfSize, cy), D2D1::Point2F(cx + halfSize, cy), plusStroke, 1.2f);
        rt->DrawLine(D2D1::Point2F(cx, cy - halfSize), D2D1::Point2F(cx, cy + halfSize), plusStroke, 1.2f);
        plusStroke->Release();
    }


    // Minimize button (−)
    {
        RECT mr = MinimizeButtonRectClient();
        D2D1_RECT_F miniRect = D2D1::RectF((float)mr.left, (float)mr.top, (float)mr.right, (float)mr.bottom);

        if (hoveredMinimize_)
        {
            ID2D1SolidColorBrush* miniBg = nullptr;
            rt->CreateSolidColorBrush(themePalette.explorerRowHover, &miniBg);
            if (miniBg)
            {
                rt->FillRoundedRectangle(D2D1::RoundedRect(miniRect, 3.0f, 3.0f), miniBg);
                miniBg->Release();
            }
        }

        float mcx = (miniRect.left + miniRect.right) * 0.5f;
        float mcy = (miniRect.top + miniRect.bottom) * 0.5f;
        float halfW = (miniRect.right - miniRect.left) * 0.28f;

        ID2D1SolidColorBrush* miniStroke = nullptr;
        D2D1_COLOR_F miniColor = hoveredMinimize_
            ? UI::Theme::PrimaryText()
            : UI::Theme::MutedText();
        rt->CreateSolidColorBrush(miniColor, &miniStroke);
        if (miniStroke)
        {
            rt->DrawLine(D2D1::Point2F(mcx - halfW, mcy), D2D1::Point2F(mcx + halfW, mcy), miniStroke, 1.5f);
            miniStroke->Release();
        }
    }

    // Content viewport
    if (!showOutput_)
    {
        outputBodyRect_ = D2D1::RectF(0, 0, 0, 0);
        outputBodyStartY_ = 0.0f;
        outputLineHeight_ = 0.0f;
        hoveredOutputLink_ = {};
    }
    if (showProblems_)
    {

        IDWriteTextFormat* listFormat = nullptr;
        dwrite->CreateTextFormat(
            fontFamily_.c_str(),
            fontCollection_,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            fontSize_,
            L"en-us",
            &listFormat);
        if (listFormat)
        {
            listFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            listFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            listFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

            ID2D1SolidColorBrush* errorBrush = nullptr;
            ID2D1SolidColorBrush* warningBrush = nullptr;
            ID2D1SolidColorBrush* rowAlt = nullptr;
            ID2D1SolidColorBrush* rowHover = nullptr;
            ID2D1SolidColorBrush* rowBorder = nullptr;
            rt->CreateSolidColorBrush(lightMode ? D2D1::ColorF(0.78f, 0.24f, 0.24f, 1.0f)
                                                : D2D1::ColorF(0.90f, 0.35f, 0.35f, 1.0f),
                                      &errorBrush);
            rt->CreateSolidColorBrush(lightMode ? D2D1::ColorF(0.58f, 0.52f, 0.12f, 1.0f)
                                                : D2D1::ColorF(0.95f, 0.70f, 0.30f, 1.0f),
                                      &warningBrush);
            D2D1_COLOR_F rowAltColor = themePalette.explorerRowHover;
            rowAltColor.a = lightMode ? 0.70f : 0.45f;
            rt->CreateSolidColorBrush(rowAltColor, &rowAlt);
            rt->CreateSolidColorBrush(themePalette.explorerRowActive, &rowHover);
            rt->CreateSolidColorBrush(UI::Theme::ChromeBorder(), &rowBorder);

            float y = contentRect.top + 12.0f;
            float x = contentRect.left + 16.0f;
            float lineH = fontSize_ + 6.0f;

            std::wstring header = L"Problems";
            rt->DrawTextW(header.c_str(), (UINT32)header.size(), listFormat,
                          D2D1::RectF(x, y, contentRect.right - 8.0f, y + lineH), fg);
            y += lineH + 4.0f;

            problemsRowHeight_ = lineH;
            problemsListRect_ = D2D1::RectF(contentRect.left + 8.0f, y, contentRect.right - 8.0f, contentRect.bottom - 8.0f);

            if (problems_.empty())
            {
                std::wstring empty = L"Aucun probleme detecte";
                rt->DrawTextW(empty.c_str(), (UINT32)empty.size(), listFormat,
                              D2D1::RectF(x, y, contentRect.right - 8.0f, y + lineH), fg);
            }
            else
            {
                rt->PushAxisAlignedClip(contentRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                int idx = 0;
                for (const auto& p : problems_)
                {
                    float rowLeft = contentRect.left + 8.0f;
                    float rowRight = contentRect.right - 8.0f;
                    float rowTop = y - 2.0f;
                    float rowBottom = y + lineH + 2.0f;
                    D2D1_RECT_F rowRect = D2D1::RectF(rowLeft, rowTop, rowRight, rowBottom);

                    if (idx % 2 == 1 && rowAlt)
                        rt->FillRectangle(rowRect, rowAlt);
                    if (idx == hoveredProblemIndex_ && rowHover)
                        rt->FillRectangle(rowRect, rowHover);
                    if (idx == hoveredProblemIndex_ && rowBorder)
                        rt->DrawRectangle(rowRect, rowBorder, 1.0f);

                    ID2D1SolidColorBrush* lineBrush = p.isError ? errorBrush : warningBrush;
                    std::wstring line = (p.isError ? L"Error " : L"Warning ") +
                        p.fileName + L":" + std::to_wstring(p.line) + L":" + std::to_wstring(p.column) + L" " + p.message;
                    if (!p.suggestion.empty())
                        line += L" | Suggestion: " + p.suggestion;
                    rt->DrawTextW(line.c_str(), (UINT32)line.size(), listFormat,
                                  D2D1::RectF(x, y, contentRect.right - 8.0f, y + lineH), lineBrush ? lineBrush : fg);
                    y += lineH;
                    idx++;
                    if (y > contentRect.bottom - lineH)
                        break;
                }
                rt->PopAxisAlignedClip();
            }

            if (errorBrush)
                errorBrush->Release();
            if (warningBrush)
                warningBrush->Release();
            if (rowAlt)
                rowAlt->Release();
            if (rowHover)
                rowHover->Release();
            if (rowBorder)
                rowBorder->Release();
            listFormat->Release();
        }
    }
    else if (showOutput_)
    {

        IDWriteTextFormat* listFormat = nullptr;
        dwrite->CreateTextFormat(
            fontFamily_.c_str(),
            fontCollection_,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            fontSize_,
            L"en-us",
            &listFormat);
        if (listFormat)
        {
            listFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            listFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            listFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

            ID2D1SolidColorBrush* accentLocal = nullptr;
            ID2D1SolidColorBrush* dim = nullptr;
            ID2D1SolidColorBrush* warn = nullptr;
            ID2D1SolidColorBrush* err = nullptr;
            ID2D1SolidColorBrush* cmd = nullptr;
            ID2D1SolidColorBrush* ok = nullptr;
            rt->CreateSolidColorBrush(UI::Theme::Accent(), &accentLocal);
            rt->CreateSolidColorBrush(UI::Theme::MutedText(), &dim);
            rt->CreateSolidColorBrush(lightMode ? D2D1::ColorF(0.58f, 0.52f, 0.12f, 1.0f)
                                                : D2D1::ColorF(0.95f, 0.80f, 0.35f, 1.0f),
                                      &warn);
            rt->CreateSolidColorBrush(lightMode ? D2D1::ColorF(0.78f, 0.24f, 0.24f, 1.0f)
                                                : D2D1::ColorF(0.98f, 0.36f, 0.36f, 1.0f),
                                      &err);
            rt->CreateSolidColorBrush(UI::Theme::AccentStrong(), &cmd);
            rt->CreateSolidColorBrush(lightMode ? D2D1::ColorF(0.18f, 0.58f, 0.24f, 1.0f)
                                                : D2D1::ColorF(0.46f, 0.85f, 0.60f, 1.0f),
                                      &ok);

            float x = contentRect.left + 16.0f;
            float lineH = (fontSize_ + 6.0f);
            float headerY = contentRect.top + 12.0f;

            std::wstring header = L"Output";
            rt->DrawTextW(header.c_str(), (UINT32)header.size(), listFormat,
                          D2D1::RectF(x, headerY, contentRect.right - 8.0f, headerY + lineH),
                          accentLocal ? accentLocal : fg);
            float headerBlock = lineH + 4.0f;
            float bodyStartY = headerY + headerBlock;
            D2D1_RECT_F bodyRect = D2D1::RectF(contentRect.left, bodyStartY, contentRect.right, contentRect.bottom);
            outputBodyRect_ = bodyRect;
            outputBodyStartY_ = bodyStartY;
            outputLineHeight_ = lineH;

            // Copy button (icon)
            {
                UINT dpi = hwnd ? win32_get_dpi_for_window(hwnd) : 96;
                int iconPx = 14;
                float btnSize = 18.0f;
                float btnX = contentRect.right - 12.0f - btnSize;
                float btnY = headerY - 2.0f;
                outputCopyRect_ = D2D1::RectF(btnX, btnY, btnX + btnSize, btnY + btnSize);

                if (hoveredOutputCopy_)
                {
                    ID2D1SolidColorBrush* hover = nullptr;
                    rt->CreateSolidColorBrush(themePalette.explorerRowHover, &hover);
                    if (hover)
                    {
                        rt->FillRoundedRectangle(
                            D2D1::RoundedRect(outputCopyRect_, 3.0f, 3.0f),
                            hover);
                        hover->Release();
                    }
                }

                DWORD now = GetTickCount();
                if (outputCopyFeedback_ && now > outputCopyFeedbackUntil_)
                    outputCopyFeedback_ = false;
                std::string iconPath = outputCopyFeedback_
                    ? "assets/ressource/icons/verified.svg"
                    : "assets/ressource/icons/document.svg";
                ID2D1Bitmap* bmp = GetExplorerManager().LoadSvgIconPublic(rt, iconPath, iconPx, dpi);
                if (bmp)
                {
                    float pad = (btnSize - (float)iconPx) * 0.5f;
                    D2D1_RECT_F dst = D2D1::RectF(btnX + pad, btnY + pad,
                                                  btnX + pad + iconPx, btnY + pad + iconPx);
                    rt->DrawBitmap(bmp, dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    bmp->Release();
                }
            }

            std::vector<std::wstring> lines;
            {
                std::lock_guard<std::mutex> lock(outputMutex_);
                lines = outputLines_;
            }

            float contentHeight = 12.0f + headerBlock;
            contentHeight += (lines.empty() ? 1.0f : (float)lines.size()) * lineH;
            contentHeight += 12.0f;

            float contentW = contentRect.right - contentRect.left;
            float contentH = contentRect.bottom - contentRect.top;
            outputScrollbar_.UpdateLayout(contentRect.left, contentRect.top, contentW, contentH, contentHeight);
            if (outputPendingScrollToBottom_)
            {
                float maxScroll = (std::max)(0.0f, contentHeight - contentH);
                outputScrollbar_.SetScrollOffset(maxScroll);
                outputPendingScrollToBottom_ = false;
            }

            float scrollOffset = outputScrollbar_.GetScrollOffset();
            float maxScroll = (std::max)(0.0f, contentHeight - contentH);
            if (!outputScrollbar_.IsDragging() && scrollOffset >= maxScroll - 2.0f)
                outputAutoFollow_ = true;

            float y = bodyStartY - scrollOffset;

            if (lines.empty())
            {
                std::wstring empty = L"Aucun output pour l'instant";
                rt->PushAxisAlignedClip(bodyRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                rt->DrawTextW(empty.c_str(), (UINT32)empty.size(), listFormat,
                              D2D1::RectF(x, y, contentRect.right - 8.0f, y + lineH),
                              dim ? dim : fg);
                rt->PopAxisAlignedClip();
            }
            else
            {
                rt->PushAxisAlignedClip(bodyRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

                int startLine = 0;
                if (scrollOffset > headerBlock)
                    startLine = (int)((scrollOffset - headerBlock) / lineH);
                if (startLine < 0) startLine = 0;

                y = bodyStartY - scrollOffset + startLine * lineH;
                for (size_t i = (size_t)startLine; i < lines.size(); ++i)
                {
                    const std::wstring& line = lines[i];
                    ID2D1SolidColorBrush* brush = fg;
                    if (line.rfind(L"$ ", 0) == 0)
                        brush = cmd ? cmd : fg;
                    else if (line.rfind(L"[run]", 0) == 0 || line.rfind(L"CMake Warning", 0) == 0 || line.find(L"warning") != std::wstring::npos)
                        brush = warn ? warn : fg;
                    else if (line.rfind(L"Reason:", 0) == 0 || line.rfind(L"CMake Error", 0) == 0 || line.find(L"error") != std::wstring::npos || line.find(L"FAILED") != std::wstring::npos)
                        brush = err ? err : fg;
                    else if (line.rfind(L"Hint:", 0) == 0)
                        brush = dim ? dim : fg;
                    else if (line.find(L"Building") != std::wstring::npos || line.find(L"Linking") != std::wstring::npos || line.find(L"done") != std::wstring::npos)
                        brush = ok ? ok : fg;
                    if (hoveredOutputLink_.active && hoveredOutputLink_.lineIndex == (int)i &&
                        (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0)
                    {
                        brush = accentLocal ? accentLocal : brush;
                    }
                    rt->DrawTextW(line.c_str(), (UINT32)line.size(), listFormat,
                                  D2D1::RectF(x, y, contentRect.right - 8.0f, y + lineH), brush);
                    y += lineH;
                    if (y > contentRect.bottom - lineH)
                        break;
                }

                rt->PopAxisAlignedClip();
            }

            if (accentLocal)
                accentLocal->Release();
            if (dim)
                dim->Release();
            if (warn)
                warn->Release();
            if (err)
                err->Release();
            if (cmd)
                cmd->Release();
            if (ok)
                ok->Release();
            listFormat->Release();
        }

        outputScrollbar_.Draw(rt);
    }
    else
    {
        TerminalSession* s = ActiveSession();
        if (s)
        {
            // Clip session content inside the rounded card (with inner padding)
            s->SetViewport(contentRect.left + 4.0f, contentRect.top + 4.0f,
                           contentRect.right - 4.0f, contentRect.bottom - 4.0f);
            s->DrawContent(rt, dwrite, fontFamily_, fontSize_, fontCollection_, (resizeHover_ || resizing_), focused_);
        }
    }

    bg->Release();
    fg->Release();
    muted->Release();
    border->Release();
    accentBrush->Release();
    resizeLine->Release();
}

void TerminalPanel::SetProblems(const std::wstring& filePath, const std::vector<ProblemItem>& problems)
{
    problemsFilePath_ = filePath;
    problems_ = problems;
}

void TerminalPanel::ClearOutput()
{
    std::lock_guard<std::mutex> lock(outputMutex_);
    outputLines_.clear();
    outputBuffer_.clear();
    outputScrollbar_.SetScrollOffset(0.0f);
    outputAutoFollow_ = true;
    outputPendingScrollToBottom_ = true;
}

void TerminalPanel::AppendOutputChunk(const std::wstring& text)
{
    if (text.empty())
        return;

    std::lock_guard<std::mutex> lock(outputMutex_);
    std::wstring normalized;
    normalized.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i)
    {
        wchar_t c = text[i];
        if (c == L'\r')
        {
            if (i + 1 < text.size() && text[i + 1] == L'\n')
                ++i;
            normalized.push_back(L'\n');
        }
        else
        {
            normalized.push_back(c);
        }
    }

    outputBuffer_.append(normalized);

    size_t pos = 0;
    while (true)
    {
        size_t nl = outputBuffer_.find(L'\n', pos);
        if (nl == std::wstring::npos)
            break;

        std::wstring line = outputBuffer_.substr(pos, nl - pos);
        if (!line.empty() && line.back() == L'\r')
            line.pop_back();

        outputLines_.push_back(line);
        pos = nl + 1;
    }

    if (pos > 0)
        outputBuffer_.erase(0, pos);

    const size_t kMaxLines = 2000;
    if (outputLines_.size() > kMaxLines)
        outputLines_.erase(outputLines_.begin(), outputLines_.begin() + (outputLines_.size() - kMaxLines));

    if (showOutput_ && outputAutoFollow_)
        outputPendingScrollToBottom_ = true;
}

void TerminalPanel::FlushOutputBuffer()
{
    std::lock_guard<std::mutex> lock(outputMutex_);
    if (outputBuffer_.empty())
        return;

    std::wstring line = outputBuffer_;
    if (!line.empty() && line.back() == L'\r')
        line.pop_back();
    outputLines_.push_back(line);
    outputBuffer_.clear();

    const size_t kMaxLines = 2000;
    if (outputLines_.size() > kMaxLines)
        outputLines_.erase(outputLines_.begin(), outputLines_.begin() + (outputLines_.size() - kMaxLines));
}

void TerminalPanel::ShowOutput(bool v)
{
    showOutput_ = v;
    if (showOutput_)
        showProblems_ = false;
    if (showOutput_)
    {
        outputAutoFollow_ = true;
        outputPendingScrollToBottom_ = true;
    }
}

bool TerminalPanel::SendCommandToActive(HWND hwnd, const std::wstring& startDir, const std::wstring& command)
{
    SetVisible(true);
    showOutput_ = false;
    showProblems_ = false;

    // Utilise le terminal actif existant — ne crée un nouveau que si aucun n'existe
    EnsureActiveInit(hwnd);
    TerminalSession* s = ActiveSession();
    if (!s)
        return false;

    // Combine cd + commande sur une seule ligne (PowerShell: point-virgule)
    std::wstring fullCmd;
    if (!startDir.empty())
        fullCmd = L"cd \"" + startDir + L"\"; " + command;
    else
        fullCmd = command;

    std::string u8 = WideToUtf8(fullCmd);
    if (!u8.empty())
        s->SendUtf8(u8.data(), (DWORD)u8.size());
    s->SendUtf8("\r", 1);
    return true;
}
