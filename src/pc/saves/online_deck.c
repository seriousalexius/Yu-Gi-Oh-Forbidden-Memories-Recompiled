/* fork: the deck a player takes to a 1v1 online duel (online_deck.h). */
#include "online_deck.h"
#include "pc/cards/cards.h"
#include "pc/platform/paths.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOT_STATE_OFFSET 0x200 /* save_slots.h: the header, then the state and its duplicate */
#define SAVE_DUELIST_CODE 0x334 /* save_data.h, SAVE_DATA_DUELIST_CODE_OFFSET */

int OnlineDeck_Slot(void)
{
    char path[1024], line[64];
    FILE *file;
    int slot = 0;
    if (Paths_User(path, sizeof(path), "online-deck.txt") || !(file = fopen(path, "rb"))) return 0;
    while (fgets(line, sizeof(line), file))
        if (!strncmp(line, "slot=", 5)) slot = atoi(line + 5);
    fclose(file);
    return slot >= 1 && slot <= DECK_SLOT_COUNT ? slot : 0;
}

void OnlineDeck_SetSlot(int slot)
{
    char path[1024];
    FILE *file;
    if (Paths_User(path, sizeof(path), "online-deck.txt") || !(file = fopen(path, "wb"))) {
        fprintf(stderr, "memories-pc: online deck: cannot write online-deck.txt\n");
        return;
    }
    fprintf(file, "slot=%d\n", slot >= 1 && slot <= DECK_SLOT_COUNT ? slot : 0);
    fclose(file);
}

/* The save's deck slots: decks/<duelist code>.txt, as deck_menu.c keeps them. */
static int read_slots(const unsigned char *state, DeckSlot slots[DECK_SLOT_COUNT])
{
    char relative[64], path[1024];
    unsigned code;
    int skipped = 0;
    memset(slots, 0, sizeof(DeckSlot) * DECK_SLOT_COUNT);
    code = (unsigned)state[SAVE_DUELIST_CODE] | (unsigned)state[SAVE_DUELIST_CODE + 1] << 8 |
           (unsigned)state[SAVE_DUELIST_CODE + 2] << 16 | (unsigned)state[SAVE_DUELIST_CODE + 3] << 24;
    snprintf(relative, sizeof(relative), "decks/%08X.txt", code);
    if (Paths_User(path, sizeof(path), relative)) return 0;
    return DeckSlots_Read(path, slots, Cards_FindIdentity, &skipped);
}

void OnlineDeck_UsedSlots(const unsigned char *state, int used[DECK_SLOT_COUNT])
{
    DeckSlot slots[DECK_SLOT_COUNT];
    int i;
    read_slots(state, slots);
    for (i = 0; i < DECK_SLOT_COUNT; i++) used[i] = slots[i].used;
}

int OnlineDeck_Apply(unsigned char *state)
{
    DeckSlot slots[DECK_SLOT_COUNT];
    int slot = OnlineDeck_Slot(), i;
    if (!slot) return 0;
    read_slots(state, slots);
    if (!slots[slot - 1].used) {
        fprintf(stderr, "memories-pc: online deck: slot %d holds no deck: the save's own\n", slot);
        return 0;
    }
    for (i = 0; i < DECK_SLOT_CARDS; i++) {
        state[2 * i] = (unsigned char)(slots[slot - 1].cards[i] & 0xFF);
        state[2 * i + 1] = (unsigned char)(slots[slot - 1].cards[i] >> 8);
    }
    return slot;
}

int OnlineDeck_ReadSave(const char *path, unsigned char *state)
{
    static unsigned char slot[SLOT_STATE_OFFSET + 2 * ONLINE_DECK_SAVE_SIZE];
    FILE *file = path && *path ? fopen(path, "rb") : NULL;
    size_t got = file ? fread(slot, 1, sizeof(slot), file) : 0;
    int copy, card;
    if (file) fclose(file);
    if (got != sizeof(slot)) return -1;
    /* The first copy whose deck is whole (40 cards, as the 2P load wants). */
    for (copy = 0; copy < 2; copy++) {
        const unsigned char *at = slot + SLOT_STATE_OFFSET + copy * ONLINE_DECK_SAVE_SIZE;
        for (card = 0; card < DECK_SLOT_CARDS && (at[2 * card] | at[2 * card + 1]); card++) {}
        if (card == DECK_SLOT_CARDS) {
            memcpy(state, at, ONLINE_DECK_SAVE_SIZE);
            return 0;
        }
    }
    return -1;
}

int OnlineDeck_Read(const char *path, unsigned short deck[DECK_SLOT_CARDS])
{
    static unsigned char state[ONLINE_DECK_SAVE_SIZE];
    int used, i;
    if (OnlineDeck_ReadSave(path, state)) return -1;
    used = OnlineDeck_Apply(state);
    for (i = 0; i < DECK_SLOT_CARDS; i++) deck[i] = (unsigned short)(state[2 * i] | state[2 * i + 1] << 8);
    return used;
}
