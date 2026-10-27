#pragma once

#include "common.h"
#include "container.h"
#include "reader.h"
#include "console.h"

struct IUpdateContainer
{
    virtual HRESULT Initialize(const wchar_t* containerPath, const wchar_t* scratchDirectory) = 0;
    virtual HRESULT GetBlobCount(size_t* blobCount) = 0;
    virtual HRESULT GetBlobNames(const wchar_t** blobNames, size_t capacity, size_t* blobCount) = 0;
    virtual HRESULT GetBlob(const wchar_t* blobName, void* buffer, size_t bufferSize, size_t* bytesWritten) = 0;
    virtual HRESULT GetBlobSize(const wchar_t* blobName, size_t* blobSize) = 0;
    virtual HRESULT DoesBlobExist(const wchar_t* blobName, bool* exists) = 0;
    virtual HRESULT DoesIndexExist(const char* indexName, bool* exists, int* valueType) = 0;
    virtual HRESULT IndexLookup(const char* indexName, const char* value, void** lookupResult) = 0;
    virtual HRESULT GetIndexCount(size_t* indexCount) = 0;
    virtual HRESULT GetIndexes(void** indexes, size_t capacity, size_t* indexCount) = 0;
    virtual int     GetType() = 0;
    virtual void    Release() = 0;
};

// Returned by IUpdateContainer::GetIndexes; slot order mirrors the reader's vtable.
struct IContainerIndex
{
    virtual const char* GetName() = 0;
    virtual int         GetKeyComparer() = 0;
    virtual int         GetValueType() = 0;
    virtual int64_t     NameId() = 0;
    virtual HRESULT     GetKeys(const char** keys, size_t capacity) = 0;
};

// Result of NativeUpdateContainer::IndexLookup. The DLL engine returns an
// IIndexLookupResult instead, so the command layer keeps the two apart.
struct NativeIndexedValue
{
    uint32_t    valueType = 0;   // 0 string, 1 boolean, 2 number
    std::string text;
    int64_t     number = 0;
    bool        boolean = false;
};

struct ContainerHandle
{
    HMODULE           turboContainerModule = nullptr;
    IUpdateContainer* updateContainer = nullptr;
    std::wstring      scratchDirectory;

    ~ContainerHandle()
    {
        if (updateContainer) { updateContainer->Release(); updateContainer = nullptr; }
        if (turboContainerModule) { FreeLibrary(turboContainerModule); turboContainerModule = nullptr; }
        if (!scratchDirectory.empty()) RemoveDirectoryW(scratchDirectory.c_str());
    }
};

class NativeUpdateContainer
{
public:
    HRESULT Initialize(const wchar_t* containerPath, const wchar_t* scratchDirectory)
    {
        (void)scratchDirectory;
        if (containerPath == nullptr) return E_INVALIDARG;

        std::wstring error;
        if (!m_reader.Open(containerPath, error)) return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
        m_namesBuilt = false;
        m_indexesBuilt = false;
        m_indexesStatus = S_OK;
        m_names.clear();
        m_entryIndex.clear();
        m_lookup.clear();
        return S_OK;
    }

    HRESULT GetBlobCount(size_t* blobCount)
    {
        if (blobCount == nullptr) return E_INVALIDARG;
        EnsureLookup();
        *blobCount = m_names.size();
        return S_OK;
    }

    HRESULT GetBlobNames(const wchar_t** blobNames, size_t capacity, size_t* blobCount)
    {
        if (blobCount == nullptr) return E_INVALIDARG;
        EnsureLookup();
        if (capacity != m_names.size()) return E_INVALIDARG;
        if (capacity != 0 && blobNames == nullptr) return E_INVALIDARG;

        for (size_t i = 0; i < m_names.size(); ++i) blobNames[i] = m_names[i].c_str();
        *blobCount = m_names.size();
        return S_OK;
    }

