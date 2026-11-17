#include "dxbc_reflector.h"
#include <cstring>

namespace sgre {

namespace {

inline uint16_t readU16LE(const uint8_t* ptr) {
    uint16_t val = 0;
    std::memcpy(&val, ptr, sizeof(uint16_t));
    return val;
}

inline uint32_t readU32LE(const uint8_t* ptr) {
    uint32_t val = 0;
    std::memcpy(&val, ptr, sizeof(uint32_t));
    return val;
}

std::string readNullTerminatedString(const uint8_t* base, size_t maxLen, uint32_t offset) {
    if (offset >= maxLen) {
        return "";
    }
    const char* start = reinterpret_cast<const char*>(base + offset);
    size_t limit = maxLen - offset;
    size_t len = 0;
    while (len < limit && start[len] != '\0') {
        ++len;
    }
    return std::string(start, len);
}

}  // namespace

bool parseDxbcRdef(const void* bytecode, size_t bytecodeSize, DxbcReflectionData& outData) {
    outData = DxbcReflectionData{};
    if (bytecode == nullptr || bytecodeSize < 36) {
        return false;
    }

    const auto* bytes = static_cast<const uint8_t*>(bytecode);
    if (std::memcmp(bytes, "DXBC", 4) != 0) {
        return false;
    }

    uint32_t totalSize = readU32LE(bytes + 24);
    if (totalSize > bytecodeSize || totalSize < 32) {
        return false;
    }

    uint32_t numChunks = readU32LE(bytes + 28);
    if (32 + static_cast<size_t>(numChunks) * 4 > bytecodeSize) {
        return false;
    }

    const uint8_t* rdefChunk = nullptr;
    size_t rdefSize = 0;

    for (uint32_t i = 0; i < numChunks; ++i) {
        uint32_t chunkOffset = readU32LE(bytes + 32 + i * 4);
        if (chunkOffset + 8 > bytecodeSize) {
            return false;
        }
        const uint8_t* chunkHeader = bytes + chunkOffset;
        uint32_t chunkSize = readU32LE(chunkHeader + 4);
        if (chunkOffset + 8 + static_cast<size_t>(chunkSize) > bytecodeSize) {
            return false;
        }
        if (std::memcmp(chunkHeader, "RDEF", 4) == 0) {
            rdefChunk = chunkHeader + 8;
            rdefSize = chunkSize;
            break;
        }
    }

    if (rdefChunk == nullptr || rdefSize < 28) {
        return false;
    }

    uint32_t cbCount = readU32LE(rdefChunk + 0);
    uint32_t cbOffset = readU32LE(rdefChunk + 4);
    uint32_t rbCount = readU32LE(rdefChunk + 8);
    uint32_t rbOffset = readU32LE(rdefChunk + 12);
    uint32_t targetVersion = readU32LE(rdefChunk + 16);
    uint32_t flags = readU32LE(rdefChunk + 20);
    uint32_t creatorOffset = readU32LE(rdefChunk + 24);

    outData.version = targetVersion;
    outData.flags = flags;
    if (creatorOffset > 0 && creatorOffset < rdefSize) {
        outData.creator = readNullTerminatedString(rdefChunk, rdefSize, creatorOffset);
    }

    // Trong RDEF version >= 0x500 (SM 5.0), mỗi variable descriptor dài 40 bytes;
    // trong SM 4.0 (0x400), mỗi variable descriptor dài 24 bytes.
    size_t varStride = ((targetVersion & 0xFFFF) >= 0x0500) ? 40 : 24;

    if (cbCount > 0) {
        if (cbOffset + static_cast<size_t>(cbCount) * 24 > rdefSize) {
            return false;
        }
        outData.constantBuffers.reserve(cbCount);
        for (uint32_t c = 0; c < cbCount; ++c) {
            const uint8_t* cbPtr = rdefChunk + cbOffset + c * 24;
            uint32_t nameOffset = readU32LE(cbPtr + 0);
            uint32_t varCount = readU32LE(cbPtr + 4);
            uint32_t varOffset = readU32LE(cbPtr + 8);
            uint32_t cbSize = readU32LE(cbPtr + 12);
            uint32_t cbFlags = readU32LE(cbPtr + 16);
            uint32_t cbType = readU32LE(cbPtr + 20);

            if (varCount > 0 && varOffset + static_cast<size_t>(varCount) * varStride > rdefSize) {
                return false;
            }

            DxbcConstantBufferInfo cbInfo{};
            cbInfo.name = readNullTerminatedString(rdefChunk, rdefSize, nameOffset);
            cbInfo.size = cbSize;
            cbInfo.uFlags = cbFlags;
            cbInfo.type = cbType;
            cbInfo.variables.reserve(varCount);

            for (uint32_t v = 0; v < varCount; ++v) {
                const uint8_t* varPtr = rdefChunk + varOffset + v * varStride;
                uint32_t vNameOffset = readU32LE(varPtr + 0);
                uint32_t startOffset = readU32LE(varPtr + 4);
                uint32_t vSize = readU32LE(varPtr + 8);
                uint32_t vFlags = readU32LE(varPtr + 12);
                uint32_t vTypeOffset = readU32LE(varPtr + 16);

                DxbcVariableInfo varInfo{};
                varInfo.name = readNullTerminatedString(rdefChunk, rdefSize, vNameOffset);
                varInfo.startOffset = startOffset;
                varInfo.size = vSize;
                varInfo.uFlags = vFlags;

                if (vTypeOffset + 16 <= rdefSize) {
                    const uint8_t* typePtr = rdefChunk + vTypeOffset;
                    varInfo.typeInfo.varClass = readU16LE(typePtr + 0);
                    varInfo.typeInfo.varType = readU16LE(typePtr + 2);
                    varInfo.typeInfo.rows = readU16LE(typePtr + 4);
                    varInfo.typeInfo.columns = readU16LE(typePtr + 6);
                    uint16_t rawElems = readU16LE(typePtr + 8);
                    // sub_140565020 kiểm tra Elements != 0
                    varInfo.typeInfo.elements = (rawElems > 0) ? rawElems : 1;
                    varInfo.typeInfo.members = readU16LE(typePtr + 10);
                    varInfo.typeInfo.offset = readU32LE(typePtr + 12);
                }

                cbInfo.variables.push_back(std::move(varInfo));
            }

            outData.constantBuffers.push_back(std::move(cbInfo));
        }
    }

    if (rbCount > 0) {
        if (rbOffset + static_cast<size_t>(rbCount) * 32 > rdefSize) {
            return false;
        }
        outData.boundResources.reserve(rbCount);
        for (uint32_t r = 0; r < rbCount; ++r) {
            const uint8_t* rbPtr = rdefChunk + rbOffset + r * 32;
            DxbcResourceBindingInfo rbInfo{};
            rbInfo.name = readNullTerminatedString(rdefChunk, rdefSize, readU32LE(rbPtr + 0));
            rbInfo.type = readU32LE(rbPtr + 4);
            rbInfo.returnType = readU32LE(rbPtr + 8);
            rbInfo.dimension = readU32LE(rbPtr + 12);
            rbInfo.numSamples = readU32LE(rbPtr + 16);
            rbInfo.bindPoint = readU32LE(rbPtr + 20);
            rbInfo.bindCount = readU32LE(rbPtr + 24);
            rbInfo.uFlags = readU32LE(rbPtr + 28);
            outData.boundResources.push_back(std::move(rbInfo));
        }
    }

    return true;
}

}  // namespace sgre
