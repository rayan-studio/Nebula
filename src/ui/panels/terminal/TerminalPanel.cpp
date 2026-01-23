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
    tabBar_.UpdateLayout(left_, top_ + resizeZoneH_, TabsBarRightEdge());

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
    r.top = (LONG)(top_ + resizeZoneH_);
    r.right = (LONG)right_;
    r.bottom = (LONG)(top_ + resizeZoneH_ + TabsBarHeightPx());
    return r;
}

float TerminalPanel::TabsBarRightEdge() const
{
    RECT plus = PlusButtonRectClient();
    float rightEdge = (float)plus.left - 6.0f;
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
    r.right = t.right - 10;
    r.left  = r.right - size;
    r.top   = t.top + 3;
    r.bottom= r.top + size;
    return r;
}

bool TerminalPanel::HitTestPlus(POINT pt) const
{
    RECT r = PlusButtonRectClient();
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
        std::wstring title = L"Terminal " + std::to_wstring(i + 1);
        tabBar_.UpdateTabPath(i, L"", title);
    }

    if (activeIndex_ >= 0 && activeIndex_ < desired)
        tabBar_.SetActiveTab(activeIndex_);
}

// ---------------- Mouse ----------------
void TerminalPanel::OnLeftButtonDown(HWND hwnd, POINT pt)
{
    if (!visible_) return;

    // Plus click => NewTerminal (startDir choisi par Window normalement via menu)
    if (HitTestPlus(pt))
    {
        // Ici: fallback current dir
        NewTerminal(hwnd, L"");
        return;
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
        s->SetViewport(left_, top_ + resizeZoneH_ + tabsH, right_, bottom_);
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

    if (IsPointInTabsBar(pt) && !hoveredPlus_)
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

    float contentW = (right_ - left_) - 20.0f; // pads approximatifs
    float contentH = (bottom_ - (top_ + resizeZoneH_ + tabsH)) - 16.0f;

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
    tabBar_.UpdateLayout(left_, top_ + resizeZoneH_, TabsBarRightEdge());

    // Tabs bar background
    ID2D1SolidColorBrush* bg = nullptr;
    ID2D1SolidColorBrush* fg = nullptr;
    ID2D1SolidColorBrush* border = nullptr;
    ID2D1SolidColorBrush* resizeBg = nullptr;
    ID2D1SolidColorBrush* resizeLine = nullptr;

    rt->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.06f, 0.06f, 1.0f), &bg);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.90f, 0.90f, 0.90f, 1.0f), &fg);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.20f, 0.20f, 0.20f, 1.0f), &border);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.06f, 0.06f, 1.0f), &resizeBg);
    rt->CreateSolidColorBrush(resizeHover_ || resizing_
                                  ? D2D1::ColorF(0.35f, 0.35f, 0.35f, 1.0f)
                                  : D2D1::ColorF(0.20f, 0.20f, 0.20f, 1.0f),
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
    if (resizeHover_ || resizing_)
    {
        float gripY = top_ + resizeZoneH_ * 0.5f;
        rt->DrawLine(D2D1::Point2F(left_ + 10.0f, gripY),
                     D2D1::Point2F(right_ - 10.0f, gripY),
                     resizeLine, 1.0f);
    }

    RECT tabsR = TabsBarRectClient();
    D2D1_RECT_F tabs = D2D1::RectF((float)tabsR.left, (float)tabsR.top, (float)tabsR.right, (float)tabsR.bottom);
    rt->FillRectangle(tabs, bg);
    if (!sessions_.empty())
        rt->FillRectangle(D2D1::RectF(left_, tabs.bottom - 1.0f, right_, tabs.bottom), border);

    tabBar_.Draw(rt, dwrite, hwnd);

    // Plus button
    RECT pr = PlusButtonRectClient();
    D2D1_RECT_F plus = D2D1::RectF((float)pr.left, (float)pr.top, (float)pr.right, (float)pr.bottom);

    ID2D1SolidColorBrush* plusBg = nullptr;
    ID2D1SolidColorBrush* plusBorder = nullptr;
    if (hoveredPlus_)
    {
        rt->CreateSolidColorBrush(D2D1::ColorF(0.22f, 0.22f, 0.22f, 1.0f), &plusBg);
        rt->CreateSolidColorBrush(D2D1::ColorF(0.40f, 0.40f, 0.40f, 1.0f), &plusBorder);
        if (plusBg)
        {
            rt->FillRoundedRectangle(D2D1::RoundedRect(plus, 5.0f, 5.0f), plusBg);
            plusBg->Release();
        }
        if (plusBorder)
        {
            rt->DrawRoundedRectangle(D2D1::RoundedRect(plus, 5.0f, 5.0f), plusBorder, 1.0f);
            plusBorder->Release();
        }
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
    TerminalSession* s = ActiveSession();
    if (s)
    {
        float tabsH = TabsBarHeightPx();
        s->SetViewport(left_, top_ + resizeZoneH_ + tabsH, right_, bottom_);

        // draw session content (chrome + text + scrollbar)
        s->DrawContent(rt, dwrite, fontFamily_, fontSize_, fontCollection_, (resizeHover_ || resizing_), focused_);
    }

    bg->Release();
    fg->Release();
    border->Release();
    resizeBg->Release();
    resizeLine->Release();
}
