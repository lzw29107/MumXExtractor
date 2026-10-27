#pragma once

#include "common.h"

extern const wchar_t* kProductName;
extern const wchar_t* kProductVersion;

extern int  g_verbose;
extern bool g_console;
extern bool g_exactSizes;

void InitConsole();

// Writes text without a trailing line break.
void WriteText(std::wstring_view text);

// Writes text followed by a line break.
void WriteLine(std::wstring_view text);

void FlushOut();

template <typename... Args>
void Log(std::wformat_string<Args...> format, Args&&... args)
{
    WriteLine(std::format(format, std::forward<Args>(args)...));
}

void LogHr(const wchar_t* operation, HRESULT hr, const wchar_t* detail = nullptr);
void LogProgress(uint64_t completed, uint64_t total);
void LogProgressDone();
void LogBanner(const wchar_t* operation, const std::wstring& container);
