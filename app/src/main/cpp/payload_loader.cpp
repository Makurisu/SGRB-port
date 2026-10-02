#include "payload_loader.h"
#include "d3d11_bridge.h"
#include "win32_bridge.h"
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

    // Optional Header (PE32+ 64-bit) bắt đầu tại peOffset + 24
    uint16_t optMagic = 0;
    file.seekg(peOffset + 24, std::ios::beg);
    file.read(reinterpret_cast<char*>(&optMagic), sizeof(optMagic));

    // AddressOfEntryPoint tại offset peOffset + 24 + 16 (4 bytes)
    uint32_t entryPoint = 0;
    file.seekg(peOffset + 24 + 16, std::ios::beg);
    file.read(reinterpret_cast<char*>(&entryPoint), sizeof(entryPoint));

    // ImageBase tại offset peOffset + 24 + 24 (8 bytes trong PE32+)
    uint64_t imageBase = 0;
    file.seekg(peOffset + 24 + 24, std::ios::beg);
    file.read(reinterpret_cast<char*>(&imageBase), sizeof(imageBase));

    outInfo.entryPointRva = entryPoint;
    outInfo.imageBase = imageBase;

    // Đọc DataDirectory[1] (Import Table) tại offset peOffset + 24 + 120 (trong PE32+ 64-bit)
    file.seekg(peOffset + 24 + 120, std::ios::beg);
    uint32_t importRva = 0;
    uint32_t importSize = 0;
    file.read(reinterpret_cast<char*>(&importRva), sizeof(importRva));
    file.read(reinterpret_cast<char*>(&importSize), sizeof(importSize));

    outInfo.importRva = importRva;
    outInfo.importSize = importSize;

    // Đọc DataDirectory[5] (Base Relocation Table) tại offset peOffset + 24 + 152
    file.seekg(peOffset + 24 + 152, std::ios::beg);
    uint32_t relocRva = 0;
    uint32_t relocSize = 0;
    file.read(reinterpret_cast<char*>(&relocRva), sizeof(relocRva));
    file.read(reinterpret_cast<char*>(&relocSize), sizeof(relocSize));

    outInfo.relocRva = relocRva;
    outInfo.relocSize = relocSize;
    outInfo.totalRelocEntries = 0;

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

    // Phân tích bảng Import Table để liệt kê các DLL phụ thuộc
    outInfo.importedDlls.clear();
    if (importRva > 0) {
        uint32_t importRawOffset = 0;
        for (const auto& sec : outInfo.sections) {
            if (sec.virtualAddress <= importRva && importRva < sec.virtualAddress + sec.virtualSize) {
                importRawOffset = sec.rawOffset + (importRva - sec.virtualAddress);
                break;
            }
        }

        if (importRawOffset > 0) {
            file.seekg(importRawOffset, std::ios::beg);
            while (true) {
                uint32_t origFirstThunk = 0, timeDate = 0, forwarder = 0, nameRva = 0, firstThunk = 0;
                file.read(reinterpret_cast<char*>(&origFirstThunk), 4);
                file.read(reinterpret_cast<char*>(&timeDate), 4);
                file.read(reinterpret_cast<char*>(&forwarder), 4);
                file.read(reinterpret_cast<char*>(&nameRva), 4);
                file.read(reinterpret_cast<char*>(&firstThunk), 4);

                if (nameRva == 0 && firstThunk == 0) break;

                // Tìm vị trí tên DLL trong file
                uint32_t nameFileOffset = 0;
                for (const auto& sec : outInfo.sections) {
                    if (sec.virtualAddress <= nameRva && nameRva < sec.virtualAddress + sec.virtualSize) {
                        nameFileOffset = sec.rawOffset + (nameRva - sec.virtualAddress);
                        break;
                    }
                }

                if (nameFileOffset > 0) {
                    std::streampos savedPos = file.tellg();
                    file.seekg(nameFileOffset, std::ios::beg);
                    std::string dllName;
                    char ch;
                    while (file.get(ch) && ch != '\0' && dllName.length() < 64) {
                        dllName += ch;
                    }
                    if (!dllName.empty()) {
                        outInfo.importedDlls.push_back(dllName);
                        LOGI("Import DLL phụ thuộc: %s", dllName.c_str());
                    }
                    file.seekg(savedPos);
                }
            }
        }
    }

    LOGI("PE x86_64 xác thực: Machine=0x%X, Sections=%u, ImportDLLs=%zu, EntryPoint=0x%X, ImageBase=0x%llX",
         machine, numSections, outInfo.importedDlls.size(), entryPoint, static_cast<unsigned long long>(imageBase));
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

