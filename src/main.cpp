#include "common.h"
#include "console.h"
#include "commands.h"
#include "engines.h"

static void Usage()
{
    WriteText(LR"(usage:
  MumXExtractor info    <container>                         show header and section table
  MumXExtractor dump    <container> <index|all> <outPath>    decompress a section payload
  MumXExtractor list    <container>                          list blobs
  MumXExtractor index   <container> [key]                    list indexes and look up a key
  MumXExtractor extract <container> <name|*> <outDir>        extract one or all blobs
  MumXExtractor mum     <container> <outDir>                 extract update.mum only
  MumXExtractor selftest [container]                         run engine self-tests

options:
  --engine auto|dll|native   auto (default) prefers the DLL and falls back to native
  --dll <path>               explicit TurboContainer.dll path
  --exact                    list: report rehydrated sizes (slow, byte exact)
  -v                         verbose output
)");
}

static bool ParseEngine(const std::wstring& text, Engine& engine)
{
    if (text == L"auto")   { engine = Engine::Auto;   return true; }
    if (text == L"dll")    { engine = Engine::Dll;    return true; }
    if (text == L"native") { engine = Engine::Native; return true; }
    return false;
}

int wmain(int argc, wchar_t** argv)
{
    InitConsole();

    std::vector<std::wstring> positionalArgs;
    std::wstring explicitDllPath;
    Engine requestedEngine = Engine::Auto;

    for (int i = 1; i < argc; ++i)
    {
        std::wstring arg = argv[i];
        if (arg == L"--dll" && i + 1 < argc) { explicitDllPath = argv[++i]; continue; }
        if (arg == L"--engine" && i + 1 < argc)
        {
            if (!ParseEngine(ToLower(argv[++i]), requestedEngine))
            {
                LogBanner(nullptr, L"");
                Log(L"[!] --engine accepts auto, dll or native only");
                return 2;
            }
            continue;
        }
        if (arg == L"-v" || arg == L"--verbose") { g_verbose = 1; continue; }
        if (arg == L"--exact") { g_exactSizes = true; continue; }
        positionalArgs.push_back(arg);
    }

    if (positionalArgs.empty()) { LogBanner(nullptr, L""); Usage(); return 2; }

    const std::wstring command = ToLower(positionalArgs[0]);

    size_t requiredArgs = 0;
    if      (command == L"info")     requiredArgs = 2;
    else if (command == L"dump")     requiredArgs = 4;
    else if (command == L"selftest") requiredArgs = 1;
    else if (command == L"list")     requiredArgs = 2;
    else if (command == L"index")    requiredArgs = 2;
    else if (command == L"extract")  requiredArgs = 4;
    else if (command == L"mum")      requiredArgs = 3;

    const bool argsValid = requiredArgs != 0 && positionalArgs.size() >= requiredArgs;
    LogBanner(argsValid ? command.c_str() : nullptr,
              argsValid && positionalArgs.size() >= 2 ? positionalArgs[1] : L"");
    if (!argsValid) { Usage(); return 2; }

    std::wstring turboContainerDllPath = LocateTurboContainerDll(explicitDllPath);

    if (command == L"info")
    {
        if (positionalArgs.size() < 2) { Usage(); return 2; }
        return DumpContainerLayout(positionalArgs[1]);
    }
    if (command == L"dump")
    {
        if (positionalArgs.size() < 4) { Usage(); return 2; }
        return DumpSection(positionalArgs[1], positionalArgs[2], positionalArgs[3]);
    }
    if (command == L"selftest")
    {
        return SelfTest(turboContainerDllPath,
                        positionalArgs.size() >= 2 ? positionalArgs[1] : L"");
    }

    if (command != L"list" && command != L"index" && command != L"extract" && command != L"mum")
    {
        Usage();
        return 2;
    }

    std::wstring containerPath = positionalArgs.size() >= 2 ? positionalArgs[1] : L"";

    Engine engine = requestedEngine;
    if (engine == Engine::Auto)
        engine = ProbeDllEngine(turboContainerDllPath, containerPath) ? Engine::Dll : Engine::Native;

    if (engine == Engine::Dll && turboContainerDllPath.empty())
    {
        Log(L"[!] TurboContainer.dll not found (use --dll, or --engine native)");
        return 3;
    }

    OpenedContainer opened;
    HRESULT openHr = OpenContainerForEngine(engine, turboContainerDllPath, containerPath, opened);
    if (FAILED(openHr))
    {
        LogHr(engine == Engine::Native ? L"NativeUpdateContainer::Initialize"
                                       : L"OpenUpdateContainer", openHr);
        return 1;
    }
    if (g_verbose) Log(L"engine    : {}", engine == Engine::Native ? L"native" : L"dll");

    if (command == L"list")
    {
        if (positionalArgs.size() < 2) { Usage(); return 2; }
        return ListBlobs(opened.api, positionalArgs[1]);
    }
    if (command == L"index")
    {
        std::string lookupKey;
        if (positionalArgs.size() >= 3)
            lookupKey.assign(positionalArgs[2].begin(), positionalArgs[2].end());
        return ShowIndexes(opened.api, positionalArgs[1], lookupKey);
    }
    if (command == L"extract")
    {
        if (positionalArgs.size() < 4) { Usage(); return 2; }
        return ExtractBlobs(opened.api, positionalArgs[1], positionalArgs[2], positionalArgs[3]);
    }
    if (command == L"mum")
    {
        if (positionalArgs.size() < 3) { Usage(); return 2; }
        return ExtractUpdateMum(opened.api, positionalArgs[2]);
    }

    Usage();
    return 2;
}
