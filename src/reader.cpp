#include "reader.h"

bool MumxNativeReader::ReadString(uint64_t offset, std::string& value) const
{
    if (offset >= m_stringPool.size()) return false;
    size_t cursor = (size_t)offset;
    uint64_t length = 0;
    if (!ReadLeb128(m_stringPool, cursor, length)) return false;
    if (cursor + length > m_stringPool.size()) return false;
    value.assign((const char*)m_stringPool.data() + cursor, (size_t)length);
    return true;
}

bool MumxNativeReader::Open(const std::wstring& containerPath, std::wstring& error)
{
    const double openStart = NowMs();
    if (!m_container.Open(containerPath, error)) return false;

    m_indexes.clear();
    m_indexesBuilt = false;

    const std::vector<MumxSection>& sections = m_container.Sections();

    bool haveStrings = false;
    for (size_t i = 0; i < sections.size(); ++i)
    {
        if (sections[i].sectionType != kSectionDataPool) continue;
        if (!m_container.ReadSectionData(i, m_stringPool, error)) return false;
        haveStrings = true;
        break;
    }
    if (!haveStrings) { error = L"primary data pool (section 0) not found"; return false; }
    if (g_verbose) Log(L"  [t] data pool: {:.0f} ms", NowMs() - openStart);

    for (size_t i = 0; i < sections.size(); ++i)
    {
        const MumxSection& section = sections[i];
        if (section.sectionType != kSectionEntries && section.sectionType != kSectionEntriesAlt)
            continue;

        EntrySection entrySection;
        std::wstring poolError;

        if (!m_container.ReadPoolAt(section.payloadOffset, entrySection.trees, poolError))
        {
            error = L"failed to decompress entry tree pool: " + poolError;
            return false;
        }

        uint64_t headerAOffset = section.payloadOffset;
        uint32_t versionA = 0;
        int64_t sizeA = 0;
        m_container.ReadU32BE(headerAOffset, versionA);
        m_container.ReadI64BE(headerAOffset + 12, sizeA);
        uint64_t headerBOffset = headerAOffset +
                                 (versionA >= 2 ? kDataPoolHeaderV2 : kDataPoolHeaderV1) +
                                 (uint64_t)sizeA;

        std::vector<uint8_t> tablePool;
        if (!m_container.ReadPoolAt(headerBOffset, tablePool, poolError))
        {
            error = L"failed to decompress entry table pool: " + poolError;
            return false;
        }

        size_t cursor = 0;
        while (cursor < tablePool.size())
        {
            uint64_t entryId = 0, offset = 0;
            if (!ReadLeb128(tablePool, cursor, entryId)) break;
            if (!ReadLeb128(tablePool, cursor, offset)) break;
            entrySection.table.emplace_back(entryId, offset);
            m_entries.push_back(EntryRef{ m_entrySections.size(), entryId, offset });
        }
        entrySection.isAsn1 = (section.sectionType == kSectionEntriesAlt);
        m_entrySections.push_back(std::move(entrySection));
        m_sectionIsTree.push_back(true);
    }

    if (m_entries.empty()) { error = L"no entries found"; return false; }
    if (g_verbose) Log(L"  [t] entry sections: {:.0f} ms", NowMs() - openStart);

    for (size_t i = 0; i < sections.size(); ++i)
    {
        uint64_t type = sections[i].sectionType;
        if (type != kSectionSecondaryDataPool && type != kSectionSecondaryDataPoolAlt) continue;

        std::vector<uint8_t> pool;
        std::wstring poolError;
        if (!m_container.ReadSectionData(i, pool, poolError)) continue;

        std::vector<std::string> names;
        std::vector<uint64_t> nameOffsets;
        std::unordered_map<uint64_t, std::string> byOffset;
        size_t cursor = 0;
        bool wellFormed = !pool.empty();
        while (wellFormed && cursor < pool.size())
        {
            uint64_t recordOffset = (uint64_t)cursor;
            uint64_t length = 0;
            if (!ReadLeb128(pool, cursor, length) || length == 0) { wellFormed = false; break; }
            if (length > 4096 || cursor + length >= pool.size()) { wellFormed = false; break; }
            std::string name((const char*)pool.data() + cursor, (size_t)length);
            cursor += (size_t)length;
            if (pool[cursor] != 0) { wellFormed = false; break; }
            ++cursor;
            byOffset.emplace(recordOffset, name);
            nameOffsets.push_back(recordOffset);
            names.push_back(std::move(name));
        }
        if (wellFormed && names.size() > 1)
        {
            m_blobNames.swap(names);
            m_blobNameOffsets.swap(nameOffsets);
            m_nameByOffset.swap(byOffset);
            break;
        }
    }

    if (g_verbose) Log(L"  [t] name table: {:.0f} ms", NowMs() - openStart);
    for (size_t s = 0; s < m_entrySections.size(); ++s)
    {
        if (m_entrySections[s].isAsn1) { m_sectionIsTree[s] = false; continue; }
        for (const EntryRef& entry : m_entries)
        {
            if (entry.sectionIndex != s) continue;
            MumxIdentity probe;
            std::wstring probeError;
            m_sectionIsTree[s] = ReadIdentity(entry, probe, probeError);
            break;
        }
    }
    return true;
}

