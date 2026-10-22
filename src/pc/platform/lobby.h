#ifndef MEMORIES_PC_PLATFORM_LOBBY_H
#define MEMORIES_PC_PLATFORM_LOBBY_H
/* The online lobby (server/worker.js, class Lobby): an account made once
 * with a name, the friends' invite code and a password (or logged into
 * elsewhere with the name and password: "Ho gia' un account"), kept in
 * lobby.txt in the user folder; an account made before the passwords is
 * asked for one, once a run. Its saves go to the account too (cloud.h).
 * From then on, whenever the game runs, a background thread stays
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
 * they are online, and MEMORIES_LOBBY_ACCEPT=1 accepts a challenge. With no
 * account, MEMORIES_LOBBY_TEST_REGISTER=NAME:INVITE:PASSWORD makes one (with
 * NAME:INVITE alone, one with no password, as before the passwords) and
 * MEMORIES_LOBBY_TEST_LOGIN=NAME:PASSWORD logs in at the start;
 * MEMORIES_LOBBY_TEST_PASSWORD=P answers the question of an account with no
 * password by itself. */

/* At start, after Platform_Open: connects when there is an account. */
void Lobby_Start(void);
/* Every frame, on the main thread: what came in, and the notices. */
void Lobby_Frame(void);
/* Game > Online... and the title's "online" action: the account's letters
 * the first time, then the players. */
void Lobby_Open(void);
/* One JSON line to the server, sent by the thread once connected (the
 * saves on the account, cloud.h). */
void Lobby_Send(const char *line);

#endif
