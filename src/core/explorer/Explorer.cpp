#include "Explorer.h"
#include <thread>
#include <atomic>
#include <functional>
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include "helpers/window_helpers.h"
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <regex>
#include "ui/components/popups/CustomPopup.h"
#include <shellapi.h>
#include "ui/components/titlebar/TitleBar.h"
#include "core/window/Window.h"
#include "utils/logger/Logger.h"
#include "ui/layout/ExplorerLayoutState.h"
#include "lsp/LspManager.h"

// Disable min/max macros from Windows headers
#undef min
#undef max

// NanoSVG integration
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244 4456 4702)
#endif

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "../external/nanosvg/src/nanosvg.h"
#include "../external/nanosvg/src/nanosvgrast.h"

#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace
{
    // Singleton instance
    ExplorerManager *g_explorerManager = nullptr;

    // Icon mapping cache
    std::unordered_map<std::string, std::string> g_iconMap;
    std::mutex g_iconMapMutex;
    std::atomic<bool> g_iconMapReady{false};
    std::atomic<bool> g_iconMapLoading{false};
    const std::string g_defaultIconPath = "assets/ressource/icons/document.svg";

}

// ============================================================================
// Icon Management
// ============================================================================

static std::string ReadFileToString(const std::wstring &wpath)
{
    // Convert wide path to UTF-8 safely. WideCharToMultiByte returns the
    // required buffer size INCLUDING the terminating null, so allocate that
    // many bytes, perform the conversion, then remove the trailing '\0'
    // before using the std::string path.
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, NULL, 0, NULL, NULL);
    if (size_needed <= 0)
        return {};

    std::string path;
    path.resize(size_needed);
    WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, &path[0], size_needed, NULL, NULL);
    if (!path.empty() && path.back() == '\0')
        path.pop_back();

    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};

    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void InvalidateMainWindow();

static void LoadIconMapBlocking()
{
    std::wstring jsonPath = L"assets\\ressource\\material-icons.json";
    std::string content = ReadFileToString(jsonPath);
    std::unordered_map<std::string, std::string> local;
    if (content.empty())
    {
        {
            std::lock_guard<std::mutex> lk(g_iconMapMutex);
            g_iconMap = std::move(local);
        }
        g_iconMapReady.store(true);
        g_iconMapLoading.store(false);
        return;
    }

    std::regex re(R"((?:"|')([a-zA-Z0-9_\-\.]+)(?:"|')\s*:\s*\{[^}]*?(?:"|')iconPath(?:"|')\s*:\s*(?:"|')([^"']+)(?:"|'))");
    std::smatch m;
    auto it = content.cbegin();

    while (std::regex_search(it, content.cend(), m, re))
    {
        if (m.size() >= 3)
        {
            std::string key = m[1].str();
            std::string path = m[2].str();
            if (path.size() >= 2 && path[0] == '.' && path[1] == '/')
            {
                path = path.substr(2);
            }
            local[key] = path;
        }
        it = m.suffix().first;
    }

    {
        std::lock_guard<std::mutex> lk(g_iconMapMutex);
        g_iconMap = std::move(local);
    }
    g_iconMapReady.store(true);
    g_iconMapLoading.store(false);
    InvalidateMainWindow();
}

static void LoadIconMapIfNeeded(bool allowBlocking)
{
    if (g_iconMapReady.load())
        return;

    bool expected = false;
    if (!g_iconMapLoading.compare_exchange_strong(expected, true))
        return;

    if (allowBlocking)
    {
        LoadIconMapBlocking();
        return;
    }

    std::thread([]()
                { LoadIconMapBlocking(); })
        .detach();
}

static std::string GetIconPathForExtension(const std::string &ext)
{
    LoadIconMapIfNeeded(false);
    if (!g_iconMapReady.load())
        return g_defaultIconPath;

    std::string key = ext;
    if (!key.empty() && key[0] == '.')
        key = key.substr(1);

    // Lowercase (safe cast)
    for (size_t i = 0; i < key.size(); ++i)
    {
        key[i] = (char)::tolower((unsigned char)key[i]);
    }

    {
        std::lock_guard<std::mutex> lk(g_iconMapMutex);
        auto it = g_iconMap.find(key);
        if (it != g_iconMap.end())
        {
            return "assets/ressource/" + it->second;
        }

        // Fallback
        auto fallback = g_iconMap.find("document");
        if (fallback != g_iconMap.end())
        {
            return "assets/ressource/" + fallback->second;
        }
    }

    return g_defaultIconPath;
}

static ID2D1Bitmap *CreateBitmapFromRGBA(ID2D1RenderTarget *ctx, unsigned char *data, int w, int h, float dpi)
{
    if (!ctx)
        return nullptr;

    // Premultiply alpha
    std::vector<unsigned char> premultiplied(w * h * 4);
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            int idx = (y * w + x) * 4;
            unsigned char r = data[idx + 0];
            unsigned char g = data[idx + 1];
            unsigned char b = data[idx + 2];
            unsigned char a = data[idx + 3];
            float af = a / 255.0f;

            premultiplied[idx + 0] = (unsigned char)(r * af + 0.5f);
            premultiplied[idx + 1] = (unsigned char)(g * af + 0.5f);
            premultiplied[idx + 2] = (unsigned char)(b * af + 0.5f);
            premultiplied[idx + 3] = a;
        }
    }

    D2D1_SIZE_U size = D2D1::SizeU(w, h);
    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);

    ID2D1Bitmap *bmp = nullptr;
    ctx->CreateBitmap(size, premultiplied.data(), w * 4, props, &bmp);
    return bmp;
}

static ID2D1Bitmap *LoadSvgIcon(ID2D1RenderTarget *ctx, const std::string &iconPath, int pxSize, UINT dpi)
{
    if (iconPath.empty())
        return nullptr;

    NSVGimage *image = nsvgParseFromFile(iconPath.c_str(), "px", 96.0f);
    if (!image)
        return nullptr;

    NSVGrasterizer *rast = nsvgCreateRasterizer();

    // Rasterize at higher internal resolution (2x) and let D2D downscale with linear filtering.
    const int oversample = 2;
    int scaledPx = pxSize * oversample;
    std::vector<unsigned char> buffer(scaledPx * scaledPx * 4);

    float scaleW = (float)scaledPx / image->width;
    float scaleH = (float)scaledPx / image->height;
    float scale = (scaleW < scaleH) ? scaleW : scaleH;
    float tx = (scaledPx - image->width * scale) * 0.5f;
    float ty = (scaledPx - image->height * scale) * 0.5f;

    nsvgRasterize(rast, image, tx, ty, scale, buffer.data(), scaledPx, scaledPx, scaledPx * 4);

    // Create a bitmap that declares a DPI multiplied by the oversample factor so D2D maps pixels correctly.
    ID2D1Bitmap *bmp = CreateBitmapFromRGBA(ctx, buffer.data(), scaledPx, scaledPx, (float)dpi * (float)oversample);

    nsvgDelete(image);
    nsvgDeleteRasterizer(rast);

    return bmp;
}

// ============================================================================
// ExplorerManager Implementation
// ============================================================================
ExplorerManager::ExplorerManager()
{
    state_.logicalWidth = 280;
    inlineVisible_ = false;
    inlineCursorPos_ = 0;
    inlineType_ = Input::Type::File;
    searchMode_ = false;

    // Configure the "Ouvrir un projet" button
    openProjectButton_.SetText(L"Ouvrir un projet");
    openProjectButton_.SetOnClick([]
    {
        HWND wnd = FindWindowW(L"NebulaTextWindowClass", NULL);
        if (wnd)
            PostMessageW(wnd, WM_COMMAND, 3003, 0);
    });
}

ExplorerManager::~ExplorerManager()
{
    // Cleanup icon cache
    for (auto &pair : iconCache_)
    {
        if (pair.second)
            pair.second->Release();
    }
    iconCache_.clear();

    // Stop watcher thread if running
    StopWatching();
}

void ExplorerManager::Initialize(const std::wstring &rootPath)
{
    state_.rootPath = rootPath;
    LoadDirectoryContents();
    // store and start watcher
    watchPath_ = rootPath;
    StartWatching();

    Lsp::LspManager::Instance().SetProjectRoot(rootPath);

    // No background language server startup.
}

void ExplorerManager::PreloadIconMapAsync()
{
    LoadIconMapIfNeeded(false);
}

// Helper: invalidate main window when change detected
static void InvalidateMainWindow()
{
    // Find Nebula main window by class name used in Window.cpp
    HWND wnd = FindWindowW(L"NebulaTextWindowClass", NULL);
    if (wnd)
    {
        RECT tb = {};
        GetClientRect(wnd, &tb);
        InvalidateRect(wnd, NULL, FALSE);
    }
}

DWORD WINAPI ExplorerManager::WatcherThreadStatic(LPVOID param)
{
    ExplorerManager *mgr = reinterpret_cast<ExplorerManager *>(param);
    if (!mgr)
        return 0;

    std::wstring dir = mgr->watchPath_;
    if (dir.empty())
        return 0;

    HANDLE hDir = CreateFileW(dir.c_str(), FILE_LIST_DIRECTORY,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);

    if (hDir == INVALID_HANDLE_VALUE)
        return 0;

    const DWORD bufSize = 16 * 1024;
    std::vector<BYTE> buffer(bufSize);
    DWORD lastReloadTime = 0;
    const DWORD minReloadInterval = 500; // Ne recharger qu'une fois toutes les 500ms

    while (WaitForSingleObject(mgr->watcherStopEvent_, 0) == WAIT_TIMEOUT)
    {
        DWORD bytesReturned = 0;
        BOOL ok = ReadDirectoryChangesW(hDir, buffer.data(), bufSize, TRUE,
                                        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME,
                                        &bytesReturned, NULL, NULL);

        if (ok && bytesReturned > 0)
        {
            // Vérifier qu'il s'agit bien d'un changement de structure (ajout/suppression)
            FILE_NOTIFY_INFORMATION *info = (FILE_NOTIFY_INFORMATION *)buffer.data();
            bool shouldReload = false;

            while (info)
            {
                // Ne recharger que si fichier/dossier créé ou supprimé
                if (info->Action == FILE_ACTION_ADDED ||
                    info->Action == FILE_ACTION_REMOVED ||
                    info->Action == FILE_ACTION_RENAMED_NEW_NAME ||
                    info->Action == FILE_ACTION_RENAMED_OLD_NAME)
                {
                    shouldReload = true;
                    break;
                }

                // Passer à l'événement suivant
                if (info->NextEntryOffset == 0)
                    break;
                info = (FILE_NOTIFY_INFORMATION *)((BYTE *)info + info->NextEntryOffset);
            }

            if (shouldReload)
            {
                // Si le flag d'ignore est activé, ignorer ce prochain changement lié
                if (mgr->ignoreNextChange_.load())
                {
                    mgr->ignoreNextChange_.store(false);
                    continue;
                }
                // Debounce: attendre qu'il n'y ait plus d'événements pendant 200ms
                DWORD quietStart = GetTickCount();
                const DWORD quietPeriod = 200;
                bool stillChanging = true;

                while (stillChanging && (GetTickCount() - quietStart < 2000)) // max 2s d'attente
                {
                    Sleep(50);
                    DWORD br = 0;
                    BOOL hasMore = ReadDirectoryChangesW(hDir, buffer.data(), bufSize, TRUE,
                                                         FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME,
                                                         &br, NULL, NULL);

                    if (!hasMore || br == 0)
                    {
                        // Plus d'événements pendant 50ms
                        if (GetTickCount() - quietStart >= quietPeriod)
                        {
                            stillChanging = false;
                        }
                    }
                    else
                    {
                        // Nouveaux événements, recommencer le timer
                        quietStart = GetTickCount();
                    }
                }

                // Vérifier l'intervalle minimum entre deux rechargements
                DWORD now = GetTickCount();
                if (now - lastReloadTime >= minReloadInterval)
                {
                    mgr->LoadDirectoryContents();
                    InvalidateMainWindow();
                    lastReloadTime = now;
                }
            }
        }

        Sleep(50);
    }

    CloseHandle(hDir);
    return 0;
}

