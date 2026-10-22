#ifndef MEMORIES_PC_DEBUG_NETPLAY_H
#define MEMORIES_PC_DEBUG_NETPLAY_H
/* Online duels: two games, one per player, run in lockstep.
 *
 * Both games start from the boot on the virtual clock (platform_common.c),
 * with the same build, settings and mods, so they compute the same frames
 * as long as they get the same pad bits at every VBlank (the recorder's
 * replays rest on the same fact, recorder.h). Each game sends its own
 * player's bits, already through the mods and View > Japanese buttons, for
 * the VBlank `delay` ahead, and waits at every VBlank for the other's: pad 1
 * is player 1's, pad 2 player 2's, on both. A relay (tools/relay.py in
 * Alessio's folder) pairs the two connections of a room and passes the
 * bytes through.
 *
 * At the start the games swap their players' saves (the 0x680-byte state of
 * a slot) and the first safe point asks for a jump to "duel2p": the title
 * gives way to the debug menu, which loads the two saves into the
 * two-player slots, as the memory card pair load does, seeds the random
 * numbers with the relay's seed and enters the two-player duel setup.
 *
 * Each game sees its own player's hand and the other's as card backs
 * (Cheats_CardViewMode, which only the picture reads), whatever the 2P
 * setup's CARD INDICATE says; MEMORIES_NETPLAY_HIDE=0 shows both, as the
 * hot-seat duel does.
 *
 * Every NETPLAY_CHECK_EVERY VBlanks each game hashes what the duel's rules
 * run on (the cards, the decks, the sides, the seed, the turn and phase) at
 * the same safe point and sends it: a mismatch is a desync, logged once,
 * with a RAM dump. With both hands shown the pictures are the same too, and
 * the hash takes all of guest RAM but the sound driver's area (which
 * follows the real-time mixer), the strictest check.
 *
 * MEMORIES_NETPLAY=host:port/room/side (side 1 or 2) turns it on,
 * MEMORIES_NETPLAY_SAVE=<slot file> is this player's save and
 * MEMORIES_NETPLAY_DELAY the VBlanks of input delay (4 by default).
 * Unset, everything here costs one test. */
#include <stdint.h>

#define NETPLAY_CHECK_EVERY 120

/* ResetCallback, before the timers start: connect, wait for the other
 * player, swap the saves. Blocks; on failure the game runs offline. */
void Netplay_Init(void);
/* The lockstep runs (from Netplay_Init on, for the whole run). */
int Netplay_Active(void);
/* run_vblank: this player's bits for this VBlank in, both players' out
 * (both[0] pad 1, both[1] pad 2). Waits for the other game's bits. */
void Netplay_Pads(uint16_t local, uint16_t both[2]);
/* The end of VSync(0): the jump at the first one, the RAM checks. */
void Netplay_Point(unsigned frame);
/* For the jump: player 1's or 2's save state (0x680 bytes), NULL offline;
 * the seed both games use. */
const unsigned char *Netplay_Save(int player);
uint32_t Netplay_Seed(void);
/* 1 or 2: the player this game is; 0 offline. */
int Netplay_Side(void);
/* The other player's hand is drawn as backs (MEMORIES_NETPLAY_HIDE). */
int Netplay_HidesOtherHand(void);

#endif
