#include "TerminalPanel.h"
#include "TerminalSession.h"

#include <algorithm>
#include <cmath>

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
    SyncTabBar();
}

// ---------------- Visible / Focus ----------------
void TerminalPanel::ToggleVisible()
{
    visible_ = !visible_;
    if (!visible_)
        focused_ = false;
}

void TerminalPanel::SetVisible(bool v)
{
    visible_ = v;
    if (!visible_)
        focused_ = false;
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
    tabBar_.UpdateLayout(left_, bottom_ - TabsBarHeightPx(), TabsBarRightEdge());

    // Update viewport for active session (content area under tabs bar)
    TerminalSession* s = ActiveSession();
    if (s)
    {
        float tabsH = TabsBarHeightPx();
        float contentTop = top_ + resizeZoneH_ + tabsH;
        s->SetViewport(left_, contentTop, right_, bottom_);
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
    r.top = (LONG)(bottom_ - TabsBarHeightPx());
    r.right = (LONG)right_;
    r.bottom = (LONG)bottom_;
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

RECT TerminalPanel::PlusButtonRectClient() const
{
    RECT t = TabsBarRectClient();
    int size = (int)TabsBarHeightPx() - 6;
    if (size < 14) size = 14;
    RECT r;
    r.right = t.right - 8;
    r.left  = r.right - size;
    r.top   = t.top + 2;
    r.bottom= t.bottom - 2;
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

// ---------------- Mouse ----------------
void TerminalPanel::OnLeftButtonDown(HWND hwnd, POINT pt)
{
    if (!visible_) return;

    if (HitTestOutput(pt))
    {
        showOutput_ = !showOutput_;
        if (showOutput_)
            showProblems_ = false;
        return;
    }

    if (HitTestProblems(pt))
    {
        showProblems_ = !showProblems_;
        if (showProblems_)
            showOutput_ = false;
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
        if (outputScrollbar_.OnLeftButtonDown(pt))
        {
            outputAutoFollow_ = false;
            outputPendingScrollToBottom_ = false;
            SetCapture(hwnd);
            return;
        }
    }

    if ((showOutput_ || showProblems_) && IsPointInPanel(pt))
    {
        if (!IsPointInTabsBar(pt) && !IsPointInResizeZone(pt))
        {
            focused_ = false;
            return;
        }
    }

    if (IsPointInTabsBar(pt))
    {
        int result = tabBar_.OnLeftButtonDown(pt);
        if (result == TabBar::TAB_CLICKED_CLOSE)
        {
            int closeIndex = tabBar_.GetLastCloseRequestIndex();
            CloseTerminal(closeIndex);
            return;
        }

        if (result >= 0)
        {
            SetActiveIndex(result);
            showOutput_ = false;
            showProblems_ = false;
            focused_ = true;
            EnsureActiveInitialized(hwnd);
            return;
        }
    }

    // Resize zone
    if (IsPointInResizeZone(pt))
    {
        resizing_ = true;
        dragStart_ = pt;
        startTop_ = top_;
        SetCapture(hwnd);
        return;
    }

    // Scrollbar capture for active session
    if (TerminalSession* s = ActiveSession())
    {
        if (s->OnScrollbarLButtonDown(pt))
        {
            SetCapture(hwnd);
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
                SetCapture(hwnd);
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
            return;
        }
    }

    if (TerminalSession* s = ActiveSession())
    {
        if (s->OnScrollbarLButtonUp())
        {
            ReleaseCapture();
            return;
        }
    }

    if (TerminalSession* s = ActiveSession())
    {
        if (s->OnLeftButtonUp())
        {
            ReleaseCapture();
            return;
        }
    }

    if (resizing_)
    {
        resizing_ = false;
        ReleaseCapture();
        return;
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

    if (IsPointInTabsBar(pt) && !hoveredPlus_ && !hoveredProblems_ && !hoveredOutput_)
    {
        int tabHover = tabBar_.OnMouseMove(pt);
        if (tabHover != -2)
            changed = true;
    }
    else
    {
        if (tabBar_.ClearHover())
            changed = true;
    }

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

    const float contentPad = 8.0f;
    float contentW = (right_ - left_) - 20.0f; // pads approximatifs
    float contentH = (bottom_ - tabsH - contentPad) - (top_ + resizeZoneH_ + contentPad);

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

    EnsureAtLeastOneSession(NULL);
    SyncTabBar();
    tabBar_.UpdateLayout(left_, bottom_ - TabsBarHeightPx(), TabsBarRightEdge());

    // Tabs bar background
    ID2D1SolidColorBrush* bg = nullptr;
    ID2D1SolidColorBrush* fg = nullptr;
    ID2D1SolidColorBrush* border = nullptr;
    ID2D1SolidColorBrush* resizeBg = nullptr;
    ID2D1SolidColorBrush* resizeLine = nullptr;

    rt->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.08f, 0.09f, 1.0f), &bg);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.92f, 0.92f, 0.92f, 1.0f), &fg);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.18f, 0.18f, 0.20f, 1.0f), &border);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.08f, 0.09f, 1.0f), &resizeBg);
    rt->CreateSolidColorBrush(resizeHover_ || resizing_
                                  ? D2D1::ColorF(0.40f, 0.40f, 0.45f, 1.0f)
                                  : D2D1::ColorF(0.22f, 0.22f, 0.26f, 1.0f),
                              &resizeLine);

    if (!bg || !fg || !border || !resizeBg || !resizeLine)
    {
        if (bg) bg->Release();
        if (fg) fg->Release();
        if (border) border->Release();
        if (resizeBg) resizeBg->Release();
        if (resizeLine) resizeLine->Release();
        return;
    }

    // Resize bar (above tabs)
    D2D1_RECT_F resizeBar = D2D1::RectF(left_, top_, right_, top_ + resizeZoneH_);
    rt->FillRectangle(resizeBar, resizeBg);
    {
        float gripY = top_ + resizeZoneH_ - 0.5f;
        rt->DrawLine(D2D1::Point2F(left_ + 0.5f, gripY),
                     D2D1::Point2F(right_ - 0.5f, gripY),
                     resizeLine, 1.0f);
    }

    RECT tabsR = TabsBarRectClient();
    D2D1_RECT_F tabs = D2D1::RectF((float)tabsR.left, (float)tabsR.top, (float)tabsR.right, (float)tabsR.bottom);
    rt->FillRectangle(tabs, bg);
    rt->DrawLine(D2D1::Point2F(left_, tabs.top + 0.5f),
                 D2D1::Point2F(right_, tabs.top + 0.5f), border, 1.0f);

    tabBar_.Draw(rt, dwrite, hwnd);

    float tabsH = TabsBarHeightPx();
    const float contentPad = 8.0f;
    float contentTop = top_ + resizeZoneH_ + contentPad;
    float contentBottom = bottom_ - tabsH - contentPad;


    // Output button
    RECT out = OutputButtonRectClient();
    D2D1_RECT_F outputRect = D2D1::RectF((float)out.left, (float)out.top, (float)out.right, (float)out.bottom);
    const float tabInset = 2.0f;
    D2D1_RECT_F outputTab = D2D1::RectF(outputRect.left, outputRect.top - 4.0f, outputRect.right, outputRect.bottom);
    D2D1_RECT_F outputTabOutline = outputTab;
    if (showOutput_)
    {
        float tabH = outputTab.bottom - outputTab.top;
        outputTabOutline.top = contentBottom - 1.0f; // align outline with content border
        outputTabOutline.bottom = outputTabOutline.top + tabH;
    }

    ID2D1SolidColorBrush* outputBg = nullptr;
    ID2D1SolidColorBrush* outputBorder = nullptr;
    if (showOutput_)
        rt->CreateSolidColorBrush(D2D1::ColorF(0.15f, 0.15f, 0.18f, 1.0f), &outputBg);
    else if (hoveredOutput_)
        rt->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.12f, 0.14f, 1.0f), &outputBg);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.22f, 0.22f, 0.26f, 1.0f), &outputBorder);

    if (outputBg)
    {
        D2D1_RECT_F fillRect = showOutput_ ? outputTabOutline : outputTab;
        if (showOutput_)
            fillRect.top += 1.0f; // keep border line visible
        rt->FillRectangle(fillRect, outputBg);
        outputBg->Release();
    }
    if (outputBorder)
    {
        // draw left/right/bottom only for active tab to look connected
        if (showOutput_)
        {
            // outline handled by panel border section
        }
        else
        {
            // no outline when inactive
        }
        outputBorder->Release();
    }
    if (showOutput_)
    {
        // no extra accent line; keep a single border
    }

    IDWriteTextFormat* outputFormat = nullptr;
    dwrite->CreateTextFormat(
        L"Segoe UI",
        NULL,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        12.0f,
        L"en-us",
        &outputFormat);
    if (outputFormat)
    {
        outputFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        outputFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        size_t outputCount = 0;
        {
            std::lock_guard<std::mutex> lock(outputMutex_);
            outputCount = outputLines_.size();
        }
        std::wstring label = L"Output (" + std::to_wstring(outputCount) + L")";
        D2D1_RECT_F textRect = showOutput_ ? outputTabOutline : outputTab;
        textRect.left += 14.0f;
        textRect.right -= 8.0f;
        ID2D1SolidColorBrush* labelBrush = fg;
        if (!showOutput_ && !hoveredOutput_)
        {
            ID2D1SolidColorBrush* muted = nullptr;
            rt->CreateSolidColorBrush(D2D1::ColorF(0.72f, 0.72f, 0.75f, 1.0f), &muted);
            labelBrush = muted ? muted : fg;
            rt->DrawTextW(label.c_str(), (UINT32)label.size(), outputFormat, textRect, labelBrush);
            if (muted) muted->Release();
        }
        else
        {
            rt->DrawTextW(label.c_str(), (UINT32)label.size(), outputFormat, textRect, labelBrush);
        }
        outputFormat->Release();
    }

    // Problems button
    RECT prb = ProblemsButtonRectClient();
    D2D1_RECT_F problemsRect = D2D1::RectF((float)prb.left, (float)prb.top, (float)prb.right, (float)prb.bottom);
    D2D1_RECT_F problemsTab = D2D1::RectF(problemsRect.left, problemsRect.top - 4.0f, problemsRect.right, problemsRect.bottom);
    D2D1_RECT_F problemsTabOutline = problemsTab;
    if (showProblems_)
    {
        float tabH = problemsTab.bottom - problemsTab.top;
        problemsTabOutline.top = contentBottom - 1.0f;
        problemsTabOutline.bottom = problemsTabOutline.top + tabH;
    }

    ID2D1SolidColorBrush* problemsBg = nullptr;
    ID2D1SolidColorBrush* problemsBorder = nullptr;
    if (showProblems_)
        rt->CreateSolidColorBrush(D2D1::ColorF(0.15f, 0.15f, 0.18f, 1.0f), &problemsBg);
    else if (hoveredProblems_)
        rt->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.12f, 0.14f, 1.0f), &problemsBg);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.22f, 0.22f, 0.26f, 1.0f), &problemsBorder);

    if (problemsBg)
    {
        D2D1_RECT_F fillRect = showProblems_ ? problemsTabOutline : problemsTab;
        if (showProblems_)
            fillRect.top += 1.0f; // keep border line visible
        rt->FillRectangle(fillRect, problemsBg);
        problemsBg->Release();
    }
    if (problemsBorder)
    {
        if (showProblems_)
        {
            // outline handled by panel border section
        }
        else
        {
            // no outline when inactive
        }
        problemsBorder->Release();
    }
    if (showProblems_)
    {
        // no extra accent line; keep a single border
    }

    IDWriteTextFormat* problemsFormat = nullptr;
    dwrite->CreateTextFormat(
        L"Segoe UI",
        NULL,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        12.0f,
        L"en-us",
        &problemsFormat);
    if (problemsFormat)
    {
        problemsFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        problemsFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        std::wstring label = L"Problems (" + std::to_wstring(problems_.size()) + L")";
        D2D1_RECT_F textRect = showProblems_ ? problemsTabOutline : problemsTab;
        textRect.left += 14.0f;
        textRect.right -= 8.0f;
        ID2D1SolidColorBrush* labelBrush = fg;
        if (!showProblems_ && !hoveredProblems_)
        {
            ID2D1SolidColorBrush* muted = nullptr;
            rt->CreateSolidColorBrush(D2D1::ColorF(0.72f, 0.72f, 0.75f, 1.0f), &muted);
            labelBrush = muted ? muted : fg;
            rt->DrawTextW(label.c_str(), (UINT32)label.size(), problemsFormat, textRect, labelBrush);
            if (muted) muted->Release();
        }
        else
        {
            rt->DrawTextW(label.c_str(), (UINT32)label.size(), problemsFormat, textRect, labelBrush);
        }
        problemsFormat->Release();
    }

    // Plus button
    RECT pr = PlusButtonRectClient();
    D2D1_RECT_F plus = D2D1::RectF((float)pr.left, (float)pr.top, (float)pr.right, (float)pr.bottom);

    ID2D1SolidColorBrush* plusBg = nullptr;
    ID2D1SolidColorBrush* plusBorder = nullptr;
    if (hoveredPlus_)
        rt->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.12f, 0.14f, 1.0f), &plusBg);
    rt->CreateSolidColorBrush(hoveredPlus_
                                  ? D2D1::ColorF(0.32f, 0.34f, 0.38f, 1.0f)
                                  : D2D1::ColorF(0.20f, 0.20f, 0.24f, 1.0f),
                              &plusBorder);
    if (plusBg)
    {
        rt->FillRectangle(plus, plusBg);
        plusBg->Release();
    }
    if (plusBorder)
    {
        rt->DrawRectangle(plus, plusBorder, 1.0f);
        plusBorder->Release();
    }

    // draw "+"
    float cx = (plus.left + plus.right) * 0.5f;
    float cy = (plus.top + plus.bottom) * 0.5f;
    float halfSize = (plus.bottom - plus.top) * 0.28f;

    ID2D1SolidColorBrush* plusStroke = nullptr;
    D2D1_COLOR_F plusStrokeColor = hoveredPlus_
        ? D2D1::ColorF(0.95f, 0.95f, 0.95f, 1.0f)
        : D2D1::ColorF(0.70f, 0.70f, 0.70f, 1.0f);
    rt->CreateSolidColorBrush(plusStrokeColor, &plusStroke);
    if (plusStroke)
    {
        rt->DrawLine(D2D1::Point2F(cx - halfSize, cy), D2D1::Point2F(cx + halfSize, cy), plusStroke, 1.2f);
        rt->DrawLine(D2D1::Point2F(cx, cy - halfSize), D2D1::Point2F(cx, cy + halfSize), plusStroke, 1.2f);
        plusStroke->Release();
    }


    // Content viewport
    if (showProblems_)
    {
        D2D1_RECT_F contentRect = D2D1::RectF(left_, contentTop, right_, contentBottom);
        ID2D1SolidColorBrush* contentBg = nullptr;
        rt->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.08f, 0.08f, 0.95f), &contentBg);
        if (contentBg)
        {
            rt->FillRectangle(contentRect, contentBg);
            contentBg->Release();
        }

        IDWriteTextFormat* listFormat = nullptr;
        dwrite->CreateTextFormat(
            L"Segoe UI",
            NULL,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            12.0f,
            L"en-us",
            &listFormat);
        if (listFormat)
        {
            listFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            listFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            listFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

            ID2D1SolidColorBrush* errorBrush = nullptr;
            ID2D1SolidColorBrush* warningBrush = nullptr;
            rt->CreateSolidColorBrush(D2D1::ColorF(0.90f, 0.35f, 0.35f, 1.0f), &errorBrush);
            rt->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.70f, 0.30f, 1.0f), &warningBrush);

            float y = contentRect.top + 12.0f;
            float x = contentRect.left + 16.0f;
            float lineH = 18.0f;

            std::wstring header = L"Problems";
            rt->DrawTextW(header.c_str(), (UINT32)header.size(), listFormat,
                          D2D1::RectF(x, y, contentRect.right - 8.0f, y + lineH), fg);
            y += lineH + 4.0f;

            if (problems_.empty())
            {
                std::wstring empty = L"Aucun probl??me d??tect??";
                rt->DrawTextW(empty.c_str(), (UINT32)empty.size(), listFormat,
                              D2D1::RectF(x, y, contentRect.right - 8.0f, y + lineH), fg);
            }
            else
            {
                rt->PushAxisAlignedClip(contentRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                for (const auto& p : problems_)
                {
                    ID2D1SolidColorBrush* lineBrush = p.isError ? errorBrush : warningBrush;
                    std::wstring line = (p.isError ? L"E " : L"W ") +
                        p.fileName + L":" + std::to_wstring(p.line) + L":" + std::to_wstring(p.column) + L" " + p.message;
                    rt->DrawTextW(line.c_str(), (UINT32)line.size(), listFormat,
                                  D2D1::RectF(x, y, contentRect.right - 8.0f, y + lineH), lineBrush ? lineBrush : fg);
                    y += lineH;
                    if (y > contentRect.bottom - lineH)
                        break;
                }
                rt->PopAxisAlignedClip();
            }

            if (errorBrush)
                errorBrush->Release();
            if (warningBrush)
                warningBrush->Release();
            listFormat->Release();
        }
    }
    else if (showOutput_)
    {
        D2D1_RECT_F contentRect = D2D1::RectF(left_, contentTop, right_, contentBottom);
        ID2D1SolidColorBrush* contentBg = nullptr;
        rt->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.08f, 0.08f, 0.95f), &contentBg);
        if (contentBg)
        {
            rt->FillRectangle(contentRect, contentBg);
            contentBg->Release();
        }

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

            ID2D1SolidColorBrush* accent = nullptr;
            ID2D1SolidColorBrush* dim = nullptr;
            ID2D1SolidColorBrush* warn = nullptr;
            ID2D1SolidColorBrush* err = nullptr;
            ID2D1SolidColorBrush* cmd = nullptr;
            ID2D1SolidColorBrush* ok = nullptr;
            rt->CreateSolidColorBrush(D2D1::ColorF(0.28f, 0.63f, 0.95f, 1.0f), &accent);
            rt->CreateSolidColorBrush(D2D1::ColorF(0.70f, 0.70f, 0.70f, 1.0f), &dim);
            rt->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.80f, 0.35f, 1.0f), &warn);
            rt->CreateSolidColorBrush(D2D1::ColorF(0.98f, 0.36f, 0.36f, 1.0f), &err);
            rt->CreateSolidColorBrush(D2D1::ColorF(0.58f, 0.80f, 1.0f, 1.0f), &cmd);
            rt->CreateSolidColorBrush(D2D1::ColorF(0.46f, 0.85f, 0.60f, 1.0f), &ok);

            float x = contentRect.left + 16.0f;
            float lineH = (fontSize_ + 6.0f);
            float headerY = contentRect.top + 12.0f;

            std::wstring header = L"Output";
            rt->DrawTextW(header.c_str(), (UINT32)header.size(), listFormat,
                          D2D1::RectF(x, headerY, contentRect.right - 8.0f, headerY + lineH),
                          accent ? accent : fg);
            float headerBlock = lineH + 4.0f;
            float bodyStartY = headerY + headerBlock;

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
                rt->DrawTextW(empty.c_str(), (UINT32)empty.size(), listFormat,
                              D2D1::RectF(x, y, contentRect.right - 8.0f, y + lineH),
                              dim ? dim : fg);
            }
            else
            {
                rt->PushAxisAlignedClip(contentRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

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
                    rt->DrawTextW(line.c_str(), (UINT32)line.size(), listFormat,
                                  D2D1::RectF(x, y, contentRect.right - 8.0f, y + lineH), brush);
                    y += lineH;
                    if (y > contentRect.bottom - lineH)
                        break;
                }

                rt->PopAxisAlignedClip();
            }

            if (accent)
                accent->Release();
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
            s->SetViewport(left_, contentTop, right_, contentBottom);

            // draw session content (chrome + text + scrollbar)
            s->DrawContent(rt, dwrite, fontFamily_, fontSize_, fontCollection_, (resizeHover_ || resizing_), focused_);
        }
    }

    // Content border (VS-style) - draw last so it stays visible
    {
        ID2D1SolidColorBrush* panelBorder = nullptr;
        D2D1_COLOR_F borderColor = D2D1::ColorF(0.30f, 0.50f, 0.80f, 1.0f); // app blue
        rt->CreateSolidColorBrush(borderColor, &panelBorder);
        if (panelBorder)
        {
            D2D1_RECT_F panelRect = D2D1::RectF(left_ + 0.5f, contentTop + 0.5f, right_ - 0.5f, contentBottom - 0.5f);
            D2D1_ANTIALIAS_MODE oldAA = rt->GetAntialiasMode();
            rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
            rt->DrawRectangle(panelRect, panelBorder, 1.0f);

            if (showOutput_ || showProblems_)
            {
                D2D1_RECT_F tabRect = showOutput_ ? outputTabOutline : problemsTabOutline;
                // cut the panel border under the active tab
                D2D1_RECT_F cut = D2D1::RectF(tabRect.left - 1.0f, contentBottom - 1.0f,
                                              tabRect.right + 1.0f, contentBottom + 2.0f);
                rt->FillRectangle(cut, bg);

                // draw active tab outline, connected to the panel border line
                float connectY = contentBottom;
                float leftX = tabRect.left;
                float rightX = tabRect.right;
                float bottomY = tabRect.bottom;

                rt->DrawLine(D2D1::Point2F(leftX, connectY),
                             D2D1::Point2F(leftX, bottomY), panelBorder, 1.0f);
                rt->DrawLine(D2D1::Point2F(rightX, connectY),
                             D2D1::Point2F(rightX, bottomY), panelBorder, 1.0f);
                rt->DrawLine(D2D1::Point2F(leftX, bottomY),
                             D2D1::Point2F(rightX, bottomY), panelBorder, 1.0f);
            }

            rt->SetAntialiasMode(oldAA);
            panelBorder->Release();
        }
    }

    bg->Release();
    fg->Release();
    border->Release();
    resizeBg->Release();
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

    EnsureSessionExists(hwnd);
    TerminalSession* s = ActiveSession();
    if (!s || !s->IsInitialized())
    {
        if (!startDir.empty())
        {
            NewTerminal(hwnd, startDir);
        }
        else
        {
            EnsureActiveInit(hwnd);
        }
    }

    s = ActiveSession();
    if (!s)
        return false;

    std::string u8 = WideToUtf8(command);
    if (!u8.empty())
        s->SendUtf8(u8.data(), (DWORD)u8.size());
    s->SendUtf8("\r", 1);
    return true;
}
