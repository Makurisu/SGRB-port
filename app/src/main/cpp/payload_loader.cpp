#include "payload_loader.h"
#include <fstream>
#include <vector>
#include <android/log.h>

#define LOG_TAG "SGRE_PayloadLoader"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

bool PayloadLoader::verifyPEHeader(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        LOGE("Không thể mở file binary: %s", filePath.c_str());
        return false;
    }

    uint16_t dosHeader = 0;
    file.read(reinterpret_cast<char*>(&dosHeader), sizeof(dosHeader));
    // Kiểm tra Magic 'MZ' = 0x5A4D
    if (dosHeader != 0x5A4D) {
        LOGE("File không có chữ ký DOS MZ hợp lệ: 0x%X", dosHeader);
        return false;
    }

    // Nhảy tới offset PE header tại 0x3C
    file.seekg(0x3C, std::ios::beg);
    uint32_t peOffset = 0;
    file.read(reinterpret_cast<char*>(&peOffset), sizeof(peOffset));

    file.seekg(peOffset, std::ios::beg);
    uint32_t peSignature = 0;
    file.read(reinterpret_cast<char*>(&peSignature), sizeof(peSignature));

    // Kiểm tra chữ ký 'PE\0\0' = 0x00004550
    if (peSignature != 0x00004550) {
        LOGE("File không có chữ ký PE hợp lệ: 0x%X", peSignature);
        return false;
    }

    LOGI("Đã xác thực thành công file PE x86_64: %s", filePath.c_str());
    return true;
}

GamePayloadInfo PayloadLoader::inspectPayload(const std::string& dataDirectory) {
    GamePayloadInfo info;
    info.dataPath = dataDirectory;
    info.exePath = dataDirectory + "/sgre_steam.exe";
    info.isExeValid = false;
    info.exeSize = 0;

    std::ifstream file(info.exePath, std::ios::binary | std::ios::ate);
    if (file.is_open()) {
        info.exeSize = static_cast<uint64_t>(file.tellg());
        file.close();
        info.isExeValid = verifyPEHeader(info.exePath);
    } else {
        LOGE("Không tìm thấy sgre_steam.exe tại: %s", info.exePath.c_str());
    }

    return info;
}
