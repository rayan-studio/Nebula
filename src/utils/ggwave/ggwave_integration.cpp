#include "ggwave_integration.h"
#include "utils/logger/Logger.h"
#include <ggwave/ggwave.h>
#include <memory>
#include <vector>
#include <string>
#include <windows.h>
#include <mmsystem.h>
#include <thread>

#pragma comment(lib, "winmm.lib")

namespace ggwave
{
    static std::unique_ptr<GGWave> g_ggwave = nullptr;
    static HWAVEOUT g_hWaveOut = nullptr;
    
    bool Initialize()
    {
        
        try
        {
            // Configuration ggwave - start from defaults and override
            GGWave::Parameters params = GGWave::getDefaultParameters();
            params.sampleRateInp = 48000.0f;
            params.sampleRateOut = 48000.0f;
            params.sampleRate = 48000.0f;
            params.samplesPerFrame = 1024;
            params.sampleFormatInp = GGWAVE_SAMPLE_FORMAT_I16;
            params.sampleFormatOut = GGWAVE_SAMPLE_FORMAT_I16;
            params.operatingMode = GGWAVE_OPERATING_MODE_TX; // Mode transmission
            params.payloadLength = 0;
            params.soundMarkerThreshold = GGWave::kDefaultSoundMarkerThreshold;

            g_ggwave = std::make_unique<GGWave>(params);
            
            // Initialiser la sortie audio Windows (waveOut)
            WAVEFORMATEX wfx = {};
            wfx.wFormatTag = WAVE_FORMAT_PCM;
            wfx.nChannels = 1;
            wfx.nSamplesPerSec = 48000;
            wfx.wBitsPerSample = 16;
            wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
            wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
            
            MMRESULT result = waveOutOpen(&g_hWaveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
            if (result != MMSYSERR_NOERROR)
            {
                Logger::Instance().Log(L"ggwave: Failed to open audio output device");
                g_ggwave.reset();
                return false;
            }
            
            return true;
        }
        catch (const std::exception& e)
        {
            std::wstring msg = L"ggwave: Exception during initialization: ";
            std::string err = e.what();
            msg += std::wstring(err.begin(), err.end());
            Logger::Instance().Log(msg);
            return false;
        }
    }
    
    bool SpeakText(const std::wstring &text)
    {
        if (!g_ggwave || !g_hWaveOut)
        {
            Logger::Instance().Log(L"ggwave: Not initialized, call Initialize() first");
            return false;
        }
        
        if (text.empty())
        {
            Logger::Instance().Log(L"ggwave: Empty text provided");
            return false;
        }
        
        try
        {
            // Convertir wstring en string UTF-8 (include null terminator)
            int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string utf8Text;
            if (sizeNeeded > 0) {
                utf8Text.resize(sizeNeeded);
                WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, &utf8Text[0], sizeNeeded, nullptr, nullptr);
                // remove trailing null added by WideCharToMultiByte
                if (!utf8Text.empty() && utf8Text.back() == '\0') utf8Text.pop_back();
            }
            
            Logger::Instance().Log(L"ggwave: Encoding text to audio...");
            
            // Utiliser l'API correcte de ggwave : init(const char*, TxProtocolId, volume)
            if (!g_ggwave->init(utf8Text.c_str(), GGWave::ProtocolId::GGWAVE_PROTOCOL_AUDIBLE_NORMAL, 50))
            {
                Logger::Instance().Log(L"ggwave: Failed to initialize transmission");
                return false;
            }
            
            // Générer les échantillons audio
            std::vector<int16_t> audioSamples;

            // Call encode() to generate the waveform. It returns the number of bytes generated.
            int nBytes = g_ggwave->encode();
            if (nBytes <= 0)
            {
                Logger::Instance().Log(L"ggwave: encode() produced no data");
                return false;
            }

            // Retrieve pointer to generated waveform and copy it. We configured sampleFormatOut = I16
            const void * waveformPtr = g_ggwave->txWaveform();
            if (!waveformPtr)
            {
                Logger::Instance().Log(L"ggwave: txWaveform() returned null");
                return false;
            }

            const int16_t * samples = reinterpret_cast<const int16_t *>(waveformPtr);
            int nSamples = nBytes / static_cast<int>(sizeof(int16_t));
            audioSamples.insert(audioSamples.end(), samples, samples + nSamples);
            
            if (audioSamples.empty())
            {
                Logger::Instance().Log(L"ggwave: No audio samples generated");
                return false;
            }
            
            Logger::Instance().Log(L"ggwave: Playing audio...");

            // Déplacer les samples dans un buffer partagé avant de préparer/écrire
            auto audioBuffer = std::make_shared<std::vector<int16_t>>(std::move(audioSamples));

            // Préparer un header sur le tas qui pointe sur le buffer partagé
            WAVEHDR* pWaveHdr = new WAVEHDR();
            ZeroMemory(pWaveHdr, sizeof(WAVEHDR));
            pWaveHdr->lpData = reinterpret_cast<LPSTR>(audioBuffer->data());
            pWaveHdr->dwBufferLength = static_cast<DWORD>(audioBuffer->size() * sizeof(int16_t));

            MMRESULT result = waveOutPrepareHeader(g_hWaveOut, pWaveHdr, sizeof(WAVEHDR));
            if (result != MMSYSERR_NOERROR)
            {
                delete pWaveHdr;
                Logger::Instance().Log(L"ggwave: Failed to prepare wave header (async)");
                return false;
            }

            result = waveOutWrite(g_hWaveOut, pWaveHdr, sizeof(WAVEHDR));
            if (result != MMSYSERR_NOERROR)
            {
                waveOutUnprepareHeader(g_hWaveOut, pWaveHdr, sizeof(WAVEHDR));
                delete pWaveHdr;
                Logger::Instance().Log(L"ggwave: Failed to write audio (async)");
                return false;
            }

            // Démarrer un thread détaché pour attendre la fin de la lecture puis nettoyer
            std::thread([pWaveHdr, audioBuffer]() mutable {
                while (!(pWaveHdr->dwFlags & WHDR_DONE))
                {
                    Sleep(10);
                }
                waveOutUnprepareHeader(g_hWaveOut, pWaveHdr, sizeof(WAVEHDR));
                delete pWaveHdr;
            }).detach();

            Logger::Instance().Log(L"ggwave: Audio playback started (async)");
            return true;
        }
        catch (const std::exception& e)
        {
            std::wstring msg = L"ggwave: Exception during text encoding: ";
            std::string err = e.what();
            msg += std::wstring(err.begin(), err.end());
            Logger::Instance().Log(msg);
            return false;
        }
    }
    
    void Shutdown()
    {
        Logger::Instance().Log(L"ggwave: Shutting down...");
        
        if (g_hWaveOut)
        {
            waveOutReset(g_hWaveOut);
            waveOutClose(g_hWaveOut);
            g_hWaveOut = nullptr;
        }
        
        g_ggwave.reset();
        
        Logger::Instance().Log(L"ggwave: Shutdown complete");
    }

} // namespace ggwave