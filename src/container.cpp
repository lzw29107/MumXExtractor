#include "container.h"

typedef LONG (NTAPI* PFN_RtlDecompressBufferEx)(USHORT compressionFormat,
                                                PUCHAR uncompressedBuffer,
                                                ULONG  uncompressedBufferSize,
                                                PUCHAR compressedBuffer,
                                                ULONG  compressedBufferSize,
                                                PULONG finalUncompressedSize,
                                                PVOID  workspace);
typedef LONG (NTAPI* PFN_RtlGetCompressionWorkSpaceSize)(USHORT compressionFormatAndEngine,
                                                         PULONG compressBufferWorkSpaceSize,
                                                         PULONG compressFragmentWorkSpaceSize);

static constexpr USHORT kRtlFormatLznt1      = 0x0002;
static constexpr USHORT kRtlFormatXpress     = 0x0003;
static constexpr USHORT kRtlFormatXpressHuff = 0x0004;
static constexpr USHORT kRtlFormatLzms       = 0x0005;

static PFN_RtlDecompressBufferEx          g_rtlDecompressBufferEx = nullptr;
static PFN_RtlGetCompressionWorkSpaceSize g_rtlGetWorkspaceSize = nullptr;

static bool EnsureDecompressorLoaded()
{
    if (g_rtlDecompressBufferEx) return true;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) ntdll = LoadLibraryW(L"ntdll.dll");
    if (!ntdll) return false;
    g_rtlDecompressBufferEx = reinterpret_cast<PFN_RtlDecompressBufferEx>(
        GetProcAddress(ntdll, "RtlDecompressBufferEx"));
    g_rtlGetWorkspaceSize = reinterpret_cast<PFN_RtlGetCompressionWorkSpaceSize>(
        GetProcAddress(ntdll, "RtlGetCompressionWorkSpaceSize"));
    return g_rtlDecompressBufferEx != nullptr;
}

static USHORT ToRtlCompressionFormat(uint32_t compressionAlg)
{
    switch (compressionAlg)
    {
    case kCompressionLzms:       return kRtlFormatLzms;
    case kCompressionXpress:     return kRtlFormatXpress;
    case kCompressionXpressHuff: return kRtlFormatXpressHuff;
    default:                     return 0;
    }
}

void MumxContainerFile::Close()
{
    if (m_file != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_file);
        m_file = INVALID_HANDLE_VALUE;
    }
}

bool MumxContainerFile::ReadAt(uint64_t offset, void* buffer, size_t byteCount)
{
    if (offset + byteCount > m_fileSize) return false;
    LARGE_INTEGER distance{};
    distance.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(m_file, distance, nullptr, FILE_BEGIN)) return false;
    DWORD bytesRead = 0;
    return ReadFile(m_file, buffer, (DWORD)byteCount, &bytesRead, nullptr) && bytesRead == byteCount;
}

bool MumxContainerFile::ReadU32BE(uint64_t offset, uint32_t& value)
{
    uint8_t raw[4];
    if (!ReadAt(offset, raw, 4)) return false;
    value = ((uint32_t)raw[0] << 24) | ((uint32_t)raw[1] << 16) |
            ((uint32_t)raw[2] << 8) | (uint32_t)raw[3];
    return true;
}

bool MumxContainerFile::ReadI64BE(uint64_t offset, int64_t& value)
{
    uint8_t raw[8];
    if (!ReadAt(offset, raw, 8)) return false;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | raw[i];
    value = (int64_t)v;
    return true;
}

