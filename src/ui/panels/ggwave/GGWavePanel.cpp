#include "GGWavePanel.h"
#include "helpers/window_helpers.h"
#include "ui/components/input/InputTheme.h"
#include "ui/theme/Theme.h"
#include <algorithm>
#include <cmath>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#pragma comment(lib, "winmm.lib")

// ============================================================================
// Global Instance
// ============================================================================

static GGWavePanel g_ggwavePanel;

GGWavePanel& GetGGWavePanel() {
    return g_ggwavePanel;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

GGWavePanel::GGWavePanel() {
}

GGWavePanel::~GGWavePanel() {
    Shutdown();
}

void GGWavePanel::Initialize() {
    // GGWave will be initialized when listening starts
}

void GGWavePanel::Shutdown() {
    StopListening();
}

// ============================================================================
// Layout
// ============================================================================

void GGWavePanel::UpdateLayout(HWND hwnd, float footerTop, float windowWidth) {
    UINT dpi = win32_get_dpi_for_window(hwnd);
    float scale = dpi / 96.0f;

    buttonSize_ = 32.0f * scale;

    // Position in bottom-right, above footer, anchored to right edge
    buttonX_ = windowWidth - buttonSize_ - 12.0f * scale;
    buttonY_ = footerTop - buttonSize_ - 8.0f * scale;
}

// ============================================================================
// Hit Testing
// ============================================================================

bool GGWavePanel::IsPointInPanel(POINT pt) const {
    return IsPointOnButton(pt);
}

bool GGWavePanel::IsPointOnButton(POINT pt) const {
    if (!visible_) return false;
    return (pt.x >= buttonX_ && pt.x <= buttonX_ + buttonSize_ &&
            pt.y >= buttonY_ && pt.y <= buttonY_ + buttonSize_);
}

bool GGWavePanel::IsPointInPopup(POINT pt) const {
    if (!visible_) return false;
    return (pt.x >= popupX_ && pt.x <= popupX_ + popupWidth_ &&
            pt.y >= popupY_ && pt.y <= popupY_ + popupHeight_);
}

// ============================================================================
// Events
// ============================================================================

void GGWavePanel::OnMouseMove(HWND /*hwnd*/, POINT clientPoint) {
    buttonHovered_ = IsPointOnButton(clientPoint);
}

void GGWavePanel::OnLeftButtonDown(HWND /*hwnd*/, POINT clientPoint) {
    if (IsPointOnButton(clientPoint)) {
        buttonPressed_ = true;
    } else if (IsPointInPopup(clientPoint)) {
        popupPressed_ = true;
    }
}

void GGWavePanel::OnLeftButtonUp(HWND hwnd) {
    // Determine release point in client coords
    POINT pt;
    if (GetCursorPos(&pt) && ScreenToClient(hwnd, &pt)) {
        if (popupPressed_ && IsPointInPopup(pt)) {
            CopyLatestMessageToClipboard();
            // refresh popup show time to keep it visible briefly
            popupShowTime_ = GetTickCount();
            showPopup_ = true;
        }
    }

    if (buttonPressed_ && buttonHovered_) {
        ToggleListening();
    }
    buttonPressed_ = false;
    popupPressed_ = false;
}

// ============================================================================
// Drawing
// ============================================================================

void GGWavePanel::Draw(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite, HWND hwnd) {
    if (!visible_ || !ctx) return;
    
    DrawButton(ctx, dwrite);
    
    if (showPopup_ && !messages_.empty()) {
        DWORD now = GetTickCount();
        if (now - popupShowTime_ < POPUP_DURATION) {
            DrawMessagePopup(ctx, dwrite, hwnd);
        } else {
            showPopup_ = false;
        }
    }
}

void GGWavePanel::DrawButton(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite) {
    const UI::Theme::Palette &themePalette = UI::Theme::GetPalette();
    auto ScaleRgb = [](D2D1_COLOR_F c, float m) -> D2D1_COLOR_F {
        c.r = (std::max)(0.0f, (std::min)(1.0f, c.r * m));
        c.g = (std::max)(0.0f, (std::min)(1.0f, c.g * m));
        c.b = (std::max)(0.0f, (std::min)(1.0f, c.b * m));
        return c;
    };

    // Button background
    ID2D1SolidColorBrush* bgBrush = nullptr;
    D2D1_COLOR_F bgColor = UI::InputTheme::Background();
    if (listening_) {
        bgColor = UI::Theme::Accent();
        if (buttonPressed_)
            bgColor = ScaleRgb(bgColor, 0.78f);
        else if (buttonHovered_)
            bgColor = ScaleRgb(bgColor, 0.90f);
    } else {
        if (buttonPressed_)
            bgColor = themePalette.explorerRowActive;
        else if (buttonHovered_)
            bgColor = themePalette.explorerRowHover;
    }

    ctx->CreateSolidColorBrush(bgColor, &bgBrush);
    
    if (bgBrush) {
        D2D1_ROUNDED_RECT roundedRect = D2D1::RoundedRect(
            D2D1::RectF(buttonX_, buttonY_, buttonX_ + buttonSize_, buttonY_ + buttonSize_),
            4.0f, 4.0f
        );
        ctx->FillRoundedRectangle(roundedRect, bgBrush);
        bgBrush->Release();
    }
    
    // Border when listening (pulsing animation)
    if (listening_) {
        DWORD now = GetTickCount();
        float elapsed = (now - lastAnimTime_) / 1000.0f;
        animPhase_ += elapsed * 2.0f; // 2 Hz pulse
        lastAnimTime_ = now;
        
        float alpha = 0.5f + 0.5f * std::sinf(animPhase_);
        
        ID2D1SolidColorBrush* borderBrush = nullptr;
        D2D1_COLOR_F pulse = UI::Theme::AccentStrong();
        pulse.a = alpha;
        ctx->CreateSolidColorBrush(pulse, &borderBrush);
        
        if (borderBrush) {
            D2D1_ROUNDED_RECT roundedRect = D2D1::RoundedRect(
                D2D1::RectF(buttonX_, buttonY_, buttonX_ + buttonSize_, buttonY_ + buttonSize_),
                4.0f, 4.0f
            );
            ctx->DrawRoundedRectangle(roundedRect, borderBrush, 2.0f);
            borderBrush->Release();
        }
    }
    
    // Icon (speaker/volume icon)
    IDWriteTextFormat* iconFormat = nullptr;
    dwrite->CreateTextFormat(L"Segoe MDL2 Assets", NULL, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             16.0f, L"en-us", &iconFormat);
    
    if (iconFormat) {
        iconFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        iconFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        
        ID2D1SolidColorBrush* iconBrush = nullptr;
        ctx->CreateSolidColorBrush(listening_ ? UI::Theme::PrimaryText() : UI::Theme::MutedText(), &iconBrush);
        
        if (iconBrush) {
            // \uE767 = Volume, \uE74F = Mute, \uE720 = Microphone
            const wchar_t* icon = listening_ ? L"\uE720" : L"\uE74F";
            
            D2D1_RECT_F iconRect = D2D1::RectF(buttonX_, buttonY_, 
                                               buttonX_ + buttonSize_, buttonY_ + buttonSize_);
            ctx->DrawTextW(icon, 1, iconFormat, iconRect, iconBrush);
            iconBrush->Release();
        }
        iconFormat->Release();
    }
}

void GGWavePanel::DrawMessagePopup(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite, HWND hwnd) {
    if (messages_.empty()) return;

    const GGWaveMessage& msg = messages_.back();

    // DPI-aware sizing
    UINT dpi = hwnd ? win32_get_dpi_for_window(hwnd) : 96;
    float scale = dpi / 96.0f;

    // Popup base geometry (DPI-aware)
    float popupWidth = 250.0f * scale;
    float popupX = buttonX_ - popupWidth - 8.0f * scale;
    if (popupX < 8.0f * scale) popupX = 8.0f * scale;

    // assign member geometry for hit-testing
    popupX_ = popupX;
    popupWidth_ = popupWidth;

    // Prepare title and message formats
    IDWriteTextFormat* titleFormat = nullptr;
    dwrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             11.0f * scale, L"en-us", &titleFormat);

    IDWriteTextFormat* textFormat = nullptr;
    dwrite->CreateTextFormat(L"Consolas", NULL, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             12.0f * scale, L"en-us", &textFormat);
    if (textFormat) {
        textFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }

    // Calculate message layout height
    float contentWidth = popupWidth - 20.0f * scale;
    float contentHeight = 0.0f;
    IDWriteTextLayout* textLayout = nullptr;
    if (textFormat) {
        if (SUCCEEDED(dwrite->CreateTextLayout(msg.text.c_str(), (UINT32)msg.text.length(), textFormat, contentWidth, 10000.0f * scale, &textLayout)) && textLayout) {
            DWRITE_TEXT_METRICS metrics = {};
            textLayout->GetMetrics(&metrics);
            contentHeight = metrics.height;
        }
    }

    // Title area + paddings
    float titleHeight = 18.0f * scale;
    float padding = 12.0f * scale;

    float popupHeight = titleHeight + contentHeight + padding * 2.0f;
    float maxPopupHeight = 200.0f * scale;
    if (popupHeight > maxPopupHeight) popupHeight = maxPopupHeight;

    // Place popup above the button so it doesn't overlap the footer
    float popupY = buttonY_ - popupHeight - 8.0f * scale;
    if (popupY < 8.0f * scale) popupY = 8.0f * scale;

    popupY_ = popupY;
    popupHeight_ = popupHeight;

    // Background
    ID2D1SolidColorBrush* bgBrush = nullptr;
    D2D1_COLOR_F popupBg = UI::Theme::ChromeBackground();
    popupBg.a = 0.95f;
    ctx->CreateSolidColorBrush(popupBg, &bgBrush);
    if (bgBrush) {
        D2D1_ROUNDED_RECT roundedRect = D2D1::RoundedRect(
            D2D1::RectF(popupX, popupY, popupX + popupWidth, popupY + popupHeight),
            6.0f, 6.0f
        );
        ctx->FillRoundedRectangle(roundedRect, bgBrush);
        bgBrush->Release();
    }

    // Border
    ID2D1SolidColorBrush* borderBrush = nullptr;
    ctx->CreateSolidColorBrush(UI::Theme::Accent(), &borderBrush);
    if (borderBrush) {
        D2D1_ROUNDED_RECT roundedRect = D2D1::RoundedRect(
            D2D1::RectF(popupX, popupY, popupX + popupWidth, popupY + popupHeight),
            6.0f, 6.0f
        );
        ctx->DrawRoundedRectangle(roundedRect, borderBrush, 1.0f);
        borderBrush->Release();
    }

    // Title
    if (titleFormat) {
        ID2D1SolidColorBrush* titleBrush = nullptr;
        ctx->CreateSolidColorBrush(UI::Theme::AccentStrong(), &titleBrush);
        if (titleBrush) {
            D2D1_RECT_F titleRect = D2D1::RectF(popupX + 10.0f * scale, popupY + 6.0f * scale,
                                                popupX + popupWidth - 10.0f * scale, popupY + 6.0f * scale + titleHeight - 4.0f * scale);
            ctx->DrawTextW(L"GGWave Received", 15, titleFormat, titleRect, titleBrush);
            titleBrush->Release();
        }
        titleFormat->Release();
    }

    // Message (use layout to ensure wrapping and clipping)
    if (textFormat) {
        ID2D1SolidColorBrush* textBrush = nullptr;
        ctx->CreateSolidColorBrush(UI::Theme::PrimaryText(), &textBrush);
        if (textBrush) {
            if (textLayout) {
                D2D1_POINT_2F origin = { popupX + 10.0f * scale, popupY + titleHeight + 6.0f * scale };
                ctx->DrawTextLayout(origin, textLayout, textBrush);
            } else {
                // Fallback
                D2D1_RECT_F textRect = D2D1::RectF(popupX + 10.0f * scale, popupY + titleHeight + 6.0f * scale,
                                                   popupX + popupWidth - 10.0f * scale, popupY + popupHeight - 6.0f * scale);
                ctx->DrawTextW(msg.text.c_str(), (UINT32)msg.text.length(), textFormat, textRect, textBrush);
            }
            textBrush->Release();
        }
        textFormat->Release();
    }

    if (textLayout) textLayout->Release();
}

