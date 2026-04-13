#include "PanelManager.h"
#include "helpers/window_helpers.h"
#include <algorithm>

namespace
{
int ClampWidth(int value, int minValue, int maxValue)
{
    if (value < minValue)
        return minValue;
    if (value > maxValue)
        return maxValue;
    return value;
}

int GetStoredPreferredWidth(const std::unordered_map<PanelId, int> &preferredWidths, Panel *panel)
{
    if (!panel)
        return 0;

    auto it = preferredWidths.find(panel->GetId());
    if (it != preferredWidths.end())
        return ClampWidth(it->second, panel->GetState().minWidth, panel->GetState().maxWidth);

    return ClampWidth(panel->GetLogicalWidth(), panel->GetState().minWidth, panel->GetState().maxWidth);
}

void RememberPreferredWidth(std::unordered_map<PanelId, int> &preferredWidths, Panel *panel)
{
    if (!panel)
        return;

    preferredWidths[panel->GetId()] =
        ClampWidth(panel->GetLogicalWidth(), panel->GetState().minWidth, panel->GetState().maxWidth);
}

struct ResponsiveWidthTargets
{
    int left = 0;
    int right = 0;
    bool adjusted = false;
};

ResponsiveWidthTargets ComputeResponsiveTargets(
    int preferredLeft,
    int preferredRight,
    int leftMin,
    int rightMin,
    int totalAvailableLogical,
    int preferredAvailableLogical)
{
    ResponsiveWidthTargets targets;
    targets.left = preferredLeft;
    targets.right = preferredRight;

    if (preferredLeft <= 0 || preferredRight <= 0)
        return targets;

    if (preferredLeft + preferredRight <= preferredAvailableLogical)
        return targets;

    const int responsiveLeftFloor = ClampWidth((leftMin * 2) / 3, 120, preferredLeft);
    const int responsiveRightFloor = ClampWidth((rightMin * 2) / 3, 120, preferredRight);
    const int floorTotal = responsiveLeftFloor + responsiveRightFloor;

    int targetTotal = preferredAvailableLogical;
    if (targetTotal < floorTotal)
        targetTotal = floorTotal;
    if (targetTotal > totalAvailableLogical)
        targetTotal = totalAvailableLogical;
    if (targetTotal < 0)
        targetTotal = 0;

    if (targetTotal >= preferredLeft + preferredRight)
        return targets;

    int overflow = (preferredLeft + preferredRight) - targetTotal;
    int leftShrinkable = preferredLeft - responsiveLeftFloor;
    if (leftShrinkable < 0)
        leftShrinkable = 0;
    int rightShrinkable = preferredRight - responsiveRightFloor;
    if (rightShrinkable < 0)
        rightShrinkable = 0;
    int shrinkableTotal = leftShrinkable + rightShrinkable;

    if (shrinkableTotal <= 0)
    {
        targets.left = responsiveLeftFloor;
        targets.right = responsiveRightFloor;
        targets.adjusted = true;
        return targets;
    }

    int leftShrink = (overflow * leftShrinkable + (shrinkableTotal / 2)) / shrinkableTotal;
    int rightShrink = overflow - leftShrink;

    targets.left = ClampWidth(preferredLeft - leftShrink, responsiveLeftFloor, preferredLeft);
    targets.right = ClampWidth(preferredRight - rightShrink, responsiveRightFloor, preferredRight);

    int currentTotal = targets.left + targets.right;
    while (currentTotal > targetTotal)
    {
        int leftSlack = targets.left - responsiveLeftFloor;
        int rightSlack = targets.right - responsiveRightFloor;
        if (leftSlack <= 0 && rightSlack <= 0)
            break;

        if (leftSlack >= rightSlack && leftSlack > 0)
            --targets.left;
        else if (rightSlack > 0)
            --targets.right;

        currentTotal = targets.left + targets.right;
    }

    targets.adjusted = (targets.left != preferredLeft || targets.right != preferredRight);
    return targets;
}
}

PanelManager& PanelManager::Instance()
{
    static PanelManager instance;
    return instance;
}

void PanelManager::RegisterPanel(std::unique_ptr<Panel> panel)
{
    if (!panel) return;
    
    PanelId id = panel->GetId();
    Panel* rawPtr = panel.get();

    rawPtr->SetActive(false);
    rawPtr->SetVisible(false);
    
    panels_[id] = std::move(panel);
    panelList_.push_back(rawPtr);
    
    rawPtr->Initialize();
    preferredLogicalWidths_[id] = rawPtr->GetLogicalWidth();
    
    if (!activePanel_)
    {
        activePanel_ = rawPtr;
        rawPtr->SetActive(true);
        rawPtr->SetVisible(true);
    }
}

