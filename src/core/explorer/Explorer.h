#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include "ui/components/scrollbar/Scrollbar.h"
#include "ui/components/input/InputTypeFixed.h"
#include <atomic>

// Structure pour représenter un item de l'explorer
struct ExplorerItem
{
    std::wstring name;
    std::wstring fullPath;
    std::string extension;
    bool isDirectory;
    int depth;       // Depth for tree indentation (0 = root)
    bool expanded;   // Is this folder expanded
    float yPosition; // Position Y de l'item dans l'interface
    float height;    // Hauteur de l'item
};

// Simple search result struct used by ExplorerManager
struct SearchResult {
    std::wstring filePath;
    std::wstring lineExcerpt;
};

// Structure pour l'état de l'Explorer
struct ExplorerState
{
    // Dimensions
    int logicalWidth;  // Largeur en pixels logiques
    int physicalWidth; // Largeur en pixels physiques (DPI-aware)
    int minWidth;      // Largeur minimale
    int maxWidth;      // Largeur maximale

    // Position et bounds
    float leftEdge;   // Position X gauche
    float rightEdge;  // Position X droite
    float topEdge;    // Position Y haut
    float bottomEdge; // Position Y bas

    // Interaction
    int hoveredItemIndex;      // Index de l'item survolé (-1 = aucun)

    // Contenu
    std::vector<ExplorerItem> items;
    std::wstring rootPath;
    std::wstring activePath; // chemin absolu de l'item marqué actif (onglet)

    // Layout constants
    float titleHeight;
    float itemHeight;
    float iconSize;
    float leftPadding;
    float topPadding;
    float itemSpacing;
    // Title toolbar button rects (client coordinates)
    RECT newFileButtonRect;
    RECT newFolderButtonRect;
    // Hover state for title toolbar buttons
    bool newFileButtonHovered = false;
    bool newFolderButtonHovered = false;
    
    // Resize state (synced from Panel)
    bool isHoveringResizeZone = false;
    bool isResizing = false;

    ExplorerState()
        : logicalWidth(280), physicalWidth(280), minWidth(150), maxWidth(600), leftEdge(0), rightEdge(0), topEdge(0), bottomEdge(0), hoveredItemIndex(-1), titleHeight(40.0f), itemHeight(24.0f), iconSize(18.0f), leftPadding(12.0f), topPadding(6.0f), itemSpacing(2.0f)
    {
    }
};

class ExplorerManager
{
public:
    ExplorerManager();
    ~ExplorerManager();
    // Ajouter un fichier ou dossier
    void CreateNewFile(const std::wstring &name);
    void CreateNewFolder(const std::wstring &name);

    // Obtenir le chemin du dossier actif (pour savoir où créer)
    std::wstring GetActiveDirectory() const;
    // Initialisation
    void Initialize(const std::wstring &rootPath);

    // Rendu
    void Draw(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd);

    // Mise à jour de la géométrie
    void UpdateLayout(HWND hwnd);

    // Gestion des événements souris
    void OnMouseMove(HWND hwnd, POINT clientPoint);
    void OnLeftButtonDown(HWND hwnd, POINT clientPoint);
    void OnLeftButtonUp(HWND hwnd);
    void OnRightButtonUp(HWND hwnd, POINT clientPoint);
    void OnMouseWheel(HWND hwnd, int delta);

    void HandleContextCommand(int commandId);

    // Marquer un fichier/dossier comme actif (ex: onglet actif)
    void SetActivePath(const std::wstring &path);

    // Requêtes d'état
    bool IsPointInExplorer(POINT clientPoint) const;
    int GetHoveredItemIndex() const { return state_.hoveredItemIndex; }
    ExplorerState& GetState() { return state_; }

    // Visibility control
    void SetVisible(bool v) { visible_ = v; }
    void ToggleVisible() { visible_ = !visible_; }
    bool IsVisible() const { return visible_; }

