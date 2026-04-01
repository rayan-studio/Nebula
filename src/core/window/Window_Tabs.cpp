#include "core/window/Window.h"
#include "ui/graphics/Skia.h"
#include "orion/caret/Caret.h"
#include "lsp/LspManager.h"
#include "core/explorer/Explorer.h"
#include "ui/panels/git/GitDiffDecorations.h"
#include <filesystem>
#include <algorithm>
#include <cwctype>
#include <string>

static const std::wstring kSettingsTabPath = L"__settings__";
static const std::wstring kMarketplaceTabPrefix = L"__marketplace_lib__:";
static const std::wstring kCodeMapTabPath = L"__codemap__";

static std::wstring ToLower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c)
                   { return (wchar_t)std::towlower(c); });
    return value;
}

static bool ShouldOpenAsPreview(const std::wstring &filePath)
{
    std::wstring ext = ToLower(std::filesystem::path(filePath).extension().wstring());
    return ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".gif" ||
           ext == L".bmp" || ext == L".tiff" || ext == L".tif" || ext == L".webp" ||
           ext == L".ico" || ext == L".pdf";
}

static bool IsMarkdownFile(const std::wstring &filePath)
{
    std::wstring ext = ToLower(std::filesystem::path(filePath).extension().wstring());
    return ext == L".md";
}

void Window::CloseEditorForTabIndex(int index)
{
    // Remove and delete the editor for the given tab index, then reindex editors_ to match TabBar
    std::map<int, Orion::Editor *> old = editors_;

    auto it = old.find(index);
    if (it != old.end())
    {
        delete it->second;
        old.erase(it);
    }

    editors_.clear();
    pendingGoToLocation_.erase(index);

    int count = tabBar_.GetTabCount();
    for (int i = 0; i < count; ++i)
    {
        const auto *t = tabBar_.GetTab(i);
        if (!t)
            continue;

        Orion::Editor *found = nullptr;
        for (auto &p : old)
        {
            if (!p.second)
                continue;
            if (p.second->GetFilePath() == t->filePath)
            {
                found = p.second;
                break;
            }
            // Match untitled placeholders
            if (t->filePath.rfind(L"__untitled__", 0) == 0 && p.second->GetFilePath().rfind(L"__untitled__", 0) == 0)
            {
                found = p.second;
                break;
            }
        }

        editors_[i] = found;
        if (found)
        {
            // Ensure the editor's document-changed callback reflects its new tab index
            found->onDocumentChanged = [this, i]()
            {
                tabBar_.SetTabDirty(i, true);
                InvalidateRect(hwnd_, nullptr, FALSE);

                Orion::Editor *ed = GetEditorForTab(i);
                if (!ed)
                    return;
                std::wstring fp = ed->GetFilePath();
                if (fp.empty() || fp.rfind(L"__untitled__", 0) == 0)
                    return;
                ScheduleDiagnosticsForTab(i);
            };
        }
    }
}

Orion::Editor *Window::GetEditor()
{
    int idx = tabBar_.GetActiveTabIndex();
    return GetEditorForTab(idx);
}

Orion::Editor *Window::GetEditorForTab(int tabIndex)
{
    auto it = editors_.find(tabIndex);
    if (it != editors_.end())
        return it->second;
    return nullptr;
}

const std::wstring &Window::SettingsTabPath()
{
    return kSettingsTabPath;
}

const std::wstring &Window::CodeMapTabPath()
{
    return kCodeMapTabPath;
}

const std::wstring &Window::MarketplaceTabPrefix()
{
    return kMarketplaceTabPrefix;
}

std::wstring Window::MarketplaceLibraryNameFromTabPath(const std::wstring &tabPath)
{
    if (tabPath.rfind(kMarketplaceTabPrefix, 0) != 0)
        return std::wstring();
    return tabPath.substr(kMarketplaceTabPrefix.size());
}

bool Window::IsSettingsTabIndex(int tabIndex) const
{
    const Tab *tab = tabBar_.GetTab(tabIndex);
    return tab && tab->filePath == kSettingsTabPath;
}

bool Window::IsMarketplaceTabIndex(int tabIndex) const
{
    const Tab *tab = tabBar_.GetTab(tabIndex);
    return tab && tab->filePath.rfind(kMarketplaceTabPrefix, 0) == 0;
}

bool Window::IsCodeMapTabIndex(int tabIndex) const
{
    const Tab *tab = tabBar_.GetTab(tabIndex);
    return tab && tab->filePath == kCodeMapTabPath;
}

bool Window::IsSettingsTabActive() const
{
    return IsSettingsTabIndex(tabBar_.GetActiveTabIndex());
}

bool Window::IsCodeMapTabActive() const
{
    return IsCodeMapTabIndex(tabBar_.GetActiveTabIndex());
}