bool MumxNativeReader::ParseNode(const std::vector<uint8_t>& pool, size_t& cursor, Node& node,
                                 int depth, uint64_t* budget) const
{
    if (depth > 512) return false;
    if (budget && (*budget)-- == 0) return false;

    uint64_t ref = 0;
    if (!ReadLeb128(pool, cursor, ref)) return false;
    if (ref != 0)
    {
        if (ref >= pool.size()) return false;
        size_t subCursor = (size_t)ref;
        return ParseNode(pool, subCursor, node, depth + 1, budget);
    }

    uint64_t flags = 0;
    if (!ReadLeb128(pool, cursor, flags)) return false;
    node.flags = flags;
    if (flags & 2)
    {
        uint64_t ignored = 0, hashLength = 0, size1 = 0, size2 = 0;
        if (!ReadLeb128(pool, cursor, ignored)) return false;
        node.declarationId = ignored;
        if (!ReadLeb128(pool, cursor, hashLength)) return false;
        if (cursor + hashLength > pool.size()) return false;
        if (hashLength >= 8)
        {
            node.hasHash = true;
            node.hashPrefix = 0;
            for (int i = 0; i < 8; ++i) node.hashPrefix = (node.hashPrefix << 8) | pool[cursor + i];
        }
        node.hash.assign((const char*)pool.data() + cursor, (size_t)hashLength);
        cursor += (size_t)hashLength;
        if (!ReadLeb128(pool, cursor, size1)) return false;
        if (!ReadLeb128(pool, cursor, size2)) return false;
        node.size = size2;
        if (flags & 0x40)
        {
            uint64_t blockLength = 0;
            if (!ReadLeb128(pool, cursor, blockLength)) return false;
            if (cursor + blockLength > pool.size()) return false;
            cursor += (size_t)blockLength;
        }
    }

    uint64_t tagId = 0, attrCount = 0, childCount = 0;
    if (!ReadLeb128(pool, cursor, tagId)) return false;
    if (!ReadString(tagId, node.tag)) return false;

    if (!ReadLeb128(pool, cursor, attrCount)) return false;
    if (attrCount > 4096) return false;
    for (uint64_t i = 0; i < attrCount; ++i)
    {
        uint64_t nameId = 0, valueId = 0;
        if (!ReadLeb128(pool, cursor, nameId)) return false;
        if (!ReadLeb128(pool, cursor, valueId)) return false;
        std::string name, value;
        if (!ReadString(nameId, name) || !ReadString(valueId, value)) return false;
        node.attrs.emplace_back(std::move(name), std::move(value));
    }

    if (!ReadLeb128(pool, cursor, childCount)) return false;
    if (childCount > 4000000) return false;
    node.children.reserve((size_t)childCount);
    for (uint64_t i = 0; i < childCount; ++i)
    {
        Node child;
        if (!ParseNode(pool, cursor, child, depth + 1, budget)) return false;
        node.children.push_back(std::move(child));
    }
    return true;
}