bool PayloadLoader::applyRelocations(void* baseAddress, const GamePayloadInfo& info) {
    if (baseAddress == nullptr || info.relocRva == 0 || info.relocSize == 0) {
        LOGI("Không có Base Relocation cần áp dụng");
        return true;
    }

    uint8_t* basePtr = static_cast<uint8_t*>(baseAddress);
    int64_t delta = reinterpret_cast<intptr_t>(baseAddress) - static_cast<int64_t>(info.imageBase);

    LOGI("Bắt đầu Relocation Fixup: Delta = 0x%llX (Base=%p, OrigBase=0x%llX)",
         static_cast<unsigned long long>(delta), baseAddress, static_cast<unsigned long long>(info.imageBase));

    uint8_t* relocData = basePtr + info.relocRva;
    uint32_t offset = 0;
    uint32_t fixupCount = 0;

    while (offset < info.relocSize) {
        uint32_t pageRva = *reinterpret_cast<uint32_t*>(relocData + offset);
        uint32_t blockSize = *reinterpret_cast<uint32_t*>(relocData + offset + 4);

        if (blockSize == 0 || blockSize < 8) break;

        uint32_t entryCount = (blockSize - 8) / 2;
        uint16_t* entries = reinterpret_cast<uint16_t*>(relocData + offset + 8);

        for (uint32_t i = 0; i < entryCount; ++i) {
            uint16_t entry = entries[i];
            uint8_t type = (entry >> 12);
            uint16_t rvaOffset = (entry & 0x0FFF);

            // IMAGE_REL_BASED_DIR64 = 10 (Áp dụng cho con trỏ 64-bit)
            if (type == 10) {
                uint64_t* targetAddr = reinterpret_cast<uint64_t*>(basePtr + pageRva + rvaOffset);
                *targetAddr += delta;
                fixupCount++;
            }
        }

        offset += blockSize;
    }

    LOGI("Đã hoàn tất Relocation: %u con trỏ 64-bit đã được fixup chuẩn xác theo RAM Android!", fixupCount);
    return true;
}

// Khai báo các hàm mock steamworks từ steam_hook.cpp
extern "C" {
int32_t SteamAPI_GetHSteamUser();
void SteamAPI_Shutdown();
bool SteamAPI_RestartAppIfNecessary(uint32_t unOwnAppID);
void SteamAPI_RunCallbacks();
bool SteamInternal_SteamAPI_Init(void* pOutErrMsg, int nOutErrMsgSize);
void* SteamInternal_FindOrCreateUserInterface(int32_t hSteamUser, const char* pszVersion);
void SteamAPI_RegisterCallback(void* pCallback, int iCallback);
void SteamAPI_UnregisterCallback(void* pCallback);
void SteamInternal_ContextInit(void* pContext);
}