    HRESULT GetBlobSize(const wchar_t* blobName, size_t* blobSize)
    {
        if (blobName == nullptr || blobSize == nullptr) return E_INVALIDARG;
        EnsureLookup();
        size_t index = 0;
        if (!Find(blobName, index)) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);   // 0x80070490

        std::vector<uint8_t> data;
        std::wstring error;
        if (!m_reader.Rehydrate(m_reader.Entries()[m_entryIndex[index]], data, error))
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);

        *blobSize = data.size();
        return S_OK;
    }

    HRESULT GetBlob(const wchar_t* blobName, void* buffer, size_t bufferSize, size_t* bytesWritten)
    {
        if (blobName == nullptr || buffer == nullptr || bufferSize == 0 || bytesWritten == nullptr)
            return E_INVALIDARG;
        EnsureLookup();

        size_t index = 0;
        if (!Find(blobName, index)) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);

        std::vector<uint8_t> data;
        std::wstring error;
        if (!m_reader.Rehydrate(m_reader.Entries()[m_entryIndex[index]], data, error))
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);

        if (data.size() > bufferSize) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);

        memcpy(buffer, data.data(), data.size());
        *bytesWritten = data.size();
        return S_OK;
    }

    HRESULT DoesBlobExist(const wchar_t* blobName, bool* exists)
    {
        if (blobName == nullptr || exists == nullptr) return E_INVALIDARG;
        EnsureLookup();
        size_t index = 0;
        *exists = Find(blobName, index);
        return S_OK;
    }

    HRESULT DoesIndexExist(const char* indexName, bool* exists, int* valueType)
    {
        if (indexName == nullptr || exists == nullptr || valueType == nullptr) return E_INVALIDARG;

        HRESULT hr = EnsureIndexes();
        if (FAILED(hr)) return hr;

        const MumxIndex* index = FindIndex(indexName);
        *exists = index != nullptr;
        *valueType = index != nullptr ? (int)index->valueType : 7;   // 7 = invalid
        return S_OK;
    }

    HRESULT IndexLookup(const char* indexName, const char* value, void** lookupResult)
    {
        if (indexName == nullptr || value == nullptr || lookupResult == nullptr) return E_INVALIDARG;
        *lookupResult = nullptr;

        HRESULT hr = EnsureIndexes();
        if (FAILED(hr)) return hr;

        const MumxIndex* index = FindIndex(indexName);
        if (index == nullptr) return S_OK;

        const MumxIndexEntry* entry = FindKey(*index, value);
        if (entry == nullptr) return S_OK;

        m_lastValue = NativeIndexedValue{};
        m_lastValue.valueType = index->valueType;
        switch (index->valueType)
        {
        case 0:
            if (!m_reader.ResolveIndexValue(*index, entry->value, m_lastValue.text))
                return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            break;
        case 1:
            m_lastValue.boolean = entry->value != 0;
            break;
        case 2:
            m_lastValue.number = entry->value;
            break;
        default:
            return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);   // object / array encodings
        }

        *lookupResult = &m_lastValue;
        return S_OK;
    }

    HRESULT GetIndexCount(size_t* indexCount)
    {
        if (indexCount == nullptr) return E_INVALIDARG;

        HRESULT hr = EnsureIndexes();
        if (FAILED(hr)) return hr;

        *indexCount = m_reader.Indexes().size();
        return S_OK;
    }

    HRESULT GetIndexes(void** indexes, size_t capacity, size_t* indexCount)
    {
        if (indexes == nullptr || indexCount == nullptr) return E_INVALIDARG;

        HRESULT hr = EnsureIndexes();
        if (FAILED(hr)) return hr;

        const std::vector<MumxIndex>& all = m_reader.Indexes();
        if (capacity != all.size()) return E_INVALIDARG;

        for (size_t i = 0; i < capacity; ++i) indexes[i] = (void*)&all[i];
        *indexCount = capacity;
        return S_OK;
    }

    const std::vector<MumxIndex>& Indexes()
    {
        EnsureIndexes();
        return m_reader.Indexes();
    }

    int GetType() { return 3; }   // ExpressContainerType::MumX

    size_t TotalEntryCount() const { return m_reader.Entries().size(); }

    bool EnumerateBlobs(const std::function<bool(size_t index, const std::string& blobName,
                                                 size_t blobSize)>& visit)
    {
        EnsureNameCache();
        if (m_blobs.empty()) return true;

        unsigned threadCount = std::thread::hardware_concurrency();
        if (threadCount == 0) threadCount = 1;
        if (threadCount > 16) threadCount = 16;

        if (g_verbose) Log(L"  [t] worker threads: {}", threadCount);
        const size_t kBatch = 256;
        std::vector<size_t> sizes(kBatch);

        for (size_t start = 0; start < m_blobs.size(); start += kBatch)
        {
            const size_t count = (std::min)(kBatch, m_blobs.size() - start);

            auto worker = [&](unsigned slot)
            {
                const size_t begin = count * slot / threadCount;
                const size_t end   = count * (slot + 1) / threadCount;
                for (size_t k = begin; k < end; ++k)
                {
                    size_t blobSize = 0;
                    sizes[k] = BlobSize(start + k, blobSize) ? blobSize : kUnknownBlobSize;
                }
            };

            std::vector<std::thread> pool;
            pool.reserve(threadCount > 0 ? threadCount - 1 : 0);
            for (unsigned t = 1; t < threadCount; ++t) pool.emplace_back(worker, t);
            worker(0);
            for (std::thread& thread : pool) thread.join();

            if (g_verbose && (start % 32768) == 0)
                Log(L"  [t] processed {} / {}", start, m_blobs.size());
            for (size_t k = 0; k < count; ++k)
                if (!visit(start + k, m_blobs[start + k].name, sizes[k])) return true;
        }
        return true;
    }

