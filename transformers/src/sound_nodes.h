#pragma once
// The sound nodes' frame counts at the PC frame rates: sound_nodes.cpp.

class PS2Runtime;

namespace sound_nodes
{
    // From applyTransformers: wraps the sound list's node update (0x27BD50).
    void install(PS2Runtime &runtime);
    // A state loaded: the console frames owed back at none, as in a game just started with it (TODO 150: they are not in
    // the state - kept from the frames before the load, a recording's start and its replay counted the sounds apart).
    void stateLoaded();
}
