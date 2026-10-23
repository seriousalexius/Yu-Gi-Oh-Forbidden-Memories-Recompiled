#ifndef MEMORIES_PC_CARDS_DECK_RULES_H
#define MEMORIES_PC_CARDS_DECK_RULES_H

/* fork: the deck rules every player keeps the same in the campaign and the
 * Free Duel (Alessio's): Blue-eyes Ultimate Dragon never in the deck (only
 * in the chest), and one copy at most of each monster with 3000 attack or
 * more. The arena and the online duels bring decks of their own. */

/* Whether one more copy of `id` may go into a deck that has `in_deck` of
 * it; when not, a notice says why. Build Deck asks it for each card added. */
int DeckRules_CanAdd(int id, int in_deck);

/* Whether the DECK_SIZE ids of `deck` keep the rules; when not, a notice
 * lists what breaks them. Asked before a campaign or Free Duel duel, so a
 * deck from before the rules (or a deck slot) is fixed in Build Deck. */
int DeckRules_DeckOk(const unsigned short *deck);

#endif