private:
    static const size_t kUnknownBlobSize = (size_t)-1;

    HRESULT EnsureIndexes()
    {
        if (m_indexesBuilt) return m_indexesStatus;
        m_indexesBuilt = true;

        std::wstring error;
        m_indexesStatus = m_reader.BuildIndexes(error) ? S_OK
                                                       : HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
        if (g_verbose && FAILED(m_indexesStatus)) Log(L"  [index] {}", error);
        return m_indexesStatus;
    }

    const MumxIndex* FindIndex(const char* indexName) const
    {
        for (const MumxIndex& index : m_reader.Indexes())
            if (index.name == indexName) return &index;   // ordinal, like std::less<std::string>
        return nullptr;
    }

    static bool KeyLess(const MumxIndex& index, const std::string& left, const std::string& right)
    {
        if (index.keyComparer == 0) return left < right;

        const size_t count = (std::min)(left.size(), right.size());
        for (size_t i = 0; i < count; ++i)
        {
            const unsigned char a = (unsigned char)AsciiLower(left[i]);
            const unsigned char b = (unsigned char)AsciiLower(right[i]);
            if (a != b) return a < b;
        }
        return left.size() < right.size();
    }

    static const MumxIndexEntry* FindKey(const MumxIndex& index, const std::string& key)
    {
        auto it = std::lower_bound(index.entries.begin(), index.entries.end(), key,
                                   [&index](const MumxIndexEntry& entry, const std::string& value)
                                   { return KeyLess(index, entry.key, value); });
        if (it == index.entries.end()) return nullptr;
        return KeyLess(index, key, it->key) ? nullptr : &*it;
    }

    struct BlobEntry
    {
        std::string name;
        size_t      entryIndex = 0;
        size_t      size = 0;
        bool        sizeKnown = false;
        bool        sizeExact = false;
    };

    static std::string LowerAscii(std::string text)
    {
        for (char& ch : text) ch = AsciiLower(ch);
        return text;
    }

    static bool NameLess(const std::string& a, const std::string& b)
    {
        size_t n = (a.size() < b.size()) ? a.size() : b.size();
        for (size_t i = 0; i < n; ++i)
        {
            unsigned char ca = (unsigned char)AsciiLower(a[i]);
            unsigned char cb = (unsigned char)AsciiLower(b[i]);
            if (ca != cb) return ca < cb;
        }
        return a.size() < b.size();
    }

    bool BlobSize(size_t blobIndex, size_t& size)
    {
        BlobEntry& blob = m_blobs[blobIndex];
        if (blob.sizeKnown && !g_exactSizes) { size = blob.size; return true; }
        if (blob.sizeKnown && blob.sizeExact) { size = blob.size; return true; }
        std::vector<uint8_t> data;
        std::wstring error;
        if (!m_reader.Rehydrate(m_reader.Entries()[blob.entryIndex], data, error)) return false;
        blob.size = data.size();
        blob.sizeKnown = true;
        blob.sizeExact = true;
        size = blob.size;
        return true;
    }

    //
    void EnsureNameCache()
    {
        if (m_namesBuilt) return;
        m_namesBuilt = true;

        const std::vector<MumxNativeReader::EntryRef>& entries = m_reader.Entries();
        const std::vector<std::string>& names  = m_reader.BlobNames();
        const std::vector<uint64_t>&    offsets = m_reader.BlobNameOffsets();

        std::unordered_map<uint64_t, size_t> entryByOffset;
        entryByOffset.reserve(entries.size());
        for (size_t i = 0; i < entries.size(); ++i)
            entryByOffset.emplace(entries[i].entryId, i);

        m_blobs.clear();
        m_blobs.reserve(entries.size());
        std::vector<bool> used(entries.size(), false);

        for (size_t k = 0; k < names.size(); ++k)
        {
            auto it = entryByOffset.find(offsets[k]);
            if (it == entryByOffset.end()) continue;
            const size_t i = it->second;
            if (used[i]) continue;
            used[i] = true;

            BlobEntry blob;
            blob.entryIndex = i;
            blob.name = names[k];

            uint64_t headerSize = 0, headerFlags = 0;
            std::string headerHash;
            std::wstring headerError;
            if (m_reader.ReadEntryHeader(entries[i], headerSize, headerHash, headerFlags, headerError))
            {
                blob.size = (size_t)headerSize;
                blob.sizeKnown = true;
            }
            m_blobs.push_back(std::move(blob));
        }

        size_t computedNames = 0;
        for (size_t i = 0; i < entries.size(); ++i)
        {
            if (used[i]) continue;
            if (m_reader.IsAsn1Section(entries[i].sectionIndex)) continue;

            MumxIdentity identity;
            std::wstring identityError;
            if (!m_reader.ReadIdentity(entries[i], identity, identityError) || !identity.valid)
                continue;

            BlobEntry blob;
            blob.entryIndex = i;
            blob.name = BuildBlobName(identity);
            m_blobs.push_back(std::move(blob));
            ++computedNames;
        }

        if (computedNames != 0)
        {
            std::stable_sort(m_blobs.begin(), m_blobs.end(),
                             [](const BlobEntry& a, const BlobEntry& b) { return NameLess(a.name, b.name); });
        }

        if (g_verbose)
            Log(L"  [names] entries={}, named={}, nameTable={}, computed={}",
                entries.size(), m_blobs.size(), names.size(), computedNames);

        m_entryIndex.clear();
        m_entryIndex.reserve(m_blobs.size());
        for (const BlobEntry& blob : m_blobs) m_entryIndex.push_back(blob.entryIndex);
    }

    void EnsureLookup()
    {
        EnsureNameCache();
        if (!m_names.empty() || m_blobs.empty()) return;
        m_names.reserve(m_blobs.size());
        m_lookup.reserve(m_blobs.size());
        for (size_t i = 0; i < m_blobs.size(); ++i)
        {
            m_names.emplace_back(m_blobs[i].name.begin(), m_blobs[i].name.end());
            m_lookup.emplace(LowerAscii(m_blobs[i].name), i);
        }
    }

    bool Find(const wchar_t* blobName, size_t& index) const
    {
        const std::wstring_view name(blobName);
        std::string key;
        key.reserve(name.size());
        for (wchar_t ch : name)
            key += (ch < 128) ? AsciiLower((char)ch) : '?';

        auto it = m_lookup.find(key);
        if (it == m_lookup.end()) return false;
        index = it->second;
        return true;
    }

    MumxNativeReader                        m_reader;
    bool                                    m_namesBuilt = false;
    bool                                    m_indexesBuilt = false;
    HRESULT                                 m_indexesStatus = S_OK;
    NativeIndexedValue                      m_lastValue;
    std::vector<BlobEntry>                  m_blobs;
    std::vector<std::wstring>               m_names;
    std::vector<size_t>                     m_entryIndex;
    std::unordered_map<std::string, size_t> m_lookup;
};

