#pragma once
// A debugging aid: the files the game opens (TF_FILE_LOG=1): file_trace.cpp.

class PS2Runtime;

namespace file_trace
{
    void install(PS2Runtime &runtime);
}