static void AppendEscaped(std::string& out, const std::string& text, bool attribute)
{
    char previous = 0;
    for (char ch : text)
    {
        switch (ch)
        {
        case '&':  out += "&amp;";  break;
        case '<':  out += "&lt;";   break;
        case '>':  out += "&gt;";   break;
        case '"':  out += attribute ? "&quot;" : "\""; break;
        case '\r': if (attribute) out += "&#xD;"; else out += ch; break;
        case '\n':
            if (attribute) out += "&#xA;";
            else { if (previous != '\r') out += '\r'; out += '\n'; }
            break;
        case '\t': if (attribute) out += "&#x9;"; else out += ch; break;
        default:   out += ch;       break;
        }
        previous = ch;
    }
}

bool MumxNativeReader::SubtreeHasText(const Node& node)
{
    for (const auto& child : node.children)
        if (child.tag.empty()) return true;
    return false;
}

void MumxNativeReader::SerializeNode(const Node& node, std::string& out, int depth) const
{
    SerializeNodeEx(node, out, depth, false);
}

void MumxNativeReader::SerializeNodeEx(const Node& node, std::string& out, int depth,
                                       bool inlineMode) const
{
    if (node.tag.rfind("<?xml", 0) == 0)
    {
        out += node.tag;
        out += "\r\n";
        for (const Node& child : node.children) SerializeNodeEx(child, out, 0, false);
        return;
    }

    if (node.tag.empty())
    {
        if (!node.attrs.empty()) AppendEscaped(out, node.attrs[0].second, false);
        return;
    }

    const bool parentFormatted = !inlineMode;
    const bool hasText = SubtreeHasText(node);
    const std::string indent = inlineMode ? std::string() : std::string((size_t)depth * 2, ' ');

    out += indent;
    out += '<';
    out += node.tag;
    for (const auto& attr : node.attrs)
    {
        out += ' ';
        out += attr.first;
        out += "=\"";
        AppendEscaped(out, attr.second, true);
        out += '"';
    }

    if (node.children.empty())
    {
        if ((node.flags & 0x10) != 0) { out += "></"; out += node.tag; out += '>'; }
        else                          { out += " />"; }
        if (parentFormatted) out += "\r\n";
        return;
    }

    out += '>';
    const bool breakLines = parentFormatted && !hasText;
    for (const Node& child : node.children)
    {
        if (breakLines)
        {
            bool atLineStart = out.size() >= 2 && out.compare(out.size() - 2, 2, "\r\n") == 0;
            if (!atLineStart) out += "\r\n";
        }
        SerializeNodeEx(child, out, depth + 1, inlineMode || hasText);
    }

    if (breakLines)
    {
        bool atLineStart = out.size() >= 2 && out.compare(out.size() - 2, 2, "\r\n") == 0;
        if (!atLineStart) out += "\r\n";
        out += indent;
    }
    out += "</";
    out += node.tag;
    out += '>';
    if (parentFormatted) out += "\r\n";
}

bool MumxNativeReader::ParseAsn1Node(const std::vector<uint8_t>& pool, size_t& cursor,
                                     Asn1Node& node, int depth, uint64_t* budget) const
{
    if (depth > 512) return false;
    if (budget && (*budget)-- == 0) return false;

    uint64_t ref = 0;
    if (!ReadLeb128(pool, cursor, ref)) return false;
    if (ref != 0)
    {
        if (ref >= pool.size()) return false;
        size_t subCursor = (size_t)ref;
        return ParseAsn1Node(pool, subCursor, node, depth + 1, budget);
    }

    uint64_t flags = 0;
    if (!ReadLeb128(pool, cursor, flags)) return false;
    node.flags = flags;

    if (flags & 2)
    {
        uint64_t ignored = 0, hashLength = 0;
        if (!ReadLeb128(pool, cursor, ignored)) return false;
        if (!ReadLeb128(pool, cursor, hashLength)) return false;
        if (hashLength > 64 || cursor + hashLength > pool.size()) return false;
        node.hash.assign((const char*)pool.data() + cursor, (size_t)hashLength);
        cursor += (size_t)hashLength;
        if (!ReadLeb128(pool, cursor, node.size1)) return false;
        if (!ReadLeb128(pool, cursor, node.size2)) return false;
    }

    if (!ReadLeb128(pool, cursor, node.bufferId)) return false;
    if (!ReadLeb128(pool, cursor, node.size)) return false;

    if (flags & 8)
    {
        uint64_t childCount = 0;
        if (!ReadLeb128(pool, cursor, childCount)) return false;
        if (childCount > 1000000) return false;
        node.children.resize((size_t)childCount);
        for (uint64_t i = 0; i < childCount; ++i)
        {
            if (!ParseAsn1Node(pool, cursor, node.children[(size_t)i], depth + 1, budget))
                return false;
        }
    }
    return true;
}

