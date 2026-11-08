// The game's own log (TODO 186): self_log.cpp has the long form.
#pragma once

namespace self_log
{
    // At the very start of main: stdout and stderr into runs\play_<date>.out / .err when nothing redirects them
    // already (the playtest launcher's and the test scripts' redirection wins). TF_SELF_LOG=0: never.
    void start();
}
