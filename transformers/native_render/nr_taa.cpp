// (TODO 299) The temporal upscaler's settings and the AMD FidelityFX SDK's loading - nr_taa.h.
#include "nr_taa.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h> // (TODO 380: the adapter's free video memory, for the upscaler's room)
#ifndef _WINDOWS
#define _WINDOWS // (ffx_api_loader.h: the Windows loader)
#endif
#include "ext/fidelityfx/ffx_api.h"
#include "ext/fidelityfx/ffx_api_loader.h"
#include "ext/fidelityfx/ffx_upscale.h"
#include "ext/fidelityfx/dx12/ffx_api_dx12.h"
#endif

namespace nr::taa
{
    float debugView()
    {
        static const float scale = [] {
            const char *v = std::getenv("TF_TAA_DEBUG");
            if (!v || !*v || *v == '0')
                return 0.0f;
            const float s = static_cast<float>(std::atof(v));
            return s == 1.0f || s <= 0.0f ? 20.0f : s;
        }();
        return scale;
    }

    namespace
    {
        std::atomic<bool> g_failed{false};
    }

    bool sdkPresent()
    {
#if defined(_WIN32)
        static const bool present = [] {
            wchar_t path[MAX_PATH] = {};
            const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
            if (n == 0 || n >= MAX_PATH)
                return false;
            std::wstring dir(path, n);
            const size_t slash = dir.find_last_of(L"\\/");
            dir = slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash + 1);
            for (const wchar_t *name : {L"amd_fidelityfx_loader_dx12.dll", L"amd_fidelityfx_upscaler_dx12.dll"})
                if (GetFileAttributesW((dir + name).c_str()) == INVALID_FILE_ATTRIBUTES)
                    return false;
            return true;
        }();
        return present;
#else
        return false;
#endif
    }

    void markFailed()
    {
        if (!g_failed.exchange(true))
            std::fprintf(stderr, "[taa] the temporal upscaler can't run here: off for the rest of the run (the picture as without it)\n");
    }

    bool upscalerOn()
    {
        static const bool on = [] {
            const char *taa = std::getenv("TF_TAA");
            const char *setting = std::getenv("TF_NATIVE_UPSCALER");
            const bool wanted = (taa && *taa == '1') || (setting && std::strcmp(setting, "fsr") == 0);
            if (wanted && !sdkPresent())
                std::fprintf(stderr, "[taa] FSR 3 / 4 asked for, but the FidelityFX SDK's DLLs are not next to the game: off\n");
            return wanted && sdkPresent();
        }();
        return on && !g_failed.load(std::memory_order_relaxed);
    }

    bool wantsMotion()
    {
        return debugView() > 0.0f || upscalerOn();
    }

    namespace
    {
        float halton(uint32_t index, uint32_t base)
        {
            float f = 1.0f, r = 0.0f;
            for (; index > 0u; index /= base)
            {
                f /= static_cast<float>(base);
                r += f * static_cast<float>(index % base);
            }
            return r;
        }
    }

    void jitter(uint64_t n, float renderWidth, float outputWidth, float &x, float &y)
    {
        const float ratio = renderWidth > 0.0f ? outputWidth / renderWidth : 1.0f;
        const uint32_t phases = static_cast<uint32_t>(std::ceil(8.0f * ratio * ratio));
        const uint32_t index = static_cast<uint32_t>(n % (phases > 0u ? phases : 1u)) + 1u;
        x = halton(index, 2u) - 0.5f;
        y = halton(index, 3u) - 0.5f;
    }

