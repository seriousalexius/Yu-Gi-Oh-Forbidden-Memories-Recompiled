/* Enciclopedia: in the Library, a button (Triangle unless the player chose
 * another) on the grid or on a card's page opens that card's sheet: the card
 * large, its password and price against the player's starchips, whether it
 * is on the wish list, and pages of lists, Left/Right between them:
 *
 *   FUSIONI     every card it fuses with and what comes out, the partners
 *               the player owns first
 *   SI OTTIENE  the fusions that make it, the ones the player can make first
 *   EQUIP       the monsters an equip fits, or the equips that fit a monster
 *   RITUALE     the rituals it is the card, a tribute or the result of
 *   DROP        who drops it, the likeliest first, in rarity colors
 *
 * X on a card in a list opens its sheet; Square puts the card on the wish
 * list or takes it off; Triangle opens the wish list: every wished card with
 * where to farm it (the duelist and rank that drop it most) and about how
 * many won duels that takes with the draft's choices. R2 (or R2 on the
 * Library itself) opens the search: every card, the ones never seen too,
 * through filters on the right (name typed on a keyboard drawn here, number,
 * ATK, DEF, level, type, attribute, guardian star, owned, wished, password),
 * X on one opens its sheet. Circle goes back,
 * Start closes everything. Only the Library opens it: never a duel, never
 * Build Deck. While it is up the game gets no input (as in drop-tables).
 *
 * The rules are the game's own: the fusion, equip and ritual tables come off
 * the disc (WA_MRG's duel blob, as Duel_LoadPackageStage loads them; outside
 * a duel the game's copies are not there) and go through the mods' rules as
 * CardRules_Fusion and CardRules_Equip take them; the drops as
 * shared/fmview.h reads them. The wish list is the mod's data (desideri.txt,
 * one card number a line), shared with the other mods as "wishlist" so the
 * draft and the drop tables can star a wished card. */
#include "types.h"
#include "pc/mods/modapi.h"

static const MemoriesModHost *host;

#include "../../shared/fmview.h"
#include "pc/cards/passwords.h"
#include "game/main_modes.h"
#include "game/fade.h"
#include "game/file_transfer.h"
#include "game/library_grid_cursor.h"
#include "game/card_constants.h"
#include "game/duel_card.h"
#include "pc/cards/stars.h"

extern unsigned char D_8009B26C; /* main_mode_state.h: the running mode */
extern u8 D_800EA1E8[];          /* library_runtime.h: the Library's state, its mode in the low nibble */

enum { MODE_MASK = 0x1F, MODE_ENTERED = 0x40 };
enum { LIBRARY_GRID = 1, LIBRARY_CARD_PAGE = 2 };

/* --- the disc's rule tables --------------------------------------------------- */

#define BLOB_SECTOR 0x16C6                   /* WA_MRG's duel blob */
#define EQUIP_SECTOR (BLOB_SECTOR + 0x44)    /* +0x22000, 0x2800 bytes */
#define EQUIP_SECTORS 5
#define FUSION_SECTOR (BLOB_SECTOR + 0x49)   /* +0x24800, 0x10000 bytes */
#define FUSION_SECTORS 32
#define RITUAL_SECTOR (FUSION_SECTOR + FUSION_SECTORS)
#define RITUAL_MAX 128
#define RITUAL_FIELDS 5 /* the ritual card, three tributes, the result */

static unsigned char fusion_table[FUSION_SECTORS * 2048];
static unsigned char equip_table[EQUIP_SECTORS * 2048];
static unsigned short rituals[RITUAL_MAX][RITUAL_FIELDS];
static int ritual_count, tables_state; /* 0 unread, 1 read, -1 unreadable */

static unsigned le16(const unsigned char *p)
{
    return p[0] | (unsigned)p[1] << 8;
}

static int tables_load(void)
{
    static unsigned char sector[2048];
    int i;

    if (tables_state) return tables_state > 0;
    tables_state = -1;
    if (fv_wa() < 0) return 0;
    if (host->disc_read(host, fv_wa() + FUSION_SECTOR, FUSION_SECTORS, fusion_table) != FUSION_SECTORS) return 0;
    if (host->disc_read(host, fv_wa() + EQUIP_SECTOR, EQUIP_SECTORS, equip_table) != EQUIP_SECTORS) return 0;
    if (host->disc_read(host, fv_wa() + RITUAL_SECTOR, 1, sector) != 1) return 0;
    for (ritual_count = 0; ritual_count < RITUAL_MAX; ritual_count++) {
        const unsigned char *record = sector + ritual_count * RITUAL_FIELDS * 2;
        if (record + RITUAL_FIELDS * 2 > sector + sizeof(sector) || !le16(record)) break;
        for (i = 0; i < RITUAL_FIELDS; i++) rituals[ritual_count][i] = (unsigned short)le16(record + i * 2);
    }
    tables_state = 1;
    return 1;
}

/* Duel_CheckFusionRetail over the disc's table: each pair is kept once,
 * under the smaller id; groups of five bytes hold two (partner, result)
 * pairs, the ids' high bits packed into the first byte. `each`, when given,
 * is called for every pair of `a`'s instead of looking `b` up. */
static int retail_fusion(int a, int b, void (*each)(int a, int b, int result))
{
    const unsigned char *p, *end = fusion_table + sizeof(fusion_table);
    unsigned off;
    int n;

    if (b < a && !each) {
        int t = a;
        a = b;
        b = t;
    }
    if (a < 1 || a > CARD_COUNT) return 0;
    off = le16(fusion_table + a * 2);
    if (!off || off + 2 > sizeof(fusion_table)) return 0;
    p = fusion_table + off;
    n = p[0];
    if (n) {
        p += 1;
    } else {
        n = 0x1FF - p[1];
        p += 2;
    }
    for (; n > 0 && p + 5 <= end; p += 5, n -= 2) {
        int partner1 = ((p[0] << 8) & 0x300) | p[1], result1 = ((p[0] << 6) & 0x300) | p[2];
        int partner2 = ((p[0] << 4) & 0x300) | p[3], result2 = ((p[0] << 2) & 0x300) | p[4];
        if (each) {
            each(a, partner1, result1);
            if (n > 1) each(a, partner2, result2);
            continue;
        }
        if (partner1 == b) return result1;
        if (partner2 == b) return result2;
    }
    return 0;
}

/* Duel_CheckEquipRetail: (equip, count, monsters...) runs, a 0 at the end. */
static int retail_equip(int equip, int monster)
{
    const unsigned char *p = equip_table, *end = equip_table + sizeof(equip_table);
    while (p + 4 <= end && le16(p)) {
        int key = le16(p), n = le16(p + 2), i;
        p += 4;
        if (key == equip) {
            for (i = 0; i < n && p + 2 * i + 2 <= end; i++) {
                if ((int)le16(p + 2 * i) == monster) return 1;
            }
            return 0;
        }
        p += 2 * n;
    }
    return 0;
}

/* CardRules_Fusion and CardRules_Equip with the disc's tables above (the
 * game's copies are only there in a duel). */
static int rule_fusion(int a, int b)
{
    int result = 0;
    if (!Tables_Fusion(a, b, &result) && !Cards_Fusion(a, b, &result)) {
        if (Cards_Valid(a) && Cards_Valid(b) && !Cards_KindChanged(a) && !Cards_KindChanged(b))
            result = Tables_FilterFusion(retail_fusion(Cards_BaseId(a), Cards_BaseId(b), NULL));
        if (result && Cards_KindChanged(result)) result = 0;
    }
    return Cards_Valid(result) ? result : 0;
}

static int rule_equip(int equip, int monster)
{
    int ruled, as;
    if (!Cards_Valid(equip) || !Cards_Valid(monster)) return 0;
    ruled = Tables_Equip(equip, monster);
    as = Cards_Type(equip) == CARD_TYPE_EQUIP && Cards_RetailType(Cards_EffectId(equip)) == CARD_TYPE_EQUIP
             ? Cards_EffectId(equip) : 0;
    if (ruled < 0)
        ruled = (as || !Cards_KindChanged(equip)) && !Cards_KindChanged(monster) &&
                retail_equip(as ? as : Cards_BaseId(equip), Cards_BaseId(monster));
    return ruled > 0;
}

/* Ritual `i`'s recipe as the duel takes it (a mod's over the disc's); 0
 * when a mod removed it. */
static int ritual_recipe(int i, unsigned short out[RITUAL_FIELDS])
{
    unsigned short own[6];
    int ruled = Tables_Ritual(rituals[i][0], own), k;
    if (!ruled) return 0;
    for (k = 0; k < RITUAL_FIELDS; k++) out[k] = ruled > 0 ? own[k] : rituals[i][k];
    return 1;
}

/* --- the wish list ---------------------------------------------------------------- */

#define WISH_FILE "desideri.txt"
static unsigned char wishes[FV_WISH_MAX];
static unsigned wish_rev; /* changes with every wish, for the overlay's signature */

static void wishes_load(void)
{
    FILE *file = host->open_data(host, WISH_FILE, "r");
    char line[32];
    memset(wishes, 0, sizeof(wishes));
    if (!file) return;
    while (fgets(line, sizeof(line), file)) {
        long card = strtol(line, NULL, 10);
        if (card > 0 && card < FV_WISH_MAX) wishes[card] = 1;
    }
    fclose(file);
}

