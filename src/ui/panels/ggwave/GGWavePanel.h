#pragma once
#include <windows.h>
#include <mmsystem.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <deque>
#include <memory>
#include <ggwave/ggwave.h>

#pragma comment(lib, "winmm.lib")

// ============================================================================
// GGWave Listener Panel - Audio data transmission receiver
// Small floating widget to enable/disable listening for ggwave signals
// ============================================================================

struct GGWaveMessage {
    std::wstring text;
    DWORD timestamp;
};

class GGWavePanel {
public:
    GGWavePanel();
    ~GGWavePanel();
    
    // Lifecycle
    void Initialize();
    void Shutdown();
    
    // Drawing
    void Draw(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite, HWND hwnd);
    void UpdateLayout(HWND hwnd, float footerTop, float windowWidth);
    void DrawMessagePopup(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite, HWND hwnd);
    
    // Events
    void OnLeftButtonDown(HWND hwnd, POINT clientPoint);
    void OnLeftButtonUp(HWND hwnd);
    void OnMouseMove(HWND hwnd, POINT clientPoint);
    
    // Hit testing
    bool IsPointInPanel(POINT pt) const;
    bool IsPointOnButton(POINT pt) const;
    bool IsPointInPopup(POINT pt) const;
    
    // Listening state
    bool IsListening() const { return listening_; }
    void StartListening();
    void StopListening();
    void ToggleListening();
    
    // Messages
    bool HasNewMessage() const;
    GGWaveMessage PopMessage();
    const std::deque<GGWaveMessage>& GetMessages() const { return messages_; }
    void ClearMessages();

    // Clipboard
    void CopyLatestMessageToClipboard();
    
    // Visibility
    bool IsVisible() const { return visible_; }
    void SetVisible(bool v) { visible_ = v; }
    
private:
    // Drawing helpers
    void DrawButton(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite);
    void DrawMessagePopup(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite);
    
    // Audio capture
    void StartAudioCapture();
    void StopAudioCapture();
    void CaptureThread();
    void ProcessAudioData(const int16_t* samples, int numSamples);
    
    // State
    bool visible_ = true;
    bool listening_ = false;
    bool buttonHovered_ = false;
    bool buttonPressed_ = false;
    
    // Position (bottom-right corner, above footer)
    float buttonX_ = 0.0f;
    float buttonY_ = 0.0f;
    float buttonSize_ = 36.0f;
    
    // GGWave instance for receiving
    std::unique_ptr<GGWave> ggwave_;
    std::mutex ggwaveMutex_;
    
    // Audio capture
    HWAVEIN hWaveIn_ = nullptr;
    std::vector<WAVEHDR> waveHeaders_;
    std::vector<std::vector<int16_t>> audioBuffers_;
    static const int NUM_BUFFERS = 4;
    static const int BUFFER_SIZE = 4096;
    
    std::atomic<bool> captureRunning_{false};
    std::thread captureThread_;
    
    // Messages received
    std::deque<GGWaveMessage> messages_;
    std::mutex messagesMutex_;
    static const size_t MAX_MESSAGES = 50;
    
    // Animation
    DWORD lastAnimTime_ = 0;
    float animPhase_ = 0.0f;
    
    // Popup for new messages
    bool showPopup_ = false;
    DWORD popupShowTime_ = 0;
    static const DWORD POPUP_DURATION = 5000; // 5 seconds

    // Popup geometry (updated during DrawMessagePopup)
    float popupX_ = 0.0f;
    float popupY_ = 0.0f;
    float popupWidth_ = 0.0f;
    float popupHeight_ = 0.0f;
    bool popupPressed_ = false;
};

// Global accessor
GGWavePanel& GetGGWavePanel();
