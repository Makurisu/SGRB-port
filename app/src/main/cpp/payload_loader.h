#pragma once
#include <string>
#include <cstdint>
#include <vector>

struct PESection {
    std::string name;
    uint32_t virtualSize;
    uint32_t virtualAddress;
    uint32_t rawSize;
    uint32_t rawOffset;
    uint32_t characteristics;
};

struct GamePayloadInfo {
    std::string exePath;
    std::string dataPath;
    bool isExeValid;
    uint64_t exeSize;
    uint16_t machineType;
    uint32_t entryPointRva;
    uint64_t imageBase;
    uint16_t numberOfSections;
    std::vector<PESection> sections;
    uint32_t importRva;
    uint32_t importSize;
    std::vector<std::string> importedDlls;
};

class PayloadLoader {
public:
    static GamePayloadInfo inspectPayload(const std::string& dataDirectory);
    static bool verifyPEHeader(const std::string& filePath, GamePayloadInfo& outInfo);
    static void* mapImageToMemory(const GamePayloadInfo& info);
    static void releaseImageMemory(void* baseAddress, size_t size);
};
