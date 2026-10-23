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
 * Without MEMORIES_NETPLAY, online.cfg in the user folder ("netplay=" and
 * "save=" lines), which the lobby writes before it restarts the game
 * (pc/platform/lobby.h), asks the same; it is removed as soon as read. Its
 * "contro=" line names the other player.
 *
 * The end of the duel is read from the duel's own state at the same safe
 * point in both games (the result phase, the winning side and why), so both
 * say it at once; a resignation (Esc, a phone's Back, Game > Online...) goes
 * to the other game as a message and changes nothing in guest RAM. Either
 * way a notice tells the result and, for a duel from the lobby, leads back
 * to the game with a restart; lobby.txt keeps the result for the lobby to
 * tell the server ("in_corso=" while the duel runs: a game that never got
 * to the end lost it). MEMORIES_NETPLAY_TEST_RESIGN=<VBlank> resigns
 * without asking at that lockstep VBlank and MEMORIES_NETPLAY_TEST_BACK=1
 * presses "Torna al gioco" about 2 s after the result shows (tests).
 * Unset, everything here costs one test. */
#include <stdint.h>
#include "pc/platform/menu.h"

#define NETPLAY_CHECK_EVERY 120 /* MEMORIES_NETPLAY_CHECK_EVERY sets another */
/* An online duel's LP: the two-player setup's choice starts there and goes
 * up to it (the arena's 12000; the hot-seat duel keeps 8000 or a mod's).
 * Online the setup is skipped and both start there (Netplay_LifePoints:
 * MEMORIES_NETPLAY_TEST_LP gives the tests another, 1 for a one-hit duel). */
#define NETPLAY_LIFE_POINTS 12000
unsigned Netplay_LifePoints(void);

/* ResetCallback, before the timers start: connect, wait for the other
 * player, swap the saves. Blocks; on failure the game runs offline. */
void Netplay_Init(void);
/* The lockstep runs (from Netplay_Init on, for the whole run). */
int Netplay_Active(void);
/* An online duel is asked for (MEMORIES_NETPLAY), known before the mods
 * load: the title keeps the game's own screen and menus (title_config.c),
 * since a mod's title or menu buttons made the two games part company for a
 * moment on the way to the duel, and the jump skips the title anyway. */
int Netplay_Asked(void);
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
/* Esc, a phone's Back, Game > Online...: while an online duel runs, asks
 * whether to resign and returns 1; else 0 (the key does what it did). */
int Netplay_AskResign(void);
/* hud.c: while the other player chooses a guardian star, a panel over the
 * dialog, which would name the stars of the card they put down; while they
 * hold a card up with Triangle, one over the picture, whose text would tell
 * the card. */
unsigned Netplay_DrawSignature(void);
void Netplay_Draw(MenuCanvas *canvas, int *x, int *y, int *w, int *h);

/* The arena online: the lobby's "Arena" challenge (online.cfg "modo=arena",
 * or MEMORIES_NETPLAY_ARENA=1). No jump at the first point: the arena mod
 * finds this under "port:netplay_arena" (events.c) and runs its draft over
 * the title, with the draft seed below, the same in both games; meanwhile
 * this game sends no buttons, so both games stay idle and alike. Once the
 * mod sets deck_ready, deck[] goes into this player's save and to the other
 * game (a 'D' message, before the ready flag that rides along with the
 * input); the first VBlank whose input carries both players' flags jumps to
 * the duel, in both games at once. Best of three: after each duel a notice
 * with the series' score, "Prossimo duello" (both flags again: the next
 * jump, another seed) or "Ritirati" (the series lost); the series' end is
 * the result, as a single duel's. MEMORIES_NETPLAY_TEST_SERIES=<VBlanks>
 * (tests): each duel counts that many VBlanks after it starts, won by
 * player 1, 2, 1 in turn, and "Prossimo duello" presses itself. */
#define NETPLAY_ARENA_VERSION 1
typedef struct {
    uint32_t version; /* NETPLAY_ARENA_VERSION */
    int32_t side;     /* 1 or 2 */
    uint32_t seed;    /* the draft's: the relay's, the same in both games */
    int32_t deck_ready; /* the mod's: deck[] is whole */
    uint16_t deck[40];
    int32_t duel;    /* duels started; 0 while drafting */
    int32_t over;    /* the series is over (or the other player left) */
    int32_t wins[2]; /* this player's and the other's */
    char rival[64];
} NetplayArena;
/* The arena session's, or NULL (not online, or a single duel). */
NetplayArena *Netplay_Arena(void);

#endif
