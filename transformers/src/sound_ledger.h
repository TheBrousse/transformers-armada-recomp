#pragma once
// TF_SOUND_LEDGER=1 (TODO 106): what the game asks the sound server to play, and what the server
// then reports as playing - on either server (native or the emulated IOP): sound_ledger.cpp.

class PS2Runtime;

namespace sound_ledger
{
    // Wraps the frame-packet command (0x1F0470) and the status parse (0x1E3760) as they are
    // registered at the time of the call: call it after the native server's install.
    void install(PS2Runtime &runtime);
}