bool MumxContainerFile::Open(const std::wstring& containerPath, std::wstring& error)
{
    Close();
    m_file = CreateFileW(containerPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_file == INVALID_HANDLE_VALUE)
    {
        error = L"cannot open container file";
        return false;
    }

    LARGE_INTEGER fileSizeInfo{};
    GetFileSizeEx(m_file, &fileSizeInfo);
    m_fileSize = (uint64_t)fileSizeInfo.QuadPart;

    uint8_t magic[4] = {};
    if (m_fileSize < 12 || !ReadAt(0, magic, 4) || memcmp(magic, "MUMX", 4) != 0)
    {
        error = L"not a MUMX container (bad magic or truncated file)";
        return false;
    }

    if (!ReadU32BE(4, m_version) || !ReadU32BE(8, m_sectionCount))
    {
        error = L"failed to read container header";
        return false;
    }
    if (m_version < 1 || m_version > (uint32_t)kMumXVersionMax)
    {
        error = L"unsupported MUMX version (expected 1 or 2)";
        return false;
    }

    uint64_t cursor = 12;
    if (m_version >= 2)
    {
        uint32_t contentHashSize = 0;
        if (!ReadU32BE(cursor, m_hashAlgId))        { error = L"truncated header"; return false; }
        if (!ReadU32BE(cursor + 4, contentHashSize)) { error = L"truncated header"; return false; }
        cursor += 8;
        if (cursor + contentHashSize + 8 > m_fileSize) { error = L"truncated header"; return false; }
        m_contentHash.resize(contentHashSize);
        if (contentHashSize && !ReadAt(cursor, m_contentHash.data(), contentHashSize))
        {
            error = L"failed to read content hash";
            return false;
        }
        cursor += contentHashSize;
        if (!ReadU32BE(cursor, m_defaultCompressionAlg)) { error = L"truncated header"; return false; }
        if (!ReadU32BE(cursor + 4, m_defaultBlockSize))   { error = L"truncated header"; return false; }
        cursor += 8;
    }

    m_sections.clear();
    m_sections.reserve(m_sectionCount);
    for (uint32_t i = 0; i < m_sectionCount; ++i)
    {
        if (cursor + kDataPoolHeaderV1 > m_fileSize) { error = L"section header out of range"; return false; }

        MumxSection section{};
        section.headerOffset = cursor;
        uint32_t rawVersion = 0;
        if (!ReadU32BE(cursor, rawVersion))                { error = L"failed to read section"; return false; }
        section.headerVersion = (int32_t)rawVersion;
        if (!ReadI64BE(cursor + 4,  section.sectionType))  { error = L"failed to read section"; return false; }
        if (!ReadI64BE(cursor + 12, section.payloadSize))  { error = L"failed to read section"; return false; }
        if (!ReadI64BE(cursor + 20, section.poolOffset))   { error = L"failed to read section"; return false; }
        section.headerLength = kDataPoolHeaderV1;

        if (section.headerVersion >= 2)
        {
            if (cursor + kDataPoolHeaderV2 > m_fileSize) { error = L"section header out of range"; return false; }
            if (!ReadU32BE(cursor + 28, section.compressionAlg))  { error = L"failed to read section"; return false; }
            if (!ReadU32BE(cursor + 32, section.blockSize))        { error = L"failed to read section"; return false; }
            if (!ReadI64BE(cursor + 36, section.uncompressedSize)) { error = L"failed to read section"; return false; }
            if (!ReadI64BE(cursor + 44, section.blockIndexSize))   { error = L"failed to read section"; return false; }
            section.headerLength = kDataPoolHeaderV2;
        }
        section.payloadOffset = section.headerOffset + section.headerLength;

        if (section.payloadSize < 0 ||
            section.payloadOffset + (uint64_t)section.payloadSize > m_fileSize)
        {
            error = L"section payload out of range";
            return false;
        }

        m_sections.push_back(section);

        cursor = section.payloadOffset + (uint64_t)section.payloadSize;
    }

    return true;
}

static bool ParseBlockIndex(const std::vector<uint8_t>& index,
                            uint32_t& blockSize, uint64_t& blockCount,
                            uint32_t& lastUncompressed, uint32_t& lastCompressed,
                            std::vector<std::pair<uint64_t, uint64_t>>& blocks,
                            std::wstring& error)
{
    if (index.size() < kBlockIndexHeaderSize) { error = L"block index too short"; return false; }

    auto readU32 = [&](size_t offset) -> uint32_t {
        return ((uint32_t)index[offset] << 24) | ((uint32_t)index[offset + 1] << 16) |
               ((uint32_t)index[offset + 2] << 8) | (uint32_t)index[offset + 3];
    };
    auto readI64 = [&](size_t offset) -> int64_t {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | index[offset + i];
        return (int64_t)v;
    };

    uint32_t indexVersion = readU32(0);
    if (indexVersion != 1) { error = L"unsupported block index version"; return false; }

    blockSize        = readU32(4);
    blockCount       = (uint64_t)readI64(8);
    lastUncompressed = readU32(16);
    lastCompressed   = readU32(20);

    if (blockCount == 0 || blockCount > (1u << 24)) { error = L"invalid block count"; return false; }

    size_t cursor = kBlockIndexHeaderSize;
    blocks.clear();
    blocks.reserve((size_t)blockCount);
    for (uint64_t i = 0; i < blockCount; ++i)
    {
        uint64_t values[2] = {};
        for (int k = 0; k < 2; ++k)
        {
            uint64_t value = 0;
            int shift = 0;
            for (;;)
            {
                if (cursor >= index.size()) { error = L"block index varint out of range"; return false; }
                uint8_t byte = index[cursor++];
                value |= (uint64_t)(byte & 0x7F) << shift;
                if ((byte & 0x80) == 0) break;
                shift += 7;
                if (shift > 63) { error = L"block index varint too long"; return false; }
            }
            values[k] = value;
        }
        blocks.emplace_back(values[0], values[1]);
    }
    return true;
}

