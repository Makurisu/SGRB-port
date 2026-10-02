#pragma once
#include <string>
#include <cstdint>

struct GamePayloadInfo {
    std::string exePath;
    std::string dataPath;
    bool isExeValid;
    uint64_t exeSize;
    uint16_t machineType;
    uint32_t entryPointRva;
    uint64_t imageBase;
    uint16_t numberOfSections;
};

class PayloadLoader {
public:
    static GamePayloadInfo inspectPayload(const std::string& dataDirectory);
    static bool verifyPEHeader(const std::string& filePath, GamePayloadInfo& outInfo);
};