Panel* PanelManager::GetPanel(PanelId id)
{
    auto it = panels_.find(id);
    if (it != panels_.end())
        return it->second.get();
    return nullptr;
}

std::vector<Panel*> PanelManager::GetSidebarPanels() const
{
    std::vector<Panel*> result;
    
    for (Panel* p : panelList_)
    {
        if (p->GetConfig().showInSidebar)
            result.push_back(p);
    }
    
    std::sort(result.begin(), result.end(), [](Panel* a, Panel* b) {
        const auto& ca = a->GetConfig();
        const auto& cb = b->GetConfig();
        if (ca.pinToBottom != cb.pinToBottom)
            return !ca.pinToBottom;
        return ca.order < cb.order;
    });
    
    return result;
}

void PanelManager::SetActivePanel(PanelId id)
{
    Panel* panel = GetPanel(id);
    if (!panel) return;

    const bool dockedRight = panel->IsDockedRight();
    for (Panel* candidate : panelList_)
    {
        if (!candidate || candidate == panel)
            continue;

        if (candidate->IsDockedRight() == dockedRight)
        {
            candidate->SetActive(false);
            candidate->SetVisible(false);
        }
    }
    
    activePanel_ = panel;
    activePanel_->SetActive(true);
    activePanel_->SetVisible(true);
}

Panel* PanelManager::GetActivePanel()
{
    if (activePanel_ && activePanel_->IsVisible())
        return activePanel_;

    if (Panel* right = GetVisiblePanel(true))
        return right;
    if (Panel* left = GetVisiblePanel(false))
        return left;

    return activePanel_;
}

PanelId PanelManager::GetActivePanelId() const
{
    if (activePanel_ && activePanel_->IsVisible())
        return activePanel_->GetId();

    if (Panel* right = GetVisiblePanel(true))
        return right->GetId();
    if (Panel* left = GetVisiblePanel(false))
        return left->GetId();

    return PanelId::Explorer;
}

bool PanelManager::IsPanelActive(PanelId id) const
{
    Panel* panel = const_cast<PanelManager*>(this)->GetPanel(id);
    return panel && panel->IsVisible() && panel->IsActive();
}

Panel* PanelManager::GetVisiblePanel(bool dockedRight) const
{
    for (Panel* panel : panelList_)
    {
        if (panel && panel->IsVisible() && panel->IsDockedRight() == dockedRight)
            return panel;
    }
    return nullptr;
}

Panel* PanelManager::GetPanelAtPoint(POINT clientPoint, bool includeResizeZone) const
{
    if (includeResizeZone)
    {
        for (Panel* panel : panelList_)
        {
            if (panel && panel->IsVisible() && panel->IsPointInResizeZone(clientPoint))
                return panel;
        }
    }

    for (Panel* panel : panelList_)
    {
        if (panel && panel->IsVisible() && panel->IsPointInPanel(clientPoint))
            return panel;
    }

    return nullptr;
}

Panel* PanelManager::GetResizingPanel() const
{
    for (Panel* panel : panelList_)
    {
        if (panel && panel->IsVisible() && panel->IsResizing())
            return panel;
    }
    return nullptr;
}

void PanelManager::DrawActivePanel(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite, HWND hwnd)
{
    for (Panel* panel : panelList_)
    {
        if (panel && panel->IsVisible())
            panel->Draw(ctx, dwrite, hwnd);
    }
}