    // Accesseurs
    int GetLogicalWidth() const { return state_.logicalWidth; }
    void SetLogicalWidth(int width) { state_.logicalWidth = width; }
    float GetPhysicalRightEdge() const { return state_.rightEdge; }
    void ClearHover(HWND hwnd);
    bool IsScrollbarDragging() const { return scrollbar_.IsDragging(); }
    int GetPhysicalWidth() const { return state_.physicalWidth; }
    bool IsScrollbarHovering() const { return scrollbar_.IsHoveringThumb() || scrollbar_.IsHoveringTrack(); }

    // Public wrapper to retrieve an icon bitmap for an ExplorerItem (thread-safe caller)
    ID2D1Bitmap *GetIconForItemPublic(ID2D1RenderTarget *ctx, const ExplorerItem &item, HWND hwnd);

    // Public helper to load an SVG icon into an ID2D1Bitmap (wraps internal loader)
    ID2D1Bitmap *LoadSvgIconPublic(ID2D1RenderTarget *ctx, const std::string &iconPath, int pxSize, UINT dpi);

    // File system watcher control
    void StartWatching();
    void StopWatching();

private:
    ExplorerState state_;
    Scrollbar scrollbar_;
    bool visible_ = true;
    std::atomic<bool> ignoreNextChange_{false};
    // Helpers internes
    void LoadDirectoryContents();
    void UpdateItemPositions();
    int HitTestItem(POINT clientPoint) const;
    void UpdateSearchResults();
    void DrawBackground(ID2D1RenderTarget *ctx);
    void DrawTitle(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite);
    void DrawItems(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd);
    void DrawSearchPanel(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd);
    void DrawRightBorder(ID2D1RenderTarget *ctx);
    ID2D1Bitmap *GetIconForItem(ID2D1RenderTarget *ctx, const ExplorerItem &item, HWND hwnd);

    // Cache d'icônes
    std::unordered_map<std::string, ID2D1Bitmap *> iconCache_;

    // File system watcher handles
    HANDLE watcherThreadHandle_ = nullptr;
    HANDLE watcherStopEvent_ = nullptr;
    std::wstring watchPath_;
    // Protect access to state_.items and watcher operations
    std::mutex itemsMutex_;

    // Preview item shown while creating a new file/folder
    ExplorerItem previewItem_;
    std::atomic<bool> hasPreview_{false};

    // Inline input state (used instead of the global Input overlay)
    bool inlineVisible_ = false;
    Input::Type inlineType_ = Input::Type::File;
    std::wstring inlineText_;
    int inlineCursorPos_ = 0;
    D2D1_RECT_F inlineRect_ = D2D1::RectF(0,0,0,0);
    // Locked target path for inline create (prevents watcher/hover from moving it)
    bool inlineTargetLocked_ = false;
    std::wstring inlineTargetFullPath_;

    // Search state
    bool searchMode_ = false;
    std::wstring searchQuery_;
    std::vector<SearchResult> searchResults_;

public:
    // Preview API used by Input overlay
    void SetPreviewName(const std::wstring &name, Input::Type type);
    void ClearPreview();

    // Inline title input (used instead of global Input overlay)
    void ShowInlineInput(Input::Type type);
    // Show inline input prefilled for renaming the item at given index
    void ShowRenameInline(int itemIndex);
    void HideInlineInput();
    bool IsInlineInputVisible() const;
    void OnCharInline(wchar_t ch);
    void OnKeyDownInline(WPARAM key);

    // Rename state: index being renamed and original full path
    int renameTargetIndex_ = -1;
    std::wstring renameOriginalFullPath_;

    // Thread entry (private static so it can access private members)
    static DWORD WINAPI WatcherThreadStatic(LPVOID param);

    // Search mode
    void EnterSearchMode();
    void ExitSearchMode();
    bool IsSearchMode() const { return searchMode_; }
    void OnCharSearch(wchar_t ch);
    void OnKeyDownSearch(WPARAM key);

    const std::vector<SearchResult>& GetSearchResults() const { return searchResults_; }
};

// Fonction globale pour obtenir l'instance singleton
ExplorerManager &GetExplorerManager();