#if defined(_WIN32)
    namespace
    {
        // The SDK's loader (amd_fidelityfx_loader_dx12.dll, next to the game; it loads amd_fidelityfx_upscaler_dx12.dll):
        // loaded on first use, never unloaded. Null functions: not there.
        const ffxFunctions *sdk()
        {
            static ffxFunctions functions{};
            static const bool loaded = [] {
                HMODULE module = LoadLibraryW(L"amd_fidelityfx_loader_dx12.dll");
                if (!module)
                    return false;
                ffxLoadFunctions(&functions, module);
                return functions.CreateContext && functions.DestroyContext && functions.Query && functions.Dispatch;
            }();
            return loaded ? &functions : nullptr;
        }
    }

    void probe(void *device)
    {
        static const bool on = std::getenv("TF_TAA_PROBE") && *std::getenv("TF_TAA_PROBE") == '1';
        if (!on || !device)
            return;
        const ffxFunctions *f = sdk();
        if (!f)
        {
            std::fprintf(stderr, "[taa] the FidelityFX SDK's loader (amd_fidelityfx_loader_dx12.dll) is not next to the game\n");
            return;
        }
        // The upscalers the SDK offers on this device (FSR 4 where the GPU has it, FSR 3.1 elsewhere).
        uint64_t count = 0;
        ffxQueryDescGetVersions versions{};
        versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        versions.device = device;
        versions.outputCount = &count;
        f->Query(nullptr, &versions.header);
        uint64_t ids[16] = {};
        const char *names[16] = {};
        count = count > 16 ? 16 : count;
        versions.versionIds = ids;
        versions.versionNames = names;
        const ffxReturnCode_t listed = f->Query(nullptr, &versions.header);
        std::fprintf(stderr, "[taa] FidelityFX SDK: %llu upscaler versions on this device (query %u):", static_cast<unsigned long long>(count), listed);
        for (uint64_t i = 0; i < count; ++i)
            std::fprintf(stderr, " %s", names[i] ? names[i] : "?");
        std::fputc('\n', stderr);
        // A context as the game would make it (1080p from 720p), and which provider it got.
        ffxCreateBackendDX12Desc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = static_cast<ID3D12Device *>(device);
        ffxCreateContextDescUpscaleVersion version{};
        version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
        version.header.pNext = &backend.header;
        version.version = FFX_UPSCALER_VERSION;
        ffxCreateContextDescUpscale create{};
        create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        create.header.pNext = &version.header;
        create.flags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
        create.maxRenderSize = {1280u, 720u};
        create.maxUpscaleSize = {1920u, 1080u};
        ffxContext context = nullptr;
        const ffxReturnCode_t made = f->CreateContext(&context, &create.header, nullptr);
        if (made != FFX_API_RETURN_OK || !context)
        {
            std::fprintf(stderr, "[taa] FidelityFX SDK: no upscaler context (code %u)\n", made);
            return;
        }
        ffxQueryGetProviderVersion provider{};
        provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        f->Query(&context, &provider.header);
        std::fprintf(stderr, "[taa] FidelityFX SDK: the upscaler context runs %s (id %llx)\n", provider.versionName ? provider.versionName : "?",
                     static_cast<unsigned long long>(provider.versionId));
        f->DestroyContext(&context, nullptr);
    }

    bool upscale(void *device, void *commandList, void *colour, void *depth, void *motion, void *reactive, void *output, unsigned renderWidth,
                 unsigned renderHeight, unsigned outputWidth, unsigned outputHeight, float jitterX, float jitterY, float frameMs, bool reset,
                 float sharpness, float motionScaleX, float motionScaleY, const Camera &camera)
    {
        const ffxFunctions *f = sdk();
        if (!f || !device || !commandList)
            return false;
        static ffxContext context = nullptr;
        static unsigned madeRW = 0, madeRH = 0, madeOW = 0, madeOH = 0;
        static bool failed = false, madeInfinite = true;
        // (TODO 380) Made once with room - twice the height it is first asked for, the size a field buffer's whole height
        // would need - and given the real sizes each frame (the dispatch's renderSize / upscaleSize, at most the context's):
        // a picture that grows within that room no longer rebuilds the context mid-play (120-160 ms on the 7900 XTX at
        // 3440 x 1440). Rebuilt only past the room (a larger window), or for the depth's kind. TF_TAA_ROOM=0: made for the
        // sizes asked and rebuilt when they change, as before.
        // (The room costs video memory - 278 MB more on the RX 5700 XT at 3456 x 1440 - so it is made only when the card has
        // 2 GB of its budget free when the context is made; else the exact sizes, as before. TF_TAA_ROOM=1: always.)
        static const char *roomEnv = std::getenv("TF_TAA_ROOM");
        static const bool roomOff = roomEnv && *roomEnv == '0', roomForced = roomEnv && *roomEnv == '1';
        static bool roomMade = false; // (the context in use was made with the room)
        if (context && (renderWidth > madeRW || renderHeight > madeRH || outputWidth > madeOW || outputHeight > madeOH ||
                        (!roomMade && (outputWidth != madeOW || outputHeight != madeOH)) || camera.infinite != madeInfinite))
        {
            f->DestroyContext(&context, nullptr);
            context = nullptr;
        }
        if (!context)
        {
            if (failed)
                return false;
            ffxCreateBackendDX12Desc backend{};
            backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
            backend.device = static_cast<ID3D12Device *>(device);
            ffxCreateContextDescUpscaleVersion version{};
            version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
            version.header.pNext = &backend.header;
            version.version = FFX_UPSCALER_VERSION;
            ffxCreateContextDescUpscale create{};
            create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
            create.header.pNext = &version.header;
            // (the depth: larger nearer - the GS's Z; infinite where the game's projection has no far plane: the camera fit)
            create.flags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
            if (camera.infinite)
                create.flags |= FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
            if (std::getenv("TF_TAA_FSRDEBUG"))
                create.flags |= FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION; // (TF_TAA_FSRDEBUG=1: the SDK's own debug view in the output)
            // (the adapter's budget less what is in use, the card's own memory - as the renderer's other budget checks)
            uint64_t freeBytes = 0;
            {
                IDXGIFactory4 *factory = nullptr;
                IDXGIAdapter3 *adapter = nullptr;
                if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
                {
                    factory->EnumAdapterByLuid(static_cast<ID3D12Device *>(device)->GetAdapterLuid(), IID_PPV_ARGS(&adapter));
                    factory->Release();
                }
                DXGI_QUERY_VIDEO_MEMORY_INFO info{};
                if (adapter && SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)) && info.Budget > info.CurrentUsage)
                    freeBytes = info.Budget - info.CurrentUsage;
                if (adapter)
                    adapter->Release();
            }
            const bool roomWanted = roomForced || (!roomOff && freeBytes >= (2ull << 30));
            const unsigned room = roomWanted ? 2u : 1u;
            roomMade = room > 1u;
            std::fprintf(stderr, "[taa] the upscaler's room: %s (%llu MB of the card's video memory free)\n",
                         roomOff ? "off (TF_TAA_ROOM=0)" : roomForced ? "on (TF_TAA_ROOM=1)" : roomMade ? "on" : "off (under 2 GB free)",
                         static_cast<unsigned long long>(freeBytes >> 20));
            create.maxRenderSize = {renderWidth, renderHeight * room};
            create.maxUpscaleSize = {outputWidth, outputHeight * room};
            const ffxReturnCode_t made = f->CreateContext(&context, &create.header, nullptr);
            if (made != FFX_API_RETURN_OK || !context)
            {
                std::fprintf(stderr, "[taa] the FidelityFX upscaler's context could not be made (code %u): the temporal upscaler is off\n", made);
                context = nullptr;
                failed = true;
                return false;
            }
            madeRW = renderWidth, madeRH = renderHeight * room, madeOW = outputWidth, madeOH = outputHeight * room, madeInfinite = camera.infinite;
            ffxQueryGetProviderVersion provider{};
            provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
            f->Query(&context, &provider.header);
            std::fprintf(stderr, "[taa] temporal upscaler: %s, %ux%u -> %ux%u%s (room to %ux%u); depth near %.4g far %.4g%s, fov %.1f deg, %.4g m a unit\n",
                         provider.versionName ? provider.versionName : "?", renderWidth, renderHeight, outputWidth, outputHeight,
                         renderWidth == outputWidth ? " (native AA)" : "", madeOW, madeOH, camera.nearZ, camera.farZ, camera.infinite ? " (infinite)" : "",
                         camera.fovY * 57.2958f, camera.metersPerUnit);
            // (TODO 380: what the room costs - the SDK's own count of the context's video memory, and what the same picture
            // would take made without the room; with TF_TAA_LOG only, a measuring aid)
            if (std::getenv("TF_TAA_LOG"))
            {
                FfxApiEffectMemoryUsage used{}, without{};
                ffxQueryDescUpscaleGetGPUMemoryUsage usage{};
                usage.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE;
                usage.gpuMemoryUsageUpscaler = &used;
                const bool haveUsed = f->Query(&context, &usage.header) == FFX_API_RETURN_OK;
                ffxQueryDescUpscaleGetGPUMemoryUsageV2 usage2{};
                usage2.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE_V2;
                usage2.device = device;
                usage2.maxRenderSize = {renderWidth, renderHeight};
                usage2.maxUpscaleSize = {outputWidth, outputHeight};
                usage2.flags = create.flags;
                usage2.gpuMemoryUsageUpscaler = &without;
                const bool haveWithout = f->Query(nullptr, &usage2.header) == FFX_API_RETURN_OK;
                std::fprintf(stderr, "[taa] the upscaler's video memory: %s MB made (room %u), %s MB for the picture alone\n",
                             haveUsed ? std::to_string(used.totalUsageInBytes >> 20).c_str() : "?", room,
                             haveWithout ? std::to_string(without.totalUsageInBytes >> 20).c_str() : "?");
            }
        }
        ffxDispatchDescUpscale d{};
        d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        d.commandList = commandList;
        d.color = ffxApiGetResourceDX12(static_cast<ID3D12Resource *>(colour), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.depth = ffxApiGetResourceDX12(static_cast<ID3D12Resource *>(depth), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.depth.description.format = FFX_API_SURFACE_FORMAT_R32_FLOAT; // (the target's depth is R32 typeless: read as its float)
        d.motionVectors = ffxApiGetResourceDX12(static_cast<ID3D12Resource *>(motion), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        // (what moves without its own motion - the particles, the effects and overlays drawn into the scene: trust this frame)
        if (reactive)
            d.reactive = ffxApiGetResourceDX12(static_cast<ID3D12Resource *>(reactive), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.output = ffxApiGetResourceDX12(static_cast<ID3D12Resource *>(output), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        d.jitterOffset = {jitterX, jitterY};
        d.motionVectorScale = {motionScaleX, motionScaleY}; // (the motion in the whole target's UV: its size)
        d.renderSize = {renderWidth, renderHeight};
        d.upscaleSize = {outputWidth, outputHeight};
        d.enableSharpening = sharpness > 0.0f;
        d.sharpness = sharpness;
        d.frameTimeDelta = frameMs;
        d.preExposure = 1.0f;
        d.reset = reset;
        d.cameraNear = camera.nearZ;
        d.cameraFar = camera.farZ;
        d.cameraFovAngleVertical = camera.fovY;
        d.viewSpaceToMetersFactor = camera.metersPerUnit;
        d.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
        if (std::getenv("TF_TAA_FSRDEBUG"))
            d.flags |= FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW;
        const ffxReturnCode_t done = f->Dispatch(&context, &d.header);
        static int told = 0;
        if (done != FFX_API_RETURN_OK && told++ < 5)
            std::fprintf(stderr, "[taa] the FidelityFX upscaler's dispatch failed (code %u)\n", done);
        return done == FFX_API_RETURN_OK;
    }
#else
    void probe(void *) {}
    bool upscale(void *, void *, void *, void *, void *, void *, void *, unsigned, unsigned, unsigned, unsigned, float, float, float, bool, float, float, float,
                 const Camera &)
    {
        return false;
    }
#endif
}