class ContainerApi
{
public:
    void AttachDll(IUpdateContainer* container) { m_dll = container; m_native = nullptr; }
    void AttachNative(NativeUpdateContainer* container) { m_native = container; m_dll = nullptr; }
    bool IsNative() const { return m_native != nullptr; }
    NativeUpdateContainer* Native() { return m_native; }

    HRESULT GetBlobCount(size_t* n)
    { return m_native ? m_native->GetBlobCount(n) : m_dll->GetBlobCount(n); }

    HRESULT GetBlobNames(const wchar_t** names, size_t capacity, size_t* n)
    { return m_native ? m_native->GetBlobNames(names, capacity, n)
                      : m_dll->GetBlobNames(names, capacity, n); }

    HRESULT GetBlobSize(const wchar_t* name, size_t* size)
    { return m_native ? m_native->GetBlobSize(name, size) : m_dll->GetBlobSize(name, size); }

    HRESULT GetBlob(const wchar_t* name, void* buffer, size_t bufferSize, size_t* written)
    { return m_native ? m_native->GetBlob(name, buffer, bufferSize, written)
                      : m_dll->GetBlob(name, buffer, bufferSize, written); }

    HRESULT DoesBlobExist(const wchar_t* name, bool* exists)
    { return m_native ? m_native->DoesBlobExist(name, exists) : m_dll->DoesBlobExist(name, exists); }

