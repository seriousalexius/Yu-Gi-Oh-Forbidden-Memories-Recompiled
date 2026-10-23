/* fork: Alessio's deck rules for the campaign and the 1v1 online (deck_rules.h). */
#include "deck_rules.h"
#include "deck_tiers.h"
#include "cards.h"
#include "tables.h"
#include "pc/platform/menu.h"
#include "game/card_constants.h"
#include "game/duel_card.h"
#include <stdio.h>
#include <string.h>

#define BANNED_CARD 380      /* Blue-eyes Ultimate Dragon */
#define ONE_COPY_ATTACK 3000 /* a monster this strong goes in once (campaign), in all (1v1) */
#define ONLINE_SPELLS_MAX 2  /* legendary spells and traps in a 1v1 deck */
#define LINES_MAX 6
#define RETAIL_CARDS 722

static const char *const ok[1] = {"OK"};

static int attack(int id)
{
    int value = (int)(gDuel_adwCardStats[id - 1] & CARD_STAT_VALUE_MASK) * CARD_STAT_SCALE;
    return value > Tables_StatCap(0) ? Tables_StatCap(0) : value;
}

static int monster(int id)
{
    int cls = Cards_Class(id);
    return cls == CARD_CLASS_MONSTER || cls == CARD_CLASS_EFFECT_MONSTER;
}

static int one_copy(int id) { return monster(id) && attack(id) >= ONE_COPY_ATTACK; }

/* The power tier: the table for the retail cards, the attack for a mod's. */
static char tier(int id)
{
    if (id >= 1 && id <= RETAIL_CARDS && id < (int)sizeof(deck_tiers) - 1) return deck_tiers[id];
    return one_copy(id) ? 'L' : 'C';
}

static int online_banned(int id)
{
    return id == BANNED_CARD || tier(id) == 'V' || (monster(id) && attack(id) > ONE_COPY_ATTACK);
}

static int online_copies(int id) { return tier(id) == 'L' ? 1 : tier(id) == 'E' ? 2 : 3; }

static void name(int id, char *out, size_t size)
{
    if (!Cards_NameUtf8(id, out, size)) snprintf(out, size, "Carta %d", id);
}

typedef struct {
    char *out;
    size_t size;
    int lines;
} Report;

static void report(Report *r, const char *line)
{
    size_t at;
    r->lines++;
    if (!r->out || r->lines > LINES_MAX + 1) return;
    at = strlen(r->out);
    snprintf(r->out + at, r->size - at, "%s\n", r->lines > LINES_MAX ? "- ..." : line);
}

/* The copies of `id` in the deck, and whether i is its first place. */
static int copies(const unsigned short *deck, int i, int *first)
{
    int j, count = 0;
    *first = 1;
    for (j = 0; j < DECK_SIZE; j++) {
        if (deck[j] != deck[i]) continue;
        count++;
        if (j < i) *first = 0;
    }
    return count;
}

int DeckRules_Problems(int mode, const unsigned short *deck, char *out, size_t size)
{
    Report r = {out, size, 0};
    char card[96], line[256], strong[160] = "";
    int i, cards = 0, strong_count = 0, spells = 0;
    if (out && size) out[0] = '\0';
    for (i = 0; i < DECK_SIZE; i++) {
        int id = deck[i], first, count, most;
        if (id <= 0 || !Cards_Valid(id)) continue;
        cards++;
        count = copies(deck, i, &first);
        if (mode == DECK_RULES_ONLINE && tier(id) == 'L' && !monster(id)) spells++;
        if (mode == DECK_RULES_ONLINE && one_copy(id) && !online_banned(id)) {
            strong_count++;
            if (first) {
                size_t at = strlen(strong);
                name(id, card, sizeof(card));
                snprintf(strong + at, sizeof(strong) - at, "%s%s", at ? ", " : "", card);
            }
        }
        if (!first) continue;
        name(id, card, sizeof(card));
        if (mode == DECK_RULES_CAMPAIGN) {
            if (id == BANNED_CARD) snprintf(line, sizeof(line), "- %s: vietato nel mazzo", card);
            else if (one_copy(id) && count > 1) snprintf(line, sizeof(line), "- %s: %d copie (massimo 1)", card, count);
            else continue;
        } else if (online_banned(id)) {
            snprintf(line, sizeof(line), "- %s: vietata nell'1v1", card);
        } else {
            /* the 3000-attack monsters are counted together below */
            most = online_copies(id);
            if (one_copy(id) || count <= most) continue;
            snprintf(line, sizeof(line), "- %s: %d copie (massimo %d)", card, count, most);
        }
        report(&r, line);
    }
    if (mode == DECK_RULES_ONLINE) {
        if (strong_count > 1) {
            snprintf(line, sizeof(line), "- Mostri da 3000 ATK: %d (massimo 1 in tutto): %s", strong_count, strong);
            report(&r, line);
        }
        if (spells > ONLINE_SPELLS_MAX) {
            snprintf(line, sizeof(line), "- Magie/trappole leggendarie: %d (massimo %d in tutto)", spells,
                     ONLINE_SPELLS_MAX);
            report(&r, line);
        }
        if (cards < DECK_SIZE) {
            snprintf(line, sizeof(line), "- Il mazzo ha %d carte su %d", cards, DECK_SIZE);
            report(&r, line);
        }
    }
    return r.lines;
}

