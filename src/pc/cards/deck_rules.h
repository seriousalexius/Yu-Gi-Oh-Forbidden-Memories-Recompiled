#ifndef MEMORIES_PC_CARDS_DECK_RULES_H
#define MEMORIES_PC_CARDS_DECK_RULES_H
#include <stddef.h>

/* fork: Alessio's deck rules, one set for each way of playing.
 *
 * Campaign (and Free Duel): Blue-eyes Ultimate Dragon never in the deck
 * (only in the chest), and one copy at most of each monster with 3000
 * attack or more.
 *
 * 1v1 online: no monster above 3000 attack (a fusion or a ritual, Gate
 * Guardian's, may still bring one; Ultimate Dragon's ritual, which wants
 * three Blue-eyes, stays out); one 3000-attack monster in all; the
 * power tiers of Alessio's tools/numeri.py (deck_tiers.h) cap the copies of
 * a card, legendary 1, epic 2, rare and common 3; two legendary spells or
 * traps in all. The arena brings decks of its own. */

enum { DECK_RULES_CAMPAIGN, DECK_RULES_ONLINE, DECK_RULES_MODES };

/* Whether one more copy of `id` may go into a deck that has `in_deck` of
 * it under the campaign's rules; when not, a notice says why. Build Deck
 * asks it for each card added. */
int DeckRules_CanAdd(int id, int in_deck);

/* The rules the DECK_SIZE ids of `deck` break under `mode`: how many, and
 * one "- ...\n" line each into `out` (NULL for the count alone), at most a
 * handful and then "- ...". 0 when the deck keeps them all; an online deck
 * also wants its forty cards. */
int DeckRules_Problems(int mode, const unsigned short *deck, char *out, size_t size);

/* The rules of `mode` in a few lines, for notices and the rules panel. */
const char *DeckRules_Text(int mode);

/* Whether the deck keeps the campaign's rules; when not, a notice lists
 * what breaks them. Asked before a campaign or Free Duel duel, so a deck
 * from before the rules (or a deck slot) is fixed in Build Deck. */
int DeckRules_DeckOk(const unsigned short *deck);

/* Game > Regole del mazzo...: the running save's deck against each mode's
 * rules, what breaks them, and the rules themselves, in a notice. */
void DeckRules_ShowPanel(void);

/* The legendary-tier cards in `deck`, for the 1v1 statistics. */
int DeckRules_Legendary(const unsigned short *deck);

#endif
