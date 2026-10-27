#include "keyform.h"

static uint64_t HashX65599Variant(const std::string& text)
{
    static const uint64_t kMultipliers[4] = { 0x1E5FFFFFD27ULL, 0xFFFFFFDC00000051ULL,
                                              0x1FFFFFFF7ULL, 1ULL };
    uint32_t accumulators[4] = { 0, 0, 0, 0 };
    for (size_t i = 0; i < text.size(); ++i)
    {
        uint32_t value = (uint32_t)(unsigned char)AsciiLower(text[i]);
        uint32_t& acc = accumulators[i % 4];
        acc = acc * 65599u + value;
    }
    uint64_t hash = 0;
    for (int i = 0; i < 4; ++i) hash += (uint64_t)accumulators[i] * kMultipliers[i];
    return hash;
}

static uint64_t AttributeNameHash(const std::string& attributeName)
{
    if (attributeName == "name")                  return HashX65599Variant("Name");
    if (attributeName == "culture")               return HashX65599Variant("Culture");
    if (attributeName == "typeName")              return HashX65599Variant("TypeName");
    if (attributeName == "type")                  return HashX65599Variant("Type");
    if (attributeName == "version")               return HashX65599Variant("Version");
    if (attributeName == "publicKeyToken")        return HashX65599Variant("PublicKeyToken");
    if (attributeName == "processorArchitecture") return HashX65599Variant("ProcessorArchitecture");
    if (attributeName == "versionScope")          return HashX65599Variant("VersionScope");
    return HashX65599Variant(attributeName);
}

static uint64_t ComputePseudoKey(const MumxIdentity& identity)
{
    const uint64_t kLonghorn = 0x1FFFFFFF7ULL;
    std::pair<std::string, std::string> attributes[8];
    size_t count = 0;

    if (!identity.name.empty())          attributes[count++] = { "name", identity.name };
    if (!identity.lang.empty())          attributes[count++] = { "culture", identity.lang };
    if (!identity.typeName.empty())      attributes[count++] = { "typeName", identity.typeName };
    if (!identity.type.empty())          attributes[count++] = { "type", identity.type };
    if (!identity.version.empty())       attributes[count++] = { "version", identity.version };
    if (!identity.publicKeyToken.empty())attributes[count++] = { "publicKeyToken", identity.publicKeyToken };
    if (!identity.arch.empty())          attributes[count++] = { "processorArchitecture", identity.arch };
    if (!identity.versionScope.empty())  attributes[count++] = { "versionScope", identity.versionScope };

    uint64_t pseudoKey = 0;
    for (size_t i = 0; i < count; ++i)
    {
        uint64_t nameValueHash = AttributeNameHash(attributes[i].first) * kLonghorn +
                                 HashX65599Variant(attributes[i].second);
        pseudoKey = pseudoKey * kLonghorn + nameValueHash;
    }
    return pseudoKey;
}

static std::string SanitizeNamePart(const std::string& value, size_t maxLength)
{
    std::string sanitized;
    sanitized.reserve(value.size());
    for (char ch : value)
    {
        char current = AsciiLower(ch);
        bool keep = (current >= '0' && current <= '9') ||
                    (current >= 'a' && current <= 'z') ||
                    current == '_' || current == '.' || current == '-';
        if (keep) sanitized += current;
    }
    if (sanitized.size() <= maxLength) return sanitized;

    size_t available = maxLength - 2;
    size_t rightLength = available / 2;
    size_t leftLength = available - rightLength;
    return sanitized.substr(0, leftLength) + ".." + sanitized.substr(sanitized.size() - rightLength);
}

std::string BuildBlobName(const MumxIdentity& identity)
{
    if (identity.isPackage)
    {
        std::string packageName = identity.name;
        packageName += '~';
        packageName += identity.publicKeyToken.empty() ? "none" : identity.publicKeyToken;
        packageName += '~';
        packageName += identity.arch.empty() ? "none" : identity.arch;
        packageName += '~';
        if (!identity.lang.empty()) packageName += identity.lang;
        packageName += '~';
        packageName += identity.version;
        packageName += ".mum";
        return packageName;
    }

    std::uint64_t pseudoKey = ComputePseudoKey(identity);
    const std::string hashText = std::format("{:016x}", pseudoKey);

    std::string blobName = identity.arch.empty() ? "none" : identity.arch;
    if (!identity.typeName.empty())
    {
        blobName += '_';
        blobName += SanitizeNamePart(identity.typeName, 16);
    }
    blobName += '_';
    blobName += SanitizeNamePart(identity.name, 40);
    blobName += '_';
    blobName += identity.publicKeyToken.empty() ? "none" : identity.publicKeyToken;
    blobName += '_';
    blobName += identity.version.empty() ? "none" : identity.version;
    blobName += '_';
    blobName += SanitizeNamePart(identity.lang.empty() ? "none" : identity.lang, 8);
    blobName += '_';
    blobName += hashText;
    blobName += ".manifest";
    return blobName;
}
