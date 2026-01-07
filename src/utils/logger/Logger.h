#pragma once
#include <string>
#include <mutex>
#include <Windows.h>

class Logger {
public:
    static Logger &Instance();
    bool Init(const std::wstring &logPath);
    void Log(const std::wstring &msg);
    void Close();
private:
    Logger();
    ~Logger();
    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;

    std::mutex mtx_;
    HANDLE hFile_;
};