void MumxNativeReader::RehydrateAsn1(const Asn1Node& node, const std::vector<uint8_t>& dataPool,
                                     std::vector<uint8_t>& output)
{
    uint64_t begin = node.bufferId;
    uint64_t end = begin + node.size;
    if (begin <= dataPool.size() && end <= dataPool.size() && node.size != 0)
    {
        output.insert(output.end(),
                      dataPool.begin() + (std::ptrdiff_t)begin,
                      dataPool.begin() + (std::ptrdiff_t)end);
    }
    for (const Asn1Node& child : node.children) RehydrateAsn1(child, dataPool, output);
}

bool MumxNativeReader::ReadAsn1Root(const EntryRef& entry, Asn1Node& root,
                                    std::wstring& error) const
{
    if (entry.sectionIndex >= m_entrySections.size()) { error = L"entry index out of range"; return false; }
    const std::vector<uint8_t>& pool = m_entrySections[entry.sectionIndex].trees;
    if (entry.offset >= pool.size()) { error = L"entry offset out of range"; return false; }

    size_t cursor = (size_t)entry.offset;
    uint64_t budget = 4000000;
    if (!ParseAsn1Node(pool, cursor, root, 0, &budget)) { error = L"failed to parse ASN.1 node"; return false; }
    return true;
}

bool MumxNativeReader::ReadEntryHeader(const EntryRef& entry, uint64_t& totalSize,
                                       std::string& hash, uint64_t& flags,
                                       std::wstring& error) const
{
    return ReadAsn1Header(entry, totalSize, hash, flags, error);
}

bool MumxNativeReader::ReadAsn1Header(const EntryRef& entry, uint64_t& totalSize,
                                      std::string& hash, uint64_t& flags,
                                      std::wstring& error) const
{
    if (entry.sectionIndex >= m_entrySections.size()) { error = L"entry index out of range"; return false; }
    const std::vector<uint8_t>& pool = m_entrySections[entry.sectionIndex].trees;
    if (entry.offset >= pool.size()) { error = L"entry offset out of range"; return false; }

    size_t cursor = (size_t)entry.offset;
    uint64_t ref = 0;
    if (!ReadLeb128(pool, cursor, ref)) { error = L"failed to read ref"; return false; }
    if (ref != 0)
    {
        if (ref >= pool.size()) { error = L"ref out of range"; return false; }
        cursor = (size_t)ref;
        if (!ReadLeb128(pool, cursor, ref)) { error = L"failed to read ref"; return false; }
        if (ref != 0) { error = L"ref nesting too deep"; return false; }
    }

    if (!ReadLeb128(pool, cursor, flags)) { error = L"failed to read flags"; return false; }
    totalSize = 0;
    hash.clear();
    if (flags & 2)
    {
        uint64_t ignored = 0, hashLength = 0;
        if (!ReadLeb128(pool, cursor, ignored)) { error = L"failed to read x"; return false; }
        if (!ReadLeb128(pool, cursor, hashLength)) { error = L"failed to read hash length"; return false; }
        if (hashLength > 64 || cursor + hashLength > pool.size()) { error = L"hash out of range"; return false; }
        hash.assign((const char*)pool.data() + cursor, (size_t)hashLength);
        cursor += (size_t)hashLength;
        uint64_t size1 = 0;
        if (!ReadLeb128(pool, cursor, size1)) { error = L"failed to read s1"; return false; }
        if (!ReadLeb128(pool, cursor, totalSize)) { error = L"failed to read s2"; return false; }
    }
    return true;
}

bool MumxNativeReader::RehydrateAsn1Entry(const EntryRef& entry, std::vector<uint8_t>& output,
                                          std::wstring& error)
{
    Asn1Node root;
    if (!ReadAsn1Root(entry, root, error)) return false;

    output.clear();
    if (root.size2 != 0 && root.size2 < (uint64_t)1 << 32) output.reserve((size_t)root.size2);
    RehydrateAsn1(root, m_stringPool, output);
    return true;
}