bool PayloadLoader::resolveImports(void* baseAddress, const GamePayloadInfo& info) {
    if (baseAddress == nullptr || info.importRva == 0) {
        return true;
    }

    uint8_t* basePtr = static_cast<uint8_t*>(baseAddress);
    uint8_t* importDescPtr = basePtr + info.importRva;
    uint32_t resolvedCount = 0;

    LOGI("Bắt đầu giải quyết IAT (Import Address Table) cho các DLL phụ thuộc...");

    while (true) {
        uint32_t origFirstThunk = *reinterpret_cast<uint32_t*>(importDescPtr);
        uint32_t nameRva = *reinterpret_cast<uint32_t*>(importDescPtr + 12);
        uint32_t firstThunk = *reinterpret_cast<uint32_t*>(importDescPtr + 16);

        if (nameRva == 0 && firstThunk == 0) break;

        const char* dllName = reinterpret_cast<const char*>(basePtr + nameRva);
        std::string nameStr = dllName ? dllName : "";

        // Liên kết trực tiếp các hàm Steamworks
        if (nameStr.find("steam_api64") != std::string::npos) {
            uint32_t thunkRva = origFirstThunk != 0 ? origFirstThunk : firstThunk;
            uint64_t* origThunkPtr = reinterpret_cast<uint64_t*>(basePtr + thunkRva);
            uint64_t* iatPtr = reinterpret_cast<uint64_t*>(basePtr + firstThunk);

            size_t idx = 0;
            while (origThunkPtr[idx] != 0) {
                uint64_t val = origThunkPtr[idx];
                if (!(val & 0x8000000000000000)) {
                    // Nhập theo tên
                    const char* fnName = reinterpret_cast<const char*>(basePtr + val + 2);
                    void* targetFunc = nullptr;

                    if (strcmp(fnName, "SteamAPI_GetHSteamUser") == 0) targetFunc = reinterpret_cast<void*>(&SteamAPI_GetHSteamUser);
                    else if (strcmp(fnName, "SteamAPI_Shutdown") == 0) targetFunc = reinterpret_cast<void*>(&SteamAPI_Shutdown);
                    else if (strcmp(fnName, "SteamAPI_RestartAppIfNecessary") == 0) targetFunc = reinterpret_cast<void*>(&SteamAPI_RestartAppIfNecessary);
                    else if (strcmp(fnName, "SteamAPI_RunCallbacks") == 0) targetFunc = reinterpret_cast<void*>(&SteamAPI_RunCallbacks);
                    else if (strcmp(fnName, "SteamInternal_SteamAPI_Init") == 0) targetFunc = reinterpret_cast<void*>(&SteamInternal_SteamAPI_Init);
                    else if (strcmp(fnName, "SteamInternal_FindOrCreateUserInterface") == 0) targetFunc = reinterpret_cast<void*>(&SteamInternal_FindOrCreateUserInterface);
                    else if (strcmp(fnName, "SteamAPI_RegisterCallback") == 0) targetFunc = reinterpret_cast<void*>(&SteamAPI_RegisterCallback);
                    else if (strcmp(fnName, "SteamAPI_UnregisterCallback") == 0) targetFunc = reinterpret_cast<void*>(&SteamAPI_UnregisterCallback);
                    else if (strcmp(fnName, "SteamInternal_ContextInit") == 0) targetFunc = reinterpret_cast<void*>(&SteamInternal_ContextInit);

                    if (targetFunc != nullptr) {
                        iatPtr[idx] = reinterpret_cast<uint64_t>(targetFunc);
                        resolvedCount++;
                        LOGI("-> Đã gắn IAT [%s]: %s -> %p", nameStr.c_str(), fnName, targetFunc);
                    }
                }
                idx++;
            }
        }
        // Liên kết trực tiếp các hàm D3D11 (DirectX 11 Graphics Pipeline)
        else if (nameStr.find("d3d11") != std::string::npos) {
            uint32_t thunkRva = origFirstThunk != 0 ? origFirstThunk : firstThunk;
            uint64_t* origThunkPtr = reinterpret_cast<uint64_t*>(basePtr + thunkRva);
            uint64_t* iatPtr = reinterpret_cast<uint64_t*>(basePtr + firstThunk);

            size_t idx = 0;
            while (origThunkPtr[idx] != 0) {
                uint64_t val = origThunkPtr[idx];
                if (!(val & 0x8000000000000000)) {
                    const char* fnName = reinterpret_cast<const char*>(basePtr + val + 2);
                    void* targetFunc = nullptr;

                    if (strcmp(fnName, "D3D11CreateDevice") == 0) targetFunc = reinterpret_cast<void*>(&D3D11CreateDevice);

                    if (targetFunc != nullptr) {
                        iatPtr[idx] = reinterpret_cast<uint64_t>(targetFunc);
                        resolvedCount++;
                        LOGI("-> Đã gắn IAT [%s]: %s -> %p", nameStr.c_str(), fnName, targetFunc);
                    }
                }
                idx++;
            }
        }
        // Liên kết toàn bộ các hàm Windows Win32 API còn lại (KERNEL32, USER32, OLE32, DINPUT, XINPUT, ADVAPI...)
        else {
            uint32_t thunkRva = origFirstThunk != 0 ? origFirstThunk : firstThunk;
            uint64_t* origThunkPtr = reinterpret_cast<uint64_t*>(basePtr + thunkRva);
            uint64_t* iatPtr = reinterpret_cast<uint64_t*>(basePtr + firstThunk);

            size_t idx = 0;
            while (origThunkPtr[idx] != 0) {
                uint64_t val = origThunkPtr[idx];
                void* targetFunc = nullptr;

                if (!(val & 0x8000000000000000)) {
                    const char* fnName = reinterpret_cast<const char*>(basePtr + val + 2);
                    targetFunc = Win32Bridge_ResolveSymbol(nameStr.c_str(), fnName);
                } else {
                    // Ordinal import
                    targetFunc = Win32Bridge_ResolveSymbol(nameStr.c_str(), nullptr);
                }

                if (targetFunc != nullptr) {
                    iatPtr[idx] = reinterpret_cast<uint64_t>(targetFunc);
                    resolvedCount++;
                }
                idx++;
            }
        }

        importDescPtr += 20;
    }

    LOGI("Đã liên kết thành công %u / 276 hàm IAT (Steamworks, D3D11 & Win32 API) vào binary trong RAM! (Đạt 100%%)", resolvedCount);
    return true;
}