void PanelManager::UpdateLayout(HWND hwnd)
{
    for (Panel* p : panelList_)
        p->UpdateLayout(hwnd);

    Panel* leftPanel = GetVisiblePanel(false);
    Panel* rightPanel = GetVisiblePanel(true);

    if (leftPanel && leftPanel->IsResizing())
        RememberPreferredWidth(preferredLogicalWidths_, leftPanel);
    if (rightPanel && rightPanel->IsResizing())
        RememberPreferredWidth(preferredLogicalWidths_, rightPanel);

    if (!leftPanel && !rightPanel)
        return;

    RECT clientRect = {};
    GetClientRect(hwnd, &clientRect);
    UINT dpi = win32_get_dpi_for_window(hwnd);

    const int clientPhysicalWidth = clientRect.right - clientRect.left;
    const int clientLogicalWidth = MulDiv(clientPhysicalWidth, 96, (int)dpi);
    const int sidebarPhysicalWidth = win32_dpi_scale(52, dpi);
    const int sidebarLogicalWidth = MulDiv(sidebarPhysicalWidth, 96, (int)dpi);
    const int minCenterLogicalWidth = 320;
    int preferredAvailableLogical = clientLogicalWidth - sidebarLogicalWidth - minCenterLogicalWidth;
    if (preferredAvailableLogical < 0)
        preferredAvailableLogical = 0;
    int totalAvailableLogical = clientLogicalWidth - sidebarLogicalWidth - 1;
    if (totalAvailableLogical < 0)
        totalAvailableLogical = 0;

    if (leftPanel && rightPanel)
    {
        const int preferredLeft = GetStoredPreferredWidth(preferredLogicalWidths_, leftPanel);
        const int preferredRight = GetStoredPreferredWidth(preferredLogicalWidths_, rightPanel);
        const auto targets = ComputeResponsiveTargets(
            preferredLeft,
            preferredRight,
            leftPanel->GetState().minWidth,
            rightPanel->GetState().minWidth,
            totalAvailableLogical,
            preferredAvailableLogical);

        bool widthChanged = false;
        if (leftPanel->GetLogicalWidth() != targets.left)
        {
            leftPanel->SetLogicalWidth(targets.left);
            widthChanged = true;
        }
        if (rightPanel->GetLogicalWidth() != targets.right)
        {
            rightPanel->SetLogicalWidth(targets.right);
            widthChanged = true;
        }

        if (widthChanged)
        {
            leftPanel->UpdateLayout(hwnd);
            rightPanel->UpdateLayout(hwnd);
        }
    }
    else
    {
        Panel* singlePanel = leftPanel ? leftPanel : rightPanel;
        const int preferredWidth = GetStoredPreferredWidth(preferredLogicalWidths_, singlePanel);

        if (singlePanel->GetLogicalWidth() != preferredWidth)
        {
            singlePanel->SetLogicalWidth(preferredWidth);
            singlePanel->UpdateLayout(hwnd);
        }
    }
}

void PanelManager::OnMouseMove(HWND hwnd, POINT clientPoint)
{
    if (Panel* panel = GetResizingPanel())
    {
        panel->OnMouseMove(hwnd, clientPoint);
        return;
    }

    if (Panel* panel = GetPanelAtPoint(clientPoint))
    {
        activePanel_ = panel;
        panel->OnMouseMove(hwnd, clientPoint);
        return;
    }

    if (Panel* panel = GetActivePanel())
        panel->OnMouseMove(hwnd, clientPoint);
}

void PanelManager::OnLeftButtonDown(HWND hwnd, POINT clientPoint)
{
    if (Panel* panel = GetPanelAtPoint(clientPoint))
    {
        activePanel_ = panel;
        panel->SetActive(true);
        panel->OnLeftButtonDown(hwnd, clientPoint);
        return;
    }
}

void PanelManager::OnLeftButtonUp(HWND hwnd)
{
    if (Panel* panel = GetResizingPanel())
    {
        panel->OnLeftButtonUp(hwnd);
        return;
    }

    if (Panel* panel = GetActivePanel())
        panel->OnLeftButtonUp(hwnd);
}

void PanelManager::OnRightButtonUp(HWND hwnd, POINT clientPoint)
{
    if (Panel* panel = GetPanelAtPoint(clientPoint, false))
    {
        activePanel_ = panel;
        panel->OnRightButtonUp(hwnd, clientPoint);
    }
}

void PanelManager::OnMouseWheel(HWND hwnd, int delta)
{
    if (Panel* panel = GetActivePanel())
        panel->OnMouseWheel(hwnd, delta);
}

void PanelManager::OnChar(wchar_t ch)
{
    if (activePanel_ && activePanel_->IsVisible())
        activePanel_->OnChar(ch);
}

void PanelManager::OnKeyDown(WPARAM key)
{
    if (activePanel_ && activePanel_->IsVisible())
        activePanel_->OnKeyDown(key);
}

bool PanelManager::IsAnyPanelResizing() const
{
    for (Panel* p : panelList_)
    {
        if (p->IsResizing())
            return true;
    }
    return false;
}

void PanelManager::ClearResizeHover(HWND hwnd)
{
    for (Panel* panel : panelList_)
    {
        if (panel && panel->IsVisible())
            panel->ClearResizeHover(hwnd);
    }
}

std::vector<PanelConfig> PanelManager::GetPanelConfigs() const
{
    std::vector<PanelConfig> configs;
    for (Panel* p : panelList_)
    {
        if (p->GetConfig().showInSidebar)
            configs.push_back(p->GetConfig());
    }
    
    std::sort(configs.begin(), configs.end(), [](const PanelConfig& a, const PanelConfig& b) {
        if (a.pinToBottom != b.pinToBottom)
            return !a.pinToBottom;
        return a.order < b.order;
    });
    
    return configs;
}
