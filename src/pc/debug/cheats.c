#include "pc/compat/fs.h"
#include "cheats.h"
#include "types.h"
#include "game/card_constants.h"
#include "game/save_data.h"
#include "pc/cards/cards.h"
#include "pc/cards/tables.h"
#include "pc/platform/settings.h"
#include "game/duel_side_state.h"
#include "game/main_modes.h"
#include "pc/tower/tower.h"
#include "pc/debug/netplay.h"
#include <stdio.h>
#include <stdlib.h>

extern u8 D_8009B26C; /* the active mode (main_mode_state.h, main_modes.h) */
extern u8 D_8009B26E; /* main_run_duel.c: Main_RunDuel's step, 0x80 once set up */

/* A game is started or loaded: the save workspace holds a deck. Before
 * that it is scratch (the port's one test, as deck_menu.c's game_loaded). */
int Cheats_SaveLoaded(void)
{
    return ((const SaveDataWorkspace *)D_801D0000)->state.player_deck[0] != 0;
}

/* Build Deck, and its screen before a duel (Main_RunDuel's first step,
 * 0x80 once set up, as deck_menu.c's duel_chest), copy the trunk into their
 * workspace as they open (func_800323F8) and write that copy back over it
 * as they close (func_800339D0): cards given in between are not listed,
 * and leaving takes them back. */
int Cheats_ChestOnScreen(void)
{
    int mode = D_8009B26C & 0x1F;
    return mode == MAIN_MODE_BUILD_DECK || (mode == MAIN_MODE_DUEL && D_8009B26E == 0x80);
}

/* The chest lives in the persistent save state at 0x801D0250: one byte per
 * card, ids 1..722, read by the Library, BUILD DECK and the duel's deck
 * checks, and written out whole by SAVE. */
static int fill_chest(int count, int top_up)
{
    int id;
    if (!Cheats_SaveLoaded() || Cheats_ChestOnScreen()) {
        return 0;
    }
    /* The added cards' trunk belongs to the save's duelist code, and
     * Cards_Frame empties it when the code changes: on the frame a NEW GAME
     * goes live that would come after this fill (MEMORIES_DEBUG_CHEST), so
     * let it take the new code first. */
    Cards_Frame();
    if (count < 0) {
        count = 0;
    }
    if (count > Tables_ChestRoom()) {
        count = Tables_ChestRoom();   /* 250, or a mod's chest past it */
    }
    for (id = 0; id < CARD_COUNT; id++) {
        if (!top_up || gLibrary_abCardChest[id] < count) gLibrary_abCardChest[id] = (u8)count;
    }
    /* And the cards mods added, whose trunk is kept outside the save. */
    for (id = CARD_ID_END; id <= gCard_nCount; id++) {
        u8 *slot = Cards_ChestSlot(gDuel_awPlayerDeck, id);
        if (!top_up || *slot < count) *slot = (u8)count;
    }
    fprintf(stderr, "memories-pc: chest now holds %s%d of every card\n", top_up ? "at least " : "", count);
    return 1;
}

int Cheats_GiveAllCards(int count) { return fill_chest(count, 0); }
int Cheats_TopUpAllCards(int count) { return fill_chest(count, 1); }

int Cheats_UnlockAllFreeDuelists(void)
{
    int opponent;
    if (!Cheats_SaveLoaded()) {
        return 0;
    }
    /* Match FreeDuel_Init's locked range. The other grid entries (including
     * Master K) are already available; these flags do not mark story wins. */
    for (opponent = FREE_DUEL_STORY_OPPONENT_FIRST_INDEX;
         opponent < FREE_DUEL_STORY_OPPONENT_INDEX_END; opponent++) {
        Library_UpdateCardUsedFlag(FREE_DUEL_UNLOCK_FLAG_BASE + opponent);
    }
    fprintf(stderr, "memories-pc: all CPU duelists unlocked; reopen Free Duel to refresh the roster, then save to keep them\n");
    return 1;
}

/* SaveDataState.starchips (0x801D07E0), which the duel's reward caps at
 * 999999 (func_800218F0) and the Password screen spends. That screen copies
 * the balance for display when it opens and at each payment step, so a
 * change made while it is open shows from the next of those. */