bool MumxNativeReader::Analyze(const EntryRef& entry, MumxIdentity& identity,
                               std::vector<uint8_t>& output, std::wstring& error)
{
    if (entry.sectionIndex >= m_entrySections.size()) { error = L"entry index out of range"; return false; }

    if (IsAsn1Section(entry.sectionIndex))
    {
        Asn1Node asn1Root;
        if (!ReadAsn1Root(entry, asn1Root, error)) return false;
        identity = MumxIdentity{};
        identity.size = asn1Root.size2;
        identity.hash = asn1Root.hash;
        identity.flags = asn1Root.flags;
        identity.valid = false;
        output.clear();
        RehydrateAsn1(asn1Root, m_stringPool, output);
        return true;
    }

    const std::vector<uint8_t>& trees = m_entrySections[entry.sectionIndex].trees;
    if (entry.offset >= trees.size()) { error = L"entry offset out of range"; return false; }

    size_t cursor = (size_t)entry.offset;
    Node root;
    uint64_t budget = 2000000;
    if (!ParseNode(trees, cursor, root, 0, &budget)) { error = L"failed to parse node"; return false; }

    identity = MumxIdentity{};
    identity.size = root.size;
    identity.hashPrefix = root.hashPrefix;
    identity.hash = root.hash;
    identity.valid = false;
    identity.flags = root.flags;
    CollectIdentity(root, identity);
    {
        struct PackageScanner
        {
            static bool Scan(const Node& node)
            {
                if (node.tag == "package") return true;
                for (const Node& child : node.children)
                    if (Scan(child)) return true;
                return false;
            }
        };
        identity.isPackage = PackageScanner::Scan(root);
    }

    {
        std::string declaration;
        if (ReadString(root.declarationId, declaration)) root.tag = declaration;
    }

    std::string text;
    if (root.size != 0 && root.size < (uint64_t)1 << 32) text.reserve((size_t)root.size);
    if ((root.flags & 0x20) != 0) text += "\xEF\xBB\xBF";
    SerializeNode(root, text, 0);
    while (text.size() >= 2 && text.compare(text.size() - 2, 2, "\r\n") == 0)
        text.resize(text.size() - 2);
    if ((root.flags & 0x08) != 0) text += "\r\n";

    output.assign(text.begin(), text.end());
    return true;
}

bool MumxNativeReader::Rehydrate(const EntryRef& entry, std::vector<uint8_t>& output,
                                 std::wstring& error)
{
    if (entry.sectionIndex >= m_entrySections.size()) { error = L"entry index out of range"; return false; }
    if (IsAsn1Section(entry.sectionIndex)) return RehydrateAsn1Entry(entry, output, error);
    const std::vector<uint8_t>& trees = m_entrySections[entry.sectionIndex].trees;
    if (entry.offset >= trees.size()) { error = L"entry offset out of range"; return false; }

    size_t cursor = (size_t)entry.offset;
    Node root;
    uint64_t budget = 2000000;
    if (!ParseNode(trees, cursor, root, 0, &budget)) { error = L"failed to parse node"; return false; }

    {
        std::string declaration;
        if (ReadString(root.declarationId, declaration)) root.tag = declaration;
    }

    std::string text;
    if (root.size != 0 && root.size < (uint64_t)1 << 32) text.reserve((size_t)root.size);
    if ((root.flags & 0x20) != 0) text += "\xEF\xBB\xBF";
    SerializeNode(root, text, 0);
    while (text.size() >= 2 && text.compare(text.size() - 2, 2, "\r\n") == 0)
        text.resize(text.size() - 2);
    if ((root.flags & 0x08) != 0) text += "\r\n";
    if (g_verbose)
    {
    }
    output.assign(text.begin(), text.end());
    return true;
}

bool MumxNativeReader::CollectIdentity(const Node& node, MumxIdentity& identity) const
{
    if (node.tag == "assemblyIdentity")
    {
        for (const auto& attr : node.attrs)
        {
            if (attr.first == "name")                  identity.name = attr.second;
            else if (attr.first == "version")          identity.version = attr.second;
            else if (attr.first == "processorArchitecture") identity.arch = attr.second;
            else if (attr.first == "language")         identity.lang = attr.second;
            else if (attr.first == "publicKeyToken")   identity.publicKeyToken = attr.second;
            else if (attr.first == "versionScope")     identity.versionScope = attr.second;
            else if (attr.first == "type")             identity.type = attr.second;
            else if (attr.first == "typeName")         identity.typeName = attr.second;
        }
        if (_stricmp(identity.lang.c_str(), "neutral") == 0) identity.lang.clear();
        identity.valid = !identity.name.empty();
        return identity.valid;
    }
    for (const Node& child : node.children)
        if (CollectIdentity(child, identity)) return true;
    return false;
}

