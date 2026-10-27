#include "console.h"

const wchar_t* kProductName    = L"MumXExtractor";
const wchar_t* kProductVersion = L"1.0.0";

int  g_verbose    = 0;
bool g_console    = false;
bool g_exactSizes = false;

namespace
{
constexpr size_t kFlushThreshold = 16 * 1024;

HANDLE       g_stdoutHandle  = INVALID_HANDLE_VALUE;
bool         g_consoleOutput = false;
std::wstring g_pending;

bool WriteConsoleText(std::wstring_view text)
{
    const wchar_t* cursor = text.data();
    size_t remaining = text.size();
    bool emitted = false;
    while (remaining != 0)
    {
        DWORD chunk = (DWORD)std::min<size_t>(remaining, 8192);
        DWORD written = 0;
        if (!WriteConsoleW(g_stdoutHandle, cursor, chunk, &written, nullptr) || written == 0)
            return emitted;   // never repeat text that already reached the console
        emitted = true;
        cursor += written;
        remaining -= written;
    }
    return true;
}

void WriteFileText(std::wstring_view text)
{
    if (text.empty()) return;

    int byteCount = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(),
                                        nullptr, 0, nullptr, nullptr);
    if (byteCount <= 0) return;

    std::string utf8((size_t)byteCount, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(),
                        utf8.data(), byteCount, nullptr, nullptr);

    const char* cursor = utf8.data();
    size_t remaining = utf8.size();
    while (remaining != 0)
    {
        DWORD chunk = (DWORD)std::min<size_t>(remaining, 1u << 20);
        DWORD written = 0;
        if (!WriteFile(g_stdoutHandle, cursor, chunk, &written, nullptr) || written == 0) return;
        cursor += written;
        remaining -= written;
    }
}

void Emit(std::wstring_view text)
{
    if (g_consoleOutput && WriteConsoleText(text)) return;

    // A console handle that rejects WriteConsoleW still accepts bytes, so fall
    // back to UTF-8 output for the rest of the run.
    g_consoleOutput = false;
    WriteFileText(text);
}

struct OutputFlusher
{
    ~OutputFlusher() { FlushOut(); }
};

OutputFlusher g_outputFlusher;
} // namespace

void InitConsole()
{
    g_stdoutHandle = GetStdHandle(STD_OUTPUT_HANDLE);

    DWORD mode = 0;
    g_consoleOutput = g_stdoutHandle != nullptr && g_stdoutHandle != INVALID_HANDLE_VALUE &&
                      GetConsoleMode(g_stdoutHandle, &mode) != 0;
    g_console = g_consoleOutput;

    g_pending.reserve(kFlushThreshold * 2);
}

void FlushOut()
{
    if (g_pending.empty()) return;
    Emit(g_pending);
    g_pending.clear();
}

void WriteText(std::wstring_view text)
{
    // Line breaks inside a block are normalized to CRLF, matching WriteLine.
    for (wchar_t ch : text)
    {
        if (ch == L'\n' && (g_pending.empty() || g_pending.back() != L'\r'))
            g_pending.push_back(L'\r');
        g_pending.push_back(ch);
    }
    if (g_pending.size() >= kFlushThreshold) FlushOut();
}

void WriteLine(std::wstring_view text)
{
    g_pending.append(text);
    g_pending.append(L"\r\n");
    if (g_pending.size() >= kFlushThreshold) FlushOut();
}

void LogProgress(uint64_t completed, uint64_t total)
{
    if (!g_console || g_verbose || total == 0) return;

    constexpr int kWidth = 58;

    double percent = 100.0 * (double)completed / (double)total;
    if (percent > 100.0) percent = 100.0;

    const std::wstring percentText = std::format(L"{:.1f}%", percent);
    const int textLength = (int)percentText.size();
    const int textStart  = (kWidth - textLength + 1) / 2;

    int filled = (int)((double)kWidth * percent / 100.0);
    if (completed != 0 && filled == 0) filled = 1;
    filled = std::clamp(filled, 0, kWidth);

    std::wstring line;
    line.reserve((size_t)kWidth + 2);
    line.push_back(L'[');
    for (int i = 0; i < textStart; ++i) line.push_back(i < filled ? L'=' : L' ');
    line += percentText;
    for (int i = textStart + textLength; i < kWidth; ++i)
        line.push_back(i < filled ? L'=' : L' ');
    line.push_back(L']');

    static std::wstring lastLine;
    if (line == lastLine) return;
    lastLine = line;

    FlushOut();
    Emit(std::wstring(L"\r") + line);
}

void LogProgressDone()
{
    if (!g_console) return;
    FlushOut();
    Emit(L"\r\n");
}

void LogHr(const wchar_t* operation, HRESULT hr, const wchar_t* detail)
{
    wchar_t message[512] = {};
    DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                  nullptr, (DWORD)hr, 0, message, (DWORD)std::size(message), nullptr);
    while (length != 0 && (message[length - 1] == L'\r' || message[length - 1] == L'\n' ||
                           message[length - 1] == L'.'))
    {
        message[--length] = L'\0';
    }

    Log(L"error: {} failed, hr = 0x{:08X} ({})", operation, (unsigned)hr,
        length != 0 ? message : L"unknown error");
    if (detail != nullptr && *detail != L'\0') Log(L"       {}", detail);
}

const wchar_t* SectionTypeName(int64_t sectionType)
{
    switch (sectionType)
    {
    case kSectionDataPool:             return L"DataPool";
    case kSectionEntries:              return L"EntriesSection";
    case kSectionEntriesAlt:           return L"EntriesSection (variant)";
    case kSectionSecondaryDataPool:    return L"DataPool (secondary)";
    case kSectionSecondaryDataPoolAlt: return L"DataPool (secondary variant)";
    case kSectionIndex:                return L"IndexSection";
    default:                           return L"unknown";
    }
}

const wchar_t* CompressionName(uint32_t compressionAlg)
{
    switch (compressionAlg)
    {
    case kCompressionNone:       return L"none";
    case kCompressionLzms:       return L"LZMS";
    case kCompressionXpress:     return L"XPRESS";
    case kCompressionXpressHuff: return L"XPRESS-HUFF";
    default:                     return L"unknown";
    }
}

void LogBanner(const wchar_t* operation, const std::wstring& container)
{
    Log(L"{} v{}", kProductName, kProductVersion);
    if (operation != nullptr)
    {
        Log(L"operation : {}", operation);
        if (!container.empty()) Log(L"container : {}", container);
    }
    Log(L"");
}
