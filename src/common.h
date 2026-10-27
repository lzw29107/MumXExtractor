#pragma once

import std;
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>

static constexpr std::uint32_t kHashAlgSha256 = CALG_SHA_256;
static constexpr std::int32_t  kMumXVersionMax   = 2;       // MumXHeader::Deserialize
static constexpr std::uint32_t kDataPoolHeaderV1 = 28;      // version/type/size/poolOffset
static constexpr std::uint32_t kDataPoolHeaderV2 = 52;      // + compressionAlg/blockSize/uncompressedSize/blockIndexSize
static constexpr std::uint32_t kBlockIndexHeaderSize = 28;

enum ContainerCompression : std::uint32_t
{
    kCompressionNone       = 0,
    kCompressionLzms       = 1,   // -> COMPRESSION_FORMAT_LZMS
    kCompressionXpress     = 2,   // -> COMPRESSION_FORMAT_XPRESS
    kCompressionXpressHuff = 3,   // -> COMPRESSION_FORMAT_XPRESS_HUFF
};

enum MumXSectionType : std::int64_t
{
    kSectionDataPool             = 0,
    kSectionEntries              = 1,  // -> EntriesSectionReader
    kSectionEntriesAlt           = 2,
    kSectionSecondaryDataPool    = 3,
    kSectionSecondaryDataPoolAlt = 4,
    kSectionIndex                = 5,  // -> IndexSectionReader
};

enum class Engine { Auto, Dll, Native };

inline bool ReadLeb128(const std::vector<std::uint8_t>& buffer, size_t& cursor, std::uint64_t& value)
{
    value = 0;
    int shift = 0;
    for (;;)
    {
        if (cursor >= buffer.size()) return false;
        uint8_t byte = buffer[cursor++];
        value |= (uint64_t)(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) return true;
        shift += 7;
        if (shift > 63) return false;
    }
}

inline double NowMs()
{
    LARGE_INTEGER frequency{}, counter{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
}


// ASCII-only case folding; identifiers and option tokens are ASCII, and this
// keeps the result independent of the active locale.
constexpr char AsciiLower(char ch)
{
    return (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
}

constexpr wchar_t AsciiLower(wchar_t ch)
{
    return (ch >= L'A' && ch <= L'Z') ? (wchar_t)(ch - L'A' + L'a') : ch;
}

inline std::wstring ToLower(std::wstring text)
{
    for (wchar_t& ch : text) ch = AsciiLower(ch);
    return text;
}

inline std::wstring NarrowToWide(std::string_view text)
{
    if (text.empty()) return std::wstring();
    int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    if (length <= 0) return std::wstring();
    std::wstring result((size_t)length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), result.data(), length);
    return result;
}

const wchar_t* SectionTypeName(int64_t sectionType);
const wchar_t* CompressionName(uint32_t compressionAlg);
