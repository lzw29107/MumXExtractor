#include "engines.h"

#pragma comment(lib, "advapi32.lib")

typedef HRESULT (WINAPI* PFN_GetIUpdateContainer)(const wchar_t* lpFileName,
                                                 IUpdateContainer** updateContainer);

static std::wstring GetExeDirectory()
{
    wchar_t modulePath[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, modulePath, (DWORD)std::size(modulePath));
    std::wstring fullPath(modulePath);
    size_t lastSlash = fullPath.find_last_of(L'\\');
    return lastSlash == std::wstring::npos ? L"." : fullPath.substr(0, lastSlash);
}

static bool FileExists(const std::wstring& filePath)
{
    DWORD attributes = GetFileAttributesW(filePath.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// "10.0.26100.5074" -> { 10, 0, 26100, 5074 }
static bool ParseVersion(const std::wstring& text, std::vector<uint32_t>& parts)
{
    parts.clear();

    uint32_t value = 0;
    bool hasDigit = false;
    for (wchar_t ch : text)
    {
        if (ch >= L'0' && ch <= L'9')
        {
            value = value * 10 + (uint32_t)(ch - L'0');
            hasDigit = true;
        }
        else if (ch == L'.' && hasDigit)
        {
            parts.push_back(value);
            value = 0;
            hasDigit = false;
        }
        else
        {
            return false;
        }
    }
    if (!hasDigit) return false;

    parts.push_back(value);
    return !parts.empty();
}

static bool VersionLess(const std::vector<uint32_t>& left, const std::vector<uint32_t>& right)
{
    const size_t count = (std::max)(left.size(), right.size());
    for (size_t i = 0; i < count; ++i)
    {
        const uint32_t a = i < left.size() ? left[i] : 0;
        const uint32_t b = i < right.size() ? right[i] : 0;
        if (a != b) return a < b;
    }
    return false;
}

// The servicing stack registers each installed version under
// HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing\Version,
// as a value whose name is the version and whose data is the WinSxS directory.
static std::wstring FindServicingStackDirectory(LSTATUS& status)
{
    static constexpr const wchar_t* kVersionKey =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Component Based Servicing\\Version";

    HKEY key = nullptr;
    status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kVersionKey, 0, KEY_READ | KEY_WOW64_64KEY, &key);
    if (status != ERROR_SUCCESS)
    {
        if (g_verbose) Log(L"  [reg] open failed, status {}", (unsigned)status);
        return std::wstring();
    }

    std::vector<uint32_t> bestVersion;
    std::wstring bestDirectory;

    wchar_t valueName[128];
    for (DWORD index = 0;; ++index)
    {
        DWORD nameLength = (DWORD)std::size(valueName);
        const LSTATUS enumStatus = RegEnumValueW(key, index, valueName, &nameLength,
                                                 nullptr, nullptr, nullptr, nullptr);
        if (enumStatus != ERROR_SUCCESS)
        {
            status = enumStatus;
            break;
        }

        std::vector<uint32_t> version;
        if (!ParseVersion(valueName, version)) continue;
        if (!bestVersion.empty() && !VersionLess(bestVersion, version)) continue;

        // The directory is stored as REG_EXPAND_SZ.
        DWORD valueType = 0;
        DWORD byteCount = 0;
        if (RegQueryValueExW(key, valueName, nullptr, &valueType, nullptr, &byteCount) !=
                ERROR_SUCCESS ||
            (valueType != REG_SZ && valueType != REG_EXPAND_SZ) ||
            byteCount < sizeof(wchar_t))
        {
            continue;
        }

        std::wstring directory(byteCount / sizeof(wchar_t), L'\0');
        if (RegQueryValueExW(key, valueName, nullptr, &valueType,
                             reinterpret_cast<BYTE*>(directory.data()), &byteCount) != ERROR_SUCCESS)
        {
            continue;
        }
        directory.resize(std::wstring_view(directory.c_str()).size());
        if (directory.empty()) continue;

        if (valueType == REG_EXPAND_SZ)
        {
            wchar_t expanded[MAX_PATH * 2] = {};
            const DWORD length = ExpandEnvironmentStringsW(directory.c_str(), expanded,
                                                           (DWORD)std::size(expanded));
            if (length != 0 && length <= std::size(expanded)) directory.assign(expanded, length - 1);
        }

        bestVersion = version;
        bestDirectory = directory;
    }

    RegCloseKey(key);
    return bestDirectory;
}

std::wstring LocateTurboContainerDll(const std::wstring& explicitDllPath)
{
    if (!explicitDllPath.empty())
        return FileExists(explicitDllPath) ? explicitDllPath : std::wstring();

    // 1) highest servicing stack version registered by CBS
    LSTATUS registryStatus = ERROR_SUCCESS;
    const std::wstring servicingStackDirectory = FindServicingStackDirectory(registryStatus);
    if (g_verbose)
    {
        Log(L"servicing stack : {}",
            servicingStackDirectory.empty() ? L"<not registered>" : servicingStackDirectory);
    }
    if (!servicingStackDirectory.empty())
    {
        const std::wstring candidate = servicingStackDirectory + L"\\TurboContainer.dll";
        if (FileExists(candidate)) return candidate;
    }

    // 2) next to this executable
    const std::wstring localCandidate = GetExeDirectory() + L"\\TurboContainer.dll";
    if (FileExists(localCandidate)) return localCandidate;

    return std::wstring();
}

static std::wstring CreateUniqueScratchDirectory()
{
    wchar_t tempPath[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempPath);
    const std::wstring directoryName =
        std::format(L"mumx_{}_{}", GetCurrentProcessId(), GetTickCount());
    std::wstring scratchDirectory = std::wstring(tempPath) + directoryName;
    CreateDirectoryW(scratchDirectory.c_str(), nullptr);
    return scratchDirectory;
}

static HRESULT OpenUpdateContainer(const std::wstring& turboContainerDllPath,
                                   const std::wstring& containerPath,
                                   ContainerHandle& handle)
{
    handle.turboContainerModule =
        LoadLibraryExW(turboContainerDllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!handle.turboContainerModule)
        return HRESULT_FROM_WIN32(GetLastError());

    PFN_GetIUpdateContainer getIUpdateContainer = reinterpret_cast<PFN_GetIUpdateContainer>(
        GetProcAddress(handle.turboContainerModule, "GetIUpdateContainer"));
    if (!getIUpdateContainer)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    IUpdateContainer* container = nullptr;
    HRESULT hr = getIUpdateContainer(containerPath.c_str(), &container);
    if (FAILED(hr)) return hr;
    if (!container) return E_NOINTERFACE;

    handle.scratchDirectory = CreateUniqueScratchDirectory();
    hr = container->Initialize(containerPath.c_str(), handle.scratchDirectory.c_str());
    if (FAILED(hr))
    {
        container->Release();
        return hr;
    }
    handle.updateContainer = container;
    return S_OK;
}


HRESULT OpenContainerForEngine(Engine engine, const std::wstring& turboContainerDllPath,
                                      const std::wstring& containerPath, OpenedContainer& opened)
{
    if (engine == Engine::Native)
    {
        HRESULT hr = opened.nativeContainer.Initialize(containerPath.c_str(), L"");
        if (SUCCEEDED(hr)) opened.api.AttachNative(&opened.nativeContainer);
        return hr;
    }

    HRESULT hr = OpenUpdateContainer(turboContainerDllPath, containerPath, opened.dllHandle);
    if (SUCCEEDED(hr)) opened.api.AttachDll(opened.dllHandle.updateContainer);
    return hr;
}

bool ProbeDllEngine(const std::wstring& turboContainerDllPath, const std::wstring& containerPath)
{
    if (turboContainerDllPath.empty()) return false;

    HMODULE module = LoadLibraryExW(turboContainerDllPath.c_str(), nullptr,
                                    LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) return false;
    auto getIUpdateContainer = reinterpret_cast<PFN_GetIUpdateContainer>(
        GetProcAddress(module, "GetIUpdateContainer"));
    FreeLibrary(module);
    if (!getIUpdateContainer) return false;

    if (containerPath.empty()) return true;

    ContainerHandle handle;
    if (FAILED(OpenUpdateContainer(turboContainerDllPath, containerPath, handle))) return false;
    size_t blobCount = 0;
    return SUCCEEDED(handle.updateContainer->GetBlobCount(&blobCount));
}