static void wishes_save(void)
{
    FILE *file = host->open_data(host, WISH_FILE, "wb");
    char line[16];
    int card, n;
    if (!file) return;
    for (card = 1; card < FV_WISH_MAX; card++) {
        if (wishes[card] && (n = snprintf(line, sizeof(line), "%d\n", card)) > 0) fwrite(line, 1, (size_t)n, file);
    }
    fclose(file);
}

static void wish_toggle(int card)
{
    if (card <= 0 || card >= FV_WISH_MAX) return;
    wishes[card] = !wishes[card];
    wish_rev++;
    wishes_save();
}

/* About how many won duels, with the draft offering `choices` cards each
 * time (1: the game's single card), until a card of weight `weight` out of
 * 2048 is among them. */
static int duels_for(unsigned weight)
{
    int choices = fv_clamp(host->setting(host, "draft_choices", 5), 1, 10), i;
    double miss = 1.0, single = 1.0 - (double)weight / DUEL_DROP_WEIGHT_TOTAL;
    if (!weight) return 0;
    for (i = 0; i < choices; i++) miss *= single;
    return (int)(1.0 / (1.0 - miss) + 0.5);
}

/* --- the screens and their rows ------------------------------------------------------ */

enum { PAGE_FUSIONS, PAGE_MADE_BY, PAGE_EQUIP, PAGE_RITUAL, PAGE_DROPS, PAGE_COUNT };
static const char *const page_names[PAGE_COUNT] = {"FUSIONI", "SI OTTIENE", "EQUIP", "RITUALE", "DROP"};

/* SCREEN_FARM: whom to farm the search's cards from; SCREEN_FARM_CARDS: those
 * cards in one duelist's pool (the view's card the duelist, its page the pool). */
enum { SCREEN_SHEET = 1, SCREEN_WISHES, SCREEN_SEARCH, SCREEN_KEYS, SCREEN_FARM, SCREEN_FARM_CARDS };
enum { KIND_RITUAL, KIND_TRIBUTE, KIND_SUMMONS };
static const char *const kind_names[] = {"Rituale", "Tributo", "Evoca"};

typedef struct {
    int screen, card, page, cursor, top;
    int focus, field, col; /* the search: on the list (1) or the filters, the filter; the keyboard's row and column */
} View;

typedef struct {
    short card, other, duelist, pool, kind;
    unsigned short weight;
    int key; /* the page's order: the larger first */
} Row;

#define DEPTH 8
#define ROWS_MAX 4096

static View stack[DEPTH];
static int depth;
static Row rows[ROWS_MAX];
static int row_count;
static int page_counts[PAGE_COUNT];
static View built;       /* what `rows` was made for */
static int visible = 10; /* list rows the last drawing had room for */
static int last_page;
static FvPad pad;
static volatile int swallow;            /* the game gets no buttons */
static volatile unsigned open_key_held; /* the open key, kept from the Library */

static View *top_view(void)
{
    return depth ? &stack[depth - 1] : NULL;
}

static Row *add_row(int card, int other)
{
    Row *row;
    if (row_count >= ROWS_MAX) return NULL;
    row = &rows[row_count++];
    memset(row, 0, sizeof(*row));
    row->card = (short)card;
    row->other = (short)other;
    return row;
}

/* What the player can use first: a card owned, then the stronger. */
static int owned_key(int card)
{
    return (fv_owned(card) > 0) * 10000;
}

static int atk_key(int card)
{
    return fv_is_monster(card) ? (int)fv_stat(card, 0) + 1 : 0;
}

static int before(const Row *a, const Row *b)
{
    if (a->key != b->key) return a->key > b->key;
    if (a->card != b->card) return a->card < b->card;
    if (a->other != b->other) return a->other < b->other;
    if (a->duelist != b->duelist) return a->duelist < b->duelist;
    return a->pool < b->pool;
}

/* By key; the ritual page keeps its recipes as they were found. */
static void sort_rows(int page)
{
    int i, j;
    if (page == PAGE_RITUAL) return;
    for (i = 1; i < row_count; i++) {
        Row row = rows[i];
        for (j = i; j > 0 && before(&row, &rows[j - 1]); j--) rows[j] = rows[j - 1];
        rows[j] = row;
    }
}

static int made_card;
static void made_by(int a, int b, int result)
{
    if (result != made_card && result != Cards_BaseId(made_card)) return;
    Row *row;
    if (rule_fusion(a, b) == made_card && (row = add_row(a, b))) row->key = (owned_key(a) + owned_key(b)) / 10000;
}

static void build_page(int card, int page)
{
    int other, i, k, duelist, pool;
    unsigned short recipe[RITUAL_FIELDS];
    unsigned weight;

    row_count = 0;
    switch (page) {
    case PAGE_FUSIONS:
        for (other = 1; other <= fv_cards; other++) {
            int result = rule_fusion(card, other);
            Row *row;
            if (result && (row = add_row(other, result))) row->key = owned_key(other) + atk_key(result);
        }
        break;
    case PAGE_MADE_BY:
        made_card = card;
        for (other = 1; other <= CARD_COUNT; other++) retail_fusion(other, 0, made_by);
        break;
    case PAGE_EQUIP:
        for (other = 1; other <= fv_cards; other++) {
            Row *row;
            int fits = Cards_Class(card) == CARD_CLASS_EQUIP ? fv_is_monster(other) && rule_equip(card, other)
                       : fv_is_monster(card) ? Cards_Class(other) == CARD_CLASS_EQUIP && rule_equip(other, card)
                                             : 0;
            if (fits && (row = add_row(other, 0))) row->key = owned_key(other) + atk_key(other);
        }
        break;
    case PAGE_RITUAL:
        for (i = 0; i < ritual_count; i++) {
            int found = 0;
            if (!ritual_recipe(i, recipe)) continue;
            for (k = 0; k < RITUAL_FIELDS; k++) found |= recipe[k] == card || recipe[k] == Cards_BaseId(card);
            if (!found) continue;
            for (k = 0; k < RITUAL_FIELDS; k++) {
                Row *row = add_row(recipe[k], 0);
                if (row) row->kind = (short)(k == 0 ? KIND_RITUAL : k == RITUAL_FIELDS - 1 ? KIND_SUMMONS : KIND_TRIBUTE);
            }
        }
        break;
    default:
        for (duelist = 1; duelist < fv_duelists; duelist++) {
            for (pool = 0; pool < FV_POOLS; pool++) {
                Row *row;
                if (!(weight = fv_weight(duelist, pool, card)) || !(row = add_row(card, 0))) continue;
                row->duelist = (short)duelist;
                row->pool = (short)pool;
                row->weight = (unsigned short)weight;
                row->key = (int)weight;
            }
        }
        break;
    }
    sort_rows(page);
}

static void build_wishes(void)
{
    int card, duelist, pool;
    row_count = 0;
    for (card = 1; card <= fv_cards && card < FV_WISH_MAX; card++) {
        Row *row;
        if (!wishes[card] || !(row = add_row(card, 0))) continue;
        for (duelist = 1; duelist < fv_duelists; duelist++) {
            for (pool = 0; pool < FV_POOLS; pool++) {
                unsigned weight = fv_weight(duelist, pool, card);
                if (weight > row->weight) {
                    row->weight = (unsigned short)weight;
                    row->duelist = (short)duelist;
                    row->pool = (short)pool;
                }
            }
        }
        row->key = row->weight;
    }
    sort_rows(PAGE_DROPS);
}

/* --- the search ----------------------------------------------------------------------- */

/* Every card, the ones never seen too, through the filters the player sets
 * on the right of the search screen; the name is typed on a keyboard the
 * mod draws (a phone has no other). */
enum { F_NAME, F_NUMBER, F_ATK, F_DEF, F_LEVEL, F_TYPE, F_ATTR, F_STAR, F_OWN, F_WISH, F_BUY, F_SORT, F_FARM, F_RESET, F_COUNT };
static const char *const field_names[F_COUNT] = {"Nome", "Numero", "ATK",      "DEF",    "Livello", "Tipo",        "Attributo",
                                                 "Stella", "Le hai", "Desideri", "Compra", "Ordine",  "DOVE FARMARLE",
                                                 "AZZERA TUTTO"};

#define TYPE_COUNT (CARD_TYPE_EQUIP + 1)
static const char *const type_names[TYPE_COUNT] = {
    "Drago",    "Incantatore", "Zombie", "Guerriero", "Guerriero-Bestia", "Bestia",  "Bestia Alata", "Demone",
    "Fata",     "Insetto",     "Dinosauro", "Rettile", "Pesce", "Serpente Marino", "Macchina", "Tuono",
    "Acqua",    "Pyro",        "Roccia", "Pianta",    "Magia",            "Trappola", "Rituale",     "Equip"};
#define ATTR_COUNT 6
static const char *const attr_english[ATTR_COUNT] = {"Light", "Dark", "Earth", "Water", "Fire", "Wind"};
static const char *const attr_names[ATTR_COUNT] = {"Luce", "Oscurita", "Terra", "Acqua", "Fuoco", "Vento"};
#define STAR_COUNT 10 /* the disc's, numbered from 1 */
static const char *const star_english[STAR_COUNT] = {"Sun",     "Moon",   "Venus",  "Mercury", "Mars",
                                                     "Jupiter", "Saturn", "Uranus", "Pluto",   "Neptune"};
