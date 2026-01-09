#include "CrashHandler.h"
#include "../logger/Logger.h"
#include <dbghelp.h>
#include <sstream>
#include <chrono>

// Linker: ensure DbgHelp is available. CMake should link Dbghelp.lib.

static LONG WINAPI HandleException(EXCEPTION_POINTERS *pExceptionInfo)
{
    using namespace std::chrono;
    wchar_t buf[MAX_PATH];
    SYSTEMTIME st;
    GetLocalTime(&st);

    swprintf_s(buf, L"logs\\nebula_crash_%04d%02d%02d_%02d%02d%02d.dmp",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    HANDLE hFile = CreateFileW(buf, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = pExceptionInfo;
        mei.ClientPointers = FALSE;

        // try to write a small dump
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, MiniDumpNormal, &mei, NULL, NULL);
        CloseHandle(hFile);

        std::wstringstream ss;
        ss << L"Wrote minidump to: " << buf;
        Logger::Instance().Log(ss.str());
    }
    else
    {
        Logger::Instance().Log(L"Failed to create minidump file");
    }

    // Let default handler proceed (terminates process)
    return EXCEPTION_EXECUTE_HANDLER;
}

namespace Utils
{
    void InitializeCrashHandler()
    {
        // Register our handler for unhandled exceptions
        SetUnhandledExceptionFilter(HandleException);
    }
}
