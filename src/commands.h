#pragma once

#include "common.h"
#include "engines.h"

int DumpContainerLayout(const std::wstring& containerPath);
int DumpSection(const std::wstring& containerPath, const std::wstring& sectionSelector,
                const std::wstring& outputPath);
int ListBlobs(ContainerApi& api, const std::wstring& containerPath);
int ShowIndexes(ContainerApi& api, const std::wstring& containerPath, const std::string& lookupKey);
int ExtractBlobs(ContainerApi& api, const std::wstring& containerPath,
                 const std::wstring& blobNameOrWildcard, const std::wstring& outputFolder);
int ExtractUpdateMum(ContainerApi& api, const std::wstring& outputFolder);
int SelfTest(const std::wstring& turboContainerDllPath, const std::wstring& containerPath);
