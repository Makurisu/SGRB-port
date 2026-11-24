#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

struct SGRE_COM_Mock {
    void* vtable = nullptr;
    std::atomic<int32_t> refCount{ 1 };
};

struct RenderFrameState {
    float clearColor[4];
    uint32_t frameIndex;
    uint32_t width;
    uint32_t height;
    uint32_t drawCalls;
    uint32_t indexDraws;
    uint32_t texturesCreated;
    uint32_t shadersCreated;
    uint32_t buffersCreated;
    uint64_t cpuBytes;
    uint32_t droppedResources;
};

class D3D11Bridge {
public:
    static int32_t createDevice(
        void* pAdapter,
        int32_t driverType,
        void* software,
        uint32_t flags,
        const void* pFeatureLevels,
        uint32_t featureLevels,
        uint32_t sdkVersion,
        void** ppDevice,
        void* pFeatureLevel,
        void** ppImmediateContext
    );
};

// Cấu trúc lệnh vẽ lưới tam giác tổng quát (Arbitrary Triangle Mesh Pass-Through)
// Hỗ trợ mọi hình học: Quads, Multi-quads, Lưới biến dạng nhân vật E-mote
struct MeshDrawCommand {
    const void* textureHandle;      // ID3D11Texture2D resource handle (Slot 0 Color)
    uint32_t textureWidth;
    uint32_t textureHeight;
    const void* alphaTextureHandle; // ID3D11Texture2D resource handle (Slot 1 Alpha Mask)
    uint32_t alphaTextureWidth;
    uint32_t alphaTextureHeight;
    const void* destTextureHandle;  // ID3D11Texture2D resource handle (Slot 2 Dest Snapshot t2)
    uint32_t destTextureWidth;
    uint32_t destTextureHeight;
    uint32_t vsShaderId;            // Bound Vertex Shader ID from M2 Engine (0 .. 11 in creation order)
    float vsCb0[64];                // Vertex Shader Constant Buffer 0 ($Globals: modelViewProj, constVtxColor)
    uint32_t shaderId;              // Bound Pixel Shader ID from M2 Engine (e.g. 2040, 240, 255)
    float cb0[32];                  // Pixel Shader Constant Buffer slot 0 ($Globals: 128 bytes / 32 floats)
    uint8_t stencilEnable;          // 0 = disabled, 1 = enabled
    uint8_t stencilRef;             // Stencil reference value
    uint8_t stencilReadMask;        // Stencil read mask
    uint8_t stencilWriteMask;       // Stencil write mask
    uint32_t stencilFunc;           // Stencil comparison function (e.g. GL_ALWAYS, GL_EQUAL)
    uint32_t stencilFailOp;         // Stencil fail operation (including GL_ZERO == 0)
    uint32_t stencilDepthFailOp;    // Stencil depth-fail operation (including GL_ZERO == 0)
    uint32_t stencilPassOp;         // Stencil pass operation (including GL_ZERO == 0)
    uint8_t colorWriteMask;         // Bitmask R=1, G=2, B=4, A=8 (0 = stencil mask only)
    uint8_t blendEnable;            // 0 = opaque, 1 = blend enabled
    uint32_t srcBlend;              // GLenum (including GL_ZERO == 0)
    uint32_t destBlend;             // GLenum (including GL_ZERO == 0)
    uint32_t blendOp;               // GLenum (e.g. GL_FUNC_ADD)
    uint32_t srcBlendAlpha;         // GLenum (including GL_ZERO == 0)
    uint32_t destBlendAlpha;        // GLenum (including GL_ZERO == 0)
    uint32_t blendOpAlpha;          // GLenum
    float blendFactor[4];           // Constant blend color from OMSetBlendState

    float viewportX;                // D3D11 Viewport topLeftX
    float viewportY;                // D3D11 Viewport topLeftY
    float viewportW;                // D3D11 Viewport width
    float viewportH;                // D3D11 Viewport height

    uint8_t scissorEnable;          // 0 = disabled, 1 = enabled
    int32_t scissorX;               // D3D11 Scissor left
    int32_t scissorY;               // D3D11 Scissor top
    int32_t scissorW;               // D3D11 Scissor width (right - left)
    int32_t scissorH;               // D3D11 Scissor height (bottom - top)

    uint8_t isClearCommand;         // 0 = Draw Mesh, 1 = Clear Render Target, 2 = Clear Stencil, 3 = Copy Subresource (GPU Blit)
    uint8_t clearStencil;           // Giá trị xóa Stencil nếu isClearCommand == 2
    float clearColor[4];            // Màu xóa nếu isClearCommand == 1
    const void* renderTargetHandle; // ID3D11Texture2D resource handle (nullptr = Backbuffer)
    uint32_t renderTargetWidth;
    uint32_t renderTargetHeight;

    std::vector<float> vertexData;     // [posX, posY, posZ, u0, v0, u1, v1, r, g, b, a] mỗi đỉnh (stride 11 floats = 44 bytes)
    std::vector<uint16_t> indexData;   // Danh sách chỉ số tam giác (Triangle List indices)
};

