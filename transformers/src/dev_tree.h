#pragma once
// The developer's own asset tree (an Extracted folder with upscaled\ and crisp_fonts\), looked in last for the HD
// textures and the sharp fonts. The private overlay names it (TF_DEV_TREE, overlay/overlay.cmake); the public build has
// none: an empty path.

#include <filesystem>

inline std::filesystem::path devTree()
{
#if defined(TF_DEV_TREE)
    return std::filesystem::path(TF_DEV_TREE);
#else
    return {};
#endif
}
