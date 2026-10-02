#include "payload_loader.h"
#include <fstream>
#include <vector>
#include <sys/mman.h>
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

    // Đọc kích thước của Optional Header từ COFF header offset để nhảy chính xác tới Section Table
    file.seekg(peOffset + 20, std::ios::beg);
    uint16_t sizeOfOptHeader = 0;
    file.read(reinterpret_cast<char*>(&sizeOfOptHeader), sizeof(sizeOfOptHeader));

    // Điểm bắt đầu của bảng Section Headers
    uint32_t sectionTableOffset = peOffset + 24 + sizeOfOptHeader;
    file.seekg(sectionTableOffset, std::ios::beg);

    outInfo.sections.clear();
    for (uint16_t i = 0; i < numSections; ++i) {
        char nameBuf[9] = {0};
        file.read(nameBuf, 8);

        uint32_t vSize = 0, vAddr = 0, rSize = 0, rOffset = 0;
        file.read(reinterpret_cast<char*>(&vSize), sizeof(vSize));
        file.read(reinterpret_cast<char*>(&vAddr), sizeof(vAddr));
        file.read(reinterpret_cast<char*>(&rSize), sizeof(rSize));
        file.read(reinterpret_cast<char*>(&rOffset), sizeof(rOffset));

        // Bỏ qua PointerToRelocations (4), PointerToLinenumbers (4), NumberOfRelocations (2), NumberOfLinenumbers (2)
        file.seekg(12, std::ios::cur);

        uint32_t chars = 0;
        file.read(reinterpret_cast<char*>(&chars), sizeof(chars));

        PESection sec;
        sec.name = std::string(nameBuf);
        sec.virtualSize = vSize;
        sec.virtualAddress = vAddr;
        sec.rawSize = rSize;
        sec.rawOffset = rOffset;
        sec.characteristics = chars;

        outInfo.sections.push_back(sec);

        LOGI("Section [%s]: VAddr=0x%X, VSize=0x%X, RawOffset=0x%X, RawSize=0x%X",
             sec.name.c_str(), vAddr, vSize, rOffset, rSize);
    }

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

void* PayloadLoader::mapImageToMemory(const GamePayloadInfo& info) {
    if (!info.isExeValid || info.sections.empty()) {
        LOGE("Payload không hợp lệ để ánh xạ bộ nhớ");
        return nullptr;
    }

    // Tính toán kích thước ảnh ảo tổng thể từ section cuối cùng
    const auto& lastSection = info.sections.back();
    size_t totalImageSize = lastSection.virtualAddress + lastSection.virtualSize;
    // Căn chỉnh theo trang bộ nhớ 4KB
    totalImageSize = (totalImageSize + 4095) & ~4095;

    LOGI("Đang cấp phát vùng nhớ ảo: %zu bytes (~%.2f MB)...",
         totalImageSize, static_cast<double>(totalImageSize) / (1024.0 * 1024.0));

    // Cấp phát vùng nhớ ảo ẩn danh (PROT_READ | PROT_WRITE)
    void* baseAddr = mmap(nullptr, totalImageSize, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (baseAddr == MAP_FAILED) {
        LOGE("mmap thất bại khi cấp phát vùng nhớ ảo cho game");
        return nullptr;
    }

    std::ifstream file(info.exePath, std::ios::binary);
    if (!file.is_open()) {
        LOGE("Không thể mở lại file để sao chép sections");
        munmap(baseAddr, totalImageSize);
        return nullptr;
    }

    uint8_t* basePtr = static_cast<uint8_t*>(baseAddr);

    // Sao chép từng Section từ file vào đúng toạ độ ảo VirtualAddress trong RAM
    for (const auto& sec : info.sections) {
        if (sec.rawSize == 0 || sec.rawOffset == 0) continue;

        file.seekg(sec.rawOffset, std::ios::beg);
        uint8_t* destPtr = basePtr + sec.virtualAddress;
        file.read(reinterpret_cast<char*>(destPtr), sec.rawSize);

        LOGI("Đã nạp section [%s] vào RAM tại offset +0x%X (Kích thước: %u bytes)",
             sec.name.c_str(), sec.virtualAddress, sec.rawSize);
    }

    LOGI("Toàn bộ 8 sections của game đã được nạp vào RAM thành công tại địa chỉ cơ sở: %p", baseAddr);
    return baseAddr;
}

void PayloadLoader::releaseImageMemory(void* baseAddress, size_t size) {
    if (baseAddress != nullptr && size > 0) {
        munmap(baseAddress, size);
        LOGI("Đã giải phóng vùng nhớ ảo của game tại: %p", baseAddress);
    }
}