const char *DeckRules_Text(int mode)
{
    if (mode == DECK_RULES_ONLINE)
        return "1v1 online:\n"
               "- vietati nel mazzo i mostri sopra 3000 ATK (fusioni e rituali si')\n"
               "- mostri da 3000 ATK: 1 in tutto il mazzo\n"
               "- copie: Leggendarie 1, Epiche 2, Rare e Comuni 3\n"
               "- magie/trappole Leggendarie: 2 in tutto";
    return "Campagna e Free Duel:\n"
           "- Drago Occhi Blu Finale vietato nel mazzo\n"
           "- mostri da 3000 ATK o piu': 1 copia ciascuno";
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
    char lines[768], text[1024];
    if (!DeckRules_Problems(DECK_RULES_CAMPAIGN, deck, lines, sizeof(lines))) return 1;
    snprintf(text, sizeof(text),
             "Il tuo mazzo non rispetta le regole:\n%s\nI mostri con 3000 ATK o piu' vanno in una copia sola.\n"
             "Sistema il mazzo in Build Deck, poi duella.",
             lines);
    Menu_ShowNotice("Regole del mazzo", text, ok, 1, 0, NULL);
    return 0;
}

extern unsigned short gDuel_awPlayerDeck[]; /* the running save's deck */

void DeckRules_ShowPanel(void)
{
    static const char *const titles[DECK_RULES_MODES] = {"Campagna e Free Duel", "1v1 online"};
    char lines[DECK_RULES_MODES][600], text[1024];
    int mode, i, cards = 0;
    size_t at;
    for (i = 0; i < DECK_SIZE; i++) cards += gDuel_awPlayerDeck[i] > 0 && Cards_Valid(gDuel_awPlayerDeck[i]);
    at = (size_t)snprintf(text, sizeof(text), "Il tuo mazzo: %d/%d carte.\n", cards, DECK_SIZE);
    for (mode = 0; mode < DECK_RULES_MODES && at < sizeof(text); mode++) {
        int broken = DeckRules_Problems(mode, gDuel_awPlayerDeck, lines[mode], sizeof(lines[mode]));
        const char *rules = strchr(DeckRules_Text(mode), '\n');
        at += (size_t)snprintf(text + at, sizeof(text) - at, "\n%s: %s\n%s%s\n", titles[mode],
                               broken ? "DA SISTEMARE" : "OK", broken ? lines[mode] : "", rules ? rules + 1 : "");
    }
    if (at < sizeof(text))
        snprintf(text + at, sizeof(text) - at,
                 "\nIn Build Deck: Select apre le regole. Per l'1v1 puoi usare uno slot: Online > Mazzo 1v1.");
    Menu_ShowNotice("Regole del mazzo", text, ok, 1, 0, NULL);
}

int DeckRules_Legendary(const unsigned short *deck)
{
    int i, count = 0;
    for (i = 0; i < DECK_SIZE; i++)
        if (deck[i] > 0 && Cards_Valid(deck[i]) && tier(deck[i]) == 'L') count++;
    return count;
}
