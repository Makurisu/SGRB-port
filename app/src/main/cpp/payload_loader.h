#pragma once
#include <string>
#include <cstdint>

struct GamePayloadInfo {
    std::string exePath;
    std::string dataPath;
    bool isExeValid;
    uint64_t exeSize;
};

class PayloadLoader {
public:
    static GamePayloadInfo inspectPayload(const std::string& dataDirectory);
    static bool verifyPEHeader(const std::string& filePath);
};