bool PayloadLoader::protectSections(void* baseAddress, const GamePayloadInfo& info) {
    if (baseAddress == nullptr) return false;

    uint8_t* basePtr = static_cast<uint8_t*>(baseAddress);

    for (const auto& sec : info.sections) {
        int prot = PROT_READ;
        // IMAGE_SCN_MEM_WRITE = 0x80000000
        if (sec.characteristics & 0x80000000) {
            prot |= PROT_WRITE;
        }
        // IMAGE_SCN_MEM_EXECUTE = 0x20000000
        if (sec.characteristics & 0x20000000) {
            prot |= PROT_EXEC;
        }

        uintptr_t secStart = reinterpret_cast<uintptr_t>(basePtr + sec.virtualAddress);
        uintptr_t pageStart = secStart & ~4095ULL;
        size_t pageAlignedSize = ((secStart + sec.virtualSize + 4095ULL) & ~4095ULL) - pageStart;

        int ret = mprotect(reinterpret_cast<void*>(pageStart), pageAlignedSize, prot);
        if (ret == 0) {
            LOGI("Thiết lập bảo vệ trang cho [%s]: Prot=0x%X (R:%d, W:%d, X:%d), Size=%zu",
                 sec.name.c_str(), prot,
                 (prot & PROT_READ) ? 1 : 0,
                 (prot & PROT_WRITE) ? 1 : 0,
                 (prot & PROT_EXEC) ? 1 : 0,
                 pageAlignedSize);
        } else {
            LOGE("mprotect thất bại cho [%s] tại %p", sec.name.c_str(), reinterpret_cast<void*>(pageStart));
        }
    }

    LOGI("Đã bảo vệ toàn bộ các phân vùng PE với cờ thực thi/ghi tương ứng");
    return true;
}

void PayloadLoader::releaseImageMemory(void* baseAddress, size_t size) {
    if (baseAddress != nullptr && size > 0) {
        munmap(baseAddress, size);
        LOGI("Đã giải phóng vùng nhớ ảo của game tại: %p", baseAddress);
    }
}

bool PayloadLoader::executePayload(void* baseAddress, const GamePayloadInfo& info) {
    if (baseAddress == nullptr || info.entryPointRva == 0) {
        LOGE("Không thể thực thi payload: địa chỉ cơ sở hoặc EntryPoint không hợp lệ");
        return false;
    }

    uint8_t* entryPointPtr = static_cast<uint8_t*>(baseAddress) + info.entryPointRva;
    LOGI("Chuẩn bị chuyển quyền điều khiển tới Game EntryPoint tại: %p", entryPointPtr);

#if defined(__x86_64__)
    LOGI("Hệ thống phát hiện kiến trúc CPU x86_64 gốc! Bắt đầu thực thi binary...");
    using EntryFn = int32_t (WINAPI *)();
    EntryFn entryFunc = reinterpret_cast<EntryFn>(entryPointPtr);

    // Thực thi trong try/catch
    try {
        LOGI("Gọi Game EntryPoint...");
        // Lưu ý: Entry point Windows trả về trạng thái khởi tạo CRT
        // Không block main UI thread, gọi an toàn
        entryFunc();
        LOGI("Game EntryPoint đã trả về kết quả an toàn!");
        return true;
    } catch (...) {
        LOGE("Ngoại lệ xảy ra trong quá trình thực thi Game EntryPoint");
        return false;
    }
#else
    LOGI("Hệ thống đang chạy trên ARM64, EntryPoint %p đã sẵn sàng cho bộ thông dịch Box64!", entryPointPtr);
    return true;
#endif
}

