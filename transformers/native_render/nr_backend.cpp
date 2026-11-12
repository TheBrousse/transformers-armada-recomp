// nr_backend.cpp - which graphics backend the native renderer uses (docs/RENDERER_BACKEND.md, section 2): chosen once at
// start by TF_GFX_API (settings.ini or the environment). d3d12 (the default): Direct3D 12, nr_d3d.cpp. vulkan: the Vulkan
// backend with its own SDL3 window, nr_vulkan.cpp (built with TF_VULKAN; in development - see that file's head for what
// it draws so far).
#include "nr_backend.h"
#include "nr_device.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nr
{
    Backend &d3d12Backend(); // (nr_d3d.cpp)
    namespace gfx
    {
        Device &deviceD3d12(); // (nr_d3d.cpp)
    }
#if defined(TF_VULKAN)
    namespace vulkan
    {
        Backend &vulkanBackend(); // (nr_vulkan.cpp)
        gfx::Device &vulkanDevice();
    }
    namespace vk::window
    {
        bool beforeWindow(); // (nr_vulkan_window.cpp)
    }
#endif

    namespace
    {
        bool vulkanChosen()
        {
            static const bool chosen = [] {
                const char *v = std::getenv("TF_GFX_API");
                const bool asked = v && (std::strcmp(v, "vulkan") == 0 || std::strcmp(v, "Vulkan") == 0);
#if defined(TF_VULKAN)
                std::fprintf(stderr, "[native render] graphics API: %s\n", asked ? "Vulkan (TF_GFX_API=vulkan; in development)" : "Direct3D 12");
                return asked;
#else
                if (asked)
                    std::fprintf(stderr, "[native render] TF_GFX_API=vulkan: this build has no Vulkan backend (TF_VULKAN off) - Direct3D 12\n");
                return false;
#endif
            }();
            return chosen;
        }
    }

    Backend &backend()
    {
#if defined(TF_VULKAN)
        if (vulkanChosen())
            return vulkan::vulkanBackend();
#else
        (void)vulkanChosen();
#endif
        return d3d12Backend();
    }
}

namespace nr
{
    void beforeWindow()
    {
#if defined(TF_VULKAN)
        // (Milestone 3) Vulkan presents SDL3's window itself: the window is made a Vulkan window.
        if (vulkanChosen())
            vk::window::beforeWindow();
#endif
    }
}

namespace nr::gfx
{
    Device &device()
    {
#if defined(TF_VULKAN)
        if (vulkanChosen())
            return vulkan::vulkanDevice();
#endif
        return deviceD3d12();
    }
}