struct SpriteDrawCommand {
    float pos[4][2];      // 4 vertices (x, y) normalized to NDC [-1, 1]
    float uv[4][2];       // 4 vertices (u, v) normalized to [0, 1]
    float color[4][4];    // 4 vertices (r, g, b, a) normalized to [0, 1]
    uint32_t indices[6];  // 2 triangles: [0, 1, 2, 2, 1, 3]
    const void* textureHandle;      // ID3D11Texture2D resource handle (Slot 0 Color)
    uint32_t textureWidth;
    uint32_t textureHeight;
    const void* alphaTextureHandle; // ID3D11Texture2D resource handle (Slot 1 Alpha Mask)
    uint32_t alphaTextureWidth;
    uint32_t alphaTextureHeight;
    const void* destTextureHandle;  // ID3D11Texture2D resource handle (Slot 2 Dest Snapshot t2)
    uint32_t destTextureWidth;
    uint32_t destTextureHeight;
    uint8_t stencilEnable;          // 0 = disabled, 1 = enabled
    uint8_t stencilRef;             // Stencil reference value
    uint32_t stencilFunc;           // Stencil comparison function (e.g. GL_ALWAYS, GL_EQUAL)
    uint32_t stencilPassOp;         // Stencil pass operation (e.g. GL_REPLACE, GL_KEEP)
    uint8_t colorWriteMask;         // 1 = write color (normal), 0 = no color write (stencil mask only)
    float cb0[4];                   // Constant Buffer slot 0 ($Globals: phase, invVague, etc.)
    uint32_t shaderId;              // Bound Pixel Shader ID from M2 Engine (e.g. 2040, 240, 255)
};

struct GpuFlushRequest {
    const void* targetHandle = nullptr;
    uint8_t* dstPtr = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t rowPitch = 0;
    bool toBgra = false;
    std::vector<MeshDrawCommand> commands;
};

struct TextureUploadRequest {
    const void* handle = nullptr;
    uint64_t generationId = 0;
    uint32_t revision = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct TextureCpuSnapshot {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint32_t revision = 0;
    uint64_t generationId = 0;
    bool isRenderTarget = false;
    bool isGpuOwned = false;
    bool isUploadedToGpu = false;
    bool isCompressedBc7 = false;
    std::shared_ptr<const std::vector<uint8_t>> pixels;
};

bool D3D11Bridge_WaitForNextFrame(RenderFrameState* outState, uint32_t timeoutMs);
void D3D11Bridge_GetLatestFrameState(RenderFrameState* outState);
bool D3D11Bridge_GetActiveTextureInfo(uint32_t* outWidth, uint32_t* outHeight, const uint8_t** outRgbaData, uint32_t* outRevision = nullptr);
bool D3D11Bridge_GetTextureInfoByHandle(const void* handle, uint32_t* outWidth, uint32_t* outHeight, const uint8_t** outRgbaData, uint32_t* outFormat = nullptr, uint32_t* outRevision = nullptr, uint64_t* outGenerationId = nullptr);
bool D3D11Bridge_AcquireTextureSnapshot(const void* handle, TextureCpuSnapshot& out);
bool D3D11Bridge_WaitAndConsumePreuploadRequests(std::vector<TextureUploadRequest>& out, uint32_t timeoutMs);
void D3D11Bridge_ConsumePreuploadRequests(std::vector<TextureUploadRequest>& out);
void D3D11Bridge_WakePreuploadThread();
void D3D11Bridge_MarkTextureUploaded(const void* handle);
bool D3D11Bridge_PreserveGpuTextureToCpu(const void* handle, const uint8_t* rgbaData, size_t byteSize);
void D3D11Bridge_ResetGpuUploadState();
uint64_t D3D11Bridge_GetCpuMemoryBytes();
void D3D11Bridge_SetSupportsBptcCompression(bool supported);
bool D3D11Bridge_SupportsBptcCompression();
bool D3D11Bridge_EnsureTextureDecompressed(const void* handle);
bool D3D11Bridge_IsStagingTexture(const void* handle);
bool D3D11Bridge_IsRenderTargetHandle(const void* handle);
bool D3D11Bridge_HasGpuContents(const void* handle);
void D3D11Bridge_SetRenderLoopActive(bool active);
bool D3D11Bridge_ConsumeFlushRequest(GpuFlushRequest& outReq);
void D3D11Bridge_CompleteFlushRequest(bool success);
void D3D11Bridge_ConsumeReleasedHandles(std::vector<const void*>& outHandles);
void D3D11Bridge_FinalizeRelease(const void* handle);
size_t D3D11Bridge_GetSpriteBatchInfo(SpriteDrawCommand* outCommands, size_t maxCommands);
size_t D3D11Bridge_SwapFrameMeshes(std::vector<MeshDrawCommand>& outMeshes);
void D3D11Bridge_NotifyFramePresented();
void D3D11Bridge_NotifySpritesConsumed();
void D3D11Bridge_WakeRenderThread();
void* D3D11Bridge_GetMockTexture2D();

#if defined(__x86_64__)
#define WINAPI __attribute__((ms_abi))
#else
#define WINAPI
#endif

#ifdef __cplusplus
extern "C" {
#endif
int32_t WINAPI D3D11CreateDevice(
    void* pAdapter,
    int32_t driverType,
    void* software,
    uint32_t flags,
    const void* pFeatureLevels,
    uint32_t featureLevels,
    uint32_t sdkVersion,
    void** ppDevice,
    void* pFeatureLevel,
    void** ppImmediateContext
);
int32_t WINAPI D3DReflect(
    const void* pSrcData,
    size_t srcDataSize,
    const void* pInterface,
    void** ppReflector
);
#ifdef __cplusplus
}
#endif