    HRESULT DoesIndexExist(const char* name, bool* exists, int* valueType)
    { return m_native ? m_native->DoesIndexExist(name, exists, valueType)
                      : m_dll->DoesIndexExist(name, exists, valueType); }

    HRESULT IndexLookup(const char* name, const char* key, void** result)
    { return m_native ? m_native->IndexLookup(name, key, result)
                      : m_dll->IndexLookup(name, key, result); }

    HRESULT GetIndexCount(size_t* count)
    { return m_native ? m_native->GetIndexCount(count) : m_dll->GetIndexCount(count); }

    HRESULT GetIndexes(void** indexes, size_t capacity, size_t* count)
    { return m_native ? m_native->GetIndexes(indexes, capacity, count)
                      : m_dll->GetIndexes(reinterpret_cast<void**>(indexes), capacity, count); }

    int GetType()
    { return m_native ? m_native->GetType() : m_dll->GetType(); }

private:
    IUpdateContainer*     m_dll = nullptr;
    NativeUpdateContainer* m_native = nullptr;
};

struct OpenedContainer
{
    ContainerHandle       dllHandle;
    NativeUpdateContainer nativeContainer;
    ContainerApi          api;
};

std::wstring LocateTurboContainerDll(const std::wstring& explicitDllPath);
bool ProbeDllEngine(const std::wstring& turboContainerDllPath, const std::wstring& containerPath);
HRESULT OpenContainerForEngine(Engine engine, const std::wstring& turboContainerDllPath,
                               const std::wstring& containerPath, OpenedContainer& opened);
