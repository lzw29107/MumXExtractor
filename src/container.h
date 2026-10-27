#pragma once

#include "common.h"

struct MumxSection
{
    int32_t  headerVersion;
    int64_t  sectionType;
    int64_t  payloadSize;
    int64_t  poolOffset;
    uint32_t compressionAlg;
    uint32_t blockSize;
    int64_t  uncompressedSize;
    int64_t  blockIndexSize;
    uint64_t headerOffset;
    uint32_t headerLength;
    uint64_t payloadOffset;

    bool IsCompressed() const { return compressionAlg != kCompressionNone; }
    bool HasInnerHeader() const
    {
        return sectionType == kSectionEntries || sectionType == kSectionEntriesAlt ||
               sectionType == kSectionSecondaryDataPool || sectionType == kSectionSecondaryDataPoolAlt;
    }
};

class MumxContainerFile
{
public:
    ~MumxContainerFile() { Close(); }

    bool Open(const std::wstring& containerPath, std::wstring& error);
    void Close();

    uint64_t FileSize() const { return m_fileSize; }
    uint32_t Version() const { return m_version; }
    uint32_t SectionCount() const { return m_sectionCount; }
    uint32_t HashAlgId() const { return m_hashAlgId; }
    const std::vector<uint8_t>& ContentHash() const { return m_contentHash; }
    uint32_t DefaultCompressionAlg() const { return m_defaultCompressionAlg; }
    uint32_t DefaultBlockSize() const { return m_defaultBlockSize; }
    const std::vector<MumxSection>& Sections() const { return m_sections; }

    bool ReadSectionData(size_t sectionIndex, std::vector<uint8_t>& output, std::wstring& error);

    bool ReadPoolAt(uint64_t headerOffset, std::vector<uint8_t>& output, std::wstring& error);

    bool ReadBytes(uint64_t offset, void* buffer, size_t byteCount);
    bool ReadU32BE(uint64_t offset, uint32_t& value);
    bool ReadI64BE(uint64_t offset, int64_t& value);

private:
    bool ReadAt(uint64_t offset, void* buffer, size_t byteCount);
    bool DecompressStream(uint64_t streamOffset, uint64_t streamSize, uint32_t compressionAlg,
                          uint32_t blockSize, uint64_t uncompressedSize, uint64_t blockIndexSize,
                          std::vector<uint8_t>& output, std::wstring& error);

    HANDLE                   m_file = INVALID_HANDLE_VALUE;
    uint64_t                 m_fileSize = 0;
    uint32_t                 m_version = 0;
    uint32_t                 m_sectionCount = 0;
    uint32_t                 m_hashAlgId = 0;
    std::vector<uint8_t>     m_contentHash;
    uint32_t                 m_defaultCompressionAlg = 0;
    uint32_t                 m_defaultBlockSize = 0;
    std::vector<MumxSection> m_sections;
};
