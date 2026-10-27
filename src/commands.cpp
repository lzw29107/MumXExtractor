#include "commands.h"

static bool WriteAllBytes(const std::wstring& filePath, const void* buffer, size_t byteCount)
{
    size_t lastSlash = filePath.find_last_of(L'\\');
    if (lastSlash != std::wstring::npos)
    {
        std::wstring directory = filePath.substr(0, lastSlash);
        for (size_t i = 3; i <= directory.size(); ++i)
        {
            if (i == directory.size() || directory[i] == L'\\')
                CreateDirectoryW(directory.substr(0, i).c_str(), nullptr);
        }
    }

    HANDLE file = CreateFileW(filePath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    DWORD bytesWritten = 0;
    BOOL ok = WriteFile(file, buffer, (DWORD)byteCount, &bytesWritten, nullptr);
    CloseHandle(file);
    return ok && bytesWritten == byteCount;
}

static bool TryParseIndex(const std::wstring& text, size_t& value)
{
    if (text.empty()) return false;

    size_t result = 0;
    for (wchar_t ch : text)
    {
        if (ch < L'0' || ch > L'9') return false;
        result = result * 10 + (size_t)(ch - L'0');
    }
    value = result;
    return true;
}

int DumpContainerLayout(const std::wstring& containerPath)
{
    MumxContainerFile container;
    std::wstring error;
    if (!container.Open(containerPath, error))
    {
        Log(L"error: {}", error);
        Log(L"       {}", containerPath);
        return 1;
    }

    Log(L"size            : {} bytes", container.FileSize());
    Log(L"magic           : MUMX");
    Log(L"version         : {}", container.Version());
    Log(L"sectionCount    : {}", container.SectionCount());

    if (g_verbose && container.Version() >= 2)
    {
        Log(L"hashAlgId       : 0x{:08X}{}", container.HashAlgId(),
            container.HashAlgId() == kHashAlgSha256 ? L"  (CALG_SHA_256)" : L"");
        Log(L"contentHash     : {} bytes", container.ContentHash().size());
        if (!container.ContentHash().empty())
        {
            std::wstring hashText;
            for (std::uint8_t byte : container.ContentHash()) hashText += std::format(L"{:02X}", byte);
            Log(L"                  {}", hashText);
        }
        Log(L"defaultCompAlg  : 0x{:08X}  ({})", container.DefaultCompressionAlg(),
            CompressionName(container.DefaultCompressionAlg()));
        Log(L"defaultBlockSize: 0x{:08X}  ({} bytes)", container.DefaultBlockSize(),
            container.DefaultBlockSize());
    }

    Log(L"");
    if (g_verbose)
    {
        Log(L"  idx  sectionType                      hdrOffset   payloadOff  payloadSize  poolOffset  blockIndex  compression");
        Log(L"  ---  -------------------------------  ----------  ----------  -----------  ----------  ----------  -----------");
        for (size_t i = 0; i < container.Sections().size(); ++i)
        {
            const MumxSection& section = container.Sections()[i];
            Log(L"  [{:2}] {:<31}  0x{:08X}  0x{:08X}  {:11}  {:10}  {:10}  {}",
                i, SectionTypeName(section.sectionType),
                (uint64_t)section.headerOffset, (uint64_t)section.payloadOffset,
                (int64_t)section.payloadSize, (int64_t)section.poolOffset,
                (int64_t)section.blockIndexSize, CompressionName(section.compressionAlg));

            if (section.headerVersion >= 2 && section.IsCompressed())
            {
                Log(L"        blockSize={}  uncompressedSize={}  blockIndex={}",
                    section.blockSize, (int64_t)section.uncompressedSize,
                    (int64_t)section.blockIndexSize);
            }
        }
    }
    else
    {
        Log(L"  idx  sectionType                      payloadSize  compression");
        Log(L"  ---  -------------------------------  -----------  -----------");
        for (size_t i = 0; i < container.Sections().size(); ++i)
        {
            const MumxSection& section = container.Sections()[i];
            Log(L"  [{:2}] {:<31}  {:11}  {}", i, SectionTypeName(section.sectionType),
                (int64_t)section.payloadSize, CompressionName(section.compressionAlg));
        }
    }

    if (g_verbose && !container.Sections().empty())
    {
        const MumxSection& lastSection = container.Sections().back();
        uint64_t chainEnd = lastSection.payloadOffset + (uint64_t)lastSection.payloadSize;
        Log(L"");
        if (chainEnd == container.FileSize())
            Log(L"section chain   : ends at 0x{:X}, matches file size", chainEnd);
        else
            Log(L"section chain   : ends at 0x{:X}, file size 0x{:X} (delta {:+})",
                chainEnd, container.FileSize(),
                (int64_t)container.FileSize() - (int64_t)chainEnd);
    }
    return 0;
}

int DumpSection(const std::wstring& containerPath, const std::wstring& sectionSelector,
                const std::wstring& outputPath)
{
    MumxContainerFile container;
    std::wstring error;
    if (!container.Open(containerPath, error))
    {
        Log(L"[!] {}: {}", containerPath, error);
        return 1;
    }
    if (container.Sections().empty()) { Log(L"[!] container has no sections"); return 1; }

    bool dumpAll = (sectionSelector == L"all");
    size_t begin = 0, end = container.Sections().size();
    if (!dumpAll)
    {
        size_t index = 0;
        if (!TryParseIndex(sectionSelector, index) || index >= container.Sections().size())
        {
            Log(L"[!] invalid section index (0..{}, or all)", container.Sections().size() - 1);
            return 1;
        }
        begin = index;
        end = index + 1;
    }

    for (size_t i = begin; i < end; ++i)
    {
        const MumxSection& section = container.Sections()[i];
        Log(L"[dump] section {}  type={}  algo={}", i,
            SectionTypeName(section.sectionType), CompressionName(section.compressionAlg));

        std::vector<uint8_t> data;
        if (!container.ReadSectionData(i, data, error))
        {
            Log(L"  [!] {}", error);
            return 1;
        }

        std::wstring target = outputPath;
        if (dumpAll) target += std::format(L".{}.bin", i);
        if (!WriteAllBytes(target, data.data(), data.size()))
        {
            Log(L"  [!] failed to write file: {}", target);
            return 1;
        }
        Log(L"  -> {} bytes  {}", data.size(), target);
    }
    return 0;
}

static HRESULT ExtractBlobToFolder(ContainerApi& api,
                                   const std::wstring& blobName,
                                   const std::wstring& outputFolder,
                                   bool* blobFound,
                                   bool quiet = false)
{
    if (blobFound) *blobFound = false;

    bool exists = false;
    HRESULT hr = api.DoesBlobExist(blobName.c_str(), &exists);
    if (FAILED(hr)) { if (!quiet) LogHr(L"DoesBlobExist", hr, blobName.c_str()); return hr; }
    if (!exists)
    {
        if (!quiet) Log(L"  [!] blob not found: {}", blobName);
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    if (blobFound) *blobFound = true;

    size_t blobSize = 0;
    hr = api.GetBlobSize(blobName.c_str(), &blobSize);
    if (FAILED(hr)) { if (!quiet) LogHr(L"GetBlobSize", hr, blobName.c_str()); return hr; }
    if (blobSize == 0)
    {
        if (!quiet) Log(L"  [!] blob is empty: {}", blobName);
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    std::vector<uint8_t> blobBuffer(blobSize);
    size_t bytesWritten = 0;
    hr = api.GetBlob(blobName.c_str(), blobBuffer.data(), blobSize, &bytesWritten);
    if (FAILED(hr)) { if (!quiet) LogHr(L"GetBlob", hr, blobName.c_str()); return hr; }
    blobBuffer.resize(bytesWritten);

    std::wstring outputPath = outputFolder;
    if (!outputPath.empty() && outputPath.back() != L'\\') outputPath += L'\\';
    outputPath += blobName;

    if (!WriteAllBytes(outputPath, blobBuffer.data(), blobBuffer.size()))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        if (!quiet) LogHr(L"WriteFile", hr, outputPath.c_str());
        return hr;
    }
    if (g_verbose)
        Log(L"  [+] {:<40} -> {} bytes  {}", blobName, blobBuffer.size(), outputPath);
    return S_OK;
}

int ListBlobs(ContainerApi& api, const std::wstring& containerPath)
{
    (void)containerPath;

    if (api.IsNative())
    {
        size_t row = 0;
        api.Native()->EnumerateBlobs([&](size_t, const std::string& blobName, size_t blobSize) {
            const std::wstring name = NarrowToWide(blobName);
            if (blobSize == (size_t)-1)
                Log(L"  [{:3}] {:>10}  {}", row++, L"?", name);
            else
                Log(L"  [{:3}] {:>10}  {}", row++, blobSize, name);
            if ((row & 255) == 0) FlushOut();
            return true;
        });
        Log(L"");
        Log(L"blobCount : {}", row);

        size_t total = api.Native()->TotalEntryCount();
        if (total != row)
        {
            Log(L"");
            Log(L"[!] {} entries, {} listed (delta {})", total, row,
                (int64_t)total - (int64_t)row);
        }
        return 0;
    }

    size_t blobCount = 0;
    HRESULT hr = api.GetBlobCount(&blobCount);
    if (FAILED(hr)) { LogHr(L"GetBlobCount", hr); return 1; }

    Log(L"blobCount : {}", blobCount);
    if (blobCount == 0) return 0;

    std::vector<const wchar_t*> blobNames(blobCount, nullptr);
    size_t returnedCount = 0;
    hr = api.GetBlobNames(blobNames.data(), blobCount, &returnedCount);
    if (FAILED(hr)) { LogHr(L"GetBlobNames", hr); return 1; }

    for (size_t i = 0; i < returnedCount; ++i)
    {
        size_t blobSize = 0;
        HRESULT sizeHr = api.GetBlobSize(blobNames[i], &blobSize);
        Log(L"  [{:3}] {:>10}  {}{}", i, blobSize, blobNames[i],
            FAILED(sizeHr) ? L"   (GetBlobSize failed)" : L"");
        FlushOut();
    }
    return 0;
}

static const wchar_t* IndexValueTypeName(uint32_t valueType)
{
    switch (valueType)
    {
    case 0:  return L"string";
    case 1:  return L"boolean";
    case 2:  return L"number";
    case 3:  return L"object";
    case 4:  return L"string[]";
    case 5:  return L"number[]";
    case 6:  return L"object[]";
    default: return L"invalid";
    }
}

static const wchar_t* IndexKeyComparerName(uint32_t keyComparer)
{
    return keyComparer == 1 ? L"ignore case" : L"ordinal";
}

int ShowIndexes(ContainerApi& api, const std::wstring& containerPath, const std::string& lookupKey)
{
    // Section headers supply the key counts; the DLL interface does not expose them.
    std::vector<MumxIndexSectionInfo> sections;
    {
        MumxContainerFile layout;
        std::wstring layoutError;
        if (!layout.Open(containerPath, layoutError))
        {
            Log(L"error: {}", layoutError);
            return 1;
        }
        if (!ReadIndexSections(layout, sections, layoutError))
        {
            Log(L"error: {}", layoutError);
            return 1;
        }
    }

    size_t indexCount = 0;
    HRESULT hr = api.GetIndexCount(&indexCount);
    if (FAILED(hr)) { LogHr(L"GetIndexCount", hr); return 1; }

    Log(L"indexCount : {}", indexCount);
    if (indexCount == 0) return 0;

    std::vector<void*> indexes(indexCount, nullptr);
    size_t returnedCount = 0;
    hr = api.GetIndexes(indexes.data(), indexCount, &returnedCount);
    if (FAILED(hr)) { LogHr(L"GetIndexes", hr); return 1; }

    for (size_t i = 0; i < returnedCount; ++i)
    {
        std::string name;
        uint32_t valueType = 0, keyComparer = 0;
        std::vector<std::string> keys;

        if (api.IsNative())
        {
            const MumxIndex& index = *static_cast<const MumxIndex*>(indexes[i]);
            name = index.name;
            valueType = index.valueType;
            keyComparer = index.keyComparer;
            keys.reserve(index.entries.size());
            for (const MumxIndexEntry& entry : index.entries) keys.push_back(entry.key);
        }
        else
        {
            IContainerIndex* index = static_cast<IContainerIndex*>(indexes[i]);
            if (const char* indexName = index->GetName()) name = indexName;
            valueType = (uint32_t)index->GetValueType();
            keyComparer = (uint32_t)index->GetKeyComparer();

            for (const MumxIndexSectionInfo& candidate : sections)
            {
                if (candidate.name != name || candidate.entryCount == 0) continue;
                std::vector<const char*> rawKeys((size_t)candidate.entryCount, nullptr);
                if (SUCCEEDED(index->GetKeys(rawKeys.data(), rawKeys.size())))
                    for (const char* key : rawKeys) keys.emplace_back(key != nullptr ? key : "");
                break;
            }
        }

        Log(L"  [{:2}] {:<28}  valueType={} ({})  comparer={} ({})  keys={}",
            i, NarrowToWide(name), valueType, IndexValueTypeName(valueType),
            keyComparer, IndexKeyComparerName(keyComparer), keys.size());

        if (g_verbose)
            for (size_t k = 0; k < keys.size(); ++k)
                Log(L"        [{:5}] {}", k, NarrowToWide(keys[k]));

        bool exists = false;
        int probedType = 0;
        hr = api.DoesIndexExist(name.c_str(), &exists, &probedType);
        if (FAILED(hr)) { LogHr(L"DoesIndexExist", hr); return 1; }
        Log(L"  DoesIndexExist(\"{}\") -> {}  valueType={}", NarrowToWide(name),
            exists ? L"true" : L"false", probedType);

        if (!lookupKey.empty())
        {
            void* result = nullptr;
            hr = api.IndexLookup(name.c_str(), lookupKey.c_str(), &result);
            if (FAILED(hr)) { LogHr(L"IndexLookup", hr); return 1; }

            Log(L"  IndexLookup(\"{}\") -> {}", NarrowToWide(lookupKey),
                result != nullptr ? L"found" : L"not found");

            if (g_verbose && result != nullptr && api.IsNative())
            {
                const NativeIndexedValue& value = *static_cast<const NativeIndexedValue*>(result);
                if (value.valueType == 0)      Log(L"        value = \"{}\"", NarrowToWide(value.text));
                else if (value.valueType == 1) Log(L"        value = {}", value.boolean);
                else if (value.valueType == 2) Log(L"        value = {}", value.number);
            }
        }
    }
    return 0;
}

int ExtractBlobs(ContainerApi& api, const std::wstring& containerPath,
                 const std::wstring& blobNameOrWildcard,
                 const std::wstring& outputFolder)
{
    (void)containerPath;
    HRESULT hr = S_OK;
    if (blobNameOrWildcard != L"*")
    {
        bool blobFound = false;
        LogProgress(0, 1);
        hr = ExtractBlobToFolder(api, blobNameOrWildcard, outputFolder, &blobFound);
        LogProgress(1, 1);
        LogProgressDone();
        return SUCCEEDED(hr) ? 0 : 1;
    }

    size_t blobCount = 0;
    hr = api.GetBlobCount(&blobCount);
    if (FAILED(hr)) { LogHr(L"GetBlobCount", hr); return 1; }
    if (blobCount == 0) { Log(L"empty container"); return 0; }

    std::vector<const wchar_t*> blobNames(blobCount, nullptr);
    size_t returnedCount = 0;
    hr = api.GetBlobNames(blobNames.data(), blobCount, &returnedCount);
    if (FAILED(hr)) { LogHr(L"GetBlobNames", hr); return 1; }

    std::vector<std::wstring> ownedNames(blobNames.begin(), blobNames.begin() + returnedCount);
    std::vector<std::wstring> failedNames;
    LogProgress(0, returnedCount);
    for (size_t i = 0; i < ownedNames.size(); ++i)
    {
        bool blobFound = false;
        if (FAILED(ExtractBlobToFolder(api, ownedNames[i], outputFolder, &blobFound, !g_verbose)))
            failedNames.push_back(ownedNames[i]);
        LogProgress(i + 1, returnedCount);
    }
    LogProgressDone();

    if (g_verbose)
    {
        Log(L"done: {} succeeded, {} failed", ownedNames.size() - failedNames.size(),
            failedNames.size());
    }
    else
    {
        for (size_t i = 0; i < failedNames.size() && i < 10; ++i)
            Log(L"[!] failed: {}", failedNames[i]);
        if (failedNames.size() > 10)
            Log(L"[!] ... {} more failed", failedNames.size() - 10);
        if (!failedNames.empty())
            Log(L"[!] {} failed", failedNames.size());
    }
    return failedNames.empty() ? 0 : 1;
}

int ExtractUpdateMum(ContainerApi& api, const std::wstring& outputFolder)
{
    Log(L"[1/4] open container ...");
    Log(L"      OK");

    Log(L"[2/4] DoesBlobExist(L\"update.mum\") ...");
    bool exists = false;
    HRESULT hr = api.DoesBlobExist(L"update.mum", &exists);
    if (FAILED(hr)) { LogHr(L"DoesBlobExist", hr); return 1; }
    if (!exists)
    {
        Log(L"      [!] update.mum not present, hr = 0x{:08X}", (unsigned)HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
        return 1;
    }
    Log(L"      OK");

    Log(L"[3/4] GetBlobSize + GetBlob -> {}\\update.mum", outputFolder);
    bool blobFound = false;
    hr = ExtractBlobToFolder(api, L"update.mum", outputFolder, &blobFound);
    if (FAILED(hr)) return 1;

    Log(L"[4/4] RecursiveRemoveDirectory(scratchDirectory) ...");
    return 0;
}

int SelfTest(const std::wstring& turboContainerDllPath, const std::wstring& containerPath)
{
    Log(L"--- dll engine ---");
    Log(L"TurboContainer.dll : {}",
        turboContainerDllPath.empty() ? L"<not found>" : turboContainerDllPath.c_str());
    bool dllOk = ProbeDllEngine(turboContainerDllPath, containerPath);
    Log(L"probe              : {}", dllOk ? L"OK" : L"FAILED");

    Log(L"");
    Log(L"--- native engine ---");
    bool nativeOk = false;
    if (!containerPath.empty())
    {
        MumxContainerFile container;
        std::wstring error;
        nativeOk = container.Open(containerPath, error);
        Log(L"MumxContainerFile  : {}", nativeOk ? L"OK" : error.c_str());
        if (nativeOk)
        {
            Log(L"sections           : {}", container.Sections().size());
            for (size_t i = 0; i < container.Sections().size(); ++i)
            {
                const MumxSection& section = container.Sections()[i];
                if (!section.IsCompressed()) continue;
                std::vector<uint8_t> data;
                std::wstring decompressError;
                bool ok = container.ReadSectionData(i, data, decompressError);
                Log(L"  section {} decompress: {} ({} bytes)", i,
                    ok ? L"OK" : decompressError.c_str(), data.size());
            }
        }
    }
    else
    {
        Log(L"(pass a container path to exercise decompression)");
    }

    Log(L"");
    Log(L"result: dll={} native={}", dllOk ? L"available" : L"unavailable",
        nativeOk ? L"available" : L"not verified");
    return (dllOk || nativeOk) ? 0 : 1;
}