bool MumxNativeReader::ReadIdentity(const EntryRef& entry, MumxIdentity& identity,
                                    std::wstring& error)
{
    if (entry.sectionIndex >= m_entrySections.size()) { error = L"entry index out of range"; return false; }
    if (IsAsn1Section(entry.sectionIndex))
    {
        uint64_t totalSize = 0, flags = 0;
        std::string hash;
        if (!ReadAsn1Header(entry, totalSize, hash, flags, error)) return false;
        identity = MumxIdentity{};
        identity.size = totalSize;
        identity.hash = std::move(hash);
        identity.flags = flags;
        identity.valid = false;
        return true;
    }
    const std::vector<uint8_t>& trees = m_entrySections[entry.sectionIndex].trees;
    if (entry.offset >= trees.size()) { error = L"entry offset out of range"; return false; }

    size_t cursor = (size_t)entry.offset;
    Node root;
    uint64_t budget = 200000;
    if (!ParseNode(trees, cursor, root, 0, &budget)) { error = L"failed to parse node"; return false; }

    identity = MumxIdentity{};
    identity.size = root.size;
    identity.hashPrefix = root.hashPrefix;
    identity.valid = false;
    identity.flags = root.flags;
    CollectIdentity(root, identity);

    struct PackageScanner
    {
        static bool Scan(const Node& node)
        {
            if (node.tag == "package") return true;
            for (const Node& child : node.children)
                if (Scan(child)) return true;
            return false;
        }
    };
    identity.isPackage = PackageScanner::Scan(root);
    return true;
}

static bool ReadPoolString(const std::vector<uint8_t>& pool, uint64_t offset, std::string& value)
{
    if (offset >= pool.size()) return false;

    size_t cursor = (size_t)offset;
    uint64_t length = 0;
    if (!ReadLeb128(pool, cursor, length)) return false;
    if (cursor + length > pool.size()) return false;

    value.assign((const char*)pool.data() + cursor, (size_t)length);
    return true;
}

static bool ReadFileLeb128(MumxContainerFile& container, uint64_t& offset, uint64_t& value)
{
    value = 0;
    int shift = 0;
    for (;;)
    {
        uint8_t byte = 0;
        if (!container.ReadBytes(offset, &byte, 1)) return false;
        ++offset;

        value |= (uint64_t)(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) return true;

        shift += 7;
        if (shift > 63) return false;
    }
}

static bool KeyLessIgnoreCase(const std::string& left, const std::string& right)
{
    const size_t count = (std::min)(left.size(), right.size());
    for (size_t i = 0; i < count; ++i)
    {
        const unsigned char a = (unsigned char)AsciiLower(left[i]);
        const unsigned char b = (unsigned char)AsciiLower(right[i]);
        if (a != b) return a < b;
    }
    return left.size() < right.size();
}

