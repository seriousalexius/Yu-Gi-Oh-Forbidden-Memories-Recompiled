/* Arena contro la CPU (Hearthstone's arena, in Forbidden Memories): a hero
 * of six styles gives ten fixed cards (eroi.txt), thirty picks of one card
 * out of three make the other thirty, and the forty play the CPU until three
 * losses or twelve wins; then StarChips by the wins, and with seven or more
 * a card of that deck for the chest.
 *
 * A button of the title's second menu, ARENA (mod.json), leaves the title
 * with a choice of the mod's own (MENU, then SCENE) into mode 13, the
 * developer slot the tower uses (Main_RunUnusedDeveloperMode, hooked: the
 * tower's runner when the arena is not up). The screens are the overlay's,
 * over the title's wall (Tower_Background), read from the pad as the
 * diario reads it.
 *
 * A duel is armed as the tower arms one (func_80024DC8, back to mode 13),
 * so it counts nowhere in the save: neither Free Duel's grid nor the
 * autosave's wins. Duel_ShuffleBothDecks is hooked: the arena's deck and the
 * CPU's arena deck are written straight into the duel's shuffled decks, so
 * the save's own deck is never touched, not even by a crash. The state
 * before the duel is copied and put back once the duel is over (the results
 * screen's SPOILS card, StarChips and counters); REWARD and STARCHIP are
 * refused in an arena duel, and draft-drops and diario ask "arena:duello"
 * before they offer or count anything.
 *
 * Everything is in mod-data/arena/arena.txt, written after every step
 * (key=value lines, the deck as card numbers): the game closed anywhere
 * comes back to the same screen; a duel left without its end (the game
 * closed in it, or a reset) counts as lost. The same file keeps the runs,
 * the best record and each hero's wins. The account (the fork's cloud.c)
 * carries it between devices: a file changed from outside is read again
 * (reread), so the run goes on where the other device left it.
 *
 * Tests only (not in mod.json): MEMORIES_MOD_ARENA_SEME (the random seed),
 * MEMORIES_MOD_ARENA_AUTO=1 (every screen takes its first choice by itself,
 * every AUTO_FRAMES; =N above 1: every N frames, for the photos),
 * MEMORIES_MOD_ARENA_ESITO=<bits> (no duel is played: bit k set wins the
 * k-th duel, clear loses it). The log's "arena: ..." lines tell each step. */
#include "types.h"
#include "pc/mods/modapi.h"

static const MemoriesModHost *host;

#include "../../shared/fmview.h"
#include "game/main_modes.h"
#include "game/fade.h"
#include "game/sound.h"
#include "game/func_80024DC8.h"
#include "game/func_80039794.h"
#include "game/duel_rewards.h"
#include "game/duel_shuffle_both_decks.h"
#include "game/rand_get_interval.h"
#include "game/duel_card_checks.h"
#include "game/duel_side_state.h"
#include "game/duel_scene_state.h"
#include "game/duel_result_display.h"
#include "game/main_reset_frontend_runtime.h"

extern u8 D_8009B26C, D_8009B26D, D_8009B268, D_8009B368, D_8009B362, D_8009B26E;
void Tower_Background(s32 on);

#define ARENA_MODE 13          /* TOWER_MODE: the developer slot */
#define ARENA_CHOICE 0x41      /* the title's choice of ours (MENU's result, SCENE's a) */
#define SECOND_MENU 5          /* D_8009B26D: the title opens on its second menu */
#define ARENA_SONG 0x72C0      /* the Free Duel screen's */
#define STATE_FILE "arena.txt"
#define HEROES_MAX 6
#define HERO_CARDS 10
#define PICKS 30
#define CHOICES 3
#define MAX_WINS 12
#define MAX_LOSSES 3
#define DUELS_MAX 15
#define AUTO_FRAMES 6

/* The Free Duel's duelists, weakest first: the k-th duel of a run (wins plus
 * losses) is against the k-th. */
static const int opponents[DUELS_MAX] = {2, 3, 4, 9, 10, 11, 12, 13, 14, 16, 15, 22, 34, 35, 36};
/* The heroes' ten cards' tiers (eroi.txt's order). The thirty picks' tiers
 * change with each run (tier_plan). */
static const char hero_tiers[] = "RRRCCCCCCC";
#define ARENA_LP 12000         /* both sides' LP in an arena duel (campaign and Free Duel keep 8000) */

enum { PHASE_NONE, PHASE_HERO, PHASE_PICK, PHASE_READY, PHASE_RESULT, PHASE_PRIZE, PHASE_SUMMARY };

typedef struct {
    char name[64], text[96], tag[4];
    int cards[HERO_CARDS];
    int valid;
} Hero;

static Hero heroes[HEROES_MAX];
static int hero_count;

/* What arena.txt keeps. */
static struct {
    int phase;
    int offered[CHOICES];     /* the heroes offered */
    int hero;                 /* the hero taken, -1 none */
    char order[PICKS + 1];    /* each pick's tier */
    int pick;                 /* picks made */
    int offer[CHOICES];       /* the pick's cards */
    int deck[DECK_SIZE], count;
    int wins, losses, last;   /* last: 1 won, 2 lost */
    int dueling;              /* a duel was armed */
    int prize_total, prize_count, prizes[2], paid, starchips, starchips_now, saved;
    unsigned rng, seed;
    int runs, best, hero_wins[HEROES_MAX], hero_runs[HEROES_MAX];
    int free_used;            /* the first run, free for everyone, was started */
} run;
static int entry_refused;     /* Inizia without the starchips the entry asks: the intro says so */

static int hide_results; /* the results screen covered: from its first frame to the result's record (rewards) */
static int duel_over;    /* the armed duel's results screen was reached: 1 won, 2 lost */
static void *original_runner, *original_shuffle, *original_run_duel, *original_init_sides, *original_rewards;
static int active;            /* mode 13 is the arena's */
static volatile int armed;    /* an arena duel is on (provided as "arena:duello") */
static int duel_k, start_duel, leave;
static unsigned char before_duel[SAVE_DATA_STATE_SIZE];
static FvPad pad;
static int cursor, button, confirm, list_top, list_rows = 8, auto_wait, auto_done;
/* The deck's list: the draft's cursor is in it, its chosen row, its first row
 * shown, how many rows show; the card sheet's card (0 closed). */
static int in_deck, deck_sel, deck_top, deck_shown = 8, sheet;
/* The whole deck at once (Square): open, and its columns' rows. */
static int deck_view;
static unsigned rev;

/* The arena online (the fork's pc/debug/netplay.c shares it as
 * "port:netplay_arena", the same layout as its NetplayArena): the draft runs
 * over the title with the session's seed, the same in both games; once the
 * deck is whole it goes to net->deck and the netplay plays the best of three.
 * The offline run waits in offline_run and comes back after; arena.txt is not
 * written meanwhile. */
typedef struct {
    uint32_t version;
    int32_t side;
    uint32_t seed;
    int32_t deck_ready;
    uint16_t deck[DECK_SIZE];
    int32_t duel, over, wins[2];
    char rival[64];
} NetArena;
static NetArena *net;
static int net_on, net_done;
static unsigned char offline_run[sizeof(run)];

static void say(const char *format, const char *text)
{
    host->log(host, format, text);
}

/* --- cards --------------------------------------------------------------------- */

static int tier_of(int id)
{
    return id > 0 && id < (int)sizeof(fv_fasce) - 1 ? fv_fasce[id] : '?';
}

static int in_arena(int id)
{
    return id > 0 && id < (int)sizeof(fv_arena) - 1 && fv_arena[id] == '1' && Cards_Valid(id);
}

static int copies_max(int tier)
{
    return tier == 'L' ? 1 : tier == 'E' ? 2 : 3;
}

static int copies(const int *deck, int n, int id)
{
    int i, c = 0;
    for (i = 0; i < n; i++) c += deck[i] == id;
    return c;
}

static void tier_counts(const int *deck, int n, char *out, size_t size)
{
    int l = 0, e = 0, r = 0, c = 0, i;
    for (i = 0; i < n; i++) {
        switch (tier_of(deck[i])) {
        case 'L': l++; break;
        case 'E': e++; break;
        case 'R': r++; break;
        default: c++; break;
        }
    }
    snprintf(out, size, "L %d  E %d  R %d  C %d", l, e, r, c);
}

/* How strong a card is for the CPU's picks: a monster's attack, anything
 * else by its tier. */
static int power(int id)
{
    if (fv_is_monster(id)) return fv_stat(id, 0);
    switch (tier_of(id)) {
    case 'L': return 3500;
    case 'E': return 2700;
    case 'R': return 2000;
    default: return 1200;
    }
}

/* Tier, then the monsters by attack, then the rest by number. */
static int show_order(const void *pa, const void *pb)
{
    static const char ranks[] = "LERC";
    int a = *(const int *)pa, b = *(const int *)pb;
    const char *ta = strchr(ranks, tier_of(a)), *tb = strchr(ranks, tier_of(b));
    int ra = ta ? (int)(ta - ranks) : 4, rb = tb ? (int)(tb - ranks) : 4;
    if (ra != rb) return ra - rb;
    if (fv_is_monster(a) != fv_is_monster(b)) return fv_is_monster(b) - fv_is_monster(a);
    if (fv_is_monster(a) && fv_stat(a, 0) != fv_stat(b, 0)) return fv_stat(b, 0) - fv_stat(a, 0);
    return a - b;
}

static void card_label(int id, char *out, size_t size)
{
    char name[64];
    fv_card_name(id, name, sizeof(name));
    snprintf(out, size, "%s (%d)", name, id);
}

/* --- the deck's list and the cards' texts ------------------------------------------ */

/* The lists' groups: monsters, spells (rituals too), equips, traps. */
static const char *const group_names[4] = {"Mostri", "Magie", "Equip", "Trappole"};

static int group_of(int id)
{
    switch (Cards_Class(id)) {
    case CARD_CLASS_SPELL:
    case CARD_CLASS_RITUAL_SPELL: return 1;
    case CARD_CLASS_EQUIP: return 2;
    case CARD_CLASS_TRAP: return 3;
    default: return 0;
    }
}

/* Group, then the monsters by attack, then show order. */
static int list_order(const void *pa, const void *pb)
{
    int a = *(const int *)pa, b = *(const int *)pb;
    if (group_of(a) != group_of(b)) return group_of(a) - group_of(b);
    if (fv_is_monster(a) && fv_stat(a, 0) != fv_stat(b, 0)) return fv_stat(b, 0) - fv_stat(a, 0);
    return show_order(pa, pb);
}

#define DECK_ROWS (DECK_SIZE + 4)

/* The deck as its list shows it: each group's heading (rows[i] = -1 - group,
 * counts[i] its cards), then the group's different cards (counts[i] the
 * copies). Row 0 is a heading whenever there are cards. */
static int deck_rows(int *rows, int *counts)
{
    int sorted[DECK_SIZE], n = 0, i, group = -1, head = 0;
    memcpy(sorted, run.deck, sizeof(int) * (size_t)run.count);
    qsort(sorted, (size_t)run.count, sizeof(int), list_order);
    for (i = 0; i < run.count; i++) {
        int g = group_of(sorted[i]);
        if (g != group) {
            group = g;
            head = n;
            rows[n] = -1 - g;
            counts[n++] = 0;
        }
        counts[head]++;
        if (rows[n - 1] == sorted[i]) {
            counts[n - 1]++;
        } else {
            rows[n] = sorted[i];
            counts[n++] = 1;
        }
    }
    return n;
}

/* deck_sel moved by `step` rows onto a card's (headings skipped), kept in
 * the list, deck_top so that it shows with the row over it. */
static void deck_move(int step)
{
    int rows[DECK_ROWS], counts[DECK_ROWS], n = deck_rows(rows, counts), dir = step < 0 ? -1 : 1, at;
    if (n < 2) {
        deck_sel = deck_top = 0;
        return;
    }
    at = fv_clamp(deck_sel + step, 0, n - 1);
    while (at > 0 && at < n - 1 && rows[at] < 0) at += dir;
    if (rows[at] < 0) at++;
    deck_sel = at;
    if (deck_sel - 1 < deck_top) deck_top = deck_sel - 1;
    if (deck_sel >= deck_top + deck_shown) deck_top = deck_sel - deck_shown + 1;
}