void ExplorerManager::CreateNewFile(const std::wstring &name)
{
    std::wstring targetPath = GetActiveDirectory() + L"\\" + name;

    try
    {
        // Indiquer au watcher d'ignorer le prochain changement (optionnel)
        ignoreNextChange_.store(true);

        // Créer un fichier vide
        std::ofstream file;
        int size = WideCharToMultiByte(CP_UTF8, 0, targetPath.c_str(), -1, NULL, 0, NULL, NULL);
        std::string path(size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, targetPath.c_str(), -1, path.data(), size, NULL, NULL);

#ifdef _DEBUG
        // Debug temporaires pour vérifier l'appel
        MessageBoxW(nullptr, (L"Création fichier: " + name).c_str(), L"Debug", MB_OK);
        MessageBoxW(nullptr, (L"Chemin complet: " + targetPath).c_str(), L"Debug", MB_OK);
#endif

        file.open(path);
        if (file.is_open())
        {
            file.close();
            // Recharger manuellement le contenu pour garantir que le nouvel item soit visible
            Logger::Instance().Log(L"Explorer: created file: " + targetPath);
            LoadDirectoryContents();
            // Sélectionner le nouvel élément afin qu'il soit visible et highlighté
            SetActivePath(targetPath);
            // Ouvrir le fichier dans l'éditeur
            HWND wnd = FindWindowW(L"NebulaTextWindowClass", NULL);
            if (wnd)
            {
                auto *heapPath = new std::wstring(targetPath);
                PostMessageW(wnd, WM_USER + 100, 0, (LPARAM)heapPath);
            }
            InvalidateMainWindow();
        }
        else
        {
            MessageBoxW(nullptr, L"Impossible de créer le fichier.", L"Erreur", MB_OK | MB_ICONERROR);
        }
    }
    catch (...)
    {
        MessageBoxW(nullptr, L"Erreur lors de la création du fichier.", L"Erreur", MB_OK | MB_ICONERROR);
    }
}

void ExplorerManager::CreateNewFolder(const std::wstring &name)
{
    std::wstring targetPath = GetActiveDirectory() + L"\\" + name;

    try
    {
        // Indiquer au watcher d'ignorer le prochain changement (optionnel)
        ignoreNextChange_.store(true);

        if (std::filesystem::create_directory(targetPath))
        {
#ifdef _DEBUG
            MessageBoxW(nullptr, (L"Création dossier: " + name).c_str(), L"Debug", MB_OK);
            MessageBoxW(nullptr, (L"Chemin complet: " + targetPath).c_str(), L"Debug", MB_OK);
#endif
            Logger::Instance().Log(L"Explorer: created folder: " + targetPath);
            // Recharger manuellement pour garantir visibilité
            LoadDirectoryContents();
            SetActivePath(targetPath);
            InvalidateMainWindow();
        }
        else
        {
            MessageBoxW(nullptr, L"Impossible de créer le dossier.", L"Erreur", MB_OK | MB_ICONERROR);
        }
    }
    catch (...)
    {
        MessageBoxW(nullptr, L"Erreur lors de la création du dossier.", L"Erreur", MB_OK | MB_ICONERROR);
    }
}

std::wstring ExplorerManager::GetActiveDirectory() const
{
    // Si un dossier est hover/sélectionné, retourner ce chemin
    if (state_.hoveredItemIndex >= 0 && state_.hoveredItemIndex < (int)state_.items.size())
    {
        const auto &item = state_.items[state_.hoveredItemIndex];
        if (item.isDirectory)
        {
            return item.fullPath;
        }
        else
        {
            // Si c'est un fichier, retourner le dossier parent
            std::filesystem::path p(item.fullPath);
            return p.parent_path().wstring();
        }
    }

    // Sinon, retourner le dossier racine
    return state_.rootPath;
}
void ExplorerManager::StartWatching()
{
    StopWatching();

    if (watchPath_.empty())
        return;

    watcherStopEvent_ = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!watcherStopEvent_)
        return;

    watcherThreadHandle_ = CreateThread(NULL, 0, ExplorerManager::WatcherThreadStatic, this, 0, NULL);
    if (!watcherThreadHandle_)
    {
        CloseHandle(watcherStopEvent_);
        watcherStopEvent_ = nullptr;
    }
}

void ExplorerManager::StopWatching()
{
    if (watcherStopEvent_)
    {
        SetEvent(watcherStopEvent_);
    }

    if (watcherThreadHandle_)
    {
        // wait briefly for thread to exit
        WaitForSingleObject(watcherThreadHandle_, 2000);
        CloseHandle(watcherThreadHandle_);
        watcherThreadHandle_ = nullptr;
    }

    if (watcherStopEvent_)
    {
        CloseHandle(watcherStopEvent_);
        watcherStopEvent_ = nullptr;
    }
}
void ExplorerManager::LoadDirectoryContents()
{
    // Preserve previous expanded state to avoid collapsing the tree when reloading.
    std::unordered_map<std::wstring, bool> prevExpanded;
    {
        std::lock_guard<std::mutex> lk(itemsMutex_);
        for (const auto &it : state_.items)
        {
            if (!it.fullPath.empty() && it.isDirectory)
                prevExpanded[it.fullPath] = it.expanded;
        }
    }

    // Lock while modifying the items vector to avoid races with watcher thread
    std::lock_guard<std::mutex> lk(itemsMutex_);
    state_.items.clear();

    if (state_.rootPath.empty())
        return;

    try
    {
        std::filesystem::path root(state_.rootPath);
        if (!std::filesystem::exists(root))
            return;

        std::unordered_set<std::wstring> seen;
        for (const auto &entry : std::filesystem::directory_iterator(root))
        {
            if (state_.items.size() >= 1000)
                break;

            std::wstring full = entry.path().wstring();
            if (seen.find(full) != seen.end())
                continue;
            seen.insert(full);

            ExplorerItem item;
            item.name = entry.path().filename().wstring();
            item.fullPath = full;
            item.extension = entry.path().extension().string();
            item.isDirectory = entry.is_directory();
            item.depth = 0;

            // CORRECTION 1: Restaurer l'état expanded IMMÉDIATEMENT
            auto f = prevExpanded.find(full);
            item.expanded = (f != prevExpanded.end()) ? f->second : false;

            state_.items.push_back(item);
        }

        std::sort(state_.items.begin(), state_.items.end(),
                  [](const ExplorerItem &a, const ExplorerItem &b) -> bool
                  {
                      if (a.isDirectory != b.isDirectory)
                          return a.isDirectory;
                      return a.name < b.name;
                  });

        std::vector<ExplorerItem> expandedItems;
        std::function<void(const std::wstring &, int, int &)> collectRec;
        const int maxRecursionDepth = 8;
        const size_t maxTotalChildren = 1000;

        collectRec = [&](const std::wstring &dirPath, int depth, int &totalCollected)
        {
            if (depth > maxRecursionDepth || totalCollected >= (int)maxTotalChildren)
                return;

            try
            {
                std::filesystem::path p(dirPath);
                std::vector<ExplorerItem> children;
                for (const auto &entry : std::filesystem::directory_iterator(p))
                {
                    if (totalCollected >= (int)maxTotalChildren)
                        break;

                    std::wstring full = entry.path().wstring();

                    // CORRECTION 2: Vérifier les doublons avec seen
                    if (seen.find(full) != seen.end())
                        continue;
                    seen.insert(full);

                    ExplorerItem ci;
                    ci.name = entry.path().filename().wstring();
                    ci.fullPath = full;
                    ci.extension = entry.path().extension().string();
                    ci.isDirectory = entry.is_directory();
                    ci.depth = depth;

                    // Restaurer l'état expanded immédiatement
                    auto f = prevExpanded.find(ci.fullPath);
                    ci.expanded = (f != prevExpanded.end()) ? f->second : false;

                    children.push_back(ci);
                    totalCollected++;
                }

                std::sort(children.begin(), children.end(), [](const ExplorerItem &a, const ExplorerItem &b)
                          {
                              if (a.isDirectory != b.isDirectory)
                                  return a.isDirectory;
                              return a.name < b.name; });

                for (auto &c : children)
                {
                    expandedItems.push_back(c);
                    if (c.isDirectory && c.expanded)
                    {
                        collectRec(c.fullPath, depth + 1, totalCollected);
                    }
                }
            }
            catch (...)
            {
            }
        };

        int total = 0;
        for (size_t i = 0; i < state_.items.size(); ++i)
        {
            ExplorerItem &parent = state_.items[i];
            expandedItems.push_back(parent);
            if (parent.isDirectory && parent.expanded)
            {
                collectRec(parent.fullPath, parent.depth + 1, total);
            }
        }

        state_.items.swap(expandedItems);

        if (!state_.activePath.empty())
        {
            for (size_t i = 0; i < state_.items.size(); ++i)
            {
                if (state_.items[i].fullPath == state_.activePath)
                {
                    state_.hoveredItemIndex = (int)i;
                    break;
                }
            }
        }
    }
    catch (...)
    {
    }

    UpdateItemPositions();
    InvalidateMainWindow();
}

void ExplorerManager::SetActivePath(const std::wstring &path)
{
    state_.activePath = path;

    // Try to find the exact item index first
    {
        std::lock_guard<std::mutex> lk(itemsMutex_);
        for (size_t i = 0; i < state_.items.size(); ++i)
        {
            if (!state_.items[i].fullPath.empty() && state_.items[i].fullPath == path)
            {
                state_.hoveredItemIndex = (int)i;
                return;
            }
        }

        // If not found, try to find a parent directory in the current items
        // and expand it so the active file becomes visible.
        for (size_t i = 0; i < state_.items.size(); ++i)
        {
            ExplorerItem &item = state_.items[i];
            if (!item.isDirectory || item.fullPath.empty())
                continue;

            // Ensure path has a trailing separator when comparing prefixes
            std::wstring parentPath = item.fullPath;
            if (parentPath.back() != L'\\' && parentPath.back() != L'/')
                parentPath.push_back(L'\\');

            if (path.size() > parentPath.size() &&
                std::equal(parentPath.begin(), parentPath.end(), path.begin()))
            {
                // Found a parent that should contain the file. If not expanded,
                // enumerate children and insert them so the file can be highlighted.
                if (!item.expanded)
                {
                    try
                    {
                        std::filesystem::path p(item.fullPath);
                        std::vector<ExplorerItem> children;
                        for (const auto &entry : std::filesystem::directory_iterator(p))
                        {
                            ExplorerItem ci;
                            ci.name = entry.path().filename().wstring();
                            ci.fullPath = entry.path().wstring();
                            ci.extension = entry.path().extension().string();
                            ci.isDirectory = entry.is_directory();
                            ci.depth = item.depth + 1;
                            ci.expanded = false;
                            children.push_back(ci);
                            if (children.size() >= 100)
                                break;
                        }
                        std::sort(children.begin(), children.end(), [](const ExplorerItem &a, const ExplorerItem &b)
                                  {
                                      if (a.isDirectory != b.isDirectory)
                                          return a.isDirectory;
                                      return a.name < b.name; });

                        state_.items.insert(state_.items.begin() + i + 1, children.begin(), children.end());
                        item.expanded = true;
                    }
                    catch (...)
                    {
                    }
                }

                // After ensuring expansion, try to find the child item index
                for (size_t j = i + 1; j < state_.items.size(); ++j)
                {
                    // children have greater depth than parent
                    if (state_.items[j].depth <= item.depth)
                        break;
                    if (state_.items[j].fullPath == path)
                    {
                        state_.hoveredItemIndex = (int)j;
                        UpdateItemPositions();
                        InvalidateMainWindow();
                        return;
                    }
                }

                // If we couldn't find the exact file, at least highlight the parent
                state_.hoveredItemIndex = (int)i;
                UpdateItemPositions();
                InvalidateMainWindow();
                return;
            }
        }
    }

    // Not found at all
    state_.hoveredItemIndex = -1;
}

void ExplorerManager::UpdateItemPositions()
{
    float y = state_.topEdge + state_.titleHeight + state_.topPadding - scrollbar_.GetScrollOffset(); // MODIFIEZ CETTE LIGNE

    for (auto &item : state_.items)
    {
        item.yPosition = y;
        item.height = state_.itemHeight;
        y += state_.itemHeight + state_.itemSpacing;
    }
}