bool ReadIndexSections(MumxContainerFile& container,
                       std::vector<MumxIndexSectionInfo>& sections, std::wstring& error)
{
    sections.clear();

    const std::vector<MumxSection>& all = container.Sections();
    for (size_t i = 0; i < all.size(); ++i)
    {
        const MumxSection& section = all[i];
        if (section.sectionType != kSectionIndex) continue;

        // IndexHeader: version, name, key comparer, value type
        uint64_t cursor = section.payloadOffset;
        uint64_t version = 0, nameLength = 0, keyComparer = 0, valueType = 0;
        if (!ReadFileLeb128(container, cursor, version) || version != 1)
        {
            error = L"unsupported index header version";
            return false;
        }
        if (!ReadFileLeb128(container, cursor, nameLength) || nameLength > 4096)
        {
            error = L"invalid index name length";
            return false;
        }

        MumxIndexSectionInfo info;
        info.sectionIndex = i;
        info.name.resize((size_t)nameLength);
        if (nameLength != 0 && !container.ReadBytes(cursor, info.name.data(), (size_t)nameLength))
        {
            error = L"failed to read index name";
            return false;
        }
        cursor += nameLength;

        if (!ReadFileLeb128(container, cursor, keyComparer) || keyComparer > 1 ||
            !ReadFileLeb128(container, cursor, valueType) || valueType > 7)
        {
            error = L"invalid index header";
            return false;
        }

        info.keyComparer = (uint32_t)keyComparer;
        info.valueType = (uint32_t)valueType;
        info.entryCount = (uint64_t)section.poolOffset;   // entry count lives in poolOffset
        info.poolOffset = cursor;

        // The compressed string pool follows the header; the entry table is
        // stored uncompressed right behind it, up to the end of the payload.
        uint32_t poolVersion = 0;
        int64_t poolSize = 0;
        if (!container.ReadU32BE(cursor, poolVersion) || !container.ReadI64BE(cursor + 12, poolSize) ||
            poolVersion < 1 || poolVersion > 2 || poolSize < 0)
        {
            error = L"invalid index data pool header";
            return false;
        }

        const uint64_t poolHeaderSize = poolVersion >= 2 ? kDataPoolHeaderV2 : kDataPoolHeaderV1;
        const uint64_t payloadEnd = section.payloadOffset + (uint64_t)section.payloadSize;
        info.tableOffset = cursor + poolHeaderSize + (uint64_t)poolSize;
        info.tableSize = payloadEnd > info.tableOffset ? payloadEnd - info.tableOffset : 0;

        sections.push_back(std::move(info));
    }
    return true;
}

bool MumxNativeReader::BuildIndexes(std::wstring& error)
{
    if (m_indexesBuilt) return true;
    m_indexesBuilt = true;

    const double buildStart = NowMs();

    std::vector<MumxIndexSectionInfo> sections;
    if (!ReadIndexSections(m_container, sections, error)) return false;

    for (const MumxIndexSectionInfo& info : sections)
    {
        MumxIndex index;
        index.name = info.name;
        index.keyComparer = info.keyComparer;
        index.valueType = info.valueType;

        std::wstring poolError;
        if (!m_container.ReadPoolAt(info.poolOffset, index.pool, poolError))
        {
            error = L"failed to decompress index pool: " + poolError;
            return false;
        }

        std::vector<uint8_t> table((size_t)info.tableSize);
        if (info.tableSize != 0 &&
            !m_container.ReadBytes(info.tableOffset, table.data(), table.size()))
        {
            error = L"failed to read index entry table";
            return false;
        }

        size_t tableCursor = 0;
        index.entries.reserve((size_t)info.entryCount);
        for (uint64_t i = 0; i < info.entryCount; ++i)
        {
            uint64_t keyId = 0, value = 0;
            if (!ReadLeb128(table, tableCursor, keyId) ||
                !ReadLeb128(table, tableCursor, value))
            {
                error = L"truncated index entry table";
                return false;
            }

            MumxIndexEntry entry;
            if (!ReadPoolString(index.pool, keyId, entry.key))
            {
                error = L"index key out of range";
                return false;
            }
            entry.value = (int64_t)value;
            index.entries.push_back(std::move(entry));
        }

        if (index.keyComparer == 1)
            std::stable_sort(index.entries.begin(), index.entries.end(),
                             [](const MumxIndexEntry& a, const MumxIndexEntry& b)
                             { return KeyLessIgnoreCase(a.key, b.key); });
        else
            std::stable_sort(index.entries.begin(), index.entries.end(),
                             [](const MumxIndexEntry& a, const MumxIndexEntry& b)
                             { return a.key < b.key; });

        if (g_verbose)
            Log(L"  [index] {}  valueType={}  comparer={}  entries={}  pool={} bytes",
                NarrowToWide(index.name), index.valueType, index.keyComparer,
                index.entries.size(), index.pool.size());

        m_indexes.push_back(std::move(index));
    }

    if (g_verbose) Log(L"  [t] index sections: {:.0f} ms", NowMs() - buildStart);
    return true;
}

bool MumxNativeReader::ResolveIndexValue(const MumxIndex& index, int64_t value,
                                         std::string& text) const
{
    if (index.valueType != 0 || value < 0) return false;
    return ReadPoolString(index.pool, (uint64_t)value, text);
}