/* The card of the list's chosen row; 0 with no cards. */
static int deck_card(void)
{
    int rows[DECK_ROWS], counts[DECK_ROWS], n = deck_rows(rows, counts);
    if (n < 2) return 0;
    if (deck_sel < 0 || deck_sel >= n || rows[deck_sel] < 0) deck_move(0);
    return rows[deck_sel] > 0 ? rows[deck_sel] : 0;
}

/* The whole deck's view (Square): where each list row shows. */
static int view_col[DECK_ROWS], view_row[DECK_ROWS];

/* Left/Right in the whole deck: the same row of the column beside, or its
 * last card. */
static void view_side(int dir)
{
    int rows[DECK_ROWS], counts[DECK_ROWS], n = deck_rows(rows, counts), i, best = -1;
    if (deck_sel < 0 || deck_sel >= n || view_col[deck_sel] < 0) return;
    for (i = 0; i < n; i++)
        if (view_col[i] == view_col[deck_sel] + dir && view_row[i] >= 0 && view_row[i] <= view_row[deck_sel]) best = i;
    if (best >= 0) deck_sel = best;
}

/* The cards' texts in Italian (testi.txt, "number|text", written from the
 * disc's descriptions by tools/testi_carte.py), in plain letters. */
static char text_buf[96 * 1024];
static int text_at[CARD_TABLE_ID_END + 1]; /* 1 + where in text_buf, 0 none */

static void load_texts(void)
{
    FILE *file = host->open_asset(host, "testi.txt");
    char line[1024];
    size_t used = 0;
    int got = 0;
    if (!file) {
        host->log(host, "arena: testi.txt non trovato: niente testo sotto le carte");
        return;
    }
    while (fgets(line, sizeof(line), file)) {
        char *end;
        long id;
        size_t len;
        if (line[0] == '#') continue;
        id = strtol(line, &end, 10);
        if (end == line || *end != '|' || id <= 0 || id > CARD_TABLE_ID_END) continue;
        for (len = 0; line[len] && line[len] != '\r' && line[len] != '\n'; len++) {}
        line[len] = 0;
        fv_ascii(end + 1, text_buf + used, sizeof(text_buf) - used);
        len = strlen(text_buf + used);
        if (used + len + 2 >= sizeof(text_buf)) break;
        text_at[id] = (int)used + 1;
        used += len + 1;
        got++;
    }
    fclose(file);
    host->log(host, "arena: testi.txt: %d testi delle carte", got);
}

static const char *card_text(int id)
{
    return id > 0 && id <= CARD_TABLE_ID_END && text_at[id] ? text_buf + text_at[id] - 1 : NULL;
}

/* --- chance -------------------------------------------------------------------- */

static unsigned next(unsigned *state)
{
    unsigned x = *state ? *state : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *state = x;
}

static int roll(unsigned *state, int n)
{
    return n > 0 ? (int)(next(state) % (unsigned)n) : 0;
}

/* A pick's letter: a tier, or M / S for a Legendary monster / spell or trap
 * (an 'L' pick, either, is a run saved before those). */
static int pick_matches(int pick, int id)
{
    if (pick == 'M') return tier_of(id) == 'L' && fv_is_monster(id);
    if (pick == 'S') return tier_of(id) == 'L' && !fv_is_monster(id);
    return tier_of(id) == pick;
}

static const char *pick_name(int pick)
{
    return pick == 'M' ? "Leggendaria (mostro)" : pick == 'S' ? "Leggendaria (magia/trappola)" :
           pick == 'L' ? "Leggendaria" : pick == 'E' ? "Epica" : pick == 'R' ? "Rara" : "Comune";
}

/* Three different cards of pick letter `tier`, of the arena, none of which
 * would pass its copies in `deck`; fewer when the tier has no more. */
static int offer_three(int tier, const int *deck, int n, unsigned *state, int out[CHOICES])
{
    static int pool[CARD_TABLE_ID_END];
    int count = 0, got = 0, id;
    for (id = 1; id < (int)sizeof(fv_fasce) - 1 && count < CARD_TABLE_ID_END; id++) {
        if (pick_matches(tier, id) && in_arena(id) && copies(deck, n, id) < copies_max(tier_of(id)))
            pool[count++] = id;
    }
    while (got < CHOICES && count > 0) {
        int at = roll(state, count);
        out[got++] = pool[at];
        pool[at] = pool[--count];
    }
    return got;
}

static const char tier_letters[5] = {'M', 'S', 'E', 'R', 'C'};

/* How many of the thirty picks are of each kind (Legendary monster M,
 * Legendary spell or trap S, E, R, C), from the run's seed alone: the
 * player's picks, the CPU's decks of that run and, online, both players'
 * with a seed in common. M 1-2 and S 1-2 (50/50%: tools/leggendarie.py),
 * E 3-6 (15/35/35/15%), R 8-12 (10/20/40/20/10%), C the rest, at least 10. */
static void tier_plan(unsigned seed, int n[5])
{
    static const int e_w[] = {15, 35, 35, 15}, r_w[] = {10, 20, 40, 20, 10};
    unsigned state = seed * 2246822519u + 0x5EED7u;
    do {
        int x, i;
        n[0] = 1 + roll(&state, 2);
        n[1] = 1 + roll(&state, 2);
        for (x = roll(&state, 100), i = 0; x >= e_w[i]; x -= e_w[i], i++) {}
        n[2] = 3 + i;
        for (x = roll(&state, 100), i = 0; x >= r_w[i]; x -= r_w[i], i++) {}
        n[3] = 8 + i;
        n[4] = PICKS - n[0] - n[1] - n[2] - n[3];
    } while (n[4] < 10);
}

/* "Leggendarie 1 mostro + 2 magie, 5 Epiche, 9 Rare, 13 Comuni" */
static void tier_plan_text(unsigned seed, char *out, size_t size)
{
    int n[5];
    tier_plan(seed, n);
    snprintf(out, size, "Leggendarie %d mostr%s + %d magi%s, %d Epich%s, %d Rar%s, %d Comun%s", n[0],
             n[0] == 1 ? "o" : "i", n[1], n[1] == 1 ? "a" : "e", n[2], n[2] == 1 ? "a" : "e", n[3],
             n[3] == 1 ? "a" : "e", n[4], n[4] == 1 ? "e" : "i");
}

static int legendary_pick(char pick)
{
    return pick == 'M' || pick == 'S' || pick == 'L';
}