void ExplorerManager::UpdateLayout(HWND hwnd)
{
    RECT client;
    GetClientRect(hwnd, &client);

    UINT dpi = GetDpiForWindow(hwnd);
    RECT tbRect = win32_titlebar_rect(hwnd);

    int sidebarWidth = win32_dpi_scale(52, dpi);
    state_.physicalWidth = win32_dpi_scale(state_.logicalWidth, dpi);

    ExplorerPlacement placement = GetExplorerLayoutState().placement;
    if (placement == ExplorerPlacement::Right)
    {
        state_.rightEdge = (float)client.right;
        state_.leftEdge = state_.rightEdge - (float)state_.physicalWidth;

        float minLeft = (float)(client.left + sidebarWidth);
        if (state_.leftEdge < minLeft)
        {
            state_.leftEdge = minLeft;
            state_.physicalWidth = (int)(state_.rightEdge - state_.leftEdge);
        }
    }
    else
    {
        state_.leftEdge = (float)(client.left + sidebarWidth);
        state_.rightEdge = state_.leftEdge + (float)state_.physicalWidth;

        if (state_.rightEdge > (float)client.right)
        {
            state_.rightEdge = (float)client.right;
            state_.physicalWidth = (int)(state_.rightEdge - state_.leftEdge);
        }
    }
    state_.topEdge = (float)tbRect.bottom;
    // Reserve space for footer so explorer content doesn't overlap it
    int footerLogicalH = 28;
    int footerH = win32_dpi_scale(footerLogicalH, dpi);
    state_.bottomEdge = (float)(client.bottom - footerH);

    UpdateItemPositions();

    // Bouton "Ouvrir un projet" (affiché seulement si rootPath vide)
    {
        float left  = state_.leftEdge + state_.leftPadding;
        float right = state_.rightEdge - state_.leftPadding;

        float top = state_.topEdge + state_.titleHeight + 10.0f;
        float h   = 34.0f;

        openProjectButtonRect_ = D2D1::RectF(left, top, right, top + h);
        openProjectButton_.SetRect(openProjectButtonRect_);
    }

    // If invisible, effectively collapse to zero width so layout uses 0
    if (!visible_)
    {
        state_.physicalWidth = 0;
        if (placement == ExplorerPlacement::Right)
        {
            state_.leftEdge = state_.rightEdge;
        }
        else
        {
            state_.rightEdge = state_.leftEdge;
        }
    }

    // AJOUTEZ CES LIGNES :
    float contentTop = state_.topEdge + state_.titleHeight + state_.topPadding;
    float viewportHeight = state_.bottomEdge - contentTop;
    float contentHeight = state_.items.size() * (state_.itemHeight + state_.itemSpacing);

    scrollbar_.UpdateLayout(
        state_.leftEdge,
        contentTop,
        (float)state_.physicalWidth,
        viewportHeight,
        contentHeight);
}

int ExplorerManager::HitTestItem(POINT clientPoint) const
{
    if (clientPoint.x < (int)state_.leftEdge || clientPoint.x > (int)state_.rightEdge)
    {
        return -1;
    }

    float contentTop = state_.topEdge + state_.titleHeight;
    if (clientPoint.y < (int)contentTop)
        return -1;

    for (size_t i = 0; i < state_.items.size(); ++i)
    {
        const auto &item = state_.items[i];
        if (clientPoint.y >= (int)item.yPosition &&
            clientPoint.y < (int)(item.yPosition + item.height))
        {
            return (int)i;
        }
    }

    return -1;
}

bool ExplorerManager::IsPointInExplorer(POINT clientPoint) const
{
    return (clientPoint.x >= (int)state_.leftEdge &&
            clientPoint.x <= (int)state_.rightEdge &&
            clientPoint.y >= (int)state_.topEdge &&
            clientPoint.y <= (int)state_.bottomEdge);
}

