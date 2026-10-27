#pragma once

#include "common.h"
#include "container.h"
#include "keyform.h"
#include "console.h"

// Index section (type 5): a named table of key -> value, where the value type
// selects the encoding of the value column.
struct MumxIndexEntry
{
    std::string key;
    int64_t     value;
};

struct MumxIndex
{
    std::string                 name;
    uint32_t                    keyComparer = 0;   // 0 = ordinal, 1 = ordinal ignore case
    uint32_t                    valueType = 0;     // 0 string, 1 boolean, 2 number, 3..6 arrays/objects
    std::vector<MumxIndexEntry> entries;           // ordered by keyComparer
    std::vector<uint8_t>        pool;              // decompressed pool the keys/values index into
};

// Header of one index section, without decompressing its pool.
struct MumxIndexSectionInfo
{
    size_t      sectionIndex = 0;
    std::string name;
    uint32_t    keyComparer = 0;
    uint32_t    valueType = 0;
    uint64_t    entryCount = 0;
    uint64_t    poolOffset = 0;    // file offset of the inner data pool header
    uint64_t    tableOffset = 0;   // file offset of the entry table
    uint64_t    tableSize = 0;
};

bool ReadIndexSections(MumxContainerFile& container,
                       std::vector<MumxIndexSectionInfo>& sections, std::wstring& error);

class MumxNativeReader
{
public:
    struct EntryRef
    {
        size_t   sectionIndex;
        uint64_t entryId;
        uint64_t offset;
    };

    bool Open(const std::wstring& containerPath, std::wstring& error);

    const std::vector<EntryRef>& Entries() const { return m_entries; }

    bool IsAsn1Section(size_t sectionIndex) const
    {
        return sectionIndex < m_entrySections.size() && m_entrySections[sectionIndex].isAsn1;
    }

    const std::vector<std::string>& BlobNames() const { return m_blobNames; }
    const std::vector<uint64_t>& BlobNameOffsets() const { return m_blobNameOffsets; }

    bool ReadEntryHeader(const EntryRef& entry, uint64_t& totalSize, std::string& hash,
                         uint64_t& flags, std::wstring& error) const;
    const std::string* NameAtOffset(uint64_t offset) const
    {
        auto it = m_nameByOffset.find(offset);
        return it == m_nameByOffset.end() ? nullptr : &it->second;
    }

    bool Rehydrate(const EntryRef& entry, std::vector<uint8_t>& output, std::wstring& error);

    bool ReadIdentity(const EntryRef& entry, MumxIdentity& identity, std::wstring& error);

    bool Analyze(const EntryRef& entry, MumxIdentity& identity, std::vector<uint8_t>& output,
                 std::wstring& error);

    // Builds every index section on first call; the result is cached.
    bool BuildIndexes(std::wstring& error);
    const std::vector<MumxIndex>& Indexes() const { return m_indexes; }

    bool ResolveIndexValue(const MumxIndex& index, int64_t value, std::string& text) const;

private:
    struct Node
    {
        std::string tag;
        std::vector<std::pair<std::string, std::string>> attrs;
        std::vector<Node> children;
        uint64_t size = 0;
        uint64_t hashPrefix = 0;
        uint64_t declarationId = 0;
        uint64_t flags = 0;
        bool     hasHash = false;
        std::string hash;
    };

    struct Asn1Node
    {
        uint64_t bufferId = 0;
        uint64_t size = 0;
        uint64_t size1 = 0;
        uint64_t size2 = 0;
        uint64_t flags = 0;
        std::string hash;
        std::vector<Asn1Node> children;
    };

    bool ReadString(uint64_t offset, std::string& value) const;
    bool ParseNode(const std::vector<uint8_t>& pool, size_t& cursor, Node& node,
                   int depth, uint64_t* budget) const;
    void SerializeNode(const Node& node, std::string& out, int depth) const;
    void SerializeNodeEx(const Node& node, std::string& out, int depth, bool inlineMode) const;
    static bool SubtreeHasText(const Node& node);
    bool CollectIdentity(const Node& node, MumxIdentity& identity) const;

    bool ParseAsn1Node(const std::vector<uint8_t>& pool, size_t& cursor, Asn1Node& node,
                       int depth, uint64_t* budget) const;
    static void RehydrateAsn1(const Asn1Node& node, const std::vector<uint8_t>& dataPool,
                              std::vector<uint8_t>& output);
    bool RehydrateAsn1Entry(const EntryRef& entry, std::vector<uint8_t>& output,
                            std::wstring& error);
    bool ReadAsn1Root(const EntryRef& entry, Asn1Node& root, std::wstring& error) const;
    bool ReadAsn1Header(const EntryRef& entry, uint64_t& totalSize, std::string& hash,
                        uint64_t& flags, std::wstring& error) const;

    MumxContainerFile        m_container;
    std::vector<uint8_t>     m_stringPool;   // section 0
    struct EntrySection
    {
        std::vector<uint8_t>                      trees;
        std::vector<std::pair<uint64_t, uint64_t>> table;
        bool                                      isAsn1 = false;  // section type 2
    };
    std::vector<EntrySection> m_entrySections;
    std::vector<bool>         m_sectionIsTree;
    std::vector<EntryRef>     m_entries;
    std::vector<std::string>  m_blobNames;
    std::vector<uint64_t>     m_blobNameOffsets;
    std::unordered_map<uint64_t, std::string> m_nameByOffset;
    std::vector<MumxIndex>    m_indexes;
    bool                      m_indexesBuilt = false;
};
