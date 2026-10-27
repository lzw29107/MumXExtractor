#pragma once

#include "common.h"

struct MumxIdentity
{
    std::string name, version, arch, lang, publicKeyToken, versionScope;
    std::string typeName, type;
    bool        isPackage = false;
    uint64_t    flags = 0;
    uint64_t    size = 0;
    uint64_t    hashPrefix = 0;
    std::string hash;
    bool        valid = false;
};

std::string BuildBlobName(const MumxIdentity& identity);