// ============================================================================
// Listening Control
// ============================================================================

void GGWavePanel::ToggleListening() {
    if (listening_) {
        StopListening();
    } else {
        StartListening();
    }
}

void GGWavePanel::StartListening() {
    if (listening_) return;
    
    std::lock_guard<std::mutex> lock(ggwaveMutex_);
    
    // Initialize GGWave for receiving
    GGWave::Parameters params = GGWave::getDefaultParameters();
    params.sampleRateInp = 48000.0f;
    params.sampleRateOut = 48000.0f;
    params.sampleRate = 48000.0f;
    params.samplesPerFrame = 1024;
    params.sampleFormatInp = GGWAVE_SAMPLE_FORMAT_I16;
    params.sampleFormatOut = GGWAVE_SAMPLE_FORMAT_I16;
    params.operatingMode = GGWAVE_OPERATING_MODE_RX; // Receive mode
    
    try {
        ggwave_ = std::make_unique<GGWave>(params);
    } catch (...) {
        return;
    }
    
    // Start audio capture
    StartAudioCapture();
    
    listening_ = true;
    lastAnimTime_ = GetTickCount();
}

void GGWavePanel::StopListening() {
    if (!listening_) return;
    
    listening_ = false;
    
    // Stop audio capture
    StopAudioCapture();
    
    // Clean up GGWave
    std::lock_guard<std::mutex> lock(ggwaveMutex_);
    ggwave_.reset();
}

