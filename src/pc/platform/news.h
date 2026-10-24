#ifndef MEMORIES_PC_NEWS_H
#define MEMORIES_PC_NEWS_H

/* fork: the update notice. At start a thread of its own asks the lobby's
 * server for /versione (server/worker.js in Alessio's folder, its VERSIONE):
 * the newest published version's number, its link and its news, one short
 * line each: {"versione":N,"link":"https://...","novita":["...", ...]}.
 * A game older than N says "Aggiornamento disponibile" with the news and an
 * Aggiorna button that opens the link; a game that is N says once "Cosa c'e'
 * di nuovo" with the same news (updates/news-seen.txt in the user folder
 * remembers it). Without internet nothing shows and the game plays on.
 * Accounts play no part: a game without one asks too. */

/* This build's number: one more at each publication, the same then in VERSIONE. */
#define NEWS_VERSION 9

/* Once, from Lobby_Start (whose host it asks, and whose test rules it keeps). */
void News_Start(const char *host, unsigned port);
/* Main thread, once a frame (Lobby_Frame): the notice, once nothing else shows. */
void News_Frame(void);

#endif
