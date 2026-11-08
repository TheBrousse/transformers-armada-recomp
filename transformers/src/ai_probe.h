#pragma once
// A testing aid (ai_probe.cpp, TODO 142/143): TF_AI_PROBE=<ticks> logs, every that many ticks of game time
// (TF_INPUT's: 60 a second at every frame rate) and whenever their state changes, the drop ships
// (taCreatureDropShip::Update, 0x3E28C0: health, update mode, slot, the body's dying/falling state, the creatures
// they carry), each slot a ship lets go (DropIt, 0x3E0D30: whether the creature is let go or stays carried),
// the creatures they carried (TF_AI_PROBE_ALL=1: every Medium Grunt; their state +0x10, goal, plan count, flags)
// and the spider tanks (their actors' updates, 0x44A8D0 / 0x402D20: position, flags, health, the running goal,
// the turret). Read only, unless asked for one of the test actions:
//   TF_AI_PROBE_KILL=<tick>[,fall|,hit|,hitfall]  the first ship still carrying dies (StartDeath), tumbles
//       (StartFallToDeath), takes hits of type 0x100 until it falls or dies, or (hitfall) is shot down: the
//       tumble's roll made to come up once, then hits every 6 ticks through the fall until no health is left.
//   TF_AI_PROBE_CINEMA=<on>,<off>  the creature system in cinema mode between the ticks, as a script's
//       DisableCreatures / EnableCreatures make it (the Mini-Con discovery's, for one).
// Its added code and state are registered at every start (guest_code.h), whether it is on or not.

class PS2Runtime;

namespace ai_probe
{
    void install(PS2Runtime &runtime);
}
