// The core's hooks to its host (nr_core.h).
#include "nr_core.h"

#include <atomic>

namespace nr_core
{
    namespace
    {
        std::atomic<ThreadPlacer> g_placer{nullptr};
    }

    void setThreadPlacer(ThreadPlacer placer) { g_placer.store(placer, std::memory_order_release); }

    namespace
    {
        TextureHost g_textureHost;
    }

    namespace
    {
        HiresHost g_hiresHost;
    }

    void setHiresHost(const HiresHost &host) { g_hiresHost = host; }
    const HiresHost &hiresHost() { return g_hiresHost; }

    namespace
    {
        PresentHost g_presentHost;
    }

    void setPresentHost(const PresentHost &host) { g_presentHost = host; }
    const PresentHost &presentHost() { return g_presentHost; }

    void setTextureHost(const TextureHost &host) { g_textureHost = host; }
    const TextureHost &textureHost() { return g_textureHost; }

    void placeThisThread(ThreadRole role)
    {
        if (const ThreadPlacer placer = g_placer.load(std::memory_order_acquire))
            placer(role);
    }
}
