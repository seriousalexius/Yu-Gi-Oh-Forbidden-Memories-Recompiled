#ifndef MEMORIES_PC_PLATFORM_BET_H
#define MEMORIES_PC_PLATFORM_BET_H
#include <stddef.h>
struct JsonValue;

/* fork: the bet before an online duel (Alessio's). Once both players have
 * said yes to a challenge, the server (server/worker.js) opens the bet in
 * place of the duel: each player puts up a card of the save the duel loads
 * (its trunk or its deck) and starchips, either, both or nothing, and sees
 * the other's; once both say "Pronto" on what they see, the duel goes, a
 * 1v1 or the whole arena series. The stakes ride to the duel in
 * scommessa.txt in the user folder; on the next start the duel's result
 * (lobby.txt) settles them in the save's slot: the winner gets the other's
 * card and starchips, the loser loses theirs, a card from the deck replaced
 * by the weakest card of the trunk so the deck keeps its forty. A duel that
 * never started voids the bet.
 *
 * MEMORIES_BET_TEST=<card>:<starchips> puts that up and says "Pronto" by
 * itself (the lobby's tests: 0:0 when unset). */

/* The server's "scommessa": the bet screen, over whatever is shown. `send`
 * takes the lines for the server; `back` is called once the bet is gone
 * without a duel (the lobby's list again). */
void Bet_Open(const char *room, int side, const char *mode, const char *rival, int test,
              void (*send)(const char *line), void (*back)(void));
/* Whether a bet is open. */
int Bet_Active(void);
/* Whether `name` is the player the open bet is against. */
int Bet_With(const char *name);
/* The server's "puntata": the other's stake. 1 when it was the bet's. */
int Bet_Handle(const char *type, const struct JsonValue *root);
/* The server's "via" for the open bet: the stakes as the server keeps them
 * into scommessa.txt (none when both are nothing), and the bet closes. */
void Bet_Go(const struct JsonValue *root);
/* A duel with no bet: an old scommessa.txt goes. */
void Bet_Forget(void);
/* The bet is off (the other cancelled or left, the link is gone): a notice
 * says `why`, then `back`. */
void Bet_Cancel(const char *why);

/* At start, once the duel's result is known: `result` gives 1 (won), 0
 * (lost) or -1 (no duel) for the rival's name. The save is changed now; the
 * notice that says how waits for Bet_Frame. */
void Bet_Settle(int (*result)(const char *rival));
/* Every frame: the settled bet's notice, once nothing else is shown. */
void Bet_Frame(void);
/* At a duel's end (pc/debug/netplay.c): what the bet in scommessa.txt
 * gives or takes, "\n\n..." into `out`, or "" when there is none. */
void Bet_Describe(int won, char *out, size_t size);
#endif