bool MumxContainerFile::DecompressStream(uint64_t streamOffset, uint64_t streamSize,
                                         uint32_t compressionAlg, uint32_t blockSize,
                                         uint64_t uncompressedSize, uint64_t blockIndexSize,
                                         std::vector<uint8_t>& output, std::wstring& error)
{
    if (!EnsureDecompressorLoaded()) { error = L"RtlDecompressBufferEx is unavailable"; return false; }
    if (blockIndexSize == 0 || blockIndexSize > streamSize)
    {
        error = L"invalid block index size";
        return false;
    }

    std::vector<uint8_t> index((size_t)blockIndexSize);
    if (!ReadAt(streamOffset + streamSize - blockIndexSize, index.data(), index.size()))
    {
        error = L"failed to read block index";
        return false;
    }

    uint32_t indexBlockSize = 0, lastUncompressed = 0, lastCompressed = 0;
    uint64_t blockCount = 0;
    std::vector<std::pair<uint64_t, uint64_t>> blocks;
    if (!ParseBlockIndex(index, indexBlockSize, blockCount, lastUncompressed, lastCompressed,
                         blocks, error))
        return false;

    USHORT format = ToRtlCompressionFormat(compressionAlg);
    if (!format) { error = L"unsupported compression algorithm"; return false; }

    ULONG compressWorkspace = 0, fragmentWorkspace = 0;
    if (g_rtlGetWorkspaceSize &&
        g_rtlGetWorkspaceSize(format, &compressWorkspace, &fragmentWorkspace) == 0 &&
        fragmentWorkspace)
    {
    }

    std::vector<uint8_t> stream((size_t)streamSize);
    if (!ReadAt(streamOffset, stream.data(), stream.size())) { error = L"failed to read compressed stream"; return false; }

    output.assign((size_t)uncompressedSize, 0);

    unsigned threadCount = std::thread::hardware_concurrency();
    if (threadCount == 0) threadCount = 1;
    if (threadCount > 32) threadCount = 32;
    if ((uint64_t)threadCount > blockCount) threadCount = (unsigned)blockCount;

    std::atomic<uint64_t> nextBlock{ 0 };
    std::atomic<bool>     aborted{ false };
    std::wstring          firstError;
    std::mutex            errorMutex;

    auto worker = [&]()
    {
        std::vector<uint8_t> workspace;
        if (fragmentWorkspace) workspace.resize(fragmentWorkspace);

        for (;;)
        {
            uint64_t i = nextBlock.fetch_add(1);
            if (i >= blockCount || aborted.load()) return;

            uint64_t uncompressedOffset = blocks[(size_t)i].first;
            uint64_t compressedOffset   = blocks[(size_t)i].second;
            uint64_t compressedSize   = (i + 1 < blockCount)
                                            ? blocks[(size_t)(i + 1)].second - compressedOffset
                                            : lastCompressed;
            uint64_t uncompressedChunk = (i + 1 < blockCount)
                                             ? blocks[(size_t)(i + 1)].first - uncompressedOffset
                                             : lastUncompressed;

            std::wstring localError;
            bool ok = true;
            if (compressedSize == 0 || uncompressedChunk == 0)
            {
                localError = L"block has zero length";
                ok = false;
            }
            else if (blockSize && uncompressedChunk > blockSize)
            {
                localError = L"block exceeds block size";
                ok = false;
            }
            else if (compressedOffset + compressedSize > streamSize)
            {
                localError = L"block out of range";
                ok = false;
            }
            else if (uncompressedOffset + uncompressedChunk > output.size())
            {
                localError = L"decompressed size out of range";
                ok = false;
            }

            if (ok)
            {
                const uint8_t* compressed = stream.data() + compressedOffset;
                uint8_t* target = output.data() + uncompressedOffset;

                if (compressedSize >= uncompressedChunk)
                {
                    memcpy(target, compressed, (size_t)uncompressedChunk);
                }
                else
                {
                    ULONG finalSize = 0;
                    LONG status = g_rtlDecompressBufferEx(
                        format,
                        target,
                        (ULONG)uncompressedChunk,
                        const_cast<uint8_t*>(compressed),
                        (ULONG)compressedSize,
                        &finalSize,
                        workspace.empty() ? nullptr : workspace.data());
                    if (status != 0)
                    {
                        localError = std::format(
                            L"block {} decompression failed (NTSTATUS 0x{:08X})", i, (unsigned)status);
                        ok = false;
                    }
                }
            }

            if (!ok)
            {
                std::lock_guard<std::mutex> guard(errorMutex);
                if (firstError.empty()) firstError = localError;
                aborted.store(true);
                return;
            }
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(threadCount > 0 ? threadCount - 1 : 0);
    for (unsigned t = 1; t < threadCount; ++t) workers.emplace_back(worker);
    worker();
    for (std::thread& thread : workers) thread.join();

    if (aborted.load())
    {
        error = firstError.empty() ? L"decompression failed" : firstError;
        return false;
    }
    return true;
}

bool MumxContainerFile::ReadSectionData(size_t sectionIndex, std::vector<uint8_t>& output,
                                        std::wstring& error)
{
    if (sectionIndex >= m_sections.size()) { error = L"section index out of range"; return false; }
    MumxSection section = m_sections[sectionIndex];

    uint64_t payloadOffset    = section.payloadOffset;
    uint64_t payloadSize      = (uint64_t)section.payloadSize;
    uint32_t compressionAlg  = section.compressionAlg;
    uint32_t blockSize        = section.blockSize;
    uint64_t uncompressedSize = (uint64_t)section.uncompressedSize;
    uint64_t blockIndexSize   = (uint64_t)section.blockIndexSize;

    if (section.HasInnerHeader() && payloadSize >= kDataPoolHeaderV2)
    {
        uint32_t innerVersion = 0;
        if (ReadU32BE(payloadOffset, innerVersion) && innerVersion >= 1 && innerVersion <= 2)
        {
            int64_t innerSize = 0, innerUncompressed = 0, innerIndexSize = 0;
            uint32_t innerAlg = 0, innerBlockSize = 0;
            if (ReadI64BE(payloadOffset + 12, innerSize) &&
                ReadU32BE(payloadOffset + 28, innerAlg) &&
                ReadU32BE(payloadOffset + 32, innerBlockSize) &&
                ReadI64BE(payloadOffset + 36, innerUncompressed) &&
                ReadI64BE(payloadOffset + 44, innerIndexSize))
            {
                payloadOffset   += innerVersion >= 2 ? kDataPoolHeaderV2 : kDataPoolHeaderV1;
                payloadSize      = (uint64_t)innerSize;
                compressionAlg  = innerAlg;
                blockSize        = innerBlockSize;
                uncompressedSize = (uint64_t)innerUncompressed;
                blockIndexSize   = (uint64_t)innerIndexSize;
            }
        }
    }

    if (payloadOffset + payloadSize > m_fileSize) { error = L"section payload out of range"; return false; }

    if (compressionAlg == kCompressionNone)
    {
        output.resize((size_t)payloadSize);
        if (payloadSize && !ReadAt(payloadOffset, output.data(), output.size()))
        {
            error = L"failed to read section data";
            return false;
        }
        return true;
    }

    return DecompressStream(payloadOffset, payloadSize, compressionAlg, blockSize,
                            uncompressedSize, blockIndexSize, output, error);
}

bool MumxContainerFile::ReadBytes(uint64_t offset, void* buffer, size_t byteCount)
{
    return ReadAt(offset, buffer, byteCount);
}

bool MumxContainerFile::ReadPoolAt(uint64_t headerOffset, std::vector<uint8_t>& output,
                                   std::wstring& error)
{
    if (headerOffset + kDataPoolHeaderV2 > m_fileSize) { error = L"inner pool header out of range"; return false; }

    uint32_t headerVersion = 0, compressionAlg = 0, blockSize = 0;
    int64_t payloadSize = 0, uncompressedSize = 0, blockIndexSize = 0;
    if (!ReadU32BE(headerOffset, headerVersion) ||
        !ReadI64BE(headerOffset + 12, payloadSize) ||
        !ReadU32BE(headerOffset + 28, compressionAlg) ||
        !ReadU32BE(headerOffset + 32, blockSize) ||
        !ReadI64BE(headerOffset + 36, uncompressedSize) ||
        !ReadI64BE(headerOffset + 44, blockIndexSize))
    {
        error = L"failed to read inner pool header";
        return false;
    }
    if (headerVersion < 1 || headerVersion > 2) { error = L"invalid inner pool header version"; return false; }

    uint64_t payloadOffset = headerOffset + (headerVersion >= 2 ? kDataPoolHeaderV2 : kDataPoolHeaderV1);
    if (payloadSize < 0 || payloadOffset + (uint64_t)payloadSize > m_fileSize)
    {
        error = L"inner pool payload out of range";
        return false;
    }

    if (compressionAlg == kCompressionNone)
    {
        output.resize((size_t)payloadSize);
        if (payloadSize && !ReadAt(payloadOffset, output.data(), output.size()))
        {
            error = L"failed to read inner pool data";
            return false;
        }
        return true;
    }

    return DecompressStream(payloadOffset, (uint64_t)payloadSize, compressionAlg, blockSize,
                            (uint64_t)uncompressedSize, (uint64_t)blockIndexSize, output, error);
}
