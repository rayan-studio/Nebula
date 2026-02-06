#include "utils/logger/Logger.h"
#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>
#include <mutex>

Logger::Logger() : hFile_(INVALID_HANDLE_VALUE) {}
Logger::~Logger() { Close(); }

Logger &Logger::Instance()
{
    static Logger instance;
    return instance;
}

bool Logger::Init(const std::wstring &logPath)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (hFile_ != INVALID_HANDLE_VALUE)
        return true; // already initialized

    // Create logs directory if needed
    size_t pos = logPath.find_last_of(L"/\\");
    if (pos != std::wstring::npos)
    {
        std::wstring dir = logPath.substr(0, pos);
        CreateDirectoryW(dir.c_str(), NULL);
    }

    // Open file with share flags so other processes can read while Nebula writes
    hFile_ = CreateFileW(logPath.c_str(), GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile_ == INVALID_HANDLE_VALUE)
        return false;

    // Move to end for append
    SetFilePointer(hFile_, 0, NULL, FILE_END);
    return true;
}

void Logger::Log(const std::wstring &msg)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (hFile_ == INVALID_HANDLE_VALUE)
        return;

    // Timestamp
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_s(&tm, &t);

    std::wostringstream ss;
    ss << std::put_time(&tm, L"%Y-%m-%d %H:%M:%S") << L" - " << msg << L"\r\n";
    std::wstring out = ss.str();

    DWORD written = 0;
    // WriteFile expects bytes count
    WriteFile(hFile_, out.c_str(), static_cast<DWORD>(out.size() * sizeof(wchar_t)), &written, NULL);
}

void Logger::Close()
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (hFile_ != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hFile_);
        hFile_ = INVALID_HANDLE_VALUE;
    }
}