// ============================================================================
// Audio Capture (Windows waveIn API)
// ============================================================================

void GGWavePanel::StartAudioCapture() {
    WAVEFORMATEX wfx = {};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = 48000;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    
    MMRESULT result = waveInOpen(&hWaveIn_, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
    if (result != MMSYSERR_NOERROR) {
        return;
    }
    
    // Prepare buffers
    audioBuffers_.resize(NUM_BUFFERS);
    waveHeaders_.resize(NUM_BUFFERS);
    
    for (int i = 0; i < NUM_BUFFERS; ++i) {
        audioBuffers_[i].resize(BUFFER_SIZE);
        
        ZeroMemory(&waveHeaders_[i], sizeof(WAVEHDR));
        waveHeaders_[i].lpData = reinterpret_cast<LPSTR>(audioBuffers_[i].data());
        waveHeaders_[i].dwBufferLength = BUFFER_SIZE * sizeof(int16_t);
        
        waveInPrepareHeader(hWaveIn_, &waveHeaders_[i], sizeof(WAVEHDR));
        waveInAddBuffer(hWaveIn_, &waveHeaders_[i], sizeof(WAVEHDR));
    }
    
    captureRunning_ = true;
    captureThread_ = std::thread(&GGWavePanel::CaptureThread, this);
    
    waveInStart(hWaveIn_);
}

void GGWavePanel::StopAudioCapture() {
    captureRunning_ = false;
    
    if (hWaveIn_) {
        waveInStop(hWaveIn_);
        waveInReset(hWaveIn_);
        
        for (auto& header : waveHeaders_) {
            waveInUnprepareHeader(hWaveIn_, &header, sizeof(WAVEHDR));
        }
        
        waveInClose(hWaveIn_);
        hWaveIn_ = nullptr;
    }
    
    if (captureThread_.joinable()) {
        captureThread_.join();
    }
    
    waveHeaders_.clear();
    audioBuffers_.clear();
}

void GGWavePanel::CaptureThread() {
    while (captureRunning_) {
        for (int i = 0; i < NUM_BUFFERS && captureRunning_; ++i) {
            WAVEHDR& header = waveHeaders_[i];
            
            if (header.dwFlags & WHDR_DONE) {
                // Process the captured audio
                int numSamples = header.dwBytesRecorded / sizeof(int16_t);
                if (numSamples > 0) {
                    ProcessAudioData(reinterpret_cast<int16_t*>(header.lpData), numSamples);
                }
                
                // Re-add buffer
                header.dwFlags = 0;
                header.dwBytesRecorded = 0;
                waveInPrepareHeader(hWaveIn_, &header, sizeof(WAVEHDR));
                waveInAddBuffer(hWaveIn_, &header, sizeof(WAVEHDR));
            }
        }
        
        Sleep(10);
    }
}

void GGWavePanel::ProcessAudioData(const int16_t* samples, int numSamples) {
    std::lock_guard<std::mutex> lock(ggwaveMutex_);
    
    if (!ggwave_) return;
    
    // Feed audio to GGWave decoder
    ggwave_->decode(samples, numSamples);
    
    // Check for decoded message
    int len = ggwave_->rxDataLength();
    if (len > 0) {
        const char* data = reinterpret_cast<const char*>(ggwave_->rxData().data());
        
        // Convert to wide string
        int wideLen = MultiByteToWideChar(CP_UTF8, 0, data, len, NULL, 0);
        std::wstring message(wideLen, 0);
        MultiByteToWideChar(CP_UTF8, 0, data, len, message.data(), wideLen);
        
        // Add to messages
        {
            std::lock_guard<std::mutex> msgLock(messagesMutex_);
            messages_.push_back({message, GetTickCount()});
            if (messages_.size() > MAX_MESSAGES) {
                messages_.pop_front();
            }
        }
        
        showPopup_ = true;
        popupShowTime_ = GetTickCount();
    }
}

// ============================================================================
// Messages
// ============================================================================

bool GGWavePanel::HasNewMessage() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(messagesMutex_));
    return !messages_.empty();
}

GGWaveMessage GGWavePanel::PopMessage() {
    std::lock_guard<std::mutex> lock(messagesMutex_);
    if (messages_.empty()) {
        return {L"", 0};
    }
    GGWaveMessage msg = messages_.front();
    messages_.pop_front();
    return msg;
}

void GGWavePanel::ClearMessages() {
    std::lock_guard<std::mutex> lock(messagesMutex_);
    messages_.clear();
}

void GGWavePanel::CopyLatestMessageToClipboard() {
    std::lock_guard<std::mutex> lock(messagesMutex_);
    if (messages_.empty()) return;
    const std::wstring& msg = messages_.back().text;
    if (msg.empty()) return;

    if (!OpenClipboard(NULL)) return;
    EmptyClipboard();

    size_t bytes = (msg.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hMem) {
        CloseClipboard();
        return;
    }
    void* ptr = GlobalLock(hMem);
    if (ptr) {
        memcpy(ptr, msg.c_str(), bytes);
        GlobalUnlock(hMem);
        SetClipboardData(CF_UNICODETEXT, hMem);
    } else {
        GlobalFree(hMem);
    }

    CloseClipboard();
}