int Cheats_SetStarchips(unsigned value)
{
    if (!Cheats_SaveLoaded()) {
        return 0;
    }
    if (value > Tables_StarchipCap()) value = Tables_StarchipCap();   /* 999999, or a mod's "limits" */
    gLibrary_dwStarchips = value;
    fprintf(stderr, "memories-pc: StarChips now %u\n", value);
    return 1;
}

int Cheats_StartingLifePoints(void)
{
    /* A tower whose mod turns cheats off: its duels begin as the console's
     * (or the floor's "life_points"), whatever the menu says (tower.h). */
    if (Tower_CheatsOff()) return DUEL_STARTING_LIFE_POINTS;
    return Settings_Get(SET_CHEAT_LIFE_POINTS);
}

int Cheats_FreeSpending(void)
{
    return Settings_Get(SET_CHEAT_FREE_SPENDING);
}

extern signed char gDuel_bOpponentID;

/* The card display reads a side's card_view_mode through this
 * (DUEL_CARD_VIEW_MODE). Duel_InitSideStates gives the CPU's record -1,
 * which draws the hand it plays from as card backs and dims it; 0, the
 * player's, draws the cards. With the cheat on the CPU's record reads 0.
 * Two-player duels (a negative opponent) keep the value their setup chose. */
s8 Cheats_CardViewMode(const DuelSideState *side)
{
    /* An online duel (netplay.h): this player's cards open, the other's as
     * backs. Only the picture differs between the two games. */
    if (gDuel_bOpponentID < 0 && Netplay_HidesOtherHand()) return side == &D_800E9FF0[Netplay_Side() - 1] ? 0 : -1;
    if (side->card_view_mode < 0 && side == &D_800E9FF0[1] && gDuel_bOpponentID >= 0 &&
        Settings_Get(SET_CHEAT_SHOW_HAND) && !Tower_CheatsOff()) {
        return 0;
    }
    return side->card_view_mode;
}

/* MEMORIES_DEBUG_DECK: the forty cards of the deck, as ids and ranges
 * ("723-762", "1,2,723"), repeated until the deck is full. */
static int parse_deck(const char *list, int ids[DECK_SIZE])
{
    int count = 0;
    const char *p = list;
    while (*p && count < DECK_SIZE) {
        char *end;
        long first = strtol(p, &end, 10), last;
        if (end == p) break;
        last = first;
        if (*end == '-') last = strtol(end + 1, &end, 10);
        for (; first <= last && count < DECK_SIZE; first++) {
            if (first >= CARD_ID_FIRST && first <= gCard_nCount) ids[count++] = (int)first;
        }
        p = *end == ',' ? end + 1 : end;
    }
    return count;
}

int Cheats_DeckCount(const char *list)
{
    int ids[DECK_SIZE];
    return parse_deck(list, ids);
}

int Cheats_SetDeck(const char *list)
{
    SaveDataWorkspace *save = (SaveDataWorkspace *)D_801D0000;
    int ids[DECK_SIZE], count = parse_deck(list, ids), i;
    if (!count) return 0;
    for (i = 0; i < DECK_SIZE; i++) save->state.player_deck[i] = (u16)ids[i % count];
    fprintf(stderr, "memories-pc: deck set from %s\n", list);
    return count;
}

void Cheats_Frame(void)
{
    static int wanted = -2; /* -2 unread, -1 off, else pending count */
    static long starchips = -1;
    static const char *deck;
    if (wanted == -2) {
        const char *value = getenv("MEMORIES_DEBUG_CHEST");
        wanted = value && *value ? atoi(value) : -1;
        deck = getenv("MEMORIES_DEBUG_DECK");
        if (deck && !*deck) deck = NULL;
        /* The balance, as Set StarChips puts it (capped the same way). */
        value = getenv("MEMORIES_DEBUG_STARCHIPS");
        starchips = value && *value ? strtol(value, NULL, 10) : -1;
    }
    if (wanted < 0 && !deck && starchips < 0) {
        return;
    }
    /* Not while Build Deck holds its copy of the chest and deck. */
    if (Cheats_SaveLoaded() && !Cheats_ChestOnScreen()) {
        if (wanted >= 0) Cheats_GiveAllCards(wanted);
        if (deck) Cheats_SetDeck(deck);
        if (starchips >= 0) Cheats_SetStarchips((unsigned)starchips);
        wanted = -1;
        starchips = -1;
        deck = NULL;
    }
}
