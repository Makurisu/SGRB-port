#include "payload_loader.h"
#include <fstream>
#include <vector>
#include <android/log.h>

#define LOG_TAG "SGRE_PayloadLoader"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

bool PayloadLoader::verifyPEHeader(const std::string& filePath, GamePayloadInfo& outInfo) {
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

    // COFF Header (20 bytes)
    uint16_t machine = 0;
    uint16_t numSections = 0;
    file.read(reinterpret_cast<char*>(&machine), sizeof(machine));
    file.read(reinterpret_cast<char*>(&numSections), sizeof(numSections));

    outInfo.machineType = machine;
    outInfo.numberOfSections = numSections;

    // Bỏ qua TimeDateStamp (4), PointerToSymbolTable (4), NumberOfSymbols (4), SizeOfOptionalHeader (2), Characteristics (2)
    file.seekg(12, std::ios::cur);

    // Optional Header (PE32+ 64-bit)
    uint16_t optMagic = 0;
    file.read(reinterpret_cast<char*>(&optMagic), sizeof(optMagic));

    // Bỏ qua MajorLinkerVersion (1), MinorLinkerVersion (1), SizeOfCode (4), SizeOfInitializedData (4), SizeOfUninitializedData (4)
    file.seekg(14, std::ios::cur);

    uint32_t entryPoint = 0;
    file.read(reinterpret_cast<char*>(&entryPoint), sizeof(entryPoint));

    // Bỏ qua BaseOfCode (4)
    file.seekg(4, std::ios::cur);

    uint64_t imageBase = 0;
    file.read(reinterpret_cast<char*>(&imageBase), sizeof(imageBase));

    outInfo.entryPointRva = entryPoint;
    outInfo.imageBase = imageBase;

    LOGI("PE x86_64 xác thực: Machine=0x%X, Sections=%u, EntryPoint=0x%X, ImageBase=0x%llX",
         machine, numSections, entryPoint, static_cast<unsigned long long>(imageBase));
    return true;
}

GamePayloadInfo PayloadLoader::inspectPayload(const std::string& dataDirectory) {
    GamePayloadInfo info;
    info.dataPath = dataDirectory;
    info.exePath = dataDirectory + "/sgre_steam.exe";
    info.isExeValid = false;
    info.exeSize = 0;
    info.machineType = 0;
    info.entryPointRva = 0;
    info.imageBase = 0;
    info.numberOfSections = 0;

    std::ifstream file(info.exePath, std::ios::binary | std::ios::ate);
    if (file.is_open()) {
        info.exeSize = static_cast<uint64_t>(file.tellg());
        file.close();
        info.isExeValid = verifyPEHeader(info.exePath, info);
    } else {
        LOGE("Không tìm thấy sgre_steam.exe tại: %s", info.exePath.c_str());
    }

    return info;
}