static const char *const star_names[STAR_COUNT] = {"Sole",  "Luna",    "Venere", "Mercurio", "Marte",
                                                   "Giove", "Saturno", "Urano",  "Plutone",  "Nettuno"};
static const char *const own_names[] = {"tutte", "le hai", "non le hai", "mai viste"};
static const char *const buy_names[] = {"tutte", "con password", "te la puoi permettere"};
static const char *const sort_names[] = {"numero", "ATK", "DEF", "nome"};
static const char *const op_names[] = {"=", ">=", "<="};
enum { SORT_NUMBER, SORT_ATK, SORT_DEF, SORT_NAME };

#define QUERY_MAX 20
#define STAT_TOP 4500 /* where Left from "tutte" starts: the strongest card's attack */

typedef struct {
    char name[QUERY_MAX + 1];
    int number, atk, atk_op, def, def_op, level, type, attr, star, own, wish, buy, sort;
} Search;

static Search search;
static unsigned search_rev; /* changes with every filter, so the rows are rebuilt */

#define NAME_W 100
#define RETAIL_NAME_OFFSETS 0x801D5800u /* u16 a card id, into the US names bank (src/pc/cards/cards.c) */
#define RETAIL_TEXT_BANK 0x801D0000u

/* A retail card's English name, whatever the language: the disc's US text,
 * where Cards_NameCodes finds it before Text_Resolve. */
static void english_name(int card, char *out, size_t size)
{
    const unsigned short *offsets = (const unsigned short *)(uintptr_t)RETAIL_NAME_OFFSETS;
    char utf8[96];
    int base = Cards_BaseId(card);
    out[0] = 0;
    if (base < 1 || base > CARD_COUNT) return;
    if (fv_glyphs_utf8((const unsigned char *)(uintptr_t)(RETAIL_TEXT_BANK + offsets[base]), utf8, sizeof(utf8)))
        fv_ascii(utf8, out, size);
}
/* Each card's name as the game shows it, then '|' and its English one (the
 * Italian names bank cuts the long ones: "Guardia del"), in lower case,
 * NAME_W bytes a card: what the name filter looks in. */
static char *names;
static int names_count;

