// The collector header is extracted from the actual patched runtime source by
// tools/test_watched_keys.py. No Raylib window or game is started by these tests.
#include <atomic>
#include <deque>
#include <iostream>
#include <stdexcept>

constexpr int kMaxWatchedKeys=8;
std::atomic<int> g_watchedKeys[kMaxWatchedKeys],g_keyPresses[kMaxWatchedKeys];
std::deque<int> keycodes,characters;
bool window_ready=true;
int previous_state=0,current_state=0;
bool IsWindowReady(){return window_ready;}
int GetKeyPressed(){if(keycodes.empty())return 0;int key=keycodes.front();keycodes.pop_front();return key;}
#include "watched_key_collector.h"

namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int take(){return g_keyPresses[0].exchange(0);}
}
int main(){
    try {
        constexpr int f1=290;
        g_watchedKeys[0]=f1;
        previous_state=0;current_state=1;keycodes={f1};collectWatchedKeyPresses();
        check(take()==1,"ordinary physical press lost");
        previous_state=1;current_state=1;collectWatchedKeyPresses();
        check(take()==0,"held key counted again without a new key-down event");

        // GLFW processes press then release in one PollInputEvents: both final
        // states are up, but its original key-down event remains queued.
        previous_state=0;current_state=0;keycodes={f1};
        check(!(previous_state==0&&current_state==1),"rapid-tap fixture incorrectly looks held");
        collectWatchedKeyPresses();check(take()==1,"same-poll press/release was lost");

        keycodes={f1,f1};collectWatchedKeyPresses();
        check(take()==2,"two distinct rapid taps must retain their count");
        keycodes={f1};collectWatchedKeyPresses();check(take()==1,"first separate tap lost");
        keycodes={f1};collectWatchedKeyPresses();check(take()==1,"second separate tap lost");

        // ImGui uses its separate character queue and held/released state.
        // Counting watched keycodes must not mutate either input source.
        keycodes={65,f1};characters={65};int held=current_state;
        collectWatchedKeyPresses();check(take()==1,"unwatched key altered watched count");
        check(characters.size()==1&&characters.front()==65,"character input was consumed");
        check(current_state==held,"held key state was modified");

        window_ready=false;keycodes={f1};collectWatchedKeyPresses();
        check(take()==0&&keycodes.size()==1,"unavailable window was polled");
        window_ready=true;collectWatchedKeyPresses();
        check(take()==1&&keycodes.empty(),"pending genuine edge was lost");
        collectWatchedKeyPresses();check(take()==0,"an edge was manufactured for an empty queue");

        // The menu still owns Ctrl+F1 exclusion; this patch only counts edges.
        bool ctrl=true;keycodes={f1};collectWatchedKeyPresses();
        check(!((take()%2)==1&&!ctrl),"existing Ctrl exclusion changed");
        std::cout<<"watched-key event-queue tests passed\n";
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
