#pragma once
#include <string>

// Simple wrapper for ggwave TTS integration.
// If GGWAVE_ENABLED is not defined, falls back to Windows SAPI TTS.

namespace ggwave
{
    // Initialize the engine (noop for fallback)
    bool Initialize();
    // Speak text (blocking). Returns true on success.
    bool SpeakText(const std::wstring &text);
    // Shutdown engine
    void Shutdown();
}
