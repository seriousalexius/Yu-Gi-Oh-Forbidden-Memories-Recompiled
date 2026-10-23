#ifndef MEMORIES_PC_SAVES_ONLINE_DECK_H
#define MEMORIES_PC_SAVES_ONLINE_DECK_H
#include "deck_slots.h"
#include <stddef.h>

/* fork: the deck a player takes to a 1v1 online duel. The duel loads the
 * save from disk (pc/debug/netplay.c), so by default its deck is the one
 * last saved; a player may name one of the deck slots kept beside that save
 * (deck_slots.h) instead, so the campaign keeps its own deck. The choice
 * is online-deck.txt in the user folder, "slot=N" (1 to DECK_SLOT_COUNT,
 * 0 or no file for the save's deck). The arena brings decks of its own. */

int OnlineDeck_Slot(void);
void OnlineDeck_SetSlot(int slot);

/* Which deck slots of the save state `state` (SAVE_SIZE bytes) hold a deck:
 * used[i] for slot i + 1. */
void OnlineDeck_UsedSlots(const unsigned char *state, int used[DECK_SLOT_COUNT]);

/* The chosen slot's forty cards over the deck at the start of `state`;
 * the slot used, or 0 when none is chosen or it holds no deck (the save's
 * own deck stays). */
int OnlineDeck_Apply(unsigned char *state);

/* The save's state as the online duel reads it, `state` SAVE_SIZE bytes:
 * the first of the slot's two copies whose deck is whole. 0 on success. */
int OnlineDeck_ReadSave(const char *path, unsigned char *state);

/* The deck the 1v1 takes from the save at `path`, with the slot applied:
 * the slot used (0 for the save's own), or -1 when the save cannot be read
 * or has no whole deck. */
int OnlineDeck_Read(const char *path, unsigned short deck[DECK_SLOT_CARDS]);

#define ONLINE_DECK_SAVE_SIZE 0x680
#endif
