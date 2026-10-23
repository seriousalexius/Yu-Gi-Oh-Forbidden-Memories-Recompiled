#ifndef MEMORIES_PC_NEWS_H
#define MEMORIES_PC_NEWS_H

/* fork: the update notice. The lobby's server (server/worker.js in Alessio's
 * folder, its VERSIONE) sends each game that connects the newest published
 * version's number, its link and its news, one short line each:
 * {"t":"versione","versione":N,"link":"https://...","novita":["...", ...]}.
 * A game older than N says "Aggiornamento disponibile" with the news and an
 * Aggiorna button that opens the link; a game that is N says once "Cosa c'e'
 * di nuovo" with the same news (updates/news-seen.txt in the user folder
 * remembers it). Without internet nothing shows and the game plays on. */

/* This build's number: one more at each publication, the same then in VERSIONE. */
#define NEWS_VERSION 1

struct JsonValue;

/* The lobby's "versione" message (lobby.c). */
void News_Version(const struct JsonValue *message);
/* Main thread, once a frame (Lobby_Frame): the notice, once nothing else shows. */
void News_Frame(void);

#endif