static void search_clear(int field)
{
    switch (field) {
    case F_NAME: search.name[0] = 0; break;
    case F_NUMBER: search.number = 0; break;
    case F_ATK: search.atk = -1; search.atk_op = 0; break;
    case F_DEF: search.def = -1; search.def_op = 0; break;
    case F_LEVEL: search.level = 0; break;
    case F_TYPE: search.type = -1; break;
    case F_ATTR: search.attr = -1; break;
    case F_STAR: search.star = 0; break;
    case F_OWN: search.own = 0; break;
    case F_WISH: search.wish = 0; break;
    case F_BUY: search.buy = 0; break;
    case F_SORT: search.sort = SORT_NUMBER; break;
    case F_FARM: return;
    default:
        for (field = 0; field < F_RESET; field++) search_clear(field);
        break;
    }
    search_rev++;
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static void names_load(void)
{
    int card, i;
    if (names && names_count == gCard_nCount) return;
    free(names);
    names_count = gCard_nCount;
    names = calloc((size_t)names_count + 1, NAME_W);
    if (!names) return;
    for (card = 1; card <= names_count; card++) {
        char *name = names + (size_t)card * NAME_W, shown[48], english[48];
        fv_card_name(card, shown, sizeof(shown));
        english_name(card, english, sizeof(english));
        snprintf(name, NAME_W, "%s|%s", shown, english);
        for (i = 0; name[i]; i++) name[i] = lower(name[i]);
    }
}

static const char *name_of(int card)
{
    return names && card > 0 && card <= names_count ? names + (size_t)card * NAME_W : "";
}

/* Every word of the query somewhere in the name, in any order. */
static int name_matches(const char *name, const char *query)
{
    char word[QUERY_MAX + 1];
    while (*query) {
        size_t n = 0;
        while (*query == ' ') query++;
        while (*query && *query != ' ' && n < QUERY_MAX) word[n++] = lower(*query++);
        word[n] = 0;
        if (n && !strstr(name, word)) return 0;
    }
    return 1;
}

static int stat_matches(int value, int want, int op)
{
    if (want < 0) return 1;
    return op == 0 ? value == want : op == 1 ? value >= want : value <= want;
}

static int has_star(int card, int star)
{
    unsigned word;
    if (card < 1 || card > CARD_COUNT) return 0;
    word = (unsigned)gDuel_adwCardStats[card - 1];
    return (int)((word >> CARD_STAT_GUARDIAN_STAR_1_SHIFT) & CARD_STAT_GUARDIAN_STAR_MASK) == star ||
           (int)((word >> CARD_STAT_GUARDIAN_STAR_2_SHIFT) & CARD_STAT_GUARDIAN_STAR_MASK) == star;
}

static int search_matches(int card, int attr)
{
    int monster = fv_is_monster(card), have = fv_owned(card);
    if (search.number && card != search.number) return 0;
    if (search.name[0] && !name_matches(name_of(card), search.name)) return 0;
    if ((search.atk >= 0 || search.def >= 0 || search.level || search.attr >= 0) && !monster) return 0;
    if (monster && !stat_matches((int)fv_stat(card, 0), search.atk, search.atk_op)) return 0;
    if (monster && !stat_matches((int)fv_stat(card, 1), search.def, search.def_op)) return 0;
    if (search.level && Cards_Level(card) != search.level) return 0;
    if (search.type >= 0 && Cards_Type(card) != search.type) return 0;
    if (search.attr >= 0 && Cards_Attribute(card) != attr) return 0;
    if (search.star && !has_star(card, search.star)) return 0;
    if (search.own == 1 && !have) return 0;
    if (search.own == 2 && have) return 0;
    if (search.own == 3 && (have || Cards_Seen(card))) return 0;
    if (search.wish && !(card < FV_WISH_MAX && wishes[card])) return 0;
    if (search.buy && Cards_Password(card) == CARD_PASSWORD_NONE) return 0;
    if (search.buy == 2 && Cards_PasswordPrice(card) > gLibrary_dwStarchips) return 0;
    return 1;
}

static int search_before(const Row *a, const Row *b)
{
    if (search.sort == SORT_NAME) {
        int order = strcmp(name_of(a->card), name_of(b->card));
        if (order) return order < 0;
    } else if (a->key != b->key) {
        return a->key > b->key;
    }
    return a->card < b->card;
}

static void build_search(void)
{
    int card, i, j, attr = search.attr >= 0 ? Cards_AttributeNamed(attr_english[search.attr]) : -1;
    Row *row;

    names_load();
    row_count = 0;
    for (card = 1; card <= gCard_nCount; card++) {
        if (!Cards_Valid(card) || !search_matches(card, attr)) continue;
        if (!(row = add_row(card, 0))) break;
        row->key = search.sort == SORT_ATK   ? atk_key(card)
                   : search.sort == SORT_DEF ? (fv_is_monster(card) ? (int)fv_stat(card, 1) + 1 : 0)
                                             : 0;
    }
    if (search.sort == SORT_NUMBER) return;
    for (i = 1; i < row_count; i++) {
        Row moved = rows[i];
        for (j = i; j > 0 && search_before(&moved, &rows[j - 1]); j--) rows[j] = rows[j - 1];
        rows[j] = moved;
    }
}

/* Whom to farm the search's cards from: each duelist's pool with the chance,
 * out of 2048, that a drop is one of them; the likeliest card kept for the
 * picture. All of them, the best first: the player picks whom he can beat. */
static void build_farm(void)
{
    int card, duelist, pool, attr = search.attr >= 0 ? Cards_AttributeNamed(attr_english[search.attr]) : -1;
    Row *row;

    names_load();
    row_count = 0;
    for (duelist = 1; duelist < fv_duelists; duelist++) {
        for (pool = 0; pool < FV_POOLS; pool++) {
            unsigned sum = 0, best = 0;
            int count = 0, best_card = 0;
            for (card = 1; card <= fv_cards; card++) {
                unsigned weight = fv_weight(duelist, pool, card);
                if (!weight || !Cards_Valid(card) || !search_matches(card, attr)) continue;
                sum += weight;
                count++;
                if (weight > best) best = weight, best_card = card;
            }
            if (!sum || !(row = add_row(best_card, count))) continue;
            row->duelist = (short)duelist;
            row->pool = (short)pool;
            row->weight = (unsigned short)(sum > DUEL_DROP_WEIGHT_TOTAL ? DUEL_DROP_WEIGHT_TOTAL : sum);
            row->key = (int)row->weight;
        }
    }
    sort_rows(PAGE_DROPS);
}

/* The search's cards that one duelist drops from one pool, the likeliest first. */
static void build_farm_cards(int duelist, int pool)
{
    int card, attr = search.attr >= 0 ? Cards_AttributeNamed(attr_english[search.attr]) : -1;
    Row *row;

    names_load();
    row_count = 0;
    for (card = 1; card <= fv_cards; card++) {
        unsigned weight = fv_weight(duelist, pool, card);
        if (!weight || !Cards_Valid(card) || !search_matches(card, attr) || !(row = add_row(card, 0))) continue;
        row->duelist = (short)duelist;
        row->pool = (short)pool;
        row->weight = (unsigned short)weight;
        row->key = (int)weight;
    }
    sort_rows(PAGE_DROPS);
}

/* `value` one step on, from `high` round to `low` and back. */
static int cycle(int value, int low, int high, int dir)
{
    value += dir;
    return value < low ? high : value > high ? low : value;
}

static int stat_step(int value, int dir, int big)
{
    if (value < 0) return dir > 0 ? 0 : STAT_TOP;
    value += dir * (big ? 500 : 50);
    return value < 0 ? -1 : value > CARD_STAT_MAX ? CARD_STAT_MAX : value;
}

static void search_change(int field, int dir, int big)
{
    int count = gCard_nCount, n;
    switch (field) {
    case F_NUMBER:
        n = search.number + dir * (big ? 50 : 1);
        search.number = n < 0 ? (search.number ? 0 : count) : n > count ? (search.number == count ? 0 : count) : n;
        break;
    case F_ATK: search.atk = stat_step(search.atk, dir, big); break;
    case F_DEF: search.def = stat_step(search.def, dir, big); break;
    case F_LEVEL: search.level = cycle(search.level, 0, 12, dir); break;
    case F_TYPE: search.type = cycle(search.type, -1, TYPE_COUNT - 1, dir); break;
    case F_ATTR: search.attr = cycle(search.attr, -1, ATTR_COUNT - 1, dir); break;
    case F_STAR: search.star = cycle(search.star, 0, STAR_COUNT, dir); break;
    case F_OWN: search.own = cycle(search.own, 0, 3, dir); break;
    case F_WISH: search.wish = cycle(search.wish, 0, 1, dir); break;
    case F_BUY: search.buy = cycle(search.buy, 0, 2, dir); break;
    case F_SORT: search.sort = cycle(search.sort, 0, 3, dir); break;
    default: return;
    }
    search_rev++;
}

static void star_name(int star, char *out, size_t size)
{
    const char *english = Stars_Name(star);
    int i;
    for (i = 0; i < STAR_COUNT; i++) {
        if (english && !strcmp(english, star_english[i])) {
            snprintf(out, size, "%s", star_names[i]);
            return;
        }
    }
    fv_ascii(english ? english : "?", out, size);
}

/* What a filter is set to, "tutte" when it lets every card through; 1 when
 * it is set. */
static int field_value(int field, char *out, size_t size)
{
    int set;
    snprintf(out, size, "tutte");
    switch (field) {
    case F_NAME:
        set = search.name[0] != 0;
        snprintf(out, size, "%s", set ? search.name : "X per scrivere");
        return set;
    case F_NUMBER:
        if (search.number) snprintf(out, size, "#%03d", search.number);
        return search.number != 0;
    case F_ATK:
    case F_DEF: {
        int value = field == F_ATK ? search.atk : search.def, op = field == F_ATK ? search.atk_op : search.def_op;
        if (value >= 0) snprintf(out, size, "%s %d", op_names[op], value);
        else if (op) snprintf(out, size, "%s ? (Sx/Dx)", op_names[op]);
        return value >= 0;
    }
    case F_LEVEL:
        if (search.level) snprintf(out, size, "%d stelle", search.level);
        return search.level != 0;
    case F_TYPE:
        if (search.type >= 0) snprintf(out, size, "%s", type_names[search.type]);
        return search.type >= 0;
    case F_ATTR:
        if (search.attr >= 0) snprintf(out, size, "%s", attr_names[search.attr]);
        return search.attr >= 0;
    case F_STAR:
        if (search.star) star_name(search.star, out, size);
        return search.star != 0;
    case F_OWN: snprintf(out, size, "%s", own_names[search.own]); return search.own != 0;
    case F_WISH: if (search.wish) snprintf(out, size, "solo desideri"); return search.wish != 0;
    case F_BUY: snprintf(out, size, "%s", buy_names[search.buy]); return search.buy != 0;
    case F_SORT: snprintf(out, size, "per %s", sort_names[search.sort]); return search.sort != SORT_NUMBER;
    case F_FARM: snprintf(out, size, "X contro chi"); return 0;
    default: out[0] = 0; return 0;
    }
}

/* The name's keyboard: four rows of keys, then SPAZIO, CANCELLA, OK. */
#define KEY_COLS 10
#define KEY_ROWS 5
static const char *const key_rows[KEY_ROWS - 1] = {"ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ-.'&", "1234567890"};
static const char *const key_actions[3] = {"SPAZIO", "CANCELLA", "OK"};

static int key_cols(int row)
{
    return row == KEY_ROWS - 1 ? 3 : KEY_COLS;
}

static void query_type(char c)
{
    size_t n = strlen(search.name);
    if (n >= QUERY_MAX || (c == ' ' && (!n || search.name[n - 1] == ' '))) return;
    search.name[n] = c;
    search.name[n + 1] = 0;
    search_rev++;
}

static void query_erase(void)
{
    size_t n = strlen(search.name);
    if (!n) return;
    search.name[n - 1] = 0;
    search_rev++;
}

static unsigned built_rev; /* search_rev when `rows` was made */

static void build_rows(const View *view)
{
    if (view->screen == SCREEN_SHEET) build_page(view->card, view->page);
    else if (view->screen == SCREEN_WISHES) build_wishes();
    else if (view->screen == SCREEN_FARM) build_farm();
    else if (view->screen == SCREEN_FARM_CARDS) build_farm_cards(view->card, view->page);
    else build_search();
    built = *view;
    built_rev = search_rev;
}

static void rows_for(const View *view)
{
    if (view->screen != built.screen || view->card != built.card || view->page != built.page ||
        (view->screen >= SCREEN_SEARCH && built_rev != search_rev))
        build_rows(view);
}

/* How many rows each page of `card` has, for the tabs. */
static void count_pages(int card)
{
    int page;
    for (page = 0; page < PAGE_COUNT; page++) {
        build_page(card, page);
        page_counts[page] = row_count;
    }
    built.screen = 0;
}

/* --- opening and closing ----------------------------------------------------------- */

static void push(int screen, int card)
{
    View *view;
    if (depth == DEPTH) {
        memmove(stack, stack + 1, sizeof(stack[0]) * (DEPTH - 1));
        depth--;
    }
    view = &stack[depth++];
    memset(view, 0, sizeof(*view));
    view->screen = screen;
    view->card = card;
    view->page = last_page;
    if (screen == SCREEN_SHEET) {
        int i;
        count_pages(card);
        /* The page last looked at, else the first with something on it. */
        for (i = 0; i < PAGE_COUNT && !page_counts[view->page]; i++) view->page = (last_page + i + 1) % PAGE_COUNT;
        if (!page_counts[view->page]) view->page = last_page;
    }
    built.screen = 0;
    swallow = 1;
}

static void pop(void)
{
    if (depth) depth--;
    if (depth && top_view()->screen == SCREEN_SHEET) count_pages(top_view()->card);
    built.screen = 0;
}

static void close_all(void)
{
    depth = 0;
    built.screen = 0;
}

/* --- the card under the Library's cursor ---------------------------------------- */

static int library_card(void)
{
    int card;
    if ((D_8009B26C & MODE_MASK) != MAIN_MODE_LIBRARY || !(D_8009B26C & MODE_ENTERED)) return 0;
    if ((D_8009B0F4 & FILE_TRANSFER_REQUEST_BLOCKED_MASK) | D_8009B134) return 0;
    if (gFade_State.flags & FADE_FLAG_ACTIVE) return 0;
    switch (D_800EA1E8[0] & 0xF) {
    case LIBRARY_GRID: card = Library_GetGridCursorCardId(D_800EA1E8); break;
    case LIBRARY_CARD_PAGE: card = *(s16 *)(D_800EA1E8 + 6); break; /* LibrarySelectionState */
    default: return 0;
    }
    return Cards_Valid(card) ? card : 0;
}

/* --- input -------------------------------------------------------------------------- */

static void move(View *view, int delta, int wrap)
{
    if (row_count <= 0) {
        view->cursor = 0;
        return;
    }
    if (wrap && view->cursor + delta < 0) view->cursor = row_count - 1;
    else if (wrap && view->cursor + delta >= row_count) view->cursor = 0;
    else view->cursor = fv_clamp(view->cursor + delta, 0, row_count - 1);
}

/* The card X opens from a row: a fusion's result, else the row's card. */
static int row_target(const View *view, int index)
{
    if (index < 0 || index >= row_count) return 0;
    if (view->screen == SCREEN_SHEET && view->page == PAGE_DROPS) return 0;
    if (view->screen == SCREEN_SHEET && view->page == PAGE_FUSIONS) return rows[index].other;
    return rows[index].card;
}

static void sheet_input(View *view, unsigned short pressed, unsigned short repeat)
{
    if (repeat & FV_UP) move(view, -1, 1);
    if (repeat & FV_DOWN) move(view, 1, 1);
    if (repeat & FV_L1) move(view, -visible, 0);
    if (repeat & FV_R1) move(view, visible, 0);
    if (pressed & (FV_LEFT | FV_RIGHT)) {
        int i;
        for (i = 0; i < PAGE_COUNT; i++) {
            view->page = (view->page + (pressed & FV_LEFT ? PAGE_COUNT - 1 : 1)) % PAGE_COUNT;
            if (page_counts[view->page]) break;
        }
        last_page = view->page;
        view->cursor = view->top = 0;
    }
    if ((pressed & FV_CROSS) && row_target(view, view->cursor)) push(SCREEN_SHEET, row_target(view, view->cursor));
    else if (pressed & FV_SQUARE) wish_toggle(view->card);
    else if (pressed & FV_TRIANGLE) push(SCREEN_WISHES, 0);
    else if (pressed & FV_R2) push(SCREEN_SEARCH, 0);
}

static void wishes_input(View *view, unsigned short pressed, unsigned short repeat)
{
    if (repeat & FV_UP) move(view, -1, 1);
    if (repeat & FV_DOWN) move(view, 1, 1);
    if (repeat & FV_L1) move(view, -visible, 0);
    if (repeat & FV_R1) move(view, visible, 0);
    if ((pressed & FV_CROSS) && row_target(view, view->cursor)) {
        push(SCREEN_SHEET, row_target(view, view->cursor));
    } else if ((pressed & FV_SQUARE) && row_target(view, view->cursor)) {
        wish_toggle(row_target(view, view->cursor));
        built.screen = 0;
    } else if (pressed & FV_R2) {
        push(SCREEN_SEARCH, 0);
    }
}

/* The filters: Up/Down choose one, Left/Right change it (L1/R1 in big
 * steps), Square clears it, X types the name, turns ATK/DEF's = >= <=, or
 * goes down to the list; Triangle too. On the list X opens a card's sheet. */
static void search_input(View *view, unsigned short pressed, unsigned short repeat)
{
    int card = row_target(view, view->cursor);

    if (view->focus) {
        if (repeat & FV_UP) move(view, -1, 1);
        if (repeat & FV_DOWN) move(view, 1, 1);
        if (repeat & FV_L1) move(view, -visible, 0);
        if (repeat & FV_R1) move(view, visible, 0);
        if ((pressed & FV_CROSS) && card) {
            push(SCREEN_SHEET, card);
        } else if ((pressed & FV_SQUARE) && card) {
            wish_toggle(card);
            if (search.wish) search_rev++;
        } else if (pressed & FV_TRIANGLE) {
            view->focus = 0;
        } else if (pressed & FV_R2) {
            push(SCREEN_FARM, 0);
        }
        return;
    }
    if (repeat & FV_UP) view->field = cycle(view->field, 0, F_COUNT - 1, -1);
    if (repeat & FV_DOWN) view->field = cycle(view->field, 0, F_COUNT - 1, 1);
    if (repeat & (FV_LEFT | FV_RIGHT)) search_change(view->field, repeat & FV_LEFT ? -1 : 1, 0);
    if (repeat & (FV_L1 | FV_R1)) search_change(view->field, repeat & FV_L1 ? -1 : 1, 1);
    if (pressed & FV_SQUARE) search_clear(view->field);
    if (pressed & FV_CROSS) {
        if (view->field == F_NAME) {
            push(SCREEN_KEYS, 0);
        } else if (view->field == F_ATK || view->field == F_DEF) {
            int *op = view->field == F_ATK ? &search.atk_op : &search.def_op;
            *op = (*op + 1) % 3;
            search_rev++;
        } else if (view->field == F_RESET) {
            search_clear(F_RESET);
        } else if (view->field == F_FARM) {
            push(SCREEN_FARM, 0);
        } else if (row_count) {
            view->focus = 1;
        }
    } else if ((pressed & FV_TRIANGLE) && row_count) {
        view->focus = 1;
    } else if (pressed & FV_R2) {
        push(SCREEN_FARM, 0);
    }
}

/* Whom to farm from: X shows the cards of that row's pool; on those, X a
 * card's sheet and Square wishes it. */
static void farm_input(View *view, unsigned short pressed, unsigned short repeat)
{
    if (repeat & FV_UP) move(view, -1, 1);
    if (repeat & FV_DOWN) move(view, 1, 1);
    if (repeat & FV_L1) move(view, -visible, 0);
    if (repeat & FV_R1) move(view, visible, 0);
    if (view->cursor >= row_count) return;
    if (view->screen == SCREEN_FARM && (pressed & FV_CROSS)) {
        int duelist = rows[view->cursor].duelist, pool = rows[view->cursor].pool;
        push(SCREEN_FARM_CARDS, duelist);
        top_view()->page = pool;
    } else if (view->screen == SCREEN_FARM_CARDS && (pressed & FV_CROSS)) {
        push(SCREEN_SHEET, rows[view->cursor].card);
    } else if (view->screen == SCREEN_FARM_CARDS && (pressed & FV_SQUARE)) {
        wish_toggle(rows[view->cursor].card);
    }
}

/* The keyboard: the D-pad moves, X types (or SPAZIO, CANCELLA, OK), Square
 * erases, Triangle puts a space; Circle or OK goes back to the filters. */
static void keys_input(View *view, unsigned short pressed, unsigned short repeat)
{
    const int last = KEY_ROWS - 1;
    if (repeat & (FV_UP | FV_DOWN)) {
        int row = cycle(view->field, 0, last, repeat & FV_UP ? -1 : 1);
        if (row == last && view->field != last) view->col = view->col * 3 / KEY_COLS;
        else if (row != last && view->field == last) view->col = view->col * KEY_COLS / 3 + 1;
        view->field = row;
    }
    if (repeat & (FV_LEFT | FV_RIGHT)) view->col = cycle(view->col, 0, key_cols(view->field) - 1, repeat & FV_LEFT ? -1 : 1);
    if (repeat & FV_SQUARE) query_erase();
    if (pressed & FV_TRIANGLE) query_type(' ');
    if (pressed & FV_CROSS) {
        if (view->field < last) query_type(key_rows[view->field][view->col]);
        else if (view->col == 0) query_type(' ');
        else if (view->col == 1) query_erase();
        else pop();
    }
}

/* The card drawn large: the sheet's, or the one under the wish list's cursor. */
static int shown_card(const View *view)
{
    if (view->screen == SCREEN_SHEET) return view->card;
    return view->screen == SCREEN_WISHES || view->screen >= SCREEN_FARM ? row_target(view, view->cursor) : 0;
}

static void frame(void)
{
    unsigned short pressed, repeat;
    unsigned key = (unsigned)host->setting(host, "open_key", FV_TRIANGLE);
    unsigned search_key = (unsigned)host->setting(host, "search_key", FV_R2);
    View *view;
    int card;

    fv_pad_read(&pad, &pressed, &repeat);
    if (!depth) {
        card = library_card();
        open_key_held = card ? key | search_key : 0;
        if (card && key && (pressed & key) && tables_load()) {
            fv_drops_load();
            push(SCREEN_SHEET, card);
        } else if (card && search_key && (pressed & search_key) && tables_load()) {
            fv_drops_load();
            push(SCREEN_SEARCH, 0);
        }
        /* Nothing reaches the game until the button that closed the panel is
           let go. */
        if (!depth && swallow && !pad.held) swallow = 0;
        if (!depth) return;
        pressed = repeat = 0;
    }

    view = top_view();
    rows_for(view);
    if (pressed & FV_START) {
        close_all();
        return;
    }
    if (pressed & FV_CIRCLE) {
        if (view->screen == SCREEN_SEARCH && view->focus) view->focus = 0;
        else pop();
        if (depth) rows_for(top_view());
        return;
    }
    if (view->screen == SCREEN_SHEET) sheet_input(view, pressed, repeat);
    else if (view->screen == SCREEN_WISHES) wishes_input(view, pressed, repeat);
    else if (view->screen == SCREEN_SEARCH) search_input(view, pressed, repeat);
    else if (view->screen == SCREEN_KEYS) keys_input(view, pressed, repeat);
    else farm_input(view, pressed, repeat);
    view = top_view();
    if ((view->screen == SCREEN_SEARCH || view->screen == SCREEN_KEYS) && built_rev != search_rev) {
        /* Other filters: the list from its top. */
        View *list = view->screen == SCREEN_KEYS && depth >= 2 ? &stack[depth - 2] : view;
        list->cursor = list->top = 0;
    }
    rows_for(view);
    if (view->screen == SCREEN_SEARCH && !row_count) view->focus = 0;
    view->cursor = fv_clamp(view->cursor, 0, row_count > 0 ? row_count - 1 : 0);
    if (view->cursor < view->top) view->top = view->cursor;
    if (visible > 0 && view->cursor >= view->top + visible) view->top = view->cursor - visible + 1;
    if (view->top < 0) view->top = 0;
    fv_art_load(shown_card(view));
}

static void input(MemoriesModEvent *event)
{
    if (event->a != 0) return;
    if (swallow) event->b = 0;
    else event->b &= ~(int)open_key_held;
}

/* --- drawing ------------------------------------------------------------------------- */

#define LINE 16
#define GAP 10
#define OWNED_RGB 0x70E070
#define DIM_RGB 0x8890A8
#define TEXT_RGB 0xB8C0D8
#define GOLD_RGB 0xF0D070

typedef struct {
    FvArea area;
    int s, line, pad, gap, x, y, w, h;
} Panel;

static void panel_begin(Panel *p, const char *title, const char *subtitle, uint32_t subtitle_rgb)
{
    char text[128];
    int s;

    fv_area(&p->area, 420);
    p->s = s = p->area.s;
    p->line = LINE * s;
    p->pad = 8 * s;
    p->gap = GAP * s;
    p->w = p->area.w > 960 * s ? 960 * s : p->area.w;
    p->h = p->area.h;
    p->x = p->area.x + (p->area.w - p->w) / 2;
    p->y = p->area.y;
    host->fill(host, 0, 0, p->area.width, p->area.height, 0x000000, 120);
    fv_box(p->x, p->y, p->w, p->h, s);
    snprintf(text, sizeof(text), "%s", title);
    fv_fit(text, p->w - 2 * p->pad, s);
    host->draw_text(host, p->x + p->pad, p->y + p->pad + p->line / 2, text, GOLD_RGB, s);
    snprintf(text, sizeof(text), "%s", subtitle);
    fv_fit(text, p->w - 2 * p->pad, s);
    host->draw_text(host, p->x + p->pad, p->y + p->pad + p->line + p->line / 2, text, subtitle_rgb, s);
}

static void panel_footer(const Panel *p, const char *hint)
{
    char text[128];
    snprintf(text, sizeof(text), "%s", hint);
    fv_fit(text, p->w - 2 * p->pad, p->s);
    host->draw_text(host, p->x + p->pad, p->y + p->h - p->pad - p->line / 2, text, DIM_RGB, p->s);
}

static void list_room(const Panel *p, int *top, int *height)
{
    *top = p->y + p->pad + 2 * p->line + 4 * p->s;
    *height = p->y + p->h - p->pad - p->line - 2 * p->s - *top;
}

static void draw_cursor_row(const Panel *p, int x, int mid, int w)
{
    host->fill(host, x - p->pad / 2, mid - p->line / 2, w + p->pad, p->line, FV_CURSOR_RGB, 230);
}

/* A card's name, starred when it is wished, cut to `width`. */
static void card_label(int card, const char *prefix, char *out, size_t size, int width, int s)
{
    char name[64];
    fv_card_name(card, name, sizeof(name));
    snprintf(out, size, "%s%s%s", prefix, card < FV_WISH_MAX && wishes[card] ? "* " : "", name);
    fv_fit(out, width - fv_icon_room(s), s);
}

static uint32_t own_rgb(int card)
{
    return fv_owned(card) ? OWNED_RGB : TEXT_RGB;
}

static void own_text(int card, char *out, size_t size)
{
    int have = fv_owned(card);
    if (have) snprintf(out, size, "hai %d", have);
    else snprintf(out, size, "-");
}

static void right_text(const Panel *p, int right, int mid, const char *text, uint32_t rgb)
{
    host->draw_text(host, right - host->text_width(host, text, p->s), mid, text, rgb, p->s);
}

/* The password and price, against the starchips the player has. */
static void draw_price(const Panel *p, int card, int x, int y, int w)
{
    char text[96];
    unsigned password = Cards_Password(card), price = Cards_PasswordPrice(card), chips = gLibrary_dwStarchips;

    if (password == CARD_PASSWORD_NONE) {
        snprintf(text, sizeof(text), "Nessuna password: non si compra");
        fv_fit(text, w, p->s);
        host->draw_text(host, x, y + p->line / 2, text, DIM_RGB, p->s);
        snprintf(text, sizeof(text), "Starchip: %u", chips);
        host->draw_text(host, x, y + p->line + p->line / 2, text, TEXT_RGB, p->s);
        return;
    }
    snprintf(text, sizeof(text), "Password %08X   Costo %u starchip", password, price);
    fv_fit(text, w, p->s);
    host->draw_text(host, x, y + p->line / 2, text, GOLD_RGB, p->s);
    if (chips >= price) snprintf(text, sizeof(text), "Hai %u starchip: te la puoi permettere", chips);
    else snprintf(text, sizeof(text), "Hai %u starchip: te ne mancano %u", chips, price - chips);
    fv_fit(text, w, p->s);
    host->draw_text(host, x, y + p->line + p->line / 2, text, chips >= price ? OWNED_RGB : 0xF08060, p->s);
}

/* The pages' names with their counts; the whole bar when it fits, else
 * only the page shown, between arrows. */
static void draw_tabs(const Panel *p, const View *view, int x, int mid, int w)
{
    char label[32];
    int page, total = 0, at = x;

    for (page = 0; page < PAGE_COUNT; page++) {
        snprintf(label, sizeof(label), "%s %d", page_names[page], page_counts[page]);
        total += host->text_width(host, label, p->s) + p->gap;
    }
    if (total > w) {
        snprintf(label, sizeof(label), "< %s %d >", page_names[view->page], page_counts[view->page]);
        host->draw_text(host, x, mid, label, 0xFFFFFF, p->s);
        return;
    }
    for (page = 0; page < PAGE_COUNT; page++) {
        int width;
        snprintf(label, sizeof(label), "%s %d", page_names[page], page_counts[page]);
        width = host->text_width(host, label, p->s);
        if (page == view->page) host->fill(host, at - p->gap / 3, mid - p->line / 2, width + 2 * p->gap / 3, p->line,
                                           FV_CURSOR_RGB, 255);
        host->draw_text(host, at, mid, label,
                        page == view->page ? 0xFFFFFF : page_counts[page] ? TEXT_RGB : 0x505870, p->s);
        at += width + p->gap;
    }
}

static void draw_row(const Panel *p, const View *view, const Row *row, int x, int mid, int w)
{
    char a[80], b[80], stat[24], own[24];
    int w_stat = host->text_width(host, "0000/0000", p->s), w_own = host->text_width(host, "hai 99", p->s);
    int half, right = x + w;

    switch (view->page) {
    case PAGE_FUSIONS:
        half = (w - w_stat - 2 * p->gap) / 2;
        card_label(row->card, "+ ", a, sizeof(a), half, p->s);
        card_label(row->other, "= ", b, sizeof(b), half, p->s);
        fv_stats(row->other, stat, sizeof(stat));
        fv_icon_text(row->card, x, mid, a, own_rgb(row->card), p->s);
        fv_icon_text(row->other, x + half + p->gap, mid, b, 0xF4F4F4, p->s);
        right_text(p, right, mid, stat, GOLD_RGB);
        break;
    case PAGE_MADE_BY: {
        int both = (fv_owned(row->card) > 0) + (fv_owned(row->other) > 0);
        half = (w - w_own - 2 * p->gap) / 2;
        card_label(row->card, "", a, sizeof(a), half, p->s);
        card_label(row->other, "+ ", b, sizeof(b), half, p->s);
        snprintf(own, sizeof(own), "%d/2", both);
        fv_icon_text(row->card, x, mid, a, own_rgb(row->card), p->s);
        fv_icon_text(row->other, x + half + p->gap, mid, b, own_rgb(row->other), p->s);
        right_text(p, right, mid, own, both == 2 ? OWNED_RGB : DIM_RGB);
        break;
    }
    case PAGE_EQUIP:
    case PAGE_RITUAL: {
        int w_kind = view->page == PAGE_RITUAL ? host->text_width(host, "Tributo", p->s) + p->gap : 0;
        int name_w = w - w_kind - w_stat - w_own - 2 * p->gap, same = row->card == view->card;
        if (w_kind) host->draw_text(host, x, mid, kind_names[row->kind], DIM_RGB, p->s);
        card_label(row->card, "", a, sizeof(a), name_w, p->s);
        fv_stats(row->card, stat, sizeof(stat));
        own_text(row->card, own, sizeof(own));
        fv_icon_text(row->card, x + w_kind, mid, a, same ? GOLD_RGB : own_rgb(row->card), p->s);
        host->draw_text(host, x + w_kind + name_w + p->gap, mid, stat, TEXT_RGB, p->s);
        right_text(p, right, mid, own, own_rgb(row->card));
        break;
    }
    default: {
        int w_pool = host->text_width(host, "S/A-POW", p->s), w_chance = host->text_width(host, "00.00%", p->s);
        int w_every = host->text_width(host, "1 ogni 9999", p->s);
        int name_w = w - w_pool - w_chance - w_every - 3 * p->gap, every = duels_for(row->weight);
        fv_duelist_name(row->duelist, a, sizeof(a));
        fv_fit(a, name_w, p->s);
        fv_percent(row->weight, b, sizeof(b));
        snprintf(own, sizeof(own), "1 ogni %d", every);
        host->draw_text(host, x, mid, a, TEXT_RGB, p->s);
        host->draw_text(host, x + name_w + p->gap, mid, fv_pool_names[row->pool], TEXT_RGB, p->s);
        host->draw_text(host, x + name_w + w_pool + 2 * p->gap + w_chance - host->text_width(host, b, p->s), mid, b,
                        fv_rarity_rgb(row->weight), p->s);
        right_text(p, right, mid, own, DIM_RGB);
        break;
    }
    }
}

static void draw_sheet(const View *view)
{
    char title[96], subtitle[96], name[64];
    int i, top, height, zoom, cw = 0, ch, x, w, list_top, have = fv_owned(view->card);
    int wished = view->card < FV_WISH_MAX && wishes[view->card];
    Panel p;

    fv_card_name(view->card, name, sizeof(name));
    snprintf(title, sizeof(title), "ENCICLOPEDIA  #%03d %s", view->card, name);
    snprintf(subtitle, sizeof(subtitle), "Ne hai %d   %s", have,
             wished ? "* Nella lista desideri ([] per toglierla)" : "[] aggiungi alla lista desideri");
    panel_begin(&p, title, subtitle, wished ? GOLD_RGB : TEXT_RGB);
    list_room(&p, &top, &height);
    /* The card large on the left, at most two fifths of the panel. */
    zoom = fv_card_zoom((p.w - 2 * p.pad) * 2 / 5, height, p.s);
    if (zoom) {
        fv_card_size(zoom, p.s, &cw, &ch);
        fv_card_draw(view->card, p.x + p.pad, top, zoom, p.s);
    }
    x = p.x + p.pad + (cw ? cw + p.gap : 0);
    w = p.x + p.w - p.pad - x;
    draw_price(&p, view->card, x, top, w);
    draw_tabs(&p, view, x + p.gap / 3, top + 2 * p.line + 4 * p.s + p.line / 2, w);
    list_top = top + 3 * p.line + 8 * p.s;
    visible = (top + height - list_top) / p.line;
    if (!row_count) {
        static const char *const empty[PAGE_COUNT] = {
            "Non si fonde con nessuna carta.", "Nessuna fusione la crea.", "Nessun equip.",
            "Non fa parte di nessun rituale.", "Nessun duellante la droppa."};
        host->draw_text(host, x, list_top + p.line / 2, empty[view->page], DIM_RGB, p.s);
    }
    for (i = 0; i < visible && view->top + i < row_count; i++) {
        int mid = list_top + i * p.line + p.line / 2;
        if (view->top + i == view->cursor) draw_cursor_row(&p, x, mid, w);
        draw_row(&p, view, &rows[view->top + i], x, mid, w);
    }
    panel_footer(&p, view->page == PAGE_DROPS
                         ? "Sx/Dx pagina  [] desideri  /\\ lista desideri  R2 cerca  O indietro"
                         : view->page == PAGE_FUSIONS
                               ? "Sx/Dx pagina  X risultato  [] desideri  /\\ lista desideri  R2 cerca  O indietro"
                               : "Sx/Dx pagina  X apri  [] desideri  /\\ lista desideri  R2 cerca  O indietro");
}

static void draw_wishes(const View *view)
{
    char subtitle[96], a[80], b[80], every[24];
    int i, top, height, x, w, cw = 0, ch, zoom, w_where, w_chance, w_every, name_w;
    int choices = fv_clamp(host->setting(host, "draft_choices", 5), 1, 10);
    Panel p;

    if (choices > 1) snprintf(subtitle, sizeof(subtitle), "%d carte - dove farmarle: duelli vinti stimati (draft da %d)",
                              row_count, choices);
    else snprintf(subtitle, sizeof(subtitle), "%d carte - dove farmarle: duelli vinti stimati", row_count);
    panel_begin(&p, "LISTA DESIDERI", subtitle, TEXT_RGB);
    list_room(&p, &top, &height);
    zoom = fv_card_zoom((p.w - 2 * p.pad) / 3, height, p.s);
    if (zoom && shown_card(view)) {
        fv_card_size(zoom, p.s, &cw, &ch);
        fv_card_draw(shown_card(view), p.x + p.w - p.pad - cw, top, zoom, p.s);
    } else {
        cw = 0;
    }
    x = p.x + p.pad;
    w = p.w - 2 * p.pad - (cw ? cw + p.gap : 0);
    w_chance = host->text_width(host, "00.00%", p.s);
    w_every = host->text_width(host, "1 ogni 9999", p.s);
    w_where = (w - w_chance - w_every - 3 * p.gap) * 9 / 20;
    name_w = w - w_where - w_chance - w_every - 3 * p.gap;
    visible = height / p.line;
    if (!row_count) {
        host->draw_text(host, x, top + p.line / 2, "Vuota: nella scheda di una carta premi [] per aggiungerla.",
                        DIM_RGB, p.s);
    }
    for (i = 0; i < visible && view->top + i < row_count; i++) {
        const Row *row = &rows[view->top + i];
        int mid = top + i * p.line + p.line / 2;
        if (view->top + i == view->cursor) draw_cursor_row(&p, x, mid, w);
        card_label(row->card, "", a, sizeof(a), name_w, p.s);
        fv_icon_text(row->card, x, mid, a, fv_rarity_rgb(row->weight), p.s);
        if (row->weight) {
            char duelist[48];
            fv_duelist_name(row->duelist, duelist, sizeof(duelist));
            snprintf(b, sizeof(b), "%s %s", duelist, fv_pool_names[row->pool]);
            fv_fit(b, w_where, p.s);
            host->draw_text(host, x + name_w + p.gap, mid, b, TEXT_RGB, p.s);
            fv_percent(row->weight, b, sizeof(b));
            host->draw_text(host, x + name_w + w_where + 2 * p.gap + w_chance - host->text_width(host, b, p.s), mid, b,
                            fv_rarity_rgb(row->weight), p.s);
            snprintf(every, sizeof(every), "1 ogni %d", duels_for(row->weight));
            right_text(&p, x + w, mid, every, fv_owned(row->card) ? OWNED_RGB : DIM_RGB);
        } else {
            host->draw_text(host, x + name_w + p.gap, mid, "nessuno la droppa", DIM_RGB, p.s);
        }
    }
    panel_footer(&p, "Su/Giu scorri  X scheda  [] togli  R2 cerca  O indietro  Start chiudi");
}

/* The keyboard over the search, above its footer. */
static void draw_keys(const Panel *p, const View *keys)
{
    char text[64], cap[2] = {0, 0};
    int kw = 2 * p->line, kh = p->line + p->line / 2, bw, bh = (KEY_ROWS + 1) * kh + 2 * p->pad, bx, by, row, col;

    if (KEY_COLS * kw + 2 * p->pad > p->w - 2 * p->pad) kw = (p->w - 4 * p->pad) / KEY_COLS;
    bw = KEY_COLS * kw + 2 * p->pad;
    bx = p->x + (p->w - bw) / 2;
    by = p->y + p->h - p->pad - p->line - 2 * p->s - bh;
    if (by < p->y + 2 * p->line) by = p->y + 2 * p->line;
    fv_box(bx, by, bw, bh, p->s);
    snprintf(text, sizeof(text), "Nome: %s_", search.name);
    host->draw_text(host, bx + p->pad, by + p->pad + kh / 2, text, 0xFFFFFF, p->s);
    for (row = 0; row < KEY_ROWS; row++) {
        int cols = key_cols(row), cw = row == KEY_ROWS - 1 ? KEY_COLS * kw / 3 : kw;
        for (col = 0; col < cols; col++) {
            int cx = bx + p->pad + col * cw, cy = by + p->pad + (row + 1) * kh;
            int chosen = keys->field == row && keys->col == col;
            const char *label = cap;
            if (row < KEY_ROWS - 1) cap[0] = key_rows[row][col];
            else label = key_actions[col];
            host->fill(host, cx + p->s, cy + p->s, cw - 2 * p->s, kh - 2 * p->s, chosen ? 0xC8A040 : 0x283058, 255);
            host->draw_text(host, cx + (cw - host->text_width(host, label, p->s)) / 2, cy + kh / 2, label,
                            chosen ? 0x10183A : 0xFFFFFF, p->s);
        }
    }
    panel_footer(p, "X scrivi  [] cancella  /\\ spazio  O fatto  Start chiudi");
}

/* The cards found on the left, the filters on the right. */
static void draw_search(const View *view, const View *keys)
{
    char subtitle[96], a[80], value[64], stat[24], own[24], prefix[8];
    int i, top, height, x, w, fx, fw, lw, list_w, name_w, w_stat, w_own, ftop, fvisible;
    Panel p;

    snprintf(subtitle, sizeof(subtitle), "%d carte trovate su %d, anche quelle mai viste", row_count, gCard_nCount);
    panel_begin(&p, "ENCICLOPEDIA - CERCA", subtitle, TEXT_RGB);
    list_room(&p, &top, &height);
    x = p.x + p.pad;
    w = p.w - 2 * p.pad;
    lw = host->text_width(host, "Attributo", p.s) + p.gap;
    fw = lw + host->text_width(host, "Guerriero-Bestia", p.s);
    if (fw > w / 2) fw = w / 2;
    fx = x + w - fw;
    list_w = w - fw - 2 * p.gap;
    host->fill(host, fx - p.gap, top, p.s, height, 0x384060, 255);

    /* The filters, scrolled to keep the one chosen in sight. */
    fvisible = height / p.line;
    ftop = view->field >= fvisible ? view->field - fvisible + 1 : 0;
    for (i = 0; i < fvisible && ftop + i < F_COUNT; i++) {
        int field = ftop + i, mid = top + i * p.line + p.line / 2, set = field_value(field, value, sizeof(value));
        int chosen = !view->focus && !keys && view->field == field;
        if (chosen) draw_cursor_row(&p, fx, mid, fw);
        if (field == F_RESET || field == F_FARM) {
            host->draw_text(host, fx, mid, field_names[field], GOLD_RGB, p.s);
            continue;
        }
        host->draw_text(host, fx, mid, field_names[field], chosen ? 0xFFFFFF : DIM_RGB, p.s);
        fv_fit(value, fw - lw, p.s);
        host->draw_text(host, fx + lw, mid, value, set ? GOLD_RGB : chosen ? TEXT_RGB : DIM_RGB, p.s);
    }

    /* The cards: number, name in the owned color (starred when wished), stats. */
    w_stat = host->text_width(host, "0000/0000", p.s);
    w_own = host->text_width(host, "hai 99", p.s);
    if (list_w - w_stat - w_own - 2 * p.gap < 120 * p.s) w_own = 0;
    name_w = list_w - w_stat - (w_own ? w_own + p.gap : 0) - p.gap;
    visible = height / p.line;
    if (!row_count) host->draw_text(host, x, top + p.line / 2, "Nessuna carta con questi filtri.", DIM_RGB, p.s);
    for (i = 0; i < visible && view->top + i < row_count; i++) {
        const Row *row = &rows[view->top + i];
        int mid = top + i * p.line + p.line / 2;
        if (view->focus && view->top + i == view->cursor) draw_cursor_row(&p, x, mid, list_w);
        snprintf(prefix, sizeof(prefix), "%03d ", row->card);
        card_label(row->card, prefix, a, sizeof(a), name_w, p.s);
        fv_stats(row->card, stat, sizeof(stat));
        fv_icon_text(row->card, x, mid, a, own_rgb(row->card), p.s);
        host->draw_text(host, x + name_w + p.gap, mid, stat, TEXT_RGB, p.s);
        if (w_own) {
            own_text(row->card, own, sizeof(own));
            right_text(&p, x + list_w, mid, own, own_rgb(row->card));
        }
    }

    if (keys) {
        draw_keys(&p, keys);
    } else if (view->focus) {
        panel_footer(&p, "Su/Giu scorri  X scheda  [] desideri  R2 dove farmarle  /\\ o O filtri");
    } else {
        char hint[128];
        const char *cross = view->field == F_NAME                          ? "scrivi"
                            : view->field == F_ATK || view->field == F_DEF ? "= >= <="
                            : view->field == F_RESET                       ? "azzera"
                            : view->field == F_FARM                        ? "apri"
                                                                           : "risultati";
        snprintf(hint, sizeof(hint),
                 "Su/Giu filtro  Sx/Dx cambia (L1/R1 veloce)  X %s  [] azzera  /\\ risultati  R2 dove farmarle  O esci",
                 cross);
        panel_footer(&p, hint);
    }
}

/* Whom to farm the search's cards from, or the cards of one duelist's pool:
 * the list on the left, the likeliest card large on the right. */
static void draw_farm(const View *view)
{
    char title[96], subtitle[128], a[80], b[80], every[24], draft[32];
    int i, top, height, x, w, cw = 0, ch, zoom, w_pool, w_chance, w_every, w_count, name_w, card = shown_card(view);
    int choices = fv_clamp(host->setting(host, "draft_choices", 5), 1, 10), cards = view->screen == SCREEN_FARM_CARDS;
    Panel p;

    if (choices > 1) snprintf(draft, sizeof(draft), " (draft da %d)", choices);
    else draft[0] = 0;
    if (cards) {
        char duelist[48];
        fv_duelist_name(view->card, duelist, sizeof(duelist));
        snprintf(title, sizeof(title), "DOVE FARMARLE - %s %s", duelist, fv_pool_names[view->page]);
        snprintf(subtitle, sizeof(subtitle), "%d carte della ricerca: duelli vinti stimati%s", row_count, draft);
    } else {
        snprintf(title, sizeof(title), "DOVE FARMARLE - le carte della ricerca");
        snprintf(subtitle, sizeof(subtitle), "Probabilita che il drop sia una di loro, dal migliore%s", draft);
    }
    panel_begin(&p, title, subtitle, TEXT_RGB);
    list_room(&p, &top, &height);
    zoom = card ? fv_card_zoom((p.w - 2 * p.pad) / 3, height, p.s) : 0;
    if (zoom) fv_card_size(zoom, p.s, &cw, &ch);
    x = p.x + p.pad;
    w_pool = host->text_width(host, "S/A-POW", p.s);
    w_chance = host->text_width(host, "100.00%", p.s);
    w_every = host->text_width(host, "1 ogni 9999", p.s);
    w_count = cards ? 0 : host->text_width(host, "999 carte", p.s) + p.gap;
    /* The name comes first: on a narrow screen (a phone) the card, then the
     * count, give way to it rather than the name shrinking to nothing. */
    for (;;) {
        w = p.w - 2 * p.pad - (cw ? cw + p.gap : 0);
        name_w = w - (cards ? 0 : w_pool + p.gap) - w_count - w_chance - w_every - 2 * p.gap;
        if (name_w >= host->text_width(host, cards ? "Drago Bianco Occh" : "Sacerdote Mag", p.s)) break;
        if (cw) cw = 0;
        else if (w_count) w_count = 0;
        else break;
    }
    if (cw) fv_card_draw(card, p.x + p.w - p.pad - cw, top, zoom, p.s);
    visible = height / p.line;
    if (!row_count) {
        host->draw_text(host, x, top + p.line / 2, "Nessun duellante le droppa: cambia i filtri della ricerca.", DIM_RGB,
                        p.s);
    }
    for (i = 0; i < visible && view->top + i < row_count; i++) {
        const Row *row = &rows[view->top + i];
        int mid = top + i * p.line + p.line / 2, at = x + name_w + p.gap;
        if (view->top + i == view->cursor) draw_cursor_row(&p, x, mid, w);
        if (cards) {
            card_label(row->card, "", a, sizeof(a), name_w, p.s);
            fv_icon_text(row->card, x, mid, a, own_rgb(row->card), p.s);
        } else {
            fv_duelist_name(row->duelist, a, sizeof(a));
            fv_fit(a, name_w, p.s);
            host->draw_text(host, x, mid, a, TEXT_RGB, p.s);
            host->draw_text(host, at, mid, fv_pool_names[row->pool], TEXT_RGB, p.s);
            at += w_pool + p.gap;
            if (w_count) {
                if (row->other == 1) snprintf(b, sizeof(b), "1 carta");
                else snprintf(b, sizeof(b), "%d carte", row->other);
                host->draw_text(host, at + w_count - p.gap - host->text_width(host, b, p.s), mid, b, DIM_RGB, p.s);
                at += w_count;
            }
        }
        fv_percent(row->weight, b, sizeof(b));
        host->draw_text(host, at + w_chance - host->text_width(host, b, p.s), mid, b, fv_rarity_rgb(row->weight), p.s);
        snprintf(every, sizeof(every), "1 ogni %d", duels_for(row->weight));
        right_text(&p, x + w, mid, every, GOLD_RGB);
    }
    panel_footer(&p, cards ? "Su/Giu scorri  X scheda  [] desideri  O indietro  Start chiudi"
                           : "Su/Giu scorri  X le carte di questo duellante  O indietro  Start chiudi");
}

static void draw(void)
{
    const View *view = top_view();
    if (!view || built.screen != view->screen) return;
    if (view->screen == SCREEN_SHEET) draw_sheet(view);
    else if (view->screen == SCREEN_WISHES) draw_wishes(view);
    else if (view->screen == SCREEN_SEARCH) draw_search(view, NULL);
    else if (view->screen >= SCREEN_FARM) draw_farm(view);
    else if (depth >= 2) draw_search(&stack[depth - 2], view);
}

static unsigned signature(void)
{
    const View *view = top_view();
    int left = 0, right = 0, card;
    unsigned hash;

    if (!view) return 1;
    card = shown_card(view);
    TouchPad_FreeSpan(&left, &right);
    hash = 2u + (unsigned)depth;
    hash = hash * 31u + (unsigned)view->screen;
    hash = hash * 1031u + (unsigned)view->card;
    hash = hash * 7u + (unsigned)view->page;
    hash = hash * 1031u + (unsigned)view->cursor;
    hash = hash * 1031u + (unsigned)view->top;
    hash = hash * 1031u + (unsigned)row_count;
    hash = hash * 3u + (unsigned)(built.screen == view->screen); /* drawn only once the rows are made */
    hash = hash * 31u + (unsigned)view->focus;
    hash = hash * 31u + (unsigned)view->field;
    hash = hash * 31u + (unsigned)view->col;
    hash = hash * 131u + search_rev;
    hash = hash * 131u + wish_rev;
    hash = hash * 3u + (unsigned)(view->card < FV_WISH_MAX && wishes[view->card]);
    hash = hash * 3u + (fv_art_find(card) != NULL);
    hash = hash * 131u + (unsigned)gLibrary_dwStarchips;
    hash = hash * 131u + (unsigned)left;
    hash = hash * 131u + (unsigned)right;
    hash = hash * 131u + (unsigned)Menu_Height();
    return hash;
}

static void applied(int on)
{
    (void)on;
    close_all();
    swallow = 0;
    open_key_held = 0;
}

static void reset(void)
{
    close_all();
}

int MemoriesModInit(const MemoriesModHost *from, MemoriesMod *mod)
{
    if (from->api < 13) return 0;
    host = from;
    mod->api = 13;
    mod->frame = frame;
    mod->applied = applied;
    mod->reset = reset;
    mod->overlay = draw;
    mod->overlay_signature = signature;
    wishes_load();
    fv_wishes = wishes;
    search_clear(F_RESET);
    return host->provide(host, "wishlist", wishes) && host->subscribe(host, MEMORIES_EVENT_INPUT, 0, input) != 0;
}