void Window::OpenMarketplaceLibraryTab(const std::wstring &libraryName)
{
    if (libraryName.empty())
        return;

    const std::wstring tabPath = kMarketplaceTabPrefix + libraryName;
    int existing = tabBar_.FindTabIndexByFilePath(tabPath);
    if (existing >= 0)
    {
        tabBar_.SetActiveTab(existing);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    tabBar_.AddTab(tabPath, libraryName);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Window::OpenCodeMapTab()
{
    int existing = tabBar_.FindTabIndexByFilePath(kCodeMapTabPath);
    if (existing >= 0)
    {
        tabBar_.SetActiveTab(existing);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    tabBar_.AddTab(kCodeMapTabPath, L"Code Map");
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Window::OpenFileInNewTab(const std::wstring &filePath, int lineNumber, int column)
{
    std::wstring display;
    size_t lastSlash = filePath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos)
        display = filePath.substr(lastSlash + 1);
    else
        display = filePath;

    int tabIndex = tabBar_.AddTab(filePath, display);
    if (!filePath.empty())
        tabBar_.SetTabMarkdown(tabIndex, IsMarkdownFile(filePath));
    if (editors_.count(tabIndex) == 0)
    {
        Orion::Editor *editor = new Orion::Editor();

        if (!customFontPath_.empty() && skia_)
            editor->LoadCustomFont(skia_->GetDWriteFactory(), customFontPath_);

        bool applyTampon = false;
        if (!filePath.empty())
        {
            if (GetExplorerManager().IsVisible())
                GetExplorerManager().SetActivePath(filePath);
            if (ShouldOpenAsPreview(filePath))
            {
                editor->LoadPreviewAsync(hwnd_, filePath, tabIndex);
            }
            else
            {
                editor->LoadFileAsync(hwnd_, filePath, tabIndex);
                if (lineNumber >= 0)
                    pendingGoToLocation_[tabIndex] = {lineNumber, column};
            }
        }
        else
        {
            editor->CreateEmpty();
            applyTampon = HasTamponText();
        }

        editors_[tabIndex] = editor;
        // Wire document-changed callback so TabBar is updated when editor becomes dirty
        editor->onDocumentChanged = [this, tabIndex]()
        {
            tabBar_.SetTabDirty(tabIndex, true);
            InvalidateRect(hwnd_, nullptr, FALSE);

            Orion::Editor *ed = GetEditorForTab(tabIndex);
            if (!ed)
                return;
            std::wstring fp = ed->GetFilePath();
            if (fp.empty() || fp.rfind(L"__untitled__", 0) == 0)
                return;
            ScheduleDiagnosticsForTab(tabIndex);
        };

        if (applyTampon)
            editor->SetTextContent(GetTamponText(), true);
    }
    else if (lineNumber >= 0)
    {
        Orion::Editor *editor = editors_[tabIndex];
        if (editor)
        {
            bool appliedSplit = false;
            if (GitDiffDecorations::ConsumePendingSplitOpen(filePath))
            {
                GitDiffDecorations::SplitViewData splitData;
                if (GitDiffDecorations::GetSplitForFile(filePath, splitData))
                {
                    std::vector<Orion::Editor::GitSplitDiffRow> rows;
                    rows.reserve(splitData.rows.size());
                    for (const auto &row : splitData.rows)
                    {
                        Orion::Editor::GitSplitDiffRow outRow;
                        outRow.leftText = row.leftText;
                        outRow.rightText = row.rightText;
                        outRow.hasLeft = row.hasLeft;
                        outRow.hasRight = row.hasRight;
                        outRow.leftDeleted = row.leftDeleted;
                        outRow.rightAdded = row.rightAdded;
                        rows.push_back(std::move(outRow));
                    }
                    editor->SetGitSplitDiffView(rows);
                    appliedSplit = true;
                }
            }

            if (!appliedSplit)
            {
                int targetLine = lineNumber < 0 ? 0 : lineNumber;
                int targetCol = column < 0 ? 0 : column;
                Orion::Caret::SetCaret(*editor, targetLine, targetCol);
                editor->RevealCaretOnNextLayout();
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    if (!filePath.empty() && GetExplorerManager().IsVisible())
        GetExplorerManager().SetActivePath(filePath);
}

void Window::OpenFileInNewTabWithMarkdownPreview(const std::wstring &filePath)
{
    if (filePath.empty())
        return;

    OpenFileInNewTab(filePath, -1, -1);

    int idx = tabBar_.FindTabIndexByFilePath(filePath);
    if (idx >= 0)
    {
        pendingMarkdownPreview_.insert(idx);
        tabBar_.SetTabMarkdown(idx, true);
        tabBar_.SetTabMarkdownViewMode(idx, Orion::MarkdownViewMode::Preview);

        Orion::Editor *ed = GetEditorForTab(idx);
        if (ed && ed->HasFile())
        {
            ed->SetMarkdownViewMode(Orion::MarkdownViewMode::Preview);
            pendingMarkdownPreview_.erase(idx);
        }
        tabBar_.SetActiveTab(idx);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void Window::OpenSettingsTab()
{
    int existing = tabBar_.FindTabIndexByFilePath(kSettingsTabPath);
    if (existing >= 0)
    {
        tabBar_.SetActiveTab(existing);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    tabBar_.AddTab(kSettingsTabPath, L"Settings");
    InvalidateRect(hwnd_, nullptr, FALSE);
}

