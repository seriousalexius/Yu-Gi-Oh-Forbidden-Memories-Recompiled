/* fork: the campaign's and Free Duel's deck rules (deck_rules.h). */
#include "deck_rules.h"
#include "cards.h"
#include "tables.h"
#include "pc/platform/menu.h"
#include "game/card_constants.h"
#include "game/duel_card.h"
#include <stdio.h>
#include <string.h>

#define BANNED_CARD 380      /* Blue-eyes Ultimate Dragon */
#define ONE_COPY_ATTACK 3000 /* a monster this strong goes in once */
#define LINES_MAX 6

static const char *const ok[1] = {"OK"};

static int attack(int id)
{
    int value = (int)(gDuel_adwCardStats[id - 1] & CARD_STAT_VALUE_MASK) * CARD_STAT_SCALE;
    return value > Tables_StatCap(0) ? Tables_StatCap(0) : value;
}

static int one_copy(int id)
{
    int cls = Cards_Class(id);
    return (cls == CARD_CLASS_MONSTER || cls == CARD_CLASS_EFFECT_MONSTER) && attack(id) >= ONE_COPY_ATTACK;
}

static void name(int id, char *out, size_t size)
{
    if (!Cards_NameUtf8(id, out, size)) snprintf(out, size, "Carta %d", id);
}

int DeckRules_CanAdd(int id, int in_deck)
{
    char card[96], text[320];
    if (id <= 0 || !Cards_Valid(id)) return 1;
    if (id == BANNED_CARD) {
        name(id, card, sizeof(card));
        snprintf(text, sizeof(text), "%s non puo' stare nel mazzo: resta nella tua collezione.", card);
    } else if (one_copy(id) && in_deck >= 1) {
        name(id, card, sizeof(card));
        snprintf(text, sizeof(text), "%s e' gia' nel mazzo.\nI mostri con 3000 ATK o piu' vanno nel mazzo in una copia sola.",
                 card);
    } else {
        return 1;
    }
    Menu_ShowNotice("Regole del mazzo", text, ok, 1, 0, NULL);
    return 0;
}

int DeckRules_DeckOk(const unsigned short *deck)
{
    char card[96], text[1024];
    int i, j, lines = 0, more = 0;
    size_t at;
    snprintf(text, sizeof(text), "Il tuo mazzo non rispetta le regole:\n");
    for (i = 0; i < DECK_SIZE; i++) {
        int id = deck[i], copies = 0, seen = 0;
        if (id <= 0 || !Cards_Valid(id) || (id != BANNED_CARD && !one_copy(id))) continue;
        for (j = 0; j < DECK_SIZE; j++) {
            if (deck[j] == id) copies++;
            if (deck[j] == id && j < i) seen = 1;
        }
        if (seen || (id != BANNED_CARD && copies < 2)) continue;
        if (lines == LINES_MAX) {
            more = 1;
            continue;
        }
        name(id, card, sizeof(card));
        at = strlen(text);
        if (id == BANNED_CARD)
            snprintf(text + at, sizeof(text) - at, "- %s: vietato nel mazzo\n", card);
        else
            snprintf(text + at, sizeof(text) - at, "- %s: %d copie (massimo 1)\n", card, copies);
        lines++;
    }
    if (!lines) return 1;
    at = strlen(text);
    snprintf(text + at, sizeof(text) - at,
             "%s\nI mostri con 3000 ATK o piu' vanno in una copia sola.\nSistema il mazzo in Build Deck, poi duella.",
             more ? "- ...\n" : "");
    Menu_ShowNotice("Regole del mazzo", text, ok, 1, 0, NULL);
    return 0;
}