/* The thirty picks' tiers (tier_plan) in an order of `state`'s. */
static void shuffle_order(char *order, unsigned seed, unsigned *state)
{
    int i, t, n[5], at = 0;
    tier_plan(seed, n);
    for (t = 0; t < 5; t++)
        for (i = 0; i < n[t] && at < PICKS; i++) order[at++] = tier_letters[t];
    order[PICKS] = 0;
    for (i = PICKS - 1; i > 0; i--) {
        int j = roll(state, i + 1);
        char t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    /* No Legendary in the first three picks. */
    for (i = 0; i < 3; i++) {
        while (legendary_pick(order[i])) {
            int j = 3 + roll(state, PICKS - 3);
            if (!legendary_pick(order[j])) {
                char t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
        }
    }
}

/* --- heroes (eroi.txt) ------------------------------------------------------------ */

static void load_heroes(void)
{
    FILE *file = host->open_asset(host, "eroi.txt");
    char line[512];
    hero_count = 0;
    if (!file) {
        host->log(host, "arena: eroi.txt non trovato");
        return;
    }
    while (hero_count < HEROES_MAX && fgets(line, sizeof(line), file)) {
        Hero *h = &heroes[hero_count];
        char *a = strchr(line, '|'), *b = a ? strchr(a + 1, '|') : NULL, *at, *end, *tag;
        int i;
        if (line[0] == '#' || !b) continue;
        memset(h, 0, sizeof(*h));
        *a = *b = 0;
        snprintf(h->name, sizeof(h->name), "%s", line);
        snprintf(h->text, sizeof(h->text), "%s", a + 1);
        at = b + 1;
        h->valid = 1;
        for (i = 0; i < HERO_CARDS; i++) {
            h->cards[i] = (int)strtol(at, &end, 10);
            if (end == at) h->valid = 0;
            at = end;
            if (!in_arena(h->cards[i]) || tier_of(h->cards[i]) != hero_tiers[i]) h->valid = 0;
        }
        /* "Il Fusionista - Heishin" -> "Fus" */
        tag = h->name;
        if (!strncmp(tag, "Il ", 3) || !strncmp(tag, "La ", 3)) tag += 3;
        else if (!strncmp(tag, "L'", 2)) tag += 2;
        snprintf(h->tag, sizeof(h->tag), "%.3s", tag);
        if (!h->valid) say("arena: eroe %s non valido (carte o fasce sbagliate)", h->name);
        else
            host->log(host, "arena: eroe %s: %d %d %d %d %d %d %d %d %d %d", h->name, h->cards[0], h->cards[1],
                      h->cards[2], h->cards[3], h->cards[4], h->cards[5], h->cards[6], h->cards[7], h->cards[8],
                      h->cards[9]);
        hero_count++;
    }
    fclose(file);
}

static int valid_heroes(void)
{
    int i, n = 0;
    for (i = 0; i < hero_count; i++) n += heroes[i].valid;
    return n;
}

static int random_hero(unsigned *state, const int *not, int n_not)
{
    int list[HEROES_MAX], n = 0, i, j;
    for (i = 0; i < hero_count; i++) {
        int skip = !heroes[i].valid;
        for (j = 0; j < n_not; j++) skip |= not[j] == i;
        if (!skip) list[n++] = i;
    }
    return n ? list[roll(state, n)] : -1;
}

/* --- arena.txt ------------------------------------------------------------------- */

/* arena.txt as this mod wrote or read it last (-1 none): another content is
 * the account's copy from another device. */
static char disk[4096];
static long disk_size = -1;
static int disk_wait;
#define REREAD_FRAMES 60

static long read_disk(char *out, size_t size)
{
    FILE *file = host->open_data(host, STATE_FILE, "rb");
    long got = 0;
    if (!file) return -1;
    /* fgets, as the game lends no fread on Android */
    while (got < (long)size - 1 && fgets(out + got, (int)(size - (size_t)got), file)) got += (long)strlen(out + got);
    fclose(file);
    return got;
}

#define PUT(...) (used += (size_t)snprintf(text + used, used < sizeof(text) ? sizeof(text) - used : 0, __VA_ARGS__))

static void save_state(void)
{
    static char text[4096];
    size_t used = 0;
    FILE *file;
    int i;
    PUT("versione=1\nfase=%d\neroi=%d %d %d\neroe=%d\nordine=%s\nscelta=%d\nproposte=%d %d %d\nmazzo=", run.phase,
        run.offered[0], run.offered[1], run.offered[2], run.hero, run.order, run.pick, run.offer[0], run.offer[1],
        run.offer[2]);
    for (i = 0; i < run.count; i++) PUT("%s%d", i ? " " : "", run.deck[i]);
    PUT("\nvinte=%d\nperse=%d\nultimo=%d\nduello=%d\npremi=%d %d %d %d\npagato=%d %d %d %d\ncaso=%u\nseme=%u\n",
        run.wins, run.losses, run.last, run.dueling, run.prize_total, run.prize_count, run.prizes[0], run.prizes[1],
        run.paid, run.starchips, run.starchips_now, run.saved, run.rng, run.seed);
    PUT("corse=%d\nrecord=%d\ngratis_usato=%d\nvittorie_eroe=", run.runs, run.best, run.free_used);
    for (i = 0; i < HEROES_MAX; i++) PUT("%s%d", i ? " " : "", run.hero_wins[i]);
    PUT("\ncorse_eroe=");
    for (i = 0; i < HEROES_MAX; i++) PUT("%s%d", i ? " " : "", run.hero_runs[i]);
    PUT("\n");
    rev++;
    if (net_on) return; /* the online draft is not the offline run */
    if (used >= sizeof(text) || !(file = host->open_data(host, STATE_FILE, "wb"))) {
        host->log(host, "arena: arena.txt non scritto");
        return;
    }
    fwrite(text, 1, used, file);
    fclose(file);
    memcpy(disk, text, used);
    disk_size = (long)used;
}

static int numbers(const char *at, int *out, int max)
{
    int n = 0;
    char *end;
    while (n < max) {
        long v = strtol(at, &end, 10);
        if (end == at) break;
        out[n++] = (int)v;
        at = end;
    }
    return n;
}

/* The game lends strtol, not atoi or strtoul. */
#define atoi(text) ((int)strtol(text, NULL, 10))

static unsigned unumber(const char *text)
{
    unsigned v = 0;
    while (*text >= '0' && *text <= '9') v = v * 10u + (unsigned)(*text++ - '0');
    return v;
}

static void load_state(void)
{
    static char text[sizeof(disk) + 1];
    char *line, *end;
    int v[8];
    memset(&run, 0, sizeof(run));
    run.hero = -1;
    disk_size = read_disk(disk, sizeof(disk));
    if (disk_size < 0) return;
    memcpy(text, disk, (size_t)disk_size);
    text[disk_size] = 0;
    for (line = text; *line; line = end) {
        char *value, *nl;
        for (end = line; *end && *end != '\n'; end++) {}
        if (*end) *end++ = 0;
        if (!(value = strchr(line, '='))) continue;
        *value++ = 0;
        for (nl = value; *nl && *nl != '\r'; nl++) {}
        *nl = 0;
        if (!strcmp(line, "fase")) run.phase = atoi(value);
        else if (!strcmp(line, "eroi")) numbers(value, run.offered, CHOICES);
        else if (!strcmp(line, "eroe")) run.hero = atoi(value);
        else if (!strcmp(line, "ordine")) snprintf(run.order, sizeof(run.order), "%s", value);
        else if (!strcmp(line, "scelta")) run.pick = atoi(value);
        else if (!strcmp(line, "proposte")) numbers(value, run.offer, CHOICES);
        else if (!strcmp(line, "mazzo")) run.count = numbers(value, run.deck, DECK_SIZE);
        else if (!strcmp(line, "vinte")) run.wins = atoi(value);
        else if (!strcmp(line, "perse")) run.losses = atoi(value);
        else if (!strcmp(line, "ultimo")) run.last = atoi(value);
        else if (!strcmp(line, "duello")) run.dueling = atoi(value);
        else if (!strcmp(line, "premi") && numbers(value, v, 4) == 4) {
            run.prize_total = v[0], run.prize_count = v[1], run.prizes[0] = v[2], run.prizes[1] = v[3];
        } else if (!strcmp(line, "pagato") && numbers(value, v, 4) == 4) {
            run.paid = v[0], run.starchips = v[1], run.starchips_now = v[2], run.saved = v[3];
        } else if (!strcmp(line, "caso")) run.rng = unumber(value);
        else if (!strcmp(line, "seme")) run.seed = unumber(value);
        else if (!strcmp(line, "corse")) run.runs = atoi(value);
        else if (!strcmp(line, "record")) run.best = atoi(value);
        else if (!strcmp(line, "gratis_usato")) run.free_used = atoi(value);
        else if (!strcmp(line, "vittorie_eroe")) numbers(value, run.hero_wins, HEROES_MAX);
        else if (!strcmp(line, "corse_eroe")) numbers(value, run.hero_runs, HEROES_MAX);
    }
    /* A file that does not add up starts over (the statistics stay). */
    if (run.phase < PHASE_NONE || run.phase > PHASE_SUMMARY || (run.phase > PHASE_HERO && (run.hero < 0 ||
        run.hero >= hero_count)) || run.pick < 0 || run.pick > PICKS || (run.phase >= PHASE_READY &&
        run.count != DECK_SIZE) || (run.phase == PHASE_PICK && (int)strlen(run.order) != PICKS)) {
        host->log(host, "arena: arena.txt non valido, corsa azzerata");
        run.phase = PHASE_NONE;
    }
    run.prize_total = fv_clamp(run.prize_total, 0, 2);
    run.prize_count = fv_clamp(run.prize_count, 0, run.prize_total);
}

/* --- the run ------------------------------------------------------------------------ */

static void clear_run(void)
{
    run.phase = PHASE_NONE;
    run.hero = -1;
    run.pick = run.count = run.wins = run.losses = run.last = run.dueling = 0;
    run.prize_total = run.prize_count = run.paid = run.starchips = run.starchips_now = run.saved = 0;
    run.prizes[0] = run.prizes[1] = 0;
    run.order[0] = 0;
}

static void new_offer(void)
{
    char a[80], b[80], c[80];
    int n, i;
    for (i = 0; i < CHOICES; i++) run.offer[i] = 0;
    n = offer_three(run.order[run.pick], run.deck, run.count, &run.rng, run.offer);
    card_label(run.offer[0], a, sizeof(a));
    card_label(run.offer[1], b, sizeof(b));
    card_label(run.offer[2], c, sizeof(c));
    host->log(host, "arena: scelta %d/%d (%c): %s, %s, %s%s", run.pick + 1, PICKS, run.order[run.pick], a, b, c,
              n < CHOICES ? " (poche carte)" : "");
    cursor = in_deck = 0;
}

static void start_run(void)
{
    int i;
    if (valid_heroes() < CHOICES) {
        host->log(host, "arena: meno di 3 eroi validi in eroi.txt");
        return;
    }
    clear_run();
    run.seed = net_on ? net->seed | 1u : (unsigned)host->setting(host, "seme", 0);
    if (!run.seed) run.seed = (unsigned)host->now_us(host) | 1u;
    /* Online: the same tiers for both (order from the seed alone), but each side its own heroes and offers. */
    run.rng = net_on ? run.seed ^ (0x9E3779B9u * (unsigned)(net->side + 1)) : run.seed;
    for (i = 0; i < CHOICES; i++) run.offered[i] = random_hero(&run.rng, run.offered, i);
    run.phase = PHASE_HERO;
    cursor = in_deck = deck_sel = 0;
    host->log(host, "arena: corsa iniziata (seme %u), eroi proposti: %s, %s, %s", run.seed,
              heroes[run.offered[0]].name, heroes[run.offered[1]].name, heroes[run.offered[2]].name);
    save_state();
}

/* What Inizia costs now: nothing the first time, then premi.txt's entry
 * (MEMORIES_MOD_ARENA_INGRESSO=N for the tests). */
static long entry_cost;

static long entry_now(void)
{
    long cost = host->setting(host, "ingresso", -1);
    if (cost < 0) cost = entry_cost;
    return run.free_used ? cost : 0;
}

/* Inizia offline: the entry is taken from the save (and the save written, as
 * the prizes are, so closing the game does not give it back), then the run. */
static void pay_entry_and_start(void)
{
    SaveDataState *save = (SaveDataState *)gDuel_awPlayerDeck;
    long cost = entry_now();
    int (*salva)(void);
    if ((long)save->starchips < cost) {
        entry_refused = 1;
        host->log(host, "arena: ingresso %ld, starchip %u: non basta", cost, (unsigned)save->starchips);
        return;
    }
    start_run();
    if (run.phase != PHASE_HERO) return;
    entry_refused = 0;
    save->starchips -= (unsigned)cost;
    run.free_used = 1;
    save_state();
    host->log(host, "arena: ingresso pagato %ld starchip, ora %u", cost, (unsigned)save->starchips);
    salva = (int (*)(void))host->find(host, "autosalvataggio:salva");
    if (salva && cost > 0) host->log(host, "arena: salvataggio dopo l'ingresso %s", salva() ? "fatto" : "NON fatto");
}

static void choose_hero(int i)
{
    const Hero *h = &heroes[run.offered[i]];
    run.hero = run.offered[i];
    memcpy(run.deck, h->cards, sizeof(h->cards));
    run.count = HERO_CARDS;
    if (net_on) {
        unsigned common = run.seed;
        shuffle_order(run.order, run.seed, &common);
    } else
        shuffle_order(run.order, run.seed, &run.rng);
    run.pick = 0;
    run.phase = PHASE_PICK;
    in_deck = deck_sel = deck_top = 0;
    say("arena: eroe scelto %s", h->name);
    {
        char plan[96];
        tier_plan_text(run.seed, plan, sizeof(plan));
        host->log(host, "arena: rarita' di questa arena: %s", plan);
    }
    host->log(host, "arena: ordine delle fasce %s", run.order);
    new_offer();
    save_state();
}

static void take(int i)
{
    char label[80], counts[48];
    if (!run.offer[i]) return;
    run.deck[run.count++] = run.offer[i];
    card_label(run.offer[i], label, sizeof(label));
    host->log(host, "arena: scelta %d/%d presa %s", run.pick + 1, PICKS, label);
    run.pick++;
    if (run.pick >= PICKS || run.count >= DECK_SIZE) {
        run.phase = PHASE_READY;
        button = 0;
        tier_counts(run.deck, run.count, counts, sizeof(counts));
        host->log(host, "arena: mazzo pronto: %s (%d carte)", counts, run.count);
    } else {
        new_offer();
    }
    save_state();
}

/* The CPU's arena deck for duel k: a hero at random and thirty picks by the
 * same tiers, each pick weighed as a player would: strength plus the
 * fusions it makes with the monsters already taken (only with the duel's
 * fusion table loaded), from the first duel: Alessio, 24/10, the arena was
 * too easy. The offers stay at random, as the player's. */
static int cpu_deck(int k, int fusions_known, int out[DECK_SIZE], int *hero, int *fused)
{
    unsigned state = run.seed * 2654435761u + (unsigned)(k + 1) * 40503u;
    int policy = 2, n, p, i, j;
    char order[PICKS + 1];
    *hero = random_hero(&state, NULL, 0);
    *fused = 0;
    if (*hero < 0) return policy;
    memcpy(out, heroes[*hero].cards, sizeof(heroes[*hero].cards));
    n = HERO_CARDS;
    shuffle_order(order, run.seed, &state);
    for (p = 0; p < PICKS; p++) {
        int offer[CHOICES], got = offer_three(order[p], out, n, &state, offer), best = 0;
        if (!got) continue;
        if (policy == 0) {
            best = roll(&state, got);
        } else {
            /* As a player drafts: the card's own strength, plus what it
             * fuses into with the monsters already taken (half of what each
             * fusion gains over its stronger material, every copy counted,
             * up to 3000): two dragons and two thunders make a warrior worth
             * more than a slightly stronger loner. */
            int score[CHOICES], synergy_of_best = 0;
            for (i = 0; i < got; i++) {
                int synergy = 0;
                score[i] = power(offer[i]);
                if (policy == 2 && fusions_known && fv_is_monster(offer[i])) {
                    for (j = 0; j < n; j++) {
                        int result = fv_is_monster(out[j]) ? Duel_CheckFusion(offer[i], out[j]) : 0;
                        int stronger = power(offer[i]) > power(out[j]) ? power(offer[i]) : power(out[j]);
                        if (result && fv_is_monster(result) && power(result) > stronger)
                            synergy += (power(result) - stronger) / 2;
                    }
                    if (synergy > 3000) synergy = 3000;
                }
                score[i] += synergy;
                if (score[i] > score[best]) best = i;
                if (i == best) synergy_of_best = synergy;
            }
            if (synergy_of_best > 0) (*fused)++;
        }
        out[n++] = offer[best];
    }
    while (n < DECK_SIZE) out[n++] = out[0]; /* never: every tier has cards enough */
    return policy;
}

static const char *const policy_names[3] = {"a caso", "la piu forte", "forza + sinergia di fusioni"};

static void log_cpu(int k, int policy, int hero, int fused, const int *deck, int fusions_known)
{
    char name[48], counts[48];
    fv_duelist_name(opponents[k], name, sizeof(name));
    tier_counts(deck, DECK_SIZE, counts, sizeof(counts));
    host->log(host, "arena: duello %d contro %s (%d): mazzo d'arena della CPU, eroe %s, politica %s%s, fusioni "
              "preferite %d, %s", k + 1, name, opponents[k], hero >= 0 ? heroes[hero].name : "-",
              policy_names[policy], policy == 2 && !fusions_known ? " (fusioni non controllate)" : "", fused, counts);
}

static void record_result(int won)
{
    char name[48];
    int k = run.wins + run.losses;
    fv_duelist_name(opponents[fv_clamp(k, 0, DUELS_MAX - 1)], name, sizeof(name));
    if (won) run.wins++;
    else run.losses++;
    run.last = won ? 1 : 2;
    run.dueling = 0;
    hide_results = 0;
    run.phase = PHASE_RESULT;
    button = 0;
    confirm = 0;
    host->log(host, "arena: esito duello %d contro %s: %s, vittorie %d sconfitte %d", k + 1, name,
              won ? "vinto" : "perso", run.wins, run.losses);
    save_state();
}

static int run_over(void)
{
    return run.wins >= MAX_WINS || run.losses >= MAX_LOSSES;
}

static long starchip_prize(void)
{
    long s = host->limit(host, "starchip_prize.S");
    return s >= 0 ? s : 5;
}

/* The starchips a run pays for its wins: premi.txt ("wins|starchips", made
 * by tools/prezzi.py), or without it the S rank's prize a win, doubled at 12. */
static long prize_table[MAX_WINS + 1];
static int prize_table_ok;
static long entry_cost; /* premi.txt's "ingresso|N": what a run costs past the first, free one */

static void load_prizes(void)
{
    FILE *file = host->open_asset(host, "premi.txt");
    char line[64];
    int got = 0, v;
    long n;
    prize_table_ok = 0;
    entry_cost = 0;
    if (!file) {
        host->log(host, "arena: premi.txt non trovato: %ld starchip a vittoria", starchip_prize());
        return;
    }
    while (fgets(line, sizeof(line), file)) {
        char *end, *at;
        if (line[0] == '#') continue;
        if (!strncmp(line, "ingresso|", 9)) {
            n = strtol(line + 9, &end, 10);
            if (end != line + 9 && n >= 0) entry_cost = n;
            continue;
        }
        v = (int)strtol(line, &end, 10);
        if (end == line || *end != '|') continue;
        at = end + 1;
        n = strtol(at, &end, 10);
        if (end == at || v < 0 || v > MAX_WINS || n < 0) continue;
        got |= 1 << v;
        prize_table[v] = n;
    }
    fclose(file);
    prize_table_ok = got == (1 << (MAX_WINS + 1)) - 1;
    if (prize_table_ok)
        host->log(host, "arena: premi.txt: %ld starchip con 12 vittorie, ingresso %ld", prize_table[MAX_WINS],
                  entry_cost);
    else
        host->log(host, "arena: premi.txt incompleto (servono 0..12): %ld starchip a vittoria", starchip_prize());
}

static long run_prize(int wins)
{
    wins = fv_clamp(wins, 0, MAX_WINS);
    if (prize_table_ok) return prize_table[wins];
    return starchip_prize() * wins * (wins >= MAX_WINS ? 2 : 1);
}

static void arm_duel(void)
{
    int esito = host->setting(host, "esito", -1);
    duel_k = fv_clamp(run.wins + run.losses, 0, DUELS_MAX - 1);
    if (esito >= 0) {
        int deck[DECK_SIZE], hero, fused, policy = cpu_deck(duel_k, 0, deck, &hero, &fused);
        log_cpu(duel_k, policy, hero, fused, deck, 0);
        host->log(host, "arena: duello finto (MEMORIES_MOD_ARENA_ESITO)");
        record_result((esito >> duel_k) & 1);
        return;
    }
    memcpy(before_duel, gDuel_awPlayerDeck, SAVE_DATA_STATE_SIZE);
    armed = 1;
    duel_over = 0;
    run.dueling = 1;
    save_state();
    start_duel = 1;
}

/* Back from the duel: the save as it was before it, then the result. */
static void put_save_back(const char *why)
{
    int same = memcmp(before_duel, gDuel_awPlayerDeck, DECK_SIZE * 2) == 0;
    memcpy(gDuel_awPlayerDeck, before_duel, SAVE_DATA_STATE_SIZE);
    host->log(host, "arena: mazzo del salvataggio rimesso a posto (%s, %s)", same ? "uguale a prima" : "RIPRISTINATO",
              why);
    armed = 0;
}

static void take_result(void)
{
    int won = D_8009B362 == 0;
    put_save_back("fine duello");
    record_result(won);
}

static void pay(void);
static void log_prize_list(void);

static void to_prizes(void)
{
    run.prize_total = run.wins >= MAX_WINS ? 2 : run.wins >= 7 ? 1 : 0;
    run.prize_count = 0;
    run.phase = PHASE_PRIZE;
    cursor = list_top = 0;
    host->log(host, "arena: corsa finita: vittorie %d sconfitte %d, carte premio da scegliere %d", run.wins,
              run.losses, run.prize_total);
    save_state();
    /* No prize card to choose: straight to the starchips and the summary. */
    if (run.prize_total == 0) pay();
    else log_prize_list();
}

static void pay(void)
{
    SaveDataState *save = (SaveDataState *)gDuel_awPlayerDeck;
    unsigned cap = Tables_StarchipCap(), was = save->starchips, gain;
    int i, (*salva)(void);
    char label[80];
    if (run.paid) return;
    gain = (unsigned)run_prize(run.wins);
    save->starchips = was + gain > cap ? cap : was + gain;
    run.starchips = (int)gain;
    run.starchips_now = (int)save->starchips;
    host->log(host, "arena: premi: starchip +%u (%s), da %u a %u", gain, prize_table_ok ? "premi.txt" : "S a vittoria",
              was, (unsigned)save->starchips);
    for (i = 0; i < run.prize_count; i++) {
        int before = *Cards_ChestSlot(gDuel_awPlayerDeck, run.prizes[i]);
        Duel_AwardCard(run.prizes[i]);
        card_label(run.prizes[i], label, sizeof(label));
        host->log(host, "arena: carta premio %s: nel baule da %d a %d", label, before,
                  (int)*Cards_ChestSlot(gDuel_awPlayerDeck, run.prizes[i]));
    }
    run.paid = 1;
    run.runs++;
    if (run.wins > run.best) run.best = run.wins;
    if (run.hero >= 0 && run.hero < HEROES_MAX) {
        run.hero_wins[run.hero] += run.wins;
        run.hero_runs[run.hero]++;
    }
    run.phase = PHASE_SUMMARY;
    save_state();
    /* The autosave mod's save, when it is there (autosalvataggio:salva). */
    salva = (int (*)(void))host->find(host, "autosalvataggio:salva");
    run.saved = salva ? (salva() ? 1 : 2) : 0;
    host->log(host, "arena: salvataggio %s", run.saved == 1 ? "automatico fatto" : run.saved == 2 ?
              "automatico NON fatto" : "da fare a mano");
    save_state();
}

/* The deck's different cards, in show order. */
static int deck_different(int *out)
{
    int sorted[DECK_SIZE], n = 0, i;
    memcpy(sorted, run.deck, sizeof(sorted));
    qsort(sorted, DECK_SIZE, sizeof(int), show_order);
    for (i = 0; i < DECK_SIZE; i++) {
        if (!n || out[n - 1] != sorted[i]) out[n++] = sorted[i];
    }
    return n;
}

/* The prize list: with 12 wins the first card is free from the whole deck;
 * otherwise 3 different cards of the deck drawn by the run's seed (so the
 * same 3 after closing and opening again), not the card already taken. */
static int prize_list(int *out)
{
    int all[DECK_SIZE], n = deck_different(all), i, k, taken[DECK_SIZE] = {0}, got = 0;
    unsigned state = run.seed * 2654435761u + 0xA11CEu;
    if (run.wins >= MAX_WINS && run.prize_count == 0) {
        memcpy(out, all, (size_t)n * sizeof(int));
        return n;
    }
    for (i = 0; i < n; i++) taken[i] = run.prize_count > 0 && all[i] == run.prizes[0];
    for (k = 0; k < 64 && got < CHOICES; k++) {
        state = state * 1103515245u + 12345u;
        i = (int)((state >> 8) % (unsigned)n);
        if (!taken[i]) taken[i] = 2, got++;
    }
    for (i = 0; i < n && got < CHOICES; i++) if (!taken[i]) taken[i] = 2, got++;
    for (i = 0, k = 0; i < n; i++) if (taken[i] == 2) out[k++] = all[i];
    return k;
}

static void log_prize_list(void)
{
    int list[DECK_SIZE], n = prize_list(list), i;
    char text[160];
    size_t used = 0;
    text[0] = 0;
    for (i = 0; i < n && i < CHOICES + 1; i++) {
        snprintf(text + used, sizeof(text) - used, "%s%d", i ? ", " : "", list[i]);
        used = strlen(text);
    }
    host->log(host, "arena: carte premio proposte (%d): %s%s", n, text, n > CHOICES + 1 ? ", ..." : "");
}

static void choose_prize(int card)
{
    char label[80];
    if (run.prize_count >= run.prize_total) return;
    run.prizes[run.prize_count++] = card;
    cursor = list_top = 0;
    card_label(card, label, sizeof(label));
    host->log(host, "arena: carta premio scelta %s", label);
    if (run.prize_count >= run.prize_total) pay();
    else {
        log_prize_list();
        save_state();
    }
}

/* --- the duel ----------------------------------------------------------------------- */

/* The arena's deck and the CPU's, written where Duel_ShuffleDeck writes them
 * (the ids at base and 80 bytes on, each card's place at base - 0x54 and
 * base - 0x2C) and shuffled with the same swaps. */
static void shuffle_both(void *player, void *opponent)
{
    u16 *ids = (u16 *)gDuel_awPlayerShuffledDeck;
    u8 *places[2];
    int cpu[DECK_SIZE], hero, fused, policy, side, i;
    if (!armed || player != (void *)gDuel_awPlayerDeck) {
        ((void (*)(void *, void *))original_shuffle)(player, opponent);
        return;
    }
    places[0] = (u8 *)gDuel_awPlayerShuffledDeck - 0x54;
    places[1] = (u8 *)gDuel_awPlayerShuffledDeck - 0x2C;
    policy = cpu_deck(duel_k, 1, cpu, &hero, &fused);
    for (i = 0; i < DECK_SIZE; i++) {
        ids[i] = (u16)run.deck[i];
        ids[DECK_SIZE + i] = (u16)cpu[i];
        places[0][i] = places[1][i] = (u8)i;
    }
    for (side = 0; side < 2; side++) {
        u16 *b16 = ids + side * DECK_SIZE;
        u8 *b8 = places[side];
        for (i = 0; i < DUEL_DECK_SHUFFLE_SWAP_COUNT; i++) {
            int x = Rand_GetInterval(DECK_SIZE), y = Rand_GetInterval(DECK_SIZE);
            u16 t16 = b16[x];
            u8 t8 = b8[x];
            b16[x] = b16[y], b16[y] = t16;
            b8[x] = b8[y], b8[y] = t8;
        }
    }
    log_cpu(duel_k, policy, hero, fused, cpu, 1);
    host->log(host, "arena: duello %d: mazzo d'arena del giocatore in campo (il mazzo del salvataggio non e' toccato)",
              duel_k + 1);
}

/* Main_RunDuel's first frame opens the deck screen before a duel against the
 * CPU (D_8009B26E 0): not the arena's deck, so an arena duel goes straight
 * on to the duel, the way the deck screen leaves it. */
static void run_duel(void)
{
    ((void (*)(void))original_run_duel)();
    if (armed && D_8009B26E == 0) {
        Fade_WaitOut();
        Main_ResetFrontendRuntime();
        D_8009B26E = 1;
        host->log(host, "arena: schermata del mazzo del salvataggio saltata");
    }
}

/* Both sides start an arena duel with ARENA_LP, healing up to it. */
static void init_sides(void)
{
    int k;
    ((void (*)(void))original_init_sides)();
    if (!armed) return;
    for (k = 0; k < DUEL_SIDE_COUNT; k++) {
        D_800E9FF0[k].life_points.signed_value = ARENA_LP;
        D_800E9FF0[k].max_life_points = ARENA_LP;
    }
    host->log(host, "arena: life points %d per entrambi", ARENA_LP);
}

/* The results screen's SPOILS (a dropped card the arena does not give):
 * covered (black, drawn by draw() over it, so no card is seen) and left at
 * once, as X leaves it; the arena's screens tell the result. */
static void rewards(void)
{
    ((void (*)(void))original_rewards)();
    if (!armed) return;
    hide_results = 1;
    if (!duel_over) duel_over = D_8009B362 == 0 ? 1 : 2;
    if ((gDuel_wSceneStateFlags & 0x8000) && !(gDuel_wSceneStateFlags & 0x4000)) {
        gDuel_wSceneStateFlags |= 0x4000;
        Fade_SetTargetLevel(0, 6);
    }
}

static void refuse(MemoriesModEvent *event)
{
    if (event->phase != MEMORIES_BEFORE || !armed) return;
    event->handled = 1;
    host->log(host, "arena: %s del gioco rifiutato (duello d'arena)", event->type == MEMORIES_EVENT_REWARD ?
              "carta" : "starchip");
}

/* --- the screens' input ----------------------------------------------------------------- */

static void leave_arena(void)
{
    if (net_on) return; /* online the draft goes on (Esc gives up the series) */
    leave = 1;
}

static int current_card(void)
{
    static int list[DECK_SIZE];
    if (sheet) return sheet;
    if (deck_view) return deck_card();
    if (run.phase == PHASE_HERO)
        return in_deck ? heroes[run.offered[fv_clamp(cursor, 0, CHOICES - 1)]].cards[fv_clamp(deck_sel, 0, HERO_CARDS - 1)]
                       : 0;
    if (run.phase == PHASE_PICK && !in_deck) return run.offer[fv_clamp(cursor, 0, CHOICES - 1)];
    if (run.phase == PHASE_PRIZE) {
        int n = prize_list(list);
        return n ? list[fv_clamp(cursor, 0, n - 1)] : 0;
    }
    if (run.phase == PHASE_PICK || run.phase == PHASE_READY || run.phase == PHASE_RESULT || run.phase == PHASE_SUMMARY)
        return deck_card();
    return 0;
}

/* The screens whose deck the whole deck's view (Square) shows. */
static int can_view_deck(void)
{
    if (!run.count || confirm) return 0;
    return run.phase == PHASE_PICK || run.phase == PHASE_READY || run.phase == PHASE_RESULT ||
           run.phase == PHASE_SUMMARY;
}

/* Up/Down and L1/R1 (a page) in the deck's list. */
static void deck_keys(unsigned short repeat)
{
    if (repeat & FV_UP) deck_move(-1);
    if (repeat & FV_DOWN) deck_move(1);
    if (repeat & FV_L1) deck_move(-deck_shown);
    if (repeat & FV_R1) deck_move(deck_shown);
}

static void step_input(void)
{
    unsigned short pressed, repeat;
    int buttons = 2, list[DECK_SIZE], n;
    fv_pad_read(&pad, &pressed, &repeat);
    if ((n = host->setting(host, "auto", 0)) != 0) {
        pressed = repeat = 0;
        if (++auto_wait >= (n > 1 ? n : AUTO_FRAMES)) {
            auto_wait = 0;
            cursor = button = confirm = in_deck = sheet = deck_view = 0;
            pressed = FV_CROSS;
            if (run.phase == PHASE_NONE && auto_done) {
                pressed = 0;
                leave_arena();
                return;
            }
        }
    }
    /* The card sheet: Triangle on a card opens it, Triangle, X or O close it. */
    if (sheet) {
        if (pressed & (FV_TRIANGLE | FV_CROSS | FV_CIRCLE)) sheet = 0;
        return;
    }
    if ((pressed & FV_TRIANGLE) && current_card()) {
        sheet = current_card();
        return;
    }
    /* The whole deck: the arrows choose a card (Left/Right a column), Square, X or O close it. */
    if (deck_view) {
        if (repeat & FV_UP) deck_move(-1);
        if (repeat & FV_DOWN) deck_move(1);
        if (repeat & FV_LEFT) view_side(-1);
        if (repeat & FV_RIGHT) view_side(1);
        if (pressed & (FV_SQUARE | FV_CROSS | FV_CIRCLE)) deck_view = 0;
        if (current_card()) fv_art_load(current_card());
        return;
    }
    if ((pressed & FV_SQUARE) && can_view_deck()) {
        deck_view = 1;
        deck_move(0);
        return;
    }
    switch (run.phase) {
    case PHASE_NONE:
        if (pressed & (FV_LEFT | FV_RIGHT)) button ^= 1, entry_refused = 0;
        if (pressed & FV_CIRCLE) leave_arena();
        else if (pressed & FV_CROSS) {
            if (button == 0) pay_entry_and_start();
            else leave_arena();
        }
        break;
    case PHASE_HERO:
        /* Right (or Triangle) goes into the hero's ten cards: Up/Down among
           them, Triangle the card sheet, Left back to the heroes. */
        if (in_deck) {
            if (repeat & FV_UP) deck_sel = (deck_sel + HERO_CARDS - 1) % HERO_CARDS;
            if (repeat & FV_DOWN) deck_sel = (deck_sel + 1) % HERO_CARDS;
            if (pressed & FV_LEFT) in_deck = 0;
        } else {
            if (repeat & FV_UP) cursor = (cursor + CHOICES - 1) % CHOICES;
            if (repeat & FV_DOWN) cursor = (cursor + 1) % CHOICES;
            if (pressed & (FV_RIGHT | FV_TRIANGLE)) in_deck = 1, deck_sel = 0;
        }
        if (pressed & FV_CIRCLE) leave_arena();
        else if (pressed & FV_CROSS) choose_hero(cursor);
        break;
    case PHASE_PICK:
        /* Down past the third card goes on into the deck's list, Up from its
           first card back to the cards. */
        if (repeat & FV_UP) {
            if (in_deck && deck_sel <= 1) in_deck = 0, cursor = CHOICES - 1;
            else if (in_deck) deck_move(-1);
            else cursor = (cursor + CHOICES - 1) % CHOICES;
        }
        if (repeat & FV_DOWN) {
            if (in_deck) deck_move(1);
            else if (cursor == CHOICES - 1 && run.count) in_deck = 1, deck_sel = deck_top = 0, deck_move(0);
            else cursor = (cursor + 1) % CHOICES;
        }
        if ((repeat & (FV_L1 | FV_R1)) && run.count) {
            if (!in_deck) in_deck = 1, deck_sel = deck_top = 0, deck_move(0);
            else deck_move(repeat & FV_L1 ? -deck_shown : deck_shown);
        }
        if (pressed & FV_CIRCLE) leave_arena();
        else if ((pressed & FV_CROSS) && !in_deck) take(cursor);
        break;
    case PHASE_READY:
        deck_keys(repeat);
        if (pressed & (FV_LEFT | FV_RIGHT)) button ^= 1;
        if (pressed & FV_CIRCLE) leave_arena();
        else if (pressed & FV_CROSS) {
            if (button == 0) arm_duel();
            else leave_arena();
        }
        break;
    case PHASE_RESULT:
        deck_keys(repeat);
        if (confirm) {
            if (pressed & (FV_LEFT | FV_RIGHT)) button ^= 1;
            if (pressed & FV_CIRCLE) confirm = 0, button = 1;
            else if (pressed & FV_CROSS) {
                if (button == 0) {
                    host->log(host, "arena: ritirato");
                    confirm = 0;
                    to_prizes();
                } else {
                    confirm = 0;
                    button = 1;
                }
            }
            break;
        }
        if (run_over()) buttons = 1, button = 0;
        if (buttons == 2 && (pressed & (FV_LEFT | FV_RIGHT))) button ^= 1;
        if (pressed & FV_CIRCLE) leave_arena();
        else if (pressed & FV_CROSS) {
            if (run_over()) to_prizes();
            else if (button == 0) arm_duel();
            else confirm = 1, button = 1;
        }
        break;
    case PHASE_PRIZE:
        /* All chosen (or none to choose, e.g. a run saved before the payment). */
        if (run.prize_count >= run.prize_total) {
            pay();
            break;
        }
        n = prize_list(list);
        if (repeat & FV_UP) cursor = (cursor + n - 1) % n;
        if (repeat & FV_DOWN) cursor = (cursor + 1) % n;
        if (repeat & FV_L1) cursor = fv_clamp(cursor - list_rows, 0, n - 1);
        if (repeat & FV_R1) cursor = fv_clamp(cursor + list_rows, 0, n - 1);
        if (cursor < list_top) list_top = cursor;
        if (cursor >= list_top + list_rows) list_top = cursor - list_rows + 1;
        if (pressed & FV_CROSS) choose_prize(list[cursor]);
        break;
    case PHASE_SUMMARY:
        deck_keys(repeat);
        if (pressed & (FV_CROSS | FV_CIRCLE)) {
            clear_run();
            button = 0;
            save_state();
            if (host->setting(host, "auto", 0)) auto_done = 1;
        }
        break;
    }
    if (current_card()) fv_art_load(current_card());
}

/* Mode 13's runner: the tower's, unless the arena is up. */
static void runner(void)
{
    if (!active) {
        ((void (*)(void))original_runner)();
        return;
    }
    if ((D_8009B26C & 0x40) == 0) {
        D_8009B26C |= 0x40;
        start_duel = leave = 0;
        cursor = button = confirm = list_top = 0;
        in_deck = deck_sel = deck_top = sheet = deck_view = 0;
        pad.held = host->pad(host, 0); /* what chose ARENA, or left the duel, is not pressed here */
        SD_BGMPlay(ARENA_SONG);
        if (armed) take_result();
        Tower_Background(1);
        Fade_WaitIn();
        return;
    }
    step_input();
    if (start_duel) {
        start_duel = 0;
        Tower_Background(0);
        SD_BGMFadeOut();
        func_80024DC8(-1, opponents[duel_k], 0x6000, 0x6000);
        D_8009B368 = ARENA_MODE;
        return;
    }
    if (leave) {
        leave = 0;
        active = 0;
        Tower_Background(0);
        SD_SEPlayFull(8);
        SD_BGMFadeOut();
        D_8009B268 = 1;
        D_8009B26D = SECOND_MENU;
        D_8009B26C = MAIN_MODE_MENU;
        host->log(host, "arena: torno al menu");
        return;
    }
    func_80039794();
}

static void menu(MemoriesModEvent *event)
{
    const char *name;
    if (event->phase != MEMORIES_BEFORE || event->handled) return;
    if (!(name = host->menu_item(host, event->a)) || strcmp(name, "arena:arena") != 0) return;
    event->handled = 1;
    event->result = ARENA_CHOICE;
}

static void scene(MemoriesModEvent *event)
{
    if (event->phase != MEMORIES_BEFORE || event->handled || event->a != ARENA_CHOICE) return;
    event->handled = 1;
    active = 1;
    auto_wait = 0;
    D_8009B268 = 1;
    D_8009B26D = (u8)event->a;
    D_8009B26C = ARENA_MODE;
    host->log(host, "arena: aperta (fase %d)", run.phase);
}

/* The online draft (see NetArena): once, before the series' first duel. */
static void online(void)
{
    int i;
    if (!net) net = (NetArena *)host->find(host, "port:netplay_arena");
    if (!net || net->version != 1 || net_done) return;
    if (net->over || net->duel > 0 || net->deck_ready) {
        net_done = 1;
        if (net_on) {
            net_on = 0;
            memcpy(&run, offline_run, sizeof(run));
            rev++;
        }
        return;
    }
    if (!net_on) {
        memcpy(offline_run, &run, sizeof(run));
        net_on = 1;
        cursor = button = confirm = list_top = auto_wait = 0;
        in_deck = deck_sel = deck_top = sheet = deck_view = 0;
        pad.held = host->pad(host, 0);
        host->log(host, "arena: arena online contro %s", net->rival);
        start_run();
        if (run.phase != PHASE_HERO) {
            net_done = 1, net_on = 0;
            memcpy(&run, offline_run, sizeof(run));
            return;
        }
    }
    step_input();
    if (run.phase == PHASE_READY && run.count == DECK_SIZE) {
        for (i = 0; i < DECK_SIZE; i++) net->deck[i] = (uint16_t)run.deck[i];
        net->deck_ready = 1;
        host->log(host, "arena: mazzo online pronto, lo passo al duello");
    }
}

/* Every second or so, off a duel: arena.txt changed from outside (the
 * account's copy from another device) is the run from then on. */
static void reread(void)
{
    static char now[sizeof(disk)];
    long size;
    if (armed || net_on || ++disk_wait < REREAD_FRAMES) return;
    disk_wait = 0;
    size = read_disk(now, sizeof(now));
    if (size < 0 || (size == disk_size && !memcmp(now, disk, (size_t)size))) return;
    host->log(host, "arena: arena.txt cambiato da fuori (account), corsa riletta");
    load_state();
    cursor = button = confirm = list_top = 0;
    in_deck = deck_sel = deck_top = sheet = deck_view = 0;
    entry_refused = 0;
    rev++;
}

static void frame(void)
{
    online();
    reread();
    /* Back on the title with a duel still armed (a reset): lost, unless its
     * results screen said won. */
    if ((D_8009B26C & 0x1F) == MAIN_MODE_MENU) {
        if (armed) {
            put_save_back("duello interrotto");
            host->log(host, "arena: duello interrotto, %s", duel_over == 1 ? "era gia' vinto" : "contato come perso");
            record_result(duel_over == 1);
        }
        active = 0;
    }
    /* A duel the game was closed in (here, or on the device the account's
     * run came from): lost. */
    if (run.dueling && !armed && !net_on) {
        host->log(host, "arena: il gioco si era chiuso durante un duello: contato come perso");
        record_result(0);
    }
}

static void reset(void)
{
    active = 0;
    hide_results = 0;
}

/* --- drawing --------------------------------------------------------------------------- */

#define TEXT_RGB 0xB8C0D8
#define DIM_RGB 0x8890A8
#define GOLD_RGB 0xF0D070
#define WIN_RGB 0x70E070
#define LOSS_RGB 0xF08060

typedef struct {
    FvArea area;
    int s, line, pad, gap, x, y, w, h, top, bottom, buttons_mid, help_mid, buttons_end;
} Panel;

static void right_text(int right, int mid, const char *text, uint32_t rgb, int s)
{
    host->draw_text(host, right - host->text_width(host, text, s), mid, text, rgb, s);
}

static void panel_begin(Panel *p, const char *title, const char *corner, const char *subtitle, const char *help)
{
    char t[128];
    int s, corner_w;
    fv_area(&p->area, 380);
    s = p->s = p->area.s;
    p->line = 18 * s;
    p->pad = 10 * s;
    p->gap = 10 * s;
    p->w = p->area.w > 760 * s ? 760 * s : p->area.w;
    p->h = p->area.h;
    p->x = p->area.x + (p->area.w - p->w) / 2;
    p->y = p->area.y;
    host->fill(host, 0, 0, p->area.width, p->area.height, 0x000000, 110);
    fv_box(p->x, p->y, p->w, p->h, s);
    corner_w = corner ? host->text_width(host, corner, s) + p->gap : 0;
    if (corner) right_text(p->x + p->w - p->pad, p->y + p->pad + p->line / 2, corner, TEXT_RGB, s);
    snprintf(t, sizeof(t), "%s", title);
    fv_fit(t, p->w - 2 * p->pad - corner_w, s);
    host->draw_text(host, p->x + p->pad, p->y + p->pad + p->line / 2, t, GOLD_RGB, s);
    snprintf(t, sizeof(t), "%s", subtitle ? subtitle : "");
    fv_fit(t, p->w - 2 * p->pad, s);
    host->draw_text(host, p->x + p->pad, p->y + p->pad + p->line + p->line / 2, t, TEXT_RGB, s);
    p->top = p->y + p->pad + 2 * p->line + 4 * s;
    p->help_mid = p->y + p->h - p->pad - p->line / 2;
    snprintf(t, sizeof(t), "%s", help);
    fv_fit(t, p->w - 2 * p->pad, s);
    host->draw_text(host, p->x + p->pad, p->help_mid, t, DIM_RGB, s);
    p->buttons_mid = p->help_mid - p->line - 4 * s;
    p->bottom = p->help_mid - p->line / 2 - 4 * s;
    p->buttons_end = p->x + p->pad;
}

/* Buttons in a row over the help line; the content then ends above them. */
static void draw_buttons(Panel *p, const char *const *labels, int n, int chosen)
{
    int i, at = p->x + p->pad, s = p->s, bh = p->line + 6 * s;
    for (i = 0; i < n; i++) {
        int bw = host->text_width(host, labels[i], s) + 24 * s, on = i == chosen;
        host->fill(host, at, p->buttons_mid - bh / 2, bw, bh, on ? FV_CURSOR_RGB : 0x1C2448, 240);
        if (on) {
            host->fill(host, at, p->buttons_mid - bh / 2, bw, s, 0xC8A040, 255);
            host->fill(host, at, p->buttons_mid + bh / 2 - s, bw, s, 0xC8A040, 255);
        }
        host->draw_text(host, at + 12 * s, p->buttons_mid, labels[i], on ? 0xFFFFFF : TEXT_RGB, s);
        at += bw + p->gap;
    }
    p->buttons_end = at;
    p->bottom = p->buttons_mid - bh / 2 - 4 * s;
}

/* "Vedi mazzo" at the buttons' row's right, with the pad's pink square: what
 * Square opens; shorter when the row has no room. */
static void draw_view_button(Panel *p)
{
    const char *label = "Vedi mazzo";
    int s = p->s, bh = p->line + 6 * s, box = 9 * s, bw, x, y = p->buttons_mid - bh / 2;
    if (!can_view_deck()) return;
    bw = 12 * s + box + 6 * s + host->text_width(host, label, s) + 12 * s;
    if (p->x + p->w - p->pad - bw < p->buttons_end) {
        label = "Mazzo";
        bw = 12 * s + box + 6 * s + host->text_width(host, label, s) + 12 * s;
    }
    x = p->x + p->w - p->pad - bw;
    host->fill(host, x, y, bw, bh, 0x1C2448, 240);
    host->fill(host, x, y, bw, s, 0x6C7498, 255);
    host->fill(host, x, y + bh - s, bw, s, 0x6C7498, 255);
    /* the square: a pink frame */
    host->fill(host, x + 12 * s, p->buttons_mid - box / 2, box, box, 0xE088C8, 255);
    host->fill(host, x + 14 * s, p->buttons_mid - box / 2 + 2 * s, box - 4 * s, box - 4 * s, 0x1C2448, 255);
    host->draw_text(host, x + 12 * s + box + 6 * s, p->buttons_mid, label, 0xF4F4F4, s);
    if (p->bottom > y - 4 * s) p->bottom = y - 4 * s;
}

/* A heart of 7x6 squares. */
static void heart(int x, int mid, int s, int full)
{
    static const char *const rows[6] = {".XX.XX.", "XXXXXXX", "XXXXXXX", ".XXXXX.", "..XXX..", "...X..."};
    int r, c, d = 2 * s;
    for (r = 0; r < 6; r++)
        for (c = 0; c < 7; c++)
            if (rows[r][c] == 'X')
                host->fill(host, x + c * d, mid - 3 * d + r * d, d, d, full ? 0xE04050 : 0x404858, 255);
}

/* The hearts and the record; its width. */
static int draw_record(int x, int mid, int s)
{
    char text[64];
    int i;
    for (i = 0; i < MAX_LOSSES; i++) heart(x + i * 16 * s, mid, s, i >= run.losses);
    snprintf(text, sizeof(text), "Vittorie %d   Sconfitte %d", run.wins, run.losses);
    host->draw_text(host, x + MAX_LOSSES * 16 * s + 6 * s, mid, text, 0xF4F4F4, s);
    return MAX_LOSSES * 16 * s + 6 * s + host->text_width(host, text, s);
}

static void card_row(int id, int x, int mid, int w, int s, int with_stats)
{
    char name[64], stat[24];
    int stat_w = with_stats ? host->text_width(host, "0000/0000", s) + 6 * s : 0;
    fv_card_name(id, name, sizeof(name));
    fv_fit(name, w - stat_w - fv_icon_room(s), s);
    fv_icon_text(id, x, mid, name, fv_tier_rgb(id), s);
    if (with_stats) {
        fv_stats(id, stat, sizeof(stat));
        right_text(x + w, mid, stat, TEXT_RGB, s);
    }
}

/* `text` in lines `w` wide, the first one's middle at `mid`, down to
 * `bottom`; the last line that fits ends in ".." when there is more. The
 * next line's middle. */
static int draw_wrapped(const char *text, int x, int mid, int w, int bottom, int line, uint32_t rgb, int s)
{
    char row[256];
    while (*text == ' ') text++;
    while (*text && mid + line / 2 <= bottom) {
        int end = 0, fit = 0, last = mid + line + line / 2 > bottom;
        /* As many words as fit. */
        for (;;) {
            int e = end;
            while (text[e] == ' ') e++;
            while (text[e] && text[e] != ' ') e++;
            if (e == end || e >= (int)sizeof(row)) break;
            memcpy(row, text, (size_t)e);
            row[e] = 0;
            if (host->text_width(host, row, s) > w) break;
            end = e;
            fit = 1;
            if (!text[e]) break;
        }
        if (!fit || (last && text[end])) {
            /* One word too long, or no line left for the rest: cut. */
            snprintf(row, sizeof(row), "%s", text);
            fv_fit(row, w, s);
            host->draw_text(host, x, mid, row, rgb, s);
            return mid + line;
        }
        memcpy(row, text, (size_t)end);
        row[end] = 0;
        host->draw_text(host, x, mid, row, rgb, s);
        text += end;
        while (*text == ' ') text++;
        mid += line;
    }
    return mid;
}

/* The card `id` at the panel's right from `top`, its text under it; the
 * width left on the left. */
static int draw_card_side(const Panel *p, int id, int top)
{
    int s = p->s, zoom, cw = 0, ch = 0, x;
    zoom = fv_card_zoom((p->w - 2 * p->pad) * 2 / 5, p->bottom - top - 4 * p->line, s);
    if (zoom > 3 * s) zoom = 3 * s;
    if (!zoom) return p->w - 2 * p->pad;
    fv_card_size(zoom, s, &cw, &ch);
    x = p->x + p->w - p->pad - cw;
    if (id) {
        fv_card_draw(id, x, top, zoom, s);
        if (card_text(id))
            draw_wrapped(card_text(id), x + 2 * s, top + ch + 4 * s + 15 * s / 2, cw - 4 * s, p->bottom, 15 * s,
                         TEXT_RGB, s);
    }
    return p->w - 2 * p->pad - cw - p->gap;
}

/* The deck's list in x..x+w from `top` to `bottom`: the groups' headings,
 * the cards with their stats and copies; the chosen row lit when `lit`. */
static void draw_deck_list(const Panel *p, int x, int top, int w, int bottom, int lit)
{
    int rows[DECK_ROWS], counts[DECK_ROWS], n = deck_rows(rows, counts), s = p->s, line = 15 * s, i, shown,
        copies_w = host->text_width(host, " x3", s);
    char text[48];
    shown = (bottom - top) / line;
    if (shown < 1) shown = 1;
    deck_shown = shown;
    if (lit && deck_sel >= deck_top + shown) deck_top = deck_sel - shown + 1;
    deck_top = fv_clamp(deck_top, 0, n > shown ? n - shown : 0);
    for (i = 0; i < shown && deck_top + i < n; i++) {
        int r = deck_top + i, mid = top + i * line + line / 2;
        if (rows[r] < 0) {
            snprintf(text, sizeof(text), "%s (%d)", group_names[-1 - rows[r]], counts[r]);
            host->draw_text(host, x, mid, text, GOLD_RGB, s);
        } else {
            if (lit && r == deck_sel)
                host->fill(host, x - p->pad / 2, mid - line / 2, w + p->pad, line, FV_CURSOR_RGB, 230);
            card_row(rows[r], x, mid, w - copies_w, s, 1);
            if (counts[r] > 1) {
                snprintf(text, sizeof(text), "x%d", counts[r]);
                right_text(x + w, mid, text, GOLD_RGB, s);
            }
        }
        /* More rows over or under the ones shown. */
        if ((i == 0 && deck_top > 0) || (i == shown - 1 && r < n - 1))
            right_text(x + w + p->pad / 2 + 4 * s, mid, i == 0 ? "^" : "v", DIM_RGB, s);
    }
}

static void draw_intro(void)
{
    static const char *const labels[2] = {"Inizia", "Indietro"};
    Panel p;
    char text[160], per[96];
    int i, mid, s;
    panel_begin(&p, "ARENA", NULL, "Contro la CPU, con un mazzo costruito ora", "Sx/Dx scegli   X conferma   O indietro");
    draw_buttons(&p, labels, 2, button);
    s = p.s;
    mid = p.top + p.line / 2;
    {
        const char *lines[6];
        char last[128], entry[128];
        long cost = entry_now();
        unsigned have = ((SaveDataState *)gDuel_awPlayerDeck)->starchips;
        snprintf(last, sizeof(last), "Premi: %ld starchip con 1 vittoria, %ld con 3, %ld con 6, %ld con 12.",
                 run_prize(1), run_prize(3), run_prize(6), run_prize(MAX_WINS));
        if (entry_refused)
            snprintf(entry, sizeof(entry), "Servono %ld starchip, ne hai %u: vinci duelli in campagna o Free Duel.",
                     cost, have);
        else if (!cost) snprintf(entry, sizeof(entry), "Ingresso: GRATIS, la tua prima arena! (hai %u starchip)", have);
        else snprintf(entry, sizeof(entry), "Ingresso: %ld starchip (ne hai %u)", cost, have);
        lines[0] = "Scegli un eroe tra 3: ti da 10 carte fisse del suo stile.";
        lines[1] = "Poi 30 scelte, 1 carta su 3: ecco il tuo mazzo da 40.";
        lines[2] = "Duelli la CPU finche perdi 3 volte o vinci 12 duelli.";
        lines[3] = last;
        lines[4] = "Da 7 vittorie: 1 carta tra 3 a caso del mazzo. Con 12: anche 1 a scelta.";
        lines[5] = entry;
        for (i = 0; i < 6; i++, mid += p.line) {
            snprintf(text, sizeof(text), "%s", lines[i]);
            fv_fit(text, p.w - 2 * p.pad, s);
            host->draw_text(host, p.x + p.pad, mid, text,
                            i < 5 ? 0xF4F4F4 : entry_refused ? LOSS_RGB : GOLD_RGB, s);
        }
    }
    mid += p.line / 2;
    per[0] = 0;
    for (i = 0; i < hero_count; i++) {
        size_t n = strlen(per);
        snprintf(per + n, sizeof(per) - n, "%s%s %d", i ? ", " : "", heroes[i].tag, run.hero_wins[i]);
    }
    snprintf(text, sizeof(text), "Corse %d   Record %d vittorie   Vittorie per eroe: %s", run.runs, run.best, per);
    fv_fit(text, p.w - 2 * p.pad, s);
    host->draw_text(host, p.x + p.pad, mid, text, GOLD_RGB, s);
    if (valid_heroes() < CHOICES)
        host->draw_text(host, p.x + p.pad, mid + p.line, "eroi.txt non valido: guarda il log", LOSS_RGB, s);
}

static void draw_hero(void)
{
    Panel p;
    const Hero *h;
    char text[128];
    int s, i, left_w, mid, rx, rw;
    panel_begin(&p, "ARENA - SCEGLI L'EROE", NULL, "Ognuno ti da 10 carte fisse: 3 Rare e 7 Comuni",
                in_deck ? "Su/Giu carte   Triangolo scheda   Sinistra eroi   X scegli"
                        : "Su/Giu scorri   Destra le sue carte   X scegli   O menu (la corsa resta)");
    s = p.s;
    left_w = (p.w - 2 * p.pad - p.gap) * 9 / 20;
    for (i = 0; i < CHOICES; i++) {
        int top = p.top + i * (3 * p.line), on = i == cursor;
        h = &heroes[run.offered[i]];
        if (on) host->fill(host, p.x + p.pad / 2, top, left_w + p.pad, 3 * p.line - 4 * s, FV_CURSOR_RGB,
                           in_deck ? 110 : 230);
        snprintf(text, sizeof(text), "%s", h->name);
        fv_fit(text, left_w - p.pad, s);
        host->draw_text(host, p.x + p.pad, top + p.line / 2 + 2 * s, text, on ? 0xFFFFFF : GOLD_RGB, s);
        snprintf(text, sizeof(text), "%s", h->text);
        fv_fit(text, left_w - p.pad, s);
        host->draw_text(host, p.x + p.pad, top + p.line + p.line / 2 + 2 * s, text, TEXT_RGB, s);
    }
    h = &heroes[run.offered[fv_clamp(cursor, 0, CHOICES - 1)]];
    rx = p.x + p.pad + left_w + p.gap;
    rw = p.x + p.w - p.pad - rx;
    mid = p.top + p.line / 2;
    for (i = 0; i < HERO_CARDS && mid < p.bottom; i++, mid += p.line) {
        if (in_deck && i == deck_sel)
            host->fill(host, rx - p.pad / 2, mid - p.line / 2, rw + p.pad, p.line, FV_CURSOR_RGB, 230);
        card_row(h->cards[i], rx, mid, rw, s, 1);
    }
}

static void draw_pick(void)
{
    Panel p;
    char title[64], corner[48], subtitle[96];
    int s, list_w, i;
    snprintf(title, sizeof(title), "SCELTA %d/%d - %s", run.pick + 1, PICKS, pick_name(run.order[run.pick]));
    tier_counts(run.deck, run.count, corner, sizeof(corner));
    snprintf(subtitle, sizeof(subtitle), "%s - prendi 1 carta su 3", run.hero >= 0 ? heroes[run.hero].name : "");
    panel_begin(&p, title, corner, subtitle,
                "Su/Giu scorri (anche il mazzo)   X prendi   Triangolo scheda   O menu (la corsa resta)");
    s = p.s;
    draw_view_button(&p);
    list_w = draw_card_side(&p, current_card(), p.top);
    for (i = 0; i < CHOICES; i++) {
        int id = run.offer[i], mid = p.top + i * (p.line + 8 * s) + p.line / 2 + 4 * s, have;
        char text[32];
        if (!id) continue;
        if (i == cursor && !in_deck)
            host->fill(host, p.x + p.pad / 2, mid - p.line / 2 - 4 * s, list_w + p.pad, p.line + 8 * s, FV_CURSOR_RGB,
                       230);
        have = copies(run.deck, run.count, id);
        text[0] = 0;
        if (have) snprintf(text, sizeof(text), "ne hai %d", have);
        card_row(id, p.x + p.pad, mid, list_w - host->text_width(host, "ne hai 0", s) - p.gap, s, 1);
        right_text(p.x + p.pad + list_w, mid, text, DIM_RGB, s);
    }
    {
        char help[128], plan[96];
        int mid = p.top + CHOICES * (p.line + 8 * s) + p.line;
        tier_plan_text(run.seed, plan, sizeof(plan));
        snprintf(help, sizeof(help), "Questa arena: %s", plan);
        fv_fit(help, list_w, s);
        host->draw_text(host, p.x + p.pad, mid, help, TEXT_RGB, s);
        mid += p.line;
        snprintf(help, sizeof(help), "Copie massime nel mazzo: L 1, E 2, R e C 3");
        fv_fit(help, list_w, s);
        host->draw_text(host, p.x + p.pad, mid, help, DIM_RGB, s);
        mid += p.line + 4 * s;
        snprintf(help, sizeof(help), "IL TUO MAZZO: %d/%d carte", run.count, DECK_SIZE);
        host->draw_text(host, p.x + p.pad, mid, help, 0xF4F4F4, s);
        draw_deck_list(&p, p.x + p.pad, mid + p.line / 2 + 2 * s, list_w, p.bottom, in_deck);
    }
}

static void opponent_name(int k, char *out, size_t size)
{
    fv_duelist_name(opponents[fv_clamp(k, 0, DUELS_MAX - 1)], out, size);
}

static void draw_ready(void)
{
    static const char *const labels[2] = {"Duella", "Indietro"};
    Panel p;
    char title[96], corner[48], text[96], name[48];
    int s, mid, list_w;
    snprintf(title, sizeof(title), "MAZZO PRONTO - %s", run.hero >= 0 ? heroes[run.hero].name : "");
    tier_counts(run.deck, run.count, corner, sizeof(corner));
    opponent_name(run.wins + run.losses, name, sizeof(name));
    snprintf(text, sizeof(text), "Prossimo avversario: %s (duello %d)", name, run.wins + run.losses + 1);
    panel_begin(&p, title, corner, text,
                "Su/Giu mazzo   Sx/Dx scegli   X conferma   Triangolo scheda   O menu (la corsa resta)");
    s = p.s;
    draw_buttons(&p, labels, 2, button);
    draw_view_button(&p);
    list_w = draw_card_side(&p, current_card(), p.top);
    mid = p.top + p.line / 2;
    draw_record(p.x + p.pad, mid, s);
    draw_deck_list(&p, p.x + p.pad, p.top + p.line + 4 * s, list_w, p.bottom, 1);
}

static void draw_result(void)
{
    static const char *const next_labels[2] = {"Prossimo duello", "Ritirati"};
    static const char *const over_labels[1] = {"Premi"};
    static const char *const confirm_labels[2] = {"Si, ritirati", "No"};
    Panel p;
    char text[128], name[48];
    int s, mid, big, list_w, won = run.last == 1;
    opponent_name(run.wins + run.losses - 1, name, sizeof(name));
    snprintf(text, sizeof(text), "Duello %d contro %s", run.wins + run.losses, name);
    panel_begin(&p, "ARENA - RISULTATO E IL TUO MAZZO", NULL, text,
                confirm ? "Sx/Dx scegli   X conferma   O no" :
                          "Su/Giu mazzo   Sx/Dx scegli   X conferma   Triangolo scheda   O menu (la corsa resta)");
    s = p.s;
    if (confirm) draw_buttons(&p, confirm_labels, 2, button);
    else if (run_over()) draw_buttons(&p, over_labels, 1, 0);
    else draw_buttons(&p, next_labels, 2, button);
    draw_view_button(&p);
    list_w = draw_card_side(&p, current_card(), p.top);
    big = 3 * s;
    mid = p.top + 18 * big / 2 + 6 * s;
    host->draw_text(host, p.x + p.pad, mid, won ? "VITTORIA!" : "SCONFITTA", won ? WIN_RGB : LOSS_RGB, big);
    mid += 18 * big / 2 + p.line;
    draw_record(p.x + p.pad, mid, s);
    mid += p.line + 6 * s;
    if (confirm) snprintf(text, sizeof(text), "Ritirarti ora? Prendi i premi di %d vittorie.", run.wins);
    else if (run.wins >= MAX_WINS) snprintf(text, sizeof(text), "12 vittorie: corsa perfetta! Premi doppi.");
    else if (run.losses >= MAX_LOSSES) snprintf(text, sizeof(text), "Tre sconfitte: la corsa e' finita.");
    else {
        opponent_name(run.wins + run.losses, name, sizeof(name));
        snprintf(text, sizeof(text), "Prossimo avversario: %s", name);
    }
    fv_fit(text, list_w, s);
    host->draw_text(host, p.x + p.pad, mid, text, confirm ? GOLD_RGB : TEXT_RGB, s);
    mid += p.line;
    snprintf(text, sizeof(text), "Premi finora: %ld starchip%s", run_prize(run.wins),
             run.wins >= MAX_WINS ? ", 2 carte" : run.wins >= 7 ? ", 1 carta" : "");
    if (!run_over()) {
        size_t n = strlen(text);
        snprintf(text + n, sizeof(text) - n, " (con %d vittorie: %ld)", run.wins + 1, run_prize(run.wins + 1));
    }
    fv_fit(text, list_w, s);
    host->draw_text(host, p.x + p.pad, mid, text, DIM_RGB, s);
    mid += p.line / 2 + 6 * s;
    draw_deck_list(&p, p.x + p.pad, mid, list_w, p.bottom, 1);
}

static void draw_prize(void)
{
    Panel p;
    char title[64], subtitle[96];
    int list[DECK_SIZE], n = prize_list(list), s, list_w, i, rows;
    snprintf(title, sizeof(title), "PREMIO - SCEGLI UNA CARTA (%d/%d)", run.prize_count + 1, run.prize_total);
    if (run.wins >= MAX_WINS && run.prize_count == 0)
        snprintf(subtitle, sizeof(subtitle), "12 vittorie: una carta a scelta dal tuo mazzo d'arena va nel baule");
    else
        snprintf(subtitle, sizeof(subtitle), "Una tra 3 carte a caso del tuo mazzo va nel baule (%d vittorie)", run.wins);
    panel_begin(&p, title, NULL, subtitle, "Su/Giu scorri   L1/R1 pagina   X prendi   Triangolo scheda");
    s = p.s;
    list_w = draw_card_side(&p, n ? list[fv_clamp(cursor, 0, n - 1)] : 0, p.top);
    rows = (p.bottom - p.top) / p.line;
    if (rows < 1) rows = 1;
    list_rows = rows;
    for (i = 0; i < rows && list_top + i < n; i++) {
        int mid = p.top + i * p.line + p.line / 2;
        if (list_top + i == cursor)
            host->fill(host, p.x + p.pad / 2, mid - p.line / 2, list_w + p.pad, p.line, FV_CURSOR_RGB, 230);
        card_row(list[list_top + i], p.x + p.pad, mid, list_w, s, 1);
    }
}

static void draw_summary(void)
{
    static const char *const labels[1] = {"OK"};
    Panel p;
    char text[160], label[64];
    int s, mid, i, list_w;
    panel_begin(&p, "ARENA - FINE DELLA CORSA", NULL, run.hero >= 0 ? heroes[run.hero].name : "",
                "X o O: chiudi   Su/Giu il tuo mazzo   Triangolo scheda");
    s = p.s;
    draw_buttons(&p, labels, 1, 0);
    draw_view_button(&p);
    list_w = draw_card_side(&p, current_card(), p.top);
    mid = p.top + p.line / 2;
    draw_record(p.x + p.pad, mid, s);
    mid += p.line + 6 * s;
    snprintf(text, sizeof(text), "Starchip: +%d (ora %d)", run.starchips, run.starchips_now);
    host->draw_text(host, p.x + p.pad, mid, text, GOLD_RGB, s);
    mid += p.line;
    if (!run.prize_count) {
        host->draw_text(host, p.x + p.pad, mid, run.wins >= 7 ? "Nessuna carta" : "Carta premio: da 7 vittorie",
                        DIM_RGB, s);
        mid += p.line;
    }
    for (i = 0; i < run.prize_count; i++, mid += p.line) {
        host->draw_text(host, p.x + p.pad, mid, "Nel baule:", TEXT_RGB, s);
        card_row(run.prizes[i], p.x + p.pad + host->text_width(host, "Nel baule: ", s), mid,
                 list_w - host->text_width(host, "Nel baule: ", s), s, 1);
    }
    mid += p.line / 2;
    snprintf(text, sizeof(text), "%s", run.saved == 1 ? "Partita salvata in automatico." : run.saved == 2 ?
             "Salvataggio automatico non riuscito: ricordati di salvare!" : "Ricordati di salvare la partita!");
    fv_fit(text, list_w, s);
    host->draw_text(host, p.x + p.pad, mid, text, run.saved == 1 ? WIN_RGB : LOSS_RGB, s);
    mid += p.line;
    snprintf(label, sizeof(label), "Record migliore: %d vittorie in %d corse", run.best, run.runs);
    fv_fit(label, list_w, s);
    host->draw_text(host, p.x + p.pad, mid, label, DIM_RGB, s);
    mid += p.line / 2 + 6 * s;
    draw_deck_list(&p, p.x + p.pad, mid, list_w, p.bottom, 1);
}

/* The whole deck at once (Square), no scrolling: the cards only (a gold line
 * where a group starts, the groups' counts in the subtitle), in the biggest
 * text whose columns still have room for the names, the fewest columns,
 * then the shortest; the attack (or ATK/DEF) when there is room; the chosen
 * card lit. Each list row's column and row stay in view_col/view_row for
 * Left/Right. */
static void draw_deck_view(void)
{
    Panel p;
    char title[64], corner[48], subtitle[128], text[64], stat[24];
    int rows[DECK_ROWS], counts[DECK_ROWS], n = deck_rows(rows, counts), cards = 0, k, line = 0, most = 2, per, cols = 1,
        col_w = 0, copies_w, stats, i, at = 0, last_group = -1;
    snprintf(title, sizeof(title), "IL TUO MAZZO - %d/%d carte", run.count, DECK_SIZE);
    tier_counts(run.deck, run.count, corner, sizeof(corner));
    snprintf(subtitle, sizeof(subtitle), "%s", run.hero >= 0 ? heroes[run.hero].name : "");
    for (i = 0; i < n; i++) {
        size_t used = strlen(subtitle);
        if (rows[i] >= 0) {
            cards++;
            continue;
        }
        snprintf(subtitle + used, sizeof(subtitle) - used, "%s%s %d", at ? ", " : used ? " - " : "",
                 group_names[-1 - rows[i]], counts[i]);
        at = 1;
    }
    panel_begin(&p, title, corner, subtitle, "Frecce scegli   Triangolo scheda   Quadrato, X o O: chiudi");
    for (k = 3 * p.s; k >= 1; k--) {
        line = 15 * k;
        most = (p.bottom - p.top) / line;
        if (most < 1 && k > 1) continue;
        if (most < 1) most = 1;
        cols = (cards + most - 1) / most;
        if (cols < 1) cols = 1;
        col_w = (p.w - 2 * p.pad - (cols - 1) * 2 * p.gap) / cols;
        if (col_w >= 160 * k || k == 1) break;
    }
    per = (cards + cols - 1) / cols; /* as many columns, as short as they can be */
    if (per < 1) per = 1;
    copies_w = host->text_width(host, " x3", k);
    stats = col_w >= 230 * k ? 2 : col_w >= 160 * k ? 1 : 0;
    for (i = 0, at = 0; i < n; i++) {
        int id = rows[i], x, mid, stat_w = 0;
        view_col[i] = view_row[i] = -1;
        if (id < 0) continue;
        view_col[i] = at / per;
        view_row[i] = at % per;
        at++;
        x = p.x + p.pad + view_col[i] * (col_w + 2 * p.gap);
        mid = p.top + view_row[i] * line + line / 2;
        if (mid + line / 2 > p.bottom) continue;
        if (i == deck_sel) host->fill(host, x - p.pad / 2, mid - line / 2, col_w + p.pad, line, FV_CURSOR_RGB, 230);
        if (last_group >= 0 && group_of(id) != last_group)
            host->fill(host, x, mid - line / 2, col_w, k, 0xC8A040, 255);
        last_group = group_of(id);
        stat[0] = 0;
        if (stats == 2) fv_stats(id, stat, sizeof(stat));
        else if (stats == 1 && fv_is_monster(id)) snprintf(stat, sizeof(stat), "%d", fv_stat(id, 0));
        if (stat[0]) {
            stat_w = host->text_width(host, stat, k) + 6 * k;
            right_text(x + col_w - copies_w, mid, stat, TEXT_RGB, k);
        }
        fv_card_name(id, text, sizeof(text));
        fv_fit(text, col_w - copies_w - stat_w - fv_icon_room(k), k);
        fv_icon_text(id, x, mid, text, fv_tier_rgb(id), k);
        if (counts[i] > 1) {
            snprintf(text, sizeof(text), "x%d", counts[i]);
            right_text(x + col_w, mid, text, GOLD_RGB, k);
        }
    }
}

/* The card sheet (Triangle on a card): the card big, what it is and its text. */
static void draw_sheet(void)
{
    static const char *const attributes[6] = {"Luce", "Oscurita'", "Terra", "Acqua", "Fuoco", "Vento"};
    /* the monster types in Italian, as in the encyclopedia (the disc's order) */
    static const char *const types[20] = {"Drago",   "Incantatore", "Zombie",    "Guerriero", "Guerriero-Bestia",
                                          "Bestia",  "Bestia Alata", "Demone",   "Fata",      "Insetto",
                                          "Dinosauro", "Rettile",   "Pesce",     "Serpente Marino", "Macchina",
                                          "Tuono",   "Acqua",       "Pyro",      "Roccia",    "Pianta"};
    Panel p;
    char name[64], kind[96], text[96];
    int id = sheet, s, zoom, cw = 0, ch = 0, x, w, mid, tier = tier_of(id), have = copies(run.deck, run.count, id);
    const char *tier_name = tier == 'L' ? "Leggendaria" : tier == 'E' ? "Epica" : tier == 'R' ? "Rara" :
                            tier == 'C' ? "Comune" : "";
    fv_card_name(id, name, sizeof(name));
    if (fv_is_monster(id)) {
        int type_id = Cards_Type(id);
        const char *type = type_id >= 0 && type_id < 20 ? types[type_id] : Cards_TypeName(type_id);
        int attribute = Cards_Attribute(id);
        char plain[48];
        fv_ascii(type ? type : "", plain, sizeof(plain));
        snprintf(kind, sizeof(kind), "Mostro %s - Livello %d%s%s", plain, Cards_Level(id),
                 attribute >= 0 && attribute < 6 ? " - " : "", attribute >= 0 && attribute < 6 ? attributes[attribute] : "");
    } else {
        fv_stats(id, kind, sizeof(kind));
    }
    panel_begin(&p, name, tier_name, kind, "Triangolo, X o O: chiudi la scheda");
    s = p.s;
    zoom = fv_card_zoom((p.w - 2 * p.pad) / 2, p.bottom - p.top, s);
    if (zoom) {
        fv_card_size(zoom, s, &cw, &ch);
        fv_card_draw(id, p.x + p.pad, p.top, zoom, s);
    }
    x = p.x + p.pad + (zoom ? cw + p.gap : 0);
    w = p.x + p.w - p.pad - x;
    mid = p.top + p.line / 2;
    if (fv_is_monster(id)) {
        snprintf(text, sizeof(text), "ATK %d   DEF %d", fv_stat(id, 0), fv_stat(id, 1));
        host->draw_text(host, x, mid, text, GOLD_RGB, s);
        mid += p.line;
    }
    snprintf(text, sizeof(text), "Fascia: %s   Copie nel mazzo: %d", tier_name[0] ? tier_name : "-", have);
    fv_fit(text, w, s);
    host->draw_text(host, x, mid, text, DIM_RGB, s);
    mid += p.line + 4 * s;
    if (card_text(id)) draw_wrapped(card_text(id), x, mid, w, p.bottom, p.line, 0xF4F4F4, s);
}

static int screen_up(void)
{
    return net_on || (active && !armed && (D_8009B26C & 0x1F) == ARENA_MODE && (D_8009B26C & 0x40));
}

static void draw(void)
{
    if (hide_results) {
        int w, h, sc;
        host->overlay_size(host, &w, &h, &sc);
        host->fill(host, 0, 0, w, h, 0x000000, 255);
        return;
    }
    if (!screen_up()) return;
    if (sheet) {
        draw_sheet();
        return;
    }
    if (deck_view) {
        draw_deck_view();
        return;
    }
    switch (run.phase) {
    case PHASE_NONE: draw_intro(); break;
    case PHASE_HERO: draw_hero(); break;
    case PHASE_PICK: draw_pick(); break;
    case PHASE_READY: draw_ready(); break;
    case PHASE_RESULT: draw_result(); break;
    case PHASE_PRIZE: draw_prize(); break;
    case PHASE_SUMMARY: draw_summary(); break;
    }
}

static unsigned signature(void)
{
    int left = 0, right = 0, card;
    unsigned hash;
    if (hide_results) return 0xB1ACu;
    if (!screen_up()) return 1;
    TouchPad_FreeSpan(&left, &right);
    card = current_card();
    hash = 2u + rev * 7u;
    hash = hash * 31u + (unsigned)run.phase;
    hash = hash * 131u + (unsigned)cursor;
    hash = hash * 131u + (unsigned)button;
    hash = hash * 131u + (unsigned)entry_refused;
    hash = hash * 131u + ((SaveDataState *)gDuel_awPlayerDeck)->starchips;
    hash = hash * 131u + (unsigned)confirm;
    hash = hash * 131u + (unsigned)list_top;
    hash = hash * 131u + (unsigned)in_deck;
    hash = hash * 131u + (unsigned)deck_sel;
    hash = hash * 131u + (unsigned)deck_top;
    hash = hash * 1031u + (unsigned)sheet;
    hash = hash * 3u + (unsigned)deck_view;
    hash = hash * 131u + (unsigned)run.count;
    hash = hash * 3u + (card && fv_art_find(card) != NULL);
    hash = hash * 131u + (unsigned)left;
    hash = hash * 131u + (unsigned)right;
    hash = hash * 131u + (unsigned)Menu_Height();
    return hash;
}

int MemoriesModInit(const MemoriesModHost *from, MemoriesMod *mod)
{
    if (from->api < 13 || !from->menu_item) return 0;
    host = from;
    mod->api = 13;
    mod->frame = frame;
    mod->reset = reset;
    mod->overlay = draw;
    mod->overlay_signature = signature;
    load_heroes();
    load_prizes();
    load_texts();
    load_state();
    return host->provide(host, "duello", (void *)&armed) &&
           host->subscribe(host, MEMORIES_EVENT_MENU, 0, menu) != 0 &&
           host->subscribe(host, MEMORIES_EVENT_SCENE, 0, scene) != 0 &&
           host->subscribe(host, MEMORIES_EVENT_REWARD, 100, refuse) != 0 &&
           host->subscribe(host, MEMORIES_EVENT_STARCHIP, 100, refuse) != 0 &&
           host->hook(host, (void *)Main_RunUnusedDeveloperMode, (void *)runner, &original_runner) != 0 &&
           host->hook(host, (void *)Duel_ShuffleBothDecks, (void *)shuffle_both, &original_shuffle) != 0 &&
           host->hook(host, (void *)Main_RunDuel, (void *)run_duel, &original_run_duel) != 0 &&
           host->hook(host, (void *)Duel_InitSideStates, (void *)init_sides, &original_init_sides) != 0 &&
           host->hook(host, (void *)DuelScene_UpdateResultRewards, (void *)rewards, &original_rewards) != 0;
}
