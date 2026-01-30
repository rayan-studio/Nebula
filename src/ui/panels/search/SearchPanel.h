#pragma once
#include "ui/panels/Panel.h"
#include "ui/components/scrollbar/Scrollbar.h"
#include "ui/components/input/TextInput.h"
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

// ============================================================================
// Search Panel - Global file search
// ============================================================================

namespace Panels {

struct SearchResult {
    std::wstring filePath;
    std::wstring fileName;
    std::wstring lineExcerpt;
    int lineNumber = 0;
    int matchStart = 0;
    int matchLength = 0;
    
    SearchResult() = default;
    SearchResult(const std::wstring& path, const std::wstring& excerpt)
        : filePath(path), lineExcerpt(excerpt), lineNumber(0), matchStart(0), matchLength(0) 
    {
        // Extract filename from path
        size_t pos = path.find_last_of(L"\\/");
        fileName = (pos != std::wstring::npos) ? path.substr(pos + 1) : path;
    }
};

} // namespace Panels

class SearchPanel : public Panel
{
public:
    SearchPanel();
    ~SearchPanel() override;
    
    // Panel interface
    void Initialize() override;
    void Draw(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite, HWND hwnd) override;
    void UpdateLayout(HWND hwnd) override;
    void OnMouseMove(HWND hwnd, POINT clientPoint) override;
    void OnLeftButtonDown(HWND hwnd, POINT clientPoint) override;
    void OnLeftButtonUp(HWND hwnd) override;
    void OnMouseWheel(HWND hwnd, int delta) override;
    void OnChar(wchar_t ch) override;
    void OnKeyDown(WPARAM key) override;
    
    // Search API
    void SetSearchRoot(const std::wstring& rootPath);
    void ClearSearch();
    const std::wstring& GetQuery() const { return searchInput_.GetText(); }
    const std::vector<Panels::SearchResult>& GetResults() const { return searchResults_; }
    
    // Focus management
    bool IsInputFocused() const { return searchInput_.IsFocused(); }
    void FocusInput() { searchInput_.SetFocused(true); }
    void UnfocusInput() { searchInput_.SetFocused(false); }
    
    // Result selection
    int GetSelectedResultIndex() const { return selectedResultIndex_; }
    void OpenSelectedResult(HWND hwnd);

private:
    void DrawResults(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite);
    void UpdateSearchResults();
    int HitTestResult(POINT clientPoint) const;
    
    // Search input component
    TextInput searchInput_;
    
    // Search state
    std::wstring searchRoot_;
    std::vector<Panels::SearchResult> searchResults_;
    
    // UI state
    int hoveredResultIndex_ = -1;
    int selectedResultIndex_ = -1;
    
    // Layout
    float resultsTop_ = 0.0f;
    float resultItemHeight_ = 56.0f;
    
    // Scrollbar for results
    Scrollbar scrollbar_;

    // Background search thread to avoid blocking UI
    std::thread searchThread_;
    std::atomic<bool> searchCancel_{false};
    std::mutex searchMutex_;
    std::wstring latestQuery_;
    std::condition_variable searchCv_;
    bool workerStarted_ = false;
    std::atomic<bool> terminateWorker_{false};
};

// Global accessor
SearchPanel& GetSearchPanel();