void ExplorerManager::OnMouseMove(HWND hwnd, POINT clientPoint)
{
    // PRIORITÉ 1 : Scrollbar (si on drag ou si la souris est dessus)
    if (scrollbar_.OnMouseMove(clientPoint))
    {
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    // Si la scrollbar gère le hover, ne pas gérer l'Explorer
    if (scrollbar_.IsHoveringThumb() || scrollbar_.IsHoveringTrack())
    {
        // Réinitialiser le hover de l'Explorer
        if (state_.hoveredItemIndex != -1)
        {
            state_.hoveredItemIndex = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        return;
    }

    // Hover bouton "Ouvrir un projet" quand pas de projet
    if (state_.rootPath.empty() && !searchMode_)
    {
        bool changed = openProjectButton_.OnMouseMove(clientPoint);
        if (changed)
            InvalidateRect(hwnd, nullptr, FALSE);

        // if mouse over the button we could early-return to avoid item hover
        // but keep current behavior (just visual)
    }

    // PRIORITÉ 2 : Mode normal - vérifier hover
    int oldHovered = state_.hoveredItemIndex;

    if (IsPointInExplorer(clientPoint))
    {
        // If inline input target is locked (creating/renaming), do not change hovered index
        if (!inlineTargetLocked_)
        {
            state_.hoveredItemIndex = HitTestItem(clientPoint);
        }
        SetCursor(LoadCursor(NULL, IDC_ARROW));
    }
    else
    {
        state_.hoveredItemIndex = -1;
        SetCursor(LoadCursor(NULL, IDC_ARROW));
    }

    // Update hover for the title toolbar buttons as well
    bool oldFileHover = state_.newFileButtonHovered;
    bool oldFolderHover = state_.newFolderButtonHovered;

    state_.newFileButtonHovered = false;
    state_.newFolderButtonHovered = false;

    // Only consider title button hover if mouse is in the title area
    float titleTop = state_.topEdge;
    float titleBottom = state_.topEdge + state_.titleHeight;
    if (clientPoint.y >= (int)titleTop && clientPoint.y <= (int)titleBottom)
    {
        if (clientPoint.x >= state_.newFileButtonRect.left && clientPoint.x <= state_.newFileButtonRect.right &&
            clientPoint.y >= state_.newFileButtonRect.top && clientPoint.y <= state_.newFileButtonRect.bottom)
        {
            state_.newFileButtonHovered = true;
        }
        if (clientPoint.x >= state_.newFolderButtonRect.left && clientPoint.x <= state_.newFolderButtonRect.right &&
            clientPoint.y >= state_.newFolderButtonRect.top && clientPoint.y <= state_.newFolderButtonRect.bottom)
        {
            state_.newFolderButtonHovered = true;
        }
    }

    if (oldHovered != state_.hoveredItemIndex ||
        oldFileHover != state_.newFileButtonHovered || oldFolderHover != state_.newFolderButtonHovered)
    {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

void ExplorerManager::OnLeftButtonDown(HWND hwnd, POINT clientPoint)
{
    // Click bouton "Ouvrir un projet" quand pas de projet
    if (state_.rootPath.empty() && !searchMode_)
    {
        if (openProjectButton_.OnMouseDown(clientPoint))
        {
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
    }
    // Check clicks on title toolbar buttons (new file / new folder)
    float titleTop = state_.topEdge;
    float titleBottom = state_.topEdge + state_.titleHeight;
    if (clientPoint.y >= (int)titleTop && clientPoint.y <= (int)titleBottom)
    {
        // New File button
        if (clientPoint.x >= state_.newFileButtonRect.left && clientPoint.x <= state_.newFileButtonRect.right &&
            clientPoint.y >= state_.newFileButtonRect.top && clientPoint.y <= state_.newFileButtonRect.bottom)
        {
            ShowInlineInput(Input::Type::File);
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }

        // New Folder button
        if (clientPoint.x >= state_.newFolderButtonRect.left && clientPoint.x <= state_.newFolderButtonRect.right &&
            clientPoint.y >= state_.newFolderButtonRect.top && clientPoint.y <= state_.newFolderButtonRect.bottom)
        {
            ShowInlineInput(Input::Type::Folder);
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
    }

    // Scrollbar
    if (scrollbar_.OnLeftButtonDown(clientPoint))
    {
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    // Items de l'Explorer
    if (state_.hoveredItemIndex >= 0)
    {
        int idx = state_.hoveredItemIndex;

        bool isDirectory = false;
        bool isExpanded = false;
        int itemDepth = 0;
        std::wstring fullPath;

        {
            std::lock_guard<std::mutex> lk(itemsMutex_);
            if (idx >= 0 && idx < (int)state_.items.size())
            {
                isDirectory = state_.items[idx].isDirectory;
                isExpanded = state_.items[idx].expanded;
                itemDepth = state_.items[idx].depth;
                fullPath = state_.items[idx].fullPath;
            }
            else
            {
                return;
            }
        }

        // If we're renaming this item, keep focus on the inline input
        if (inlineVisible_ && renameTargetIndex_ == idx)
        {
            return;
        }

        if (isDirectory)
        {
            // Mettre le dossier en "active" pour le surbrillance
            state_.activePath = fullPath;

            // Toggle expansion
            if (!isExpanded)
            {
                try
                {
                    std::filesystem::path p(fullPath);
                    std::vector<ExplorerItem> children;

                    std::unordered_set<std::wstring> existingPaths;

                    {
                        std::lock_guard<std::mutex> lk(itemsMutex_);
                        for (const auto &existing : state_.items)
                        {
                            existingPaths.insert(existing.fullPath);
                        }
                    }

                    for (const auto &entry : std::filesystem::directory_iterator(p))
                    {
                        std::wstring childPath = entry.path().wstring();

                        if (existingPaths.find(childPath) != existingPaths.end())
                            continue;

                        ExplorerItem ci;
                        ci.name = entry.path().filename().wstring();
                        ci.fullPath = childPath;
                        ci.extension = entry.path().extension().string();
                        ci.isDirectory = entry.is_directory();
                        ci.depth = itemDepth + 1;
                        ci.expanded = false;
                        children.push_back(ci);
                        existingPaths.insert(childPath);

                        if (children.size() >= 100)
                            break;
                    }

                    std::sort(children.begin(), children.end(), [](const ExplorerItem &a, const ExplorerItem &b)
                              {
                        if (a.isDirectory != b.isDirectory) return a.isDirectory;
                        return a.name < b.name; });

                    {
                        std::lock_guard<std::mutex> lk(itemsMutex_);
                        if (idx >= 0 && idx < (int)state_.items.size())
                        {
                            state_.items.insert(state_.items.begin() + idx + 1, children.begin(), children.end());
                            state_.items[idx].expanded = true;
                        }
                    }

                    UpdateItemPositions();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                catch (...)
                {
                }
            }
            else
            {
                // Collapse
                {
                    std::lock_guard<std::mutex> lk(itemsMutex_);
                    if (idx >= 0 && idx < (int)state_.items.size())
                    {
                        int removeStart = idx + 1;
                        int removeCount = 0;
                        for (int j = removeStart; j < (int)state_.items.size(); ++j)
                        {
                            if (state_.items[j].depth > itemDepth)
                                removeCount++;
                            else
                                break;
                        }
                        if (removeCount > 0)
                        {
                            state_.items.erase(state_.items.begin() + removeStart,
                                               state_.items.begin() + removeStart + removeCount);
                        }
                        state_.items[idx].expanded = false;
                    }
                }

                UpdateItemPositions();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        else
        {
            // Fichier - ouvrir
            auto *heapPath = new std::wstring(fullPath);
            PostMessageW(hwnd, WM_USER + 100, 0, (LPARAM)heapPath);
        }
    }
    else
    {
        // Clic dans le vide de l'explorer
        // 1. Fermer l'input inline s'il est ouvert
        if (inlineVisible_)
        {
            HideInlineInput();
        }

        // 2. Désélectionner le dossier actif (revenir à la racine)
        state_.activePath.clear();

        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

void ExplorerManager::OnMouseWheel(HWND hwnd, int delta)
{
    if (scrollbar_.OnMouseWheel(delta))
    {
        UpdateItemPositions();
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

void ExplorerManager::OnLeftButtonUp(HWND hwnd)
{
    // bouton open project
    if (state_.rootPath.empty() && !searchMode_)
    {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);

        if (openProjectButton_.OnMouseUp(pt))
        {
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
        else
        {
            openProjectButton_.CancelPress();
        }
    }

    // Scrollbar
    if (scrollbar_.OnLeftButtonUp())
    {
        ReleaseCapture();
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
}

void ExplorerManager::OnLeftButtonDoubleClick(HWND hwnd, POINT clientPoint)
{
    int idx = HitTestItem(clientPoint);
    // Double-click in empty area => create new file inline (like Ctrl+N)
    if (idx < 0)
    {
        ShowInlineInput(Input::Type::File);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    std::wstring fullPath;
    bool isDir = false;

    {
        std::lock_guard<std::mutex> lk(itemsMutex_);
        if (idx >= (int)state_.items.size())
            return;
        fullPath = state_.items[idx].fullPath;
        isDir = state_.items[idx].isDirectory;
    }

    if (!isDir)
    {
        auto* heapPath = new std::wstring(fullPath);
        PostMessageW(hwnd, WM_USER + 100, 0, (LPARAM)heapPath);
    }
    else
    {
        // For directories, reuse existing left-button logic to toggle expansion
        state_.hoveredItemIndex = idx;
        OnLeftButtonDown(hwnd, clientPoint);
    }
}

void ExplorerManager::OnRightButtonUp(HWND hwnd, POINT clientPoint)
{
    int idx = HitTestItem(clientPoint);
    if (idx < 0)
        return;
    if (idx >= (int)state_.items.size())
        return;

    const ExplorerItem &item = state_.items[idx];

    std::vector<std::wstring> menuItems;
    menuItems.push_back(item.isDirectory ? L"Open Folder" : L"Open File");
    menuItems.push_back(L"Copy Path");
    menuItems.push_back(L"Duplicate");
    menuItems.push_back(L"Rename");
    menuItems.push_back(L"Delete");

    D2D1_POINT_2F pos = D2D1::Point2F((float)clientPoint.x, (float)clientPoint.y);
    int baseId = 5000 + idx * 10;

    ShowContextMenuDropdown(hwnd, menuItems, pos, baseId);
}

void ExplorerManager::HandleContextCommand(int commandId)
{
    if (commandId < 5000 || commandId >= 6000)
        return;
    int rel = commandId - 5000;
    int itemIndex = rel / 10;
    int cmdIndex = rel % 10;
    if (itemIndex < 0 || itemIndex >= (int)state_.items.size())
        return;

    const ExplorerItem &item = state_.items[itemIndex];
    std::wstring path = item.fullPath;

    if (cmdIndex == 0)
    {
        // Open
        ShellExecuteW(NULL, L"open", path.c_str(), NULL, NULL, SW_SHOWNORMAL);
    }
    else if (cmdIndex == 1)
    {
        // Copy path to clipboard
        if (OpenClipboard(NULL))
        {
            EmptyClipboard();
            size_t sz = (path.size() + 1) * sizeof(wchar_t);
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sz);
            if (hMem)
            {
                void *dst = GlobalLock(hMem);
                memcpy(dst, path.c_str(), sz);
                GlobalUnlock(hMem);
                SetClipboardData(CF_UNICODETEXT, hMem);
            }
            CloseClipboard();
        }
    }
    else if (cmdIndex == 2)
    {
        // Duplicate: create a copy of the file or folder next to the original with a unique name
        namespace fs = std::filesystem;
        try
        {
            fs::path src(path);
            fs::path parent = src.parent_path();
            std::wstring base = src.stem().wstring();
            std::wstring ext = src.has_extension() ? src.extension().wstring() : L"";

            auto makeCandidate = [&](int n)
            {
                if (n == 0)
                {
                    return parent / std::wstring(base + L" - Copy" + ext);
                }
                wchar_t buf[128];
                swprintf_s(buf, 128, L"%s - Copy (%d)%s", base.c_str(), n, ext.c_str());
                return parent / std::wstring(buf);
            };

            int n = 0;
            fs::path dest = makeCandidate(n);
            while (fs::exists(dest))
            {
                n++;
                dest = makeCandidate(n);
            }

            if (item.isDirectory)
            {
                fs::copy(src, dest, fs::copy_options::recursive);
            }
            else
            {
                fs::copy_file(src, dest);
            }

            Logger::Instance().Log(L"Explorer: duplicated " + path + L" -> " + dest.wstring());
            // Reload explorer contents and refresh UI
            LoadDirectoryContents();
            UpdateItemPositions();
            InvalidateMainWindow();
        }
        catch (...)
        {
            Logger::Instance().Log(L"Explorer: duplicate failed for " + path);
        }
    }
    else if (cmdIndex == 3)
    {
        // Rename: show inline input for this item
        ShowRenameInline(itemIndex);
        InvalidateMainWindow();
    }
    else if (cmdIndex == 4)
    {
        // Delete (modern TaskDialog if available)
        std::wstring mainInstr = item.isDirectory ? (L"Delete folder: \n" + path) : (L"Delete file: \n" + path);
        BOOL confirm = FALSE;

        // Try TaskDialogIndirect for a modern look
        HMODULE hComCtl = LoadLibraryW(L"Comctl32.dll");
        if (hComCtl)
        {
            typedef HRESULT(WINAPI * TaskDialogIndirect_t)(const void *, int *, int *, void *);
            // Use TaskDialogIndirect via header-less invocation to avoid extra headers here
            // We'll call TaskDialog to keep it simple if available.
            // Fallback to MessageBox if TaskDialog not available.
            FreeLibrary(hComCtl);
        }

        // Simpler approach: use TaskDialog if available via TaskDialog API
        int tdResult = 0;
        // Use TaskDialog via explicit function pointer to avoid requiring commctrl headers
        HMODULE h = LoadLibraryW(L"comctl32.dll");
        if (h)
        {
            auto proc = (HRESULT(WINAPI *)(HWND, HINSTANCE, PCWSTR, PCWSTR, PCWSTR, unsigned, PCWSTR, int *))GetProcAddress(h, "TaskDialog");
            if (proc)
            {
                // Use common Yes/No buttons. Numeric flag values (from commctrl.h):
                // TDCBF_YES_BUTTON = 0x0004, TDCBF_NO_BUTTON = 0x0008
                unsigned dwCommon = 0x0004 | 0x0008;
                HRESULT hr = proc(NULL, NULL, L"Confirm Delete", mainInstr.c_str(), L"Are you sure you want to delete this item?", dwCommon, NULL, &tdResult);
                if (SUCCEEDED(hr))
                {
                    confirm = (tdResult == IDYES);
                }
                else
                {
                    // fallback to MessageBox
                    int mb = MessageBoxW(NULL, mainInstr.c_str(), L"Confirm Delete", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
                    confirm = (mb == IDYES);
                }
            }
            else
            {
                // fallback
                int mb = MessageBoxW(NULL, mainInstr.c_str(), L"Confirm Delete", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
                confirm = (mb == IDYES);
            }
            FreeLibrary(h);
        }
        else
        {
            int mb = MessageBoxW(NULL, mainInstr.c_str(), L"Confirm Delete", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
            confirm = (mb == IDYES);
        }

        if (confirm)
        {
            try
            {
                bool ok = false;
                if (item.isDirectory)
                {
                    ok = std::filesystem::remove_all(path) > 0;
                }
                else
                {
                    ok = std::filesystem::remove(path);
                }

                if (ok)
                {
                    // Reload explorer contents and refresh UI
                    LoadDirectoryContents();
                    InvalidateMainWindow();
                    // If a file was deleted, close its tab if open
                    try
                    {
                        HWND wnd = FindWindowW(L"NebulaTextWindowClass", NULL);
                        if (wnd)
                        {
                            Window *window = GetWindowFromHwnd(wnd);
                            if (window)
                            {
                                TabBar *tb = window->GetTabBar();
                                if (tb)
                                {
                                    int tidx = tb->FindTabIndexByFilePath(path);
                                    if (tidx >= 0)
                                        tb->CloseTab(tidx);
                                }
                            }
                        }
                    }
                    catch (...)
                    {
                        // ignore any errors while attempting to close tabs
                    }
                }
                else
                {
                    MessageBoxW(NULL, L"Failed to delete item.", L"Delete", MB_OK | MB_ICONERROR);
                }
            }
            catch (const std::exception &)
            {
                MessageBoxW(NULL, L"Failed to delete item.", L"Delete", MB_OK | MB_ICONERROR);
            }
        }
    }
}

void ExplorerManager::DrawBackground(ID2D1RenderTarget *ctx)
{
    ID2D1SolidColorBrush *bgBrush = nullptr;
    D2D1_COLOR_F bgColor = D2D1::ColorF(
        18.0f / 255.0f,
        18.0f / 255.0f,
        18.0f / 255.0f,
        1.0f);
    ctx->CreateSolidColorBrush(bgColor, &bgBrush);

    D2D1_RECT_F rect = D2D1::RectF(
        state_.leftEdge,
        state_.topEdge,
        state_.rightEdge,
        state_.bottomEdge);

    ctx->FillRectangle(rect, bgBrush);

    if (bgBrush)
        bgBrush->Release();
}

// ============================================================================
// Drawing
// ============================================================================
void ExplorerManager::Draw(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd)
{
    if (!ctx || !dwrite)
        return;

    // Always update layout first so physicalWidth is correct when hidden
    UpdateLayout(hwnd);

    // If Explorer is hidden or has no width, skip drawing the content
    if (!visible_ || state_.physicalWidth <= 0)
        return;

    D2D1_ANTIALIAS_MODE oldAA = ctx->GetAntialiasMode();
    D2D1_TEXT_ANTIALIAS_MODE oldTextAA = ctx->GetTextAntialiasMode();
    ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    ctx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

    D2D1_RECT_F clipRect = D2D1::RectF(
        state_.leftEdge,
        state_.topEdge,
        state_.rightEdge,
        state_.bottomEdge);

    ctx->PushAxisAlignedClip(clipRect, D2D1_ANTIALIAS_MODE_ALIASED);

    DrawBackground(ctx);
    DrawTitle(ctx, dwrite);
    // If no project root set, show a helpful placeholder prompting to open a project
    if (state_.rootPath.empty() && !searchMode_)
    {
        // petit texte (optionnel) + bouton en haut
        IDWriteTextFormat* fmt = nullptr;
        dwrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL,
                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                 12.5f, L"fr-fr", &fmt);

        ID2D1SolidColorBrush* msgBrush = nullptr;
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.60f, 0.60f, 0.60f, 1.0f), &msgBrush);

        if (fmt)
        {
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

            D2D1_RECT_F info = D2D1::RectF(
                state_.leftEdge + state_.leftPadding,
                openProjectButtonRect_.bottom + 10.0f,
                state_.rightEdge - state_.leftPadding,
                openProjectButtonRect_.bottom + 10.0f + 40.0f);

            std::wstring msg = L"Aucun projet ouvert.";
            if (msgBrush)
                ctx->DrawTextW(msg.c_str(), (UINT32)msg.size(), fmt, info, msgBrush);

            fmt->Release();
        }
        if (msgBrush) msgBrush->Release();

        // Dessiner le bouton
        openProjectButton_.Draw(ctx, dwrite);
    }
    else if (searchMode_)
        DrawSearchPanel(ctx, dwrite, hwnd);
    else
        DrawItems(ctx, dwrite, hwnd);
    DrawRightBorder(ctx);
    scrollbar_.Draw(ctx);

    ctx->PopAxisAlignedClip();

    ctx->SetAntialiasMode(oldAA);
    ctx->SetTextAntialiasMode(oldTextAA);
}

void ExplorerManager::DrawTitle(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite)
{
    IDWriteTextFormat *titleFormat = nullptr;
    dwrite->CreateTextFormat(
        L"Segoe UI", NULL,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        13.0f, L"en-us",
        &titleFormat);

    if (titleFormat)
    {
        titleFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        titleFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    ID2D1SolidColorBrush *textBrush = nullptr;
    D2D1_COLOR_F textColor = D2D1::ColorF(204.0f / 255.0f, 204.0f / 255.0f, 204.0f / 255.0f, 1.0f);
    ctx->CreateSolidColorBrush(textColor, &textBrush);

    D2D1_RECT_F titleRect = D2D1::RectF(
        state_.leftEdge + state_.leftPadding,
        state_.topEdge + state_.topPadding,
        state_.rightEdge - state_.leftPadding,
        state_.topEdge + state_.titleHeight);

    // Draw title text
    ctx->DrawTextW(L"EXPLORER", 8, titleFormat, titleRect, textBrush);

    // Draw small action buttons (New File / New Folder) to the right of the title
    float btnSize = state_.titleHeight - 12.0f; // compact square
    float spacing = 6.0f;
    float rightX = state_.rightEdge - state_.leftPadding;

    float folderRight = rightX;
    float folderLeft = folderRight - btnSize;
    float fileRight = folderLeft - spacing;
    float fileLeft = fileRight - btnSize;

    float top = state_.topEdge + state_.topPadding + 6.0f;
    float bottom = top + btnSize;

    // Save hit rects (integers) into state_ for click testing
    state_.newFileButtonRect.left = static_cast<LONG>(fileLeft);
    state_.newFileButtonRect.top = static_cast<LONG>(top);
    state_.newFileButtonRect.right = static_cast<LONG>(fileRight);
    state_.newFileButtonRect.bottom = static_cast<LONG>(bottom);

    state_.newFolderButtonRect.left = static_cast<LONG>(folderLeft);
    state_.newFolderButtonRect.top = static_cast<LONG>(top);
    state_.newFolderButtonRect.right = static_cast<LONG>(folderRight);
    state_.newFolderButtonRect.bottom = static_cast<LONG>(bottom);

    // Draw SVG icons for buttons (cached in iconCache_). Keep hover rounded background.
    ID2D1SolidColorBrush *btnHoverBrush = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0x2a2d2e), &btnHoverBrush); // same hover color as titlebar menus

    // Determine DPI from render target
    float dpiX = 96.0f, dpiY = 96.0f;
    ctx->GetDpi(&dpiX, &dpiY);
    UINT dpi = (UINT)std::round(dpiX);

    // Prepare rects
    D2D1_RECT_F fileRectF = D2D1::RectF(fileLeft, top, fileRight, bottom);
    D2D1_RECT_F folderRectF = D2D1::RectF(folderLeft, top, folderRight, bottom);

    // File icon (cache key: __title_file)
    ID2D1Bitmap *fileBmp = nullptr;
    auto fit = iconCache_.find("__title_file");
    if (fit != iconCache_.end())
        fileBmp = fit->second;
    else
    {
        std::string path = "assets\\ressource\\icons\\file.svg";
        ID2D1Bitmap *bmp = LoadSvgIcon(ctx, path, (int)btnSize, dpi);
        if (bmp)
            iconCache_["__title_file"] = bmp, fileBmp = bmp;
    }

    // Folder icon (cache key: __title_folder)
    ID2D1Bitmap *folderBmp = nullptr;
    auto fot = iconCache_.find("__title_folder");
    if (fot != iconCache_.end())
        folderBmp = fot->second;
    else
    {
        std::string path = "assets\\ressource\\icons\\folder.svg";
        ID2D1Bitmap *bmp = LoadSvgIcon(ctx, path, (int)btnSize, dpi);
        if (bmp)
            iconCache_["__title_folder"] = bmp, folderBmp = bmp;
    }

    // Draw File button
    if (state_.newFileButtonHovered)
    {
        D2D1_RECT_F hoverRect = D2D1::RectF(fileRectF.left + 2.0f, fileRectF.top + 2.0f, fileRectF.right - 2.0f, fileRectF.bottom - 2.0f);
        D2D1_ROUNDED_RECT hoverRounded = D2D1::RoundedRect(hoverRect, 4.0f, 4.0f);
        ctx->FillRoundedRectangle(hoverRounded, btnHoverBrush);
    }
    if (fileBmp)
    {
        // Draw the SVG smaller inside the button to avoid oversized icon
        float inset = btnSize * 0.18f;
        D2D1_RECT_F dst = D2D1::RectF(fileRectF.left + inset, fileRectF.top + inset, fileRectF.right - inset, fileRectF.bottom - inset);
        ctx->DrawBitmap(fileBmp, dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    }

    // Draw Folder button
    if (state_.newFolderButtonHovered)
    {
        D2D1_RECT_F hoverRect = D2D1::RectF(folderRectF.left + 2.0f, folderRectF.top + 2.0f, folderRectF.right - 2.0f, folderRectF.bottom - 2.0f);
        D2D1_ROUNDED_RECT hoverRounded = D2D1::RoundedRect(hoverRect, 4.0f, 4.0f);
        ctx->FillRoundedRectangle(hoverRounded, btnHoverBrush);
    }
    if (folderBmp)
    {
        // smaller icon for folder as well
        float insetF = btnSize * 0.18f;
        D2D1_RECT_F dstF = D2D1::RectF(folderRectF.left + insetF, folderRectF.top + insetF, folderRectF.right - insetF, folderRectF.bottom - insetF);
        ctx->DrawBitmap(folderBmp, dstF, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    }

    if (btnHoverBrush)
        btnHoverBrush->Release();

    if (titleFormat)
        titleFormat->Release();
    if (textBrush)
        textBrush->Release();
}
void ExplorerManager::ShowInlineInput(Input::Type type)
{
    inlineVisible_ = true;
    inlineType_ = type;
    inlineText_.clear();
    inlineCursorPos_ = 0;
    inlineHasSelection_ = false;
    inlineSelStart_ = 0;
    inlineSelEnd_ = 0;

    inlineTargetLocked_ = true;
    inlineTargetFullPath_.clear();

    std::wstring targetDir;

    // 1. Utiliser UNIQUEMENT le dossier actif s'il est valide
    if (!state_.activePath.empty())
    {
        std::lock_guard<std::mutex> lk(itemsMutex_);
        for (const auto &item : state_.items)
        {
            if (item.fullPath == state_.activePath && item.isDirectory)
            {
                targetDir = state_.activePath;
                break;
            }
        }
    }

    // 2. Sinon → racine
    if (targetDir.empty())
    {
        targetDir = GetActiveDirectory();
    }

    inlineTargetFullPath_ = targetDir;

    std::lock_guard<std::mutex> lk(itemsMutex_);

    // Vérifier si un placeholder existe déjà
    bool placeholderExists = false;
    int existingPos = -1;
    for (size_t i = 0; i < state_.items.size(); ++i)
    {
        if (state_.items[i].fullPath == L"__inline_placeholder__")
        {
            placeholderExists = true;
            existingPos = (int)i;
            state_.items[i].isDirectory = (type == Input::Type::Folder);
            break;
        }
    }

    // Si déjà présent, juste mettre à jour
    if (placeholderExists)
    {
        UpdateItemPositions();
        return;
    }

    // Créer le placeholder
    ExplorerItem placeholder;
    placeholder.name = L"__inline_placeholder__";
    placeholder.fullPath = L"__inline_placeholder__";
    placeholder.isDirectory = (type == Input::Type::Folder);
    placeholder.expanded = false;
    placeholder.depth = 0;

    int insertPos = -1;

    // Chercher le dossier parent
    for (size_t i = 0; i < state_.items.size(); ++i)
    {
        if (state_.items[i].fullPath == inlineTargetFullPath_)
        {
            int parentDepth = state_.items[i].depth;
            placeholder.depth = parentDepth + 1;

            // Forcer le parent à être déplié
            if (!state_.items[i].expanded)
            {
                state_.items[i].expanded = true;
            }

            // Insérer après tous ses enfants
            insertPos = (int)i + 1;
            while (insertPos < (int)state_.items.size() &&
                   state_.items[insertPos].depth > parentDepth)
            {
                insertPos++;
            }
            break;
        }
    }

    // Parent non trouvé → insérer à la racine
    if (insertPos == -1)
    {
        placeholder.depth = 0;

        // Trouver la fin de la racine (après le dernier élément depth 0)
        insertPos = (int)state_.items.size();

        for (int i = (int)state_.items.size() - 1; i >= 0; --i)
        {
            if (state_.items[i].depth == 0)
            {
                insertPos = i + 1;

                // sauter ses enfants éventuels
                while (insertPos < (int)state_.items.size() &&
                       state_.items[insertPos].depth > 0)
                {
                    insertPos++;
                }
                break;
            }
        }
    }

    // Insérer le placeholder
    state_.items.insert(state_.items.begin() + insertPos, placeholder);

    UpdateItemPositions();
}

void ExplorerManager::ShowRenameInline(int itemIndex)
{
    if (itemIndex < 0 || itemIndex >= (int)state_.items.size())
        return;

    std::lock_guard<std::mutex> lk(itemsMutex_);
    // store original full path
    renameTargetIndex_ = itemIndex;
    renameOriginalFullPath_ = state_.items[itemIndex].fullPath;
    renameTargetIsDir_ = state_.items[itemIndex].isDirectory;

    // prefill with the display name
    inlineType_ = state_.items[itemIndex].isDirectory ? Input::Type::Folder : Input::Type::File;
    inlineText_ = state_.items[itemIndex].name;
    // Select base name (like VSCode) when possible
    inlineSelStart_ = 0;
    inlineSelEnd_ = (int)inlineText_.size();
    if (inlineType_ == Input::Type::File)
    {
        size_t dot = inlineText_.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0)
            inlineSelEnd_ = (int)dot;
    }
    inlineHasSelection_ = (inlineSelEnd_ > inlineSelStart_);
    inlineCursorPos_ = inlineSelEnd_;
    inlineVisible_ = true;

    // Lock inline target to this item's path to avoid watcher moving it
    inlineTargetLocked_ = true;
    inlineTargetFullPath_ = renameOriginalFullPath_;

    UpdateItemPositions();
    InvalidateMainWindow();
}

void ExplorerManager::HideInlineInput()
{
    inlineVisible_ = false;
    inlineText_.clear();
    inlineCursorPos_ = 0;
    inlineHasSelection_ = false;
    inlineSelStart_ = 0;
    inlineSelEnd_ = 0;

    // Remove the placeholder item if it exists
    {
        std::lock_guard<std::mutex> lk(itemsMutex_);
        for (auto it = state_.items.begin(); it != state_.items.end(); ++it)
        {
            if (it->fullPath == L"__inline_placeholder__")
            {
                state_.items.erase(it);
                UpdateItemPositions();
                break;
            }
        }
    }

    // clear any rename target state
    renameTargetIndex_ = -1;
    renameOriginalFullPath_.clear();
    renameTargetIsDir_ = false;
    // clear locked inline target
    inlineTargetLocked_ = false;
    inlineTargetFullPath_.clear();
}

bool ExplorerManager::IsInlineInputVisible() const
{
    return inlineVisible_;
}

void ExplorerManager::OnCharInline(wchar_t ch)
{
    if (!inlineVisible_)
        return;
    if (ch < 32 && ch != 8)
        return;
    if (ch == 8)
    {
        if (inlineHasSelection_)
        {
            int a = std::min(inlineSelStart_, inlineSelEnd_);
            int b = std::max(inlineSelStart_, inlineSelEnd_);
            inlineText_.erase(a, b - a);
            inlineCursorPos_ = a;
            inlineHasSelection_ = false;
            return;
        }
        if (inlineCursorPos_ > 0)
        {
            inlineText_.erase(inlineCursorPos_ - 1, 1);
            inlineCursorPos_--;
        }
    }
    else
    {
        if (inlineHasSelection_)
        {
            int a = std::min(inlineSelStart_, inlineSelEnd_);
            int b = std::max(inlineSelStart_, inlineSelEnd_);
            inlineText_.erase(a, b - a);
            inlineCursorPos_ = a;
            inlineHasSelection_ = false;
        }
        inlineText_.insert(inlineCursorPos_, 1, ch);
        inlineCursorPos_++;
    }
}

void ExplorerManager::OnKeyDownInline(WPARAM key)
{
    if (!inlineVisible_)
        return;
    switch (key)
    {
    case VK_RETURN:
        if (!inlineText_.empty())
        {
            // If a rename target is set, perform rename
            if (renameTargetIndex_ != -1)
            {
                try
                {
                    std::filesystem::path oldPath(renameOriginalFullPath_);
                    std::filesystem::path parent = oldPath.parent_path();
                    std::filesystem::path newPath = parent / inlineText_;

                    // If same name, just refresh
                    if (newPath == oldPath)
                    {
                        LoadDirectoryContents();
                    }
                    else
                    {
                        std::error_code ec;
                        std::filesystem::rename(oldPath, newPath, ec);
                        if (ec)
                        {
                            MessageBoxW(NULL, L"Failed to rename item.", L"Rename", MB_OK | MB_ICONERROR);
                        }
                        else
                        {
                            LoadDirectoryContents();
                            SetActivePath(newPath.wstring());
                            if (!renameTargetIsDir_)
                            {
                                HWND wnd = FindWindowW(L"NebulaTextWindowClass", NULL);
                                if (wnd)
                                {
                                    auto *payload = new RenamePathPayload{oldPath.wstring(), newPath.wstring()};
                                    PostMessageW(wnd, WM_EDITOR_FILE_RENAMED, 0, (LPARAM)payload);
                                }
                            }
                        }
                    }
                }
                catch (...)
                {
                    MessageBoxW(NULL, L"Error while renaming.", L"Rename", MB_OK | MB_ICONERROR);
                }

                // clear rename state
                renameTargetIndex_ = -1;
                renameOriginalFullPath_.clear();
                renameTargetIsDir_ = false;
            }
            else
            {
                // Create using the locked inline target directory when available
                std::wstring parentDir;
                if (inlineTargetLocked_ && !inlineTargetFullPath_.empty())
                    parentDir = inlineTargetFullPath_;
                else
                    parentDir = GetActiveDirectory();

                std::wstring targetPath = parentDir;
                if (!targetPath.empty() && targetPath.back() != L'\\' && targetPath.back() != L'/')
                    targetPath.push_back(L'\\');
                targetPath += inlineText_;

                try
                {
                    if (inlineType_ == Input::Type::File)
                    {
                        int size = WideCharToMultiByte(CP_UTF8, 0, targetPath.c_str(), -1, NULL, 0, NULL, NULL);
                        std::string path(size, '\0');
                        WideCharToMultiByte(CP_UTF8, 0, targetPath.c_str(), -1, path.data(), size, NULL, NULL);
                        std::ofstream file;
                        file.open(path);
                        if (file.is_open())
                        {
                            file.close();
                            Logger::Instance().Log(L"Explorer: created file: " + targetPath);
                            LoadDirectoryContents();
                            SetActivePath(targetPath);
                            // Open in editor
                            HWND wnd = FindWindowW(L"NebulaTextWindowClass", NULL);
                            if (wnd)
                            {
                                auto *heapPath = new std::wstring(targetPath);
                                PostMessageW(wnd, WM_USER + 100, 0, (LPARAM)heapPath);
                            }
                            InvalidateMainWindow();
                        }
                        else
                        {
                            MessageBoxW(NULL, L"Impossible de créer le fichier.", L"Erreur", MB_OK | MB_ICONERROR);
                        }
                    }
                    else
                    {
                        if (std::filesystem::create_directory(targetPath))
                        {
                            Logger::Instance().Log(L"Explorer: created folder: " + targetPath);
                            LoadDirectoryContents();
                            SetActivePath(targetPath);
                            InvalidateMainWindow();
                        }
                        else
                        {
                            MessageBoxW(NULL, L"Impossible de créer le dossier.", L"Erreur", MB_OK | MB_ICONERROR);
                        }
                    }
                }
                catch (...)
                {
                    MessageBoxW(NULL, L"Erreur lors de la création.", L"Erreur", MB_OK | MB_ICONERROR);
                }
            }
        }
        HideInlineInput();
        break;
    case VK_ESCAPE:
        HideInlineInput();
        break;
    case VK_LEFT:
        inlineHasSelection_ = false;
        if (inlineCursorPos_ > 0)
            inlineCursorPos_--;
        break;
    case VK_RIGHT:
        inlineHasSelection_ = false;
        if (inlineCursorPos_ < (int)inlineText_.size())
            inlineCursorPos_++;
        break;
    case VK_HOME:
        inlineHasSelection_ = false;
        inlineCursorPos_ = 0;
        break;
    case VK_END:
        inlineHasSelection_ = false;
        inlineCursorPos_ = (int)inlineText_.size();
        break;
    case VK_DELETE:
        if (inlineHasSelection_)
        {
            int a = std::min(inlineSelStart_, inlineSelEnd_);
            int b = std::max(inlineSelStart_, inlineSelEnd_);
            inlineText_.erase(a, b - a);
            inlineCursorPos_ = a;
            inlineHasSelection_ = false;
            break;
        }
        if (inlineCursorPos_ < (int)inlineText_.size())
            inlineText_.erase(inlineCursorPos_, 1);
        break;
    }
}

ID2D1Bitmap *ExplorerManager::GetIconForItem(ID2D1RenderTarget *ctx, const ExplorerItem &item, HWND hwnd)
{
    std::string iconKey = item.isDirectory ? "folder" : item.extension;

    // Check cache
    auto it = iconCache_.find(iconKey);
    if (it != iconCache_.end())
    {
        return it->second;
    }

    // Load icon
    std::string iconPath = GetIconPathForExtension(iconKey);
    if (iconPath.empty())
        return nullptr;

    UINT dpi = GetDpiForWindow(hwnd);
    int iconPxSize = win32_dpi_scale((int)state_.iconSize, dpi);

    ID2D1Bitmap *bitmap = LoadSvgIcon(ctx, iconPath, iconPxSize, dpi);
    if (bitmap)
    {
        iconCache_[iconKey] = bitmap;
    }

    return bitmap;
}

void ExplorerManager::DrawItems(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd)
{
    IDWriteTextFormat *itemFormat = nullptr;
    IDWriteTextFormat *itemFormatBold = nullptr;
    dwrite->CreateTextFormat(
        L"Segoe UI", NULL,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        13.0f, L"en-us",
        &itemFormat);
    dwrite->CreateTextFormat(
        L"Segoe UI", NULL,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        13.0f, L"en-us",
        &itemFormatBold);

    if (itemFormat)
    {
        itemFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        itemFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    if (itemFormatBold)
    {
        itemFormatBold->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        itemFormatBold->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    ID2D1SolidColorBrush *textBrush = nullptr;
    D2D1_COLOR_F textColor = D2D1::ColorF(204.0f / 255.0f, 204.0f / 255.0f, 204.0f / 255.0f, 1.0f);
    ctx->CreateSolidColorBrush(textColor, &textBrush);

    ID2D1SolidColorBrush *hoverBrush = nullptr;
    D2D1_COLOR_F hoverColor = D2D1::ColorF(30.0f / 255.0f, 30.0f / 255.0f, 30.0f / 255.0f, 1.0f);
    ctx->CreateSolidColorBrush(hoverColor, &hoverBrush);

    UINT dpi = GetDpiForWindow(hwnd);
    float iconPx = (float)win32_dpi_scale((int)state_.iconSize, dpi);

    // Rounded background constants and helper for crisp rounded fills
    const float corner = 4.0f;           // petit arrondi (réduit)
    const float insetX = 4.0f;           // marge gauche/droite du fond (réduite)
    const float insetY = 0.0f;           // marge haut/bas du fond (aucune, couvre toute la hauteur)

    auto DrawRoundedFill = [&](const D2D1_RECT_F& r, ID2D1Brush* brush)
    {
        // Snap pour éviter le flou
        D2D1_RECT_F rr = D2D1::RectF(
            std::round(r.left),
            std::round(r.top),
            std::round(r.right),
            std::round(r.bottom));

        // AA smooth uniquement pour les arrondis
        D2D1_ANTIALIAS_MODE oldAA = ctx->GetAntialiasMode();
        ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        ctx->FillRoundedRectangle(D2D1::RoundedRect(rr, corner, corner), brush);

        ctx->SetAntialiasMode(oldAA);
    };

    for (size_t i = 0; i < state_.items.size(); ++i)
    {
        const auto &item = state_.items[i];

        // Rename inline input (draw over the existing item row)
        if (inlineVisible_ && renameTargetIndex_ == (int)i)
        {
            float indent = state_.leftPadding + (float)(item.depth * 12);
            float iconWidth = item.isDirectory ? (float)win32_dpi_scale(16, dpi) : iconPx;

            float baseLeft = state_.leftEdge + insetX;
            float inputLeft = std::round(baseLeft + indent + iconWidth + 6.0f);
            float inputWidth = std::round(state_.rightEdge - state_.leftPadding - inputLeft);
            float inputTop = std::round(item.yPosition);
            float inputHeight = item.height;

            inlineRect_ = D2D1::RectF(inputLeft, inputTop, inputLeft + inputWidth, inputTop + inputHeight);

            // Background
            ID2D1SolidColorBrush *bg = nullptr;
            ctx->CreateSolidColorBrush(D2D1::ColorF(0.16f, 0.16f, 0.16f, 1.0f), &bg);
            DrawRoundedFill(inlineRect_, bg);

            // Border
            ID2D1SolidColorBrush *border = nullptr;
            ctx->CreateSolidColorBrush(D2D1::ColorF(0.38f, 0.57f, 0.95f, 1.0f), &border);
            {
                D2D1_RECT_F rr = D2D1::RectF(
                    std::round(inlineRect_.left),
                    std::round(inlineRect_.top),
                    std::round(inlineRect_.right),
                    std::round(inlineRect_.bottom));
                D2D1_ANTIALIAS_MODE oldAA = ctx->GetAntialiasMode();
                ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                ctx->DrawRoundedRectangle(D2D1::RoundedRect(rr, corner, corner), border, 1.0f);
                ctx->SetAntialiasMode(oldAA);
            }

            // Icon (keep file/folder icon next to rename input)
            if (!item.isDirectory)
            {
                ID2D1Bitmap *icon = GetIconForItem(ctx, item, hwnd);
                if (icon)
                {
                    float iconY = std::round(item.yPosition + (item.height - iconPx) * 0.5f);
                    D2D1_RECT_F iconRect = D2D1::RectF(
                        std::round(baseLeft + indent),
                        iconY,
                        std::round(baseLeft + indent + iconPx),
                        iconY + iconPx);
                    ctx->DrawBitmap(icon, iconRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
                }
            }

            // Text
            IDWriteTextFormat *tf = nullptr;
            dwrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL,
                                     DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                     13.0f, L"en-us", &tf);
            if (tf)
            {
                tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }

            ID2D1SolidColorBrush *txtBrush = nullptr;
            ctx->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.95f, 0.95f, 1.0f), &txtBrush);

            D2D1_RECT_F textRect = D2D1::RectF(
                inlineRect_.left + 8.0f, inlineRect_.top,
                inlineRect_.right - 8.0f, inlineRect_.bottom);

            if (inlineHasSelection_ && !inlineText_.empty())
            {
                int a = std::min(inlineSelStart_, inlineSelEnd_);
                int b = std::max(inlineSelStart_, inlineSelEnd_);
                std::wstring before = inlineText_.substr(0, a);
                std::wstring selected = inlineText_.substr(0, b);

                float startX = textRect.left;
                float endX = textRect.left;
                IDWriteTextLayout *beforeLayout = nullptr;
                IDWriteTextLayout *selEndLayout = nullptr;
                if (dwrite->CreateTextLayout(before.c_str(), (UINT32)before.size(), tf,
                                             textRect.right - textRect.left, textRect.bottom - textRect.top, &beforeLayout) == S_OK && beforeLayout)
                {
                    DWRITE_TEXT_METRICS m1;
                    beforeLayout->GetMetrics(&m1);
                    startX += m1.width;
                    beforeLayout->Release();
                }
                if (dwrite->CreateTextLayout(selected.c_str(), (UINT32)selected.size(), tf,
                                             textRect.right - textRect.left, textRect.bottom - textRect.top, &selEndLayout) == S_OK && selEndLayout)
                {
                    DWRITE_TEXT_METRICS m2;
                    selEndLayout->GetMetrics(&m2);
                    endX += m2.width;
                    selEndLayout->Release();
                }

                ID2D1SolidColorBrush *selBrush = nullptr;
                ctx->CreateSolidColorBrush(D2D1::ColorF(0.25f, 0.45f, 0.85f, 0.45f), &selBrush);
                if (selBrush)
                {
                    D2D1_RECT_F selRect = D2D1::RectF(startX, textRect.top + 2.0f, endX, textRect.bottom - 2.0f);
                    ctx->FillRectangle(selRect, selBrush);
                    selBrush->Release();
                }
            }

            if (!inlineText_.empty())
            {
                ctx->DrawTextW(inlineText_.c_str(), (UINT32)inlineText_.size(), tf, textRect, txtBrush);
            }

            // Caret
            std::wstring before = inlineText_.substr(0, inlineCursorPos_);
            IDWriteTextLayout *layout = nullptr;
            dwrite->CreateTextLayout(before.c_str(), (UINT32)before.size(), tf,
                                     textRect.right - textRect.left, textRect.bottom - textRect.top, &layout);
            if (layout)
            {
                DWRITE_TEXT_METRICS metrics;
                layout->GetMetrics(&metrics);
                float cx = textRect.left + metrics.width;
                ctx->DrawLine(
                    D2D1::Point2F(cx, textRect.top + 2.0f),
                    D2D1::Point2F(cx, textRect.bottom - 2.0f),
                    txtBrush, 1.0f);
                layout->Release();
            }

            if (tf)
                tf->Release();
            if (txtBrush)
                txtBrush->Release();
            if (border)
                border->Release();
            if (bg)
                bg->Release();

            continue;
        }

        // Si c'est le placeholder, dessiner l'input inline à cet endroit
        if (item.fullPath == L"__inline_placeholder__")
        {
            if (inlineVisible_)
            {
                float indent = state_.leftPadding + (float)(item.depth * 12);
                float iconWidth = item.isDirectory ? (float)win32_dpi_scale(16, dpi) : iconPx;

                float baseLeft = state_.leftEdge + insetX;
                float inputLeft = std::round(baseLeft + indent + iconWidth + 6.0f);
                float inputWidth = std::round(state_.rightEdge - state_.leftPadding - inputLeft);
                float inputTop = std::round(item.yPosition);
                float inputHeight = item.height;

                inlineRect_ = D2D1::RectF(inputLeft, inputTop, inputLeft + inputWidth, inputTop + inputHeight);

                // Background
                ID2D1SolidColorBrush *bg = nullptr;
                ctx->CreateSolidColorBrush(D2D1::ColorF(0.16f, 0.16f, 0.16f, 1.0f), &bg);
                DrawRoundedFill(inlineRect_, bg);

                // Border
                ID2D1SolidColorBrush *border = nullptr;
                ctx->CreateSolidColorBrush(D2D1::ColorF(0.38f, 0.57f, 0.95f, 1.0f), &border);
                {
                    D2D1_RECT_F rr = D2D1::RectF(
                        std::round(inlineRect_.left),
                        std::round(inlineRect_.top),
                        std::round(inlineRect_.right),
                        std::round(inlineRect_.bottom));
                    D2D1_ANTIALIAS_MODE oldAA = ctx->GetAntialiasMode();
                    ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                    ctx->DrawRoundedRectangle(D2D1::RoundedRect(rr, corner, corner), border, 1.0f);
                    ctx->SetAntialiasMode(oldAA);
                }

                // File icon preview (based on typed extension)
                if (inlineType_ == Input::Type::File)
                {
                    ExplorerItem previewItem;
                    previewItem.isDirectory = false;
                    if (!inlineText_.empty())
                    {
                        previewItem.extension = std::filesystem::path(inlineText_).extension().string();
                    }
                    ID2D1Bitmap *icon = GetIconForItem(ctx, previewItem, hwnd);
                    if (icon)
                    {
                        float iconY = std::round(item.yPosition + (item.height - iconPx) * 0.5f);
                        D2D1_RECT_F iconRect = D2D1::RectF(
                            std::round(baseLeft + indent),
                            iconY,
                            std::round(baseLeft + indent + iconPx),
                            iconY + iconPx);
                        ctx->DrawBitmap(icon, iconRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
                    }
                }

                // Text
                IDWriteTextFormat *tf = nullptr;
                dwrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL,
                                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                         13.0f, L"en-us", &tf);
                if (tf)
                {
                    tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                    tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                }

                ID2D1SolidColorBrush *txtBrush = nullptr;
                ctx->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.95f, 0.95f, 1.0f), &txtBrush);

                D2D1_RECT_F textRect = D2D1::RectF(
                    inlineRect_.left + 8.0f, inlineRect_.top,
                    inlineRect_.right - 8.0f, inlineRect_.bottom);

                if (inlineText_.empty())
                {
                    ID2D1SolidColorBrush *ph = nullptr;
                    ctx->CreateSolidColorBrush(D2D1::ColorF(0.6f, 0.6f, 0.6f, 1.0f), &ph);
                    std::wstring placeholder = inlineType_ == Input::Type::File ? L"Nom du fichier..." : L"Nom du dossier...";
                    ctx->DrawTextW(placeholder.c_str(), (UINT32)placeholder.size(), tf, textRect, ph);
                    if (ph)
                        ph->Release();

                    float cx = textRect.left;
                    ctx->DrawLine(
                        D2D1::Point2F(cx, textRect.top + 2.0f),
                        D2D1::Point2F(cx, textRect.bottom - 2.0f),
                        txtBrush, 1.0f);
                }
                else
                {
                    if (inlineHasSelection_)
                    {
                        int a = std::min(inlineSelStart_, inlineSelEnd_);
                        int b = std::max(inlineSelStart_, inlineSelEnd_);
                        IDWriteTextLayout *selLayout = nullptr;
                        dwrite->CreateTextLayout(inlineText_.c_str(), (UINT32)inlineText_.size(), tf,
                                                 textRect.right - textRect.left, textRect.bottom - textRect.top, &selLayout);
                        if (selLayout)
                        {
                            DWRITE_TEXT_METRICS metrics;
                            selLayout->GetMetrics(&metrics);
                            float fullWidth = metrics.width;
                            std::wstring before = inlineText_.substr(0, a);
                            std::wstring selected = inlineText_.substr(0, b);

                            IDWriteTextLayout *beforeLayout = nullptr;
                            IDWriteTextLayout *selEndLayout = nullptr;
                            float startX = textRect.left;
                            float endX = textRect.left;
                            if (dwrite->CreateTextLayout(before.c_str(), (UINT32)before.size(), tf,
                                                         textRect.right - textRect.left, textRect.bottom - textRect.top, &beforeLayout) == S_OK && beforeLayout)
                            {
                                DWRITE_TEXT_METRICS m1;
                                beforeLayout->GetMetrics(&m1);
                                startX += m1.width;
                                beforeLayout->Release();
                            }
                            if (dwrite->CreateTextLayout(selected.c_str(), (UINT32)selected.size(), tf,
                                                         textRect.right - textRect.left, textRect.bottom - textRect.top, &selEndLayout) == S_OK && selEndLayout)
                            {
                                DWRITE_TEXT_METRICS m2;
                                selEndLayout->GetMetrics(&m2);
                                endX += m2.width;
                                selEndLayout->Release();
                            }

                            ID2D1SolidColorBrush *selBrush = nullptr;
                            ctx->CreateSolidColorBrush(D2D1::ColorF(0.25f, 0.45f, 0.85f, 0.45f), &selBrush);
                            if (selBrush)
                            {
                                D2D1_RECT_F selRect = D2D1::RectF(startX, textRect.top + 2.0f, endX, textRect.bottom - 2.0f);
                                ctx->FillRectangle(selRect, selBrush);
                                selBrush->Release();
                            }
                            selLayout->Release();
                        }
                    }
                    ctx->DrawTextW(inlineText_.c_str(), (UINT32)inlineText_.size(), tf, textRect, txtBrush);

                    // Caret
                    std::wstring before = inlineText_.substr(0, inlineCursorPos_);
                    IDWriteTextLayout *layout = nullptr;
                    dwrite->CreateTextLayout(before.c_str(), (UINT32)before.size(), tf,
                                             textRect.right - textRect.left, textRect.bottom - textRect.top, &layout);
                    if (layout)
                    {
                        DWRITE_TEXT_METRICS metrics;
                        layout->GetMetrics(&metrics);
                        float cx = textRect.left + metrics.width;
                        ctx->DrawLine(
                            D2D1::Point2F(cx, textRect.top + 2.0f),
                            D2D1::Point2F(cx, textRect.bottom - 2.0f),
                            txtBrush, 1.0f);
                        layout->Release();
                    }
                }

                if (tf)
                    tf->Release();
                if (txtBrush)
                    txtBrush->Release();
                if (border)
                    border->Release();
                if (bg)
                    bg->Release();
            }

            continue; // Ne pas dessiner le placeholder comme un item normal
        }

        // Dessiner les items normaux (pas de changement ici, juste le code existant)
        bool isActiveItem = (!state_.activePath.empty() && item.fullPath == state_.activePath);

        if (isActiveItem)
        {
            D2D1_RECT_F activeRect = D2D1::RectF(
                state_.leftEdge + insetX,
                std::round(item.yPosition) + insetY,
                state_.rightEdge - insetX,
                std::round(item.yPosition + item.height) - insetY);

            ID2D1SolidColorBrush* activeBrush = nullptr;
            D2D1_COLOR_F activeColor = D2D1::ColorF(0.12f, 0.18f, 0.25f, 1.0f);
            ctx->CreateSolidColorBrush(activeColor, &activeBrush);

            if (activeBrush)
            {
                DrawRoundedFill(activeRect, activeBrush);
                activeBrush->Release();
            }
        }

        if ((int)i == state_.hoveredItemIndex)
        {
            D2D1_RECT_F hoverRect = D2D1::RectF(
                state_.leftEdge + insetX,
                std::round(item.yPosition) + insetY,
                state_.rightEdge - insetX,
                std::round(item.yPosition + item.height) - insetY);

            DrawRoundedFill(hoverRect, hoverBrush);
        }

        float baseLeft = state_.leftEdge + insetX;

        float indent = state_.leftPadding + (float)(item.depth * 12);
        float iconWidth = item.isDirectory ? (float)win32_dpi_scale(16, dpi) : iconPx;

        if (item.isDirectory)
        {
            float arrowSize = (float)win32_dpi_scale(12, dpi);
            float iconY = std::round(item.yPosition + item.height * 0.5f);
            float iconLeft = std::round(baseLeft + indent + 2.0f);

            std::string key = item.expanded ? "chevron_up" : "chevron_right";

            ID2D1Bitmap *chevBmp = nullptr;
            auto cit = iconCache_.find(key);
            if (cit != iconCache_.end())
            {
                chevBmp = cit->second;
            }
            else
            {
                std::string assetPath = item.expanded ? "assets\\ressource\\icons\\chevron-up.svg" : "assets\\ressource\\icons\\chevron-right.svg";
                ID2D1Bitmap *bmp = LoadSvgIcon(ctx, assetPath, (int)arrowSize, dpi);
                if (bmp)
                {
                    iconCache_[key] = bmp;
                    chevBmp = bmp;
                }
            }

            if (chevBmp)
            {
                float left = iconLeft - arrowSize * 0.5f;
                float top = iconY - arrowSize * 0.5f;
                D2D1_RECT_F dst = D2D1::RectF(left, top, left + arrowSize, top + arrowSize);
                ctx->DrawBitmap(chevBmp, dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            }
        }
        else
        {
            ID2D1Bitmap *icon = GetIconForItem(ctx, item, hwnd);
            if (icon)
            {
                float iconY = std::round(item.yPosition + (item.height - iconPx) * 0.5f);
                D2D1_RECT_F iconRect = D2D1::RectF(
                    std::round(baseLeft + indent),
                    iconY,
                    std::round(baseLeft + indent + iconPx),
                    iconY + iconPx);
                ctx->DrawBitmap(icon, iconRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            }
        }

        D2D1_RECT_F textRect = D2D1::RectF(
            std::round(baseLeft + indent + iconWidth + 6.0f),
            std::round(item.yPosition),
            state_.rightEdge - insetX - state_.leftPadding,
            std::round(item.yPosition + item.height));

        IDWriteTextFormat *useFmt = item.isDirectory ? itemFormatBold : itemFormat;
        ctx->DrawTextW(
            item.name.c_str(),
            (UINT32)item.name.size(),
            useFmt,
            textRect,
            textBrush,
            D2D1_DRAW_TEXT_OPTIONS_NONE,
            DWRITE_MEASURING_MODE_NATURAL);
    }

    if (itemFormat)
        itemFormat->Release();
    if (itemFormatBold)
        itemFormatBold->Release();
    if (textBrush)
        textBrush->Release();
    if (hoverBrush)
        hoverBrush->Release();
}

void ExplorerManager::DrawRightBorder(ID2D1RenderTarget *ctx)
{
    // Draw border - cyan when hovering/resizing, subtle gray otherwise
    ID2D1SolidColorBrush *brush = nullptr;
    D2D1_COLOR_F borderColor = (state_.isHoveringResizeZone || state_.isResizing)
                                   ? D2D1::ColorF(0x00ccff) // Cyan on hover/resize
                                   : D2D1::ColorF(48.0f / 255.0f, 48.0f / 255.0f, 48.0f / 255.0f, 1.0f);
    ctx->CreateSolidColorBrush(borderColor, &brush);

    if (brush)
    {
        // Draw a crisp aliased vertical line
        float bx = std::round(state_.rightEdge) - 0.5f;
        D2D1_POINT_2F p1 = D2D1::Point2F(bx, state_.topEdge);
        D2D1_POINT_2F p2 = D2D1::Point2F(bx, state_.bottomEdge);

        D2D1_ANTIALIAS_MODE oldAA = ctx->GetAntialiasMode();
        ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        float thickness = (state_.isHoveringResizeZone || state_.isResizing) ? 2.0f : 1.0f;
        ctx->DrawLine(p1, p2, brush, thickness);
        ctx->SetAntialiasMode(oldAA);

        brush->Release();
    }
}

// ============================================================================
// Singleton Access
// ============================================================================

ExplorerManager &GetExplorerManager()
{
    if (!g_explorerManager)
    {
        // Create manager but do NOT auto-initialize to any folder.
        // Explorer will remain empty until the user selects "Open Project".
        g_explorerManager = new ExplorerManager();
    }

    return *g_explorerManager;
}

void ExplorerManager::ClearHover(HWND hwnd)
{
    if (state_.hoveredItemIndex != -1)
    {
        state_.hoveredItemIndex = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

ID2D1Bitmap *ExplorerManager::GetIconForItemPublic(ID2D1RenderTarget *ctx, const ExplorerItem &item, HWND hwnd)
{
    return GetIconForItem(ctx, item, hwnd);
}

ID2D1Bitmap *ExplorerManager::LoadSvgIconPublic(ID2D1RenderTarget *ctx, const std::string &iconPath, int pxSize, UINT dpi)
{
    return LoadSvgIcon(ctx, iconPath, pxSize, dpi);
}

void ExplorerManager::SetPreviewName(const std::wstring &name, Input::Type type)
{
    if (name.empty())
        return;

    ExplorerItem pi;
    pi.name = name;
    // build a plausible fullPath (not created yet)
    std::wstring full = GetActiveDirectory();
    if (!full.empty() && full.back() != L'\\' && full.back() != L'/')
        full.push_back(L'\\');
    pi.fullPath = full + name;
    pi.extension = type == Input::Type::File ? std::filesystem::path(name).extension().string() : std::string();
    pi.isDirectory = (type == Input::Type::Folder);
    pi.depth = 0;
    pi.expanded = false;

    std::lock_guard<std::mutex> lk(itemsMutex_);
    if (!hasPreview_.load())
    {
        // insert at top for visibility
        state_.items.insert(state_.items.begin(), pi);
        previewItem_ = pi;
        hasPreview_.store(true);
    }
    else
    {
        // update existing preview at index 0 if it matches
        if (!state_.items.empty())
        {
            state_.items[0] = pi;
            previewItem_ = pi;
        }
    }

    UpdateItemPositions();
}

void ExplorerManager::ClearPreview()
{
    std::lock_guard<std::mutex> lk(itemsMutex_);
    if (!hasPreview_.load())
        return;

    // remove first item if it matches preview fullPath
    if (!state_.items.empty() && state_.items[0].fullPath == previewItem_.fullPath)
    {
        state_.items.erase(state_.items.begin());
    }

    hasPreview_.store(false);
    UpdateItemPositions();
    InvalidateMainWindow();
}

// Simple search helper: case-insensitive substring search across project files
void ExplorerManager::UpdateSearchResults()
{
    searchResults_.clear();
    if (searchQuery_.empty())
        return;

    std::wstring root = state_.rootPath;
    if (root.empty())
        return;

    // helper to lowercase a narrow string
    auto toLower = [](const std::string &s)
    {
        std::string out = s;
        for (auto &c : out)
            c = (char)tolower((unsigned char)c);
        return out;
    };

    std::string q;
    {
        int needed = WideCharToMultiByte(CP_UTF8, 0, searchQuery_.c_str(), -1, NULL, 0, NULL, NULL);
        if (needed > 0)
        {
            q.resize(needed - 1);
            WideCharToMultiByte(CP_UTF8, 0, searchQuery_.c_str(), -1, &q[0], needed, NULL, NULL);
        }
        q = toLower(q);
    }

    std::vector<std::string> textExt = {".cpp", ".c", ".h", ".hpp", ".py", ".js", ".ts", ".java", ".txt", ".md", ".json", ".css", ".html", ".xml", ".rs", ".cs"};

    try
    {
        for (auto &p : std::filesystem::recursive_directory_iterator(root))
        {
            if (!p.is_regular_file())
                continue;
            std::string spth;
            int needed = WideCharToMultiByte(CP_UTF8, 0, p.path().wstring().c_str(), -1, NULL, 0, NULL, NULL);
            if (needed > 0)
            {
                spth.resize(needed - 1);
                WideCharToMultiByte(CP_UTF8, 0, p.path().wstring().c_str(), -1, &spth[0], needed, NULL, NULL);
            }

            // check extension
            std::string ext = p.path().extension().string();
            bool okExt = false;
            for (auto &e : textExt)
                if (ext == e)
                {
                    okExt = true;
                    break;
                }
            if (!okExt)
                continue;

            std::string content = ReadFileToString(p.path().wstring());
            if (content.empty())
                continue;
            std::string contentLower = toLower(content);
            size_t pos = contentLower.find(q);
            if (pos != std::string::npos)
            {
                // create excerpt around match
                size_t start = (pos > 40) ? pos - 40 : 0;
                size_t len = std::min<size_t>(200, content.size() - start);
                std::string excerpt = content.substr(start, len);
                // convert path and excerpt to wstring
                int neededP = MultiByteToWideChar(CP_UTF8, 0, spth.c_str(), -1, NULL, 0);
                std::wstring wpath(neededP, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, spth.c_str(), -1, &wpath[0], neededP);
                int neededE = MultiByteToWideChar(CP_UTF8, 0, excerpt.c_str(), -1, NULL, 0);
                std::wstring wexcerpt(neededE, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, excerpt.c_str(), -1, &wexcerpt[0], neededE);

                SearchResult r;
                r.filePath = wpath.c_str();
                r.lineExcerpt = wexcerpt.c_str();
                searchResults_.push_back(r);
                if (searchResults_.size() >= 200)
                    break; // cap
            }
        }
    }
    catch (...)
    {
    }
}

void ExplorerManager::EnterSearchMode()
{
    searchMode_ = true;
    searchQuery_.clear();
    searchResults_.clear();
}

void ExplorerManager::ExitSearchMode()
{
    searchMode_ = false;
    searchQuery_.clear();
    searchResults_.clear();
}

void ExplorerManager::OnCharSearch(wchar_t ch)
{
    if (!searchMode_)
        return;
    if (ch == 8) // backspace
    {
        if (!searchQuery_.empty())
            searchQuery_.pop_back();
    }
    else if (ch >= 32)
    {
        searchQuery_.push_back(ch);
    }
    try
    {
        UpdateSearchResults();
    }
    catch (const std::exception &e)
    {
        Logger::Instance().Log(L"Explorer: UpdateSearchResults exception");
    }
    catch (...)
    {
        Logger::Instance().Log(L"Explorer: UpdateSearchResults unknown exception");
    }
}

void ExplorerManager::OnKeyDownSearch(WPARAM key)
{
    if (!searchMode_)
        return;
    if (key == VK_ESCAPE)
    {
        ExitSearchMode();
    }
    else if (key == VK_RETURN)
    {
        // TODO: open first result
        if (!searchResults_.empty())
        {
            SetActivePath(searchResults_[0].filePath);
            // open editor logic handled elsewhere by SetActivePath
        }
    }
}

// Draw search input and results inside explorer panel
void ExplorerManager::DrawSearchPanel(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd)
{
    // Draw input box under title area
    IDWriteTextFormat *tf = nullptr;
    dwrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &tf);
    if (tf)
    {
        tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    ID2D1SolidColorBrush *bg = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.12f, 0.12f, 1.0f), &bg);
    ID2D1SolidColorBrush *border = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.3f, 0.3f, 0.3f, 1.0f), &border);
    ID2D1SolidColorBrush *txt = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.9f, 0.9f, 0.9f, 1.0f), &txt);

    float left = state_.leftEdge + state_.leftPadding;
    float right = state_.rightEdge - state_.leftPadding;
    float top = state_.topEdge + state_.titleHeight + 8.0f;
    float inputH = 30.0f;
    D2D1_RECT_F inputRect = D2D1::RectF(left, top, right, top + inputH);
    // Rounded input like explorer items
    {
        D2D1_RECT_F rr = D2D1::RectF(
            std::round(inputRect.left),
            std::round(inputRect.top),
            std::round(inputRect.right),
            std::round(inputRect.bottom));
        D2D1_ANTIALIAS_MODE oldAA = ctx->GetAntialiasMode();
        ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        ctx->FillRoundedRectangle(D2D1::RoundedRect(rr, 4.0f, 4.0f), bg);
        ctx->DrawRoundedRectangle(D2D1::RoundedRect(rr, 4.0f, 4.0f), border, 1.0f);
        ctx->SetAntialiasMode(oldAA);
    }

    // Draw query
    std::wstring display = searchQuery_.empty() ? std::wstring(L"Search...") : searchQuery_;
    ID2D1SolidColorBrush *phBrush = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.6f, 0.6f, 0.6f, 1.0f), &phBrush);
    ctx->DrawTextW(display.c_str(), (UINT32)display.size(), tf, D2D1::RectF(inputRect.left + 8.0f, inputRect.top, inputRect.right - 8.0f, inputRect.bottom), searchQuery_.empty() ? phBrush : txt);
    if (phBrush)
        phBrush->Release();

    // Draw results list below
    float y = inputRect.bottom + 8.0f;
    float itemH = 28.0f;
    int maxDisplay = 20;
    for (size_t i = 0; i < searchResults_.size() && i < (size_t)maxDisplay; ++i)
    {
        D2D1_RECT_F r = D2D1::RectF(left, y, right, y + itemH);
        ctx->DrawTextW(searchResults_[i].filePath.c_str(), (UINT32)searchResults_[i].filePath.size(), tf, D2D1::RectF(r.left + 4.0f, r.top, r.right - 4.0f, r.top + itemH * 0.5f), txt);
        ctx->DrawTextW(searchResults_[i].lineExcerpt.c_str(), (UINT32)searchResults_[i].lineExcerpt.size(), tf, D2D1::RectF(r.left + 4.0f, r.top + itemH * 0.5f, r.right - 4.0f, r.bottom), phBrush);
        y += itemH + 4.0f;
    }

    if (tf)
        tf->Release();
    if (bg)
        bg->Release();
    if (border)
        border->Release();
    if (txt)
        txt->Release();
}
