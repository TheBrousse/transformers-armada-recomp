#pragma once
// Crisp text and PS5 button symbols (TODO 119): two Display settings for the native renderer's font
// atlases (transformers/native_render/nr_hires.cpp reads them when it loads the large images).
// TF_CRISP_TEXT=1 (default) / 0: the menus', hints' and HUD's letters drawn sharp at the screen's size
// (the menu font from a free twin of its typeface, the others rebuilt with sharp edges); 0 = the
// upscaled set's letters, as before. TF_BUTTON_SYMBOLS=ps5 (default) / ps2: the button symbols as our own
// DualSense-style drawings or the PS2's. The atlases are made from your disc by
// tools\make_hd_textures.ps1 (tools\hd_textures\make_crisp_fonts.py); without them both change nothing.
// Taken at the next start (the panel offers to restart there and then).

namespace crisp_text
{
    // TF_PAD_PROMPTS=auto (default), ps, xbox chooses the controller style live.
    // Before the window loop starts (overrides.cpp's addSettings).
    void addSettings();
}
