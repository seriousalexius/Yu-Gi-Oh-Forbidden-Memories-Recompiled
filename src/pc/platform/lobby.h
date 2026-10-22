#ifndef MEMORIES_PC_PLATFORM_LOBBY_H
#define MEMORIES_PC_PLATFORM_LOBBY_H
/* The online lobby (server/worker.js, class Lobby): an account made once
 * with a name and the friends' invite code, kept in lobby.txt in the user
 * folder; from then on, whenever the game runs, a background thread stays
 * connected to ws://<host>/lobby (with the socket of pc/debug/netplay_net.h,
 * never in a run with an online duel), so the others see this player online
 * and can challenge them. The screens are notices (menu.h): the account's
 * letter grid, the players (green online, grey offline), a challenge, its
 * wait, and the notice of a challenge that comes in, over whatever the game
 * shows. Accepting writes online.cfg (read by pc/debug/netplay.c) and
 * restarts the game into the duel.
 *
 * MEMORIES_LOBBY=0 turns it off; =1 turns it on in scripted runs
 * (MEMORIES_SDL_SCRIPT, MEMORIES_INPUT), which otherwise leave it off.
 * MEMORIES_LOBBY_HOST is host[:port] instead of the Cloudflare server. For
 * tests, MEMORIES_LOBBY_CHALLENGE=<name> challenges that player as soon as
 * they are online, and MEMORIES_LOBBY_ACCEPT=1 accepts a challenge. */

/* At start, after Platform_Open: connects when there is an account. */
void Lobby_Start(void);
/* Every frame, on the main thread: what came in, and the notices. */
void Lobby_Frame(void);
/* Game > Online... and the title's "online" action: the account's letters
 * the first time, then the players. */
void Lobby_Open(void);

#endif
