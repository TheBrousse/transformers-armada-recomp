#pragma once
// The save-state and recording manager (TODO 156): the F1 menu's Debug page.
//
// Save states: any number of them, each with a name, in the `states` folder beside the quick state
// (ps2_savestate::quickStatePath(), F6 / F9 - the one quick slot, listed first). A new one is saved
// under a name typed in (the date and time by default), each can be loaded from the list, renamed and
// deleted (a second click confirms). Saving and loading go through the scheduler's requests as F6 / F9
// do (EeScheduler::requestSaveState / requestLoadState); the result - saved, refused (a loading screen),
// a state of another build - is shown under the buttons.
//
// Play recordings (playrec.h): a Start button that begins one as F2 does once the menu has closed
// (opening the menu ends a running one), the key named, and the recordings folder's list with rename
// and delete. With the Gameplay setting "Play recording" off the page says how to turn it on.

class PS2Runtime;

namespace state_manager
{
    // In the Debug page (window thread, inside the overlay's frame). `close` closes the menu (a state loaded, the
    // recording's Start button).
    void drawStates(PS2Runtime &runtime, void (*close)());
    // Every window frame, the menu open or not: the answer to a save or load asked for, and the test aid's
    // actions (TF_STATES_TEST, docs/DEBUGGING.md).
    void frame(PS2Runtime &runtime);
    void drawRecordings(void (*close)());
}
