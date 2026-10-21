/* Shared by Alessio's mods (draft-drops, drop-tables): a card's name and
 * stats in plain letters, its picture off the disc drawn with the overlay's
 * fill, the duelists' drop pools as the game rolls them, and the panel's
 * layout and pad helpers.
 *
 * A mod's .c includes this once, after `static const MemoriesModHost *host;`.
 * Everything here is static, so each mod has its own copy.
 *
 * The picture is the card's art record, seven sectors of WA_MRG.MRG from
 * sector 722 on, as the card view and the 3D Monsters mod read it (102x96 at
 * 8 bits, then 256 BGR555 colors), with a mod's own artwork patched over it
 * by Cards_PatchArtRecord. The overlay has no images, only rectangles, so the
 * picture is drawn as one rectangle per run of equal pixels in a row.
 *
 * The drop pools are read as View > Free Duel progress reads them
 * (src/pc/cards/free_duel_progress.c): WA_MRG 0xE9B000 + 0x1800 * (id - 1),
 * three sectors an opponent, rows of 722 weights out of 2048 (deck, S/A-POW,
 * B/C/D, S/A-TEC); each row then goes through Tables_PoolFor, the call the
 * game's own roll makes, so a mod that edits drops (drop-missing-cards) is
 * counted. */
#ifndef ALESSIO_FMVIEW_H
#define ALESSIO_FMVIEW_H
#include "types.h"
#include "pc/mods/modapi.h"
#include "pc/cards/cards.h"
#include "pc/cards/art.h"
#include "pc/cards/tables.h"
#include "pc/free_duel/duelists.h"
#include "pc/text/glyphs.h"
#include "game/card_constants.h"
#include "game/duel_card.h"
#include "game/save_data.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The port's menu bar, which the overlay canvas includes, and the span left
 * free between the touch pad's two columns (0 while the pad is hidden). */
int Menu_Height(void);
int TouchPad_FreeSpan(int *left, int *right);

/* host->pad's bits: the PS1 pad's order, Select first. */
#define FV_SELECT 0x0001
#define FV_START 0x0008
#define FV_UP 0x0010
#define FV_RIGHT 0x0020
#define FV_DOWN 0x0040
#define FV_LEFT 0x0080
#define FV_L2 0x0100
#define FV_R2 0x0200
#define FV_L1 0x0400
#define FV_R1 0x0800
#define FV_TRIANGLE 0x1000
#define FV_CIRCLE 0x2000
#define FV_CROSS 0x4000
#define FV_SQUARE 0x8000

#define FV_WA_PATH "\\DATA\\WA_MRG.MRG;1"
static int fv_wa_start = -2;

static int fv_wa(void)
{
    if (fv_wa_start == -2) fv_wa_start = host->disc_file_start(host, FV_WA_PATH);
    return fv_wa_start;
}

static int fv_clamp(int value, int low, int high)
{
    return value < low ? low : value > high ? high : value;
}

/* --- text ------------------------------------------------------------------ */

/* UTF-8 in plain letters: draw_text has ASCII only. */
static void fv_ascii(const char *utf8, char *out, size_t size)
{
    static const char plain[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    const unsigned char *in = (const unsigned char *)utf8;
    size_t n = 0;

    while (*in && n + 1 < size) {
        if (*in < 0x80) {
            out[n++] = (char)*in++;
            continue;
        }
        /* An accented Latin letter (two bytes): its plain letter. */
        if (in[0] == 0xC3 && in[1] >= 0x80 && in[1] < 0xC0) {
            out[n++] = plain[in[1] - 0x80];
            in += 2;
            continue;
        }
        out[n++] = '?';
        in++;
        while ((*in & 0xC0) == 0x80) in++;
    }
    out[n] = 0;
}

/* The game's text (glyph codes, 0xFF at the end) in UTF-8, as
 * Cards_NameUtf8 writes a name; 0 when there is none. Full-width letters,
 * as the retail font writes them, come out plain. */
static int fv_glyphs_utf8(const unsigned char *text, char *out, size_t size)
{
    size_t n = 0;
    while (*text < 0xF6 && n + 4 < size) {
        int code = *text++;
        uint32_t c;
        if (code >= 0xF0) code = (code - 0xF0) << 8 | *text++;
        c = code ? Glyphs_Character(code) : ' ';
        if (c >= 0xFF01 && c <= 0xFF5E) c -= 0xFEE0;
        else if (c == 0x3000) c = ' ';
        if (!c) c = '?';
        if (c < 0x80) {
            out[n++] = (char)c;
        } else if (c < 0x800) {
            out[n++] = (char)(0xC0 | c >> 6);
            out[n++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[n++] = (char)(0xE0 | (c >> 12 & 0x0F));
            out[n++] = (char)(0x80 | (c >> 6 & 0x3F));
            out[n++] = (char)(0x80 | (c & 0x3F));
        }
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = 0;
    return n > 0;
}

/* A card's name as the game shows it, in Game > Language's language. */
static void fv_card_name(int id, char *out, size_t size)
{
    char utf8[96];
    if (!Cards_NameUtf8(id, utf8, sizeof(utf8))) snprintf(utf8, sizeof(utf8), "Carta %d", id);
    fv_ascii(utf8, out, size);
}

static void fv_duelist_name(int duelist, char *out, size_t size)
{
    const char *name = Duelists_Valid(duelist) ? Duelists_Name(duelist) : NULL;
    char fallback[24];
    if (!name) {
        snprintf(fallback, sizeof(fallback), "Duellante %d", duelist);
        name = fallback;
    }
    fv_ascii(name, out, size);
}

/* A card's printed attack (def 0) or defence (def 1): Duel_GetBaseCardStat
 * adds the field's boost, so a Dragon dropped on a Desert duel showed +500. */
static int fv_stat(int id, int def)
{
    int v = (int)((gDuel_adwCardStats[id - 1] >> (def ? CARD_STAT_DEFENSE_SHIFT : 0)) & CARD_STAT_VALUE_MASK) *
            CARD_STAT_SCALE;
    return v > Tables_StatCap(def) ? Tables_StatCap(def) : v;
}

static int fv_is_monster(int id)
{
    int cls = Cards_Class(id);
    return cls == CARD_CLASS_MONSTER || cls == CARD_CLASS_EFFECT_MONSTER;
}

/* "2500/2100" for a monster, else what kind of card it is. */
static void fv_stats(int id, char *out, size_t size)
{
    switch (Cards_Class(id)) {
    case CARD_CLASS_SPELL: snprintf(out, size, "Magia"); break;
    case CARD_CLASS_EQUIP: snprintf(out, size, "Equip"); break;
    case CARD_CLASS_RITUAL_SPELL: snprintf(out, size, "Rituale"); break;
    case CARD_CLASS_TRAP: snprintf(out, size, "Trappola"); break;
    default:
        snprintf(out, size, "%d/%d", (int)fv_stat(id, 0), (int)fv_stat(id, 1));
        break;
    }
}

/* Copies in the deck and the trunk. */
static int fv_owned(int id)
{
    int count = *Cards_ChestSlot(gDuel_awPlayerDeck, id), i;
    for (i = 0; i < DECK_SIZE; i++) count += gDuel_awPlayerDeck[i] == id;
    return count;
}

/* `text` cut to `width` pixels, ".." where it was cut. */
static void fv_fit(char *text, int width, int scale)
{
    size_t n = strlen(text);
    if (host->text_width(host, text, scale) <= width) return;
    while (n > 0) {
        n--;
        text[n] = 0;
        if (n >= 1 && host->text_width(host, text, scale) + host->text_width(host, "..", scale) <= width) break;
    }
    if (n >= 1) strcat(text, "..");
}

/* A weight out of 2048 as a percentage with two decimals ("0.05%"). */
static void fv_percent(unsigned weight, char *out, size_t size)
{
    unsigned hundredths = (weight * 10000u + DUEL_DROP_WEIGHT_TOTAL / 2) / DUEL_DROP_WEIGHT_TOTAL;
    if (weight && !hundredths) hundredths = 1;
    snprintf(out, size, "%u.%02u%%", hundredths / 100, hundredths % 100);
}

/* --- the type icons ------------------------------------------------------------ */

/* The game's own 16x16 type icons, Dragon to Plant then Magic, Trap, Ritual
 * and Equip (the card types' own numbers), as the Build Deck list and the card
 * view draw them: three rows of eight at WA_MRG 0xB50040, 4 bits a pixel, 128
 * bytes a line, 2048 bytes a row of icons; their 16-color palettes one after
 * another at 0xB60200 (docs/asset-catalog.json, monster_type/...). Color 0
 * is see-through. */
#define FV_ICONS 24
#define FV_ICON 16
#define FV_ICON_SECTOR 5792 /* 0xB50000 / 2048 */
#define FV_ICON_CLUT_SECTOR 5824 /* 0xB60000 / 2048 */
#define FV_ICON_CLEAR 0x1000000u

static uint32_t fv_icon_rgb[FV_ICONS][FV_ICON * FV_ICON];
static int fv_icons_state; /* 0 unread, 1 read, -1 unreadable */

/* On the game's frame, as fv_art_load (which calls it). */
static void fv_icons_load(void)
{
    static unsigned char sheet[3 * 2048], clut[2048];
    int icon, row, col;

    if (fv_icons_state || fv_wa() < 0) return;
    fv_icons_state = -1;
    if (host->disc_read(host, fv_wa() + FV_ICON_SECTOR, 3, sheet) != 3) return;
    if (host->disc_read(host, fv_wa() + FV_ICON_CLUT_SECTOR, 1, clut) != 1) return;
    for (icon = 0; icon < FV_ICONS; icon++) {
        const unsigned char *pixels = sheet + 0x40 + icon / 8 * 2048 + icon % 8 * 8;
        const unsigned char *colors = clut + 0x200 + icon * 32;
        for (row = 0; row < FV_ICON; row++) {
            for (col = 0; col < FV_ICON; col++) {
                unsigned index = pixels[row * 128 + col / 2] >> (col & 1) * 4 & 15;
                unsigned color = colors[index * 2] | (unsigned)colors[index * 2 + 1] << 8;
                unsigned r = (color & 31) << 3, g = (color >> 5 & 31) << 3, b = (color >> 10 & 31) << 3;
                fv_icon_rgb[icon][row * FV_ICON + col] =
                    color ? (r | r >> 5) << 16 | (g | g >> 5) << 8 | (b | b >> 5) : FV_ICON_CLEAR;
            }
        }
    }
    fv_icons_state = 1;
}

/* The icon of a card's type, or -1 (a mod's own type has none here). A
 * card that is not a monster gets its class's icon. */
static int fv_type_icon(int id)
{
    int type;
    switch (Cards_Class(id)) {
    case CARD_CLASS_SPELL: return 20;
    case CARD_CLASS_TRAP: return 21;
    case CARD_CLASS_RITUAL_SPELL: return 22;
    case CARD_CLASS_EQUIP: return 23;
    default: break;
    }
    type = Cards_Type(id);
    return type >= 0 && type < 20 ? type : -1;
}

/* Card `id`'s type icon, each pixel `zoom` window pixels square. */
static void fv_icon_draw(int id, int x, int y, int zoom)
{
    int icon = fv_type_icon(id), row, col, run;
    if (icon < 0 || fv_icons_state != 1) return;
    for (row = 0; row < FV_ICON; row++) {
        const uint32_t *line = fv_icon_rgb[icon] + row * FV_ICON;
        for (col = 0; col < FV_ICON; col += run) {
            for (run = 1; col + run < FV_ICON && line[col + run] == line[col]; run++) {}
            if (line[col] != FV_ICON_CLEAR) host->fill(host, x + col * zoom, y + row * zoom, run * zoom, zoom, line[col], 255);
        }
    }
}

/* The room a list row's icon takes before its text. */
static int fv_icon_room(int s)
{
    return (FV_ICON + 4) * s;
}

/* A list row's card: its type icon at x, centred on `mid`, then `text`. */
static void fv_icon_text(int id, int x, int mid, const char *text, uint32_t rgb, int s)
{
    fv_icon_draw(id, x, mid - FV_ICON * s / 2, s);
    host->draw_text(host, x + fv_icon_room(s), mid, text, rgb, s);
}

/* --- the card's picture ------------------------------------------------------ */

#define FV_ART_W CARD_ART_WIDTH
#define FV_ART_H CARD_ART_HEIGHT
#define FV_ART_SECTORS 7
#define FV_ART_CACHE 12

typedef struct {
    int card;
    unsigned used;
    uint32_t rgb[FV_ART_W * FV_ART_H];
} FvArt;

static FvArt fv_arts[FV_ART_CACHE];
static unsigned fv_art_clock;

static FvArt *fv_art_find(int card)
{
    int i;
    for (i = 0; i < FV_ART_CACHE; i++) {
        if (fv_arts[i].card == card && card) return &fv_arts[i];
    }
    return NULL;
}

/* Bring `card`'s picture in; 0 when the disc cannot be read. On the game's
 * frame, not in the overlay, which runs while the picture is presented. */
static int fv_art_load(int card)
{
    static unsigned char record[FV_ART_SECTORS * 2048];
    FvArt *slot = fv_art_find(card);
    const unsigned char *pixels = record + CARD_ART_PIXELS, *clut = record + CARD_ART_CLUT;
    int base, i;

    fv_icons_load();
    if (slot) {
        slot->used = ++fv_art_clock;
        return 1;
    }
    if (!Cards_Valid(card) || fv_wa() < 0) return 0;
    base = Cards_BaseId(card);
    if (base < 1 || base > CARD_COUNT) return 0;
    if (host->disc_read(host, fv_wa() + CARD_COUNT + (base - 1) * FV_ART_SECTORS, FV_ART_SECTORS, record) !=
        FV_ART_SECTORS)
        return 0;
    Cards_PatchArtRecord(card, record);
    for (i = 0; i < FV_ART_CACHE; i++) {
        if (!slot || fv_arts[i].used < slot->used) slot = &fv_arts[i];
    }
    for (i = 0; i < FV_ART_W * FV_ART_H; i++) {
        unsigned color = clut[pixels[i] * 2] | (unsigned)clut[pixels[i] * 2 + 1] << 8;
        unsigned r = (color & 31) << 3, g = (color >> 5 & 31) << 3, b = (color >> 10 & 31) << 3;
        slot->rgb[i] = (r | r >> 5) << 16 | (g | g >> 5) << 8 | (b | b >> 5);
    }
    slot->card = card;
    slot->used = ++fv_art_clock;
    return 1;
}

/* Each pixel `zoom` window pixels square; a dark box while it is not in. */
static void fv_art_draw(int card, int x, int y, int zoom)
{
    const FvArt *art = fv_art_find(card);
    int row, col, run;

    if (!art) {
        host->fill(host, x, y, FV_ART_W * zoom, FV_ART_H * zoom, 0x181818, 255);
        return;
    }
    for (row = 0; row < FV_ART_H; row++) {
        const uint32_t *line = art->rgb + row * FV_ART_W;
        for (col = 0; col < FV_ART_W; col += run) {
            for (run = 1; col + run < FV_ART_W && line[col + run] == line[col]; run++) {}
            host->fill(host, x + col * zoom, y + row * zoom, run * zoom, zoom, line[col], 255);
        }
    }
}

/* --- the card, as a panel: name, picture, stats ------------------------------ */

static uint32_t fv_frame_rgb(int id)
{
    switch (Cards_Class(id)) {
    case CARD_CLASS_EFFECT_MONSTER: return 0xC86A30;
    case CARD_CLASS_SPELL:
    case CARD_CLASS_EQUIP: return 0x2E9A78;
    case CARD_CLASS_RITUAL_SPELL: return 0x4868C0;
    case CARD_CLASS_TRAP: return 0xA8407C;
    default: return 0xB89048;
    }
}

/* The card panel's size at picture zoom `zoom` and text scale `s`. */
static void fv_card_size(int zoom, int s, int *w, int *h)
{
    *w = FV_ART_W * zoom + 8 * s;
    *h = 4 * s + 16 * s + 3 * s + FV_ART_H * zoom + 3 * s + 32 * s + 4 * s;
}

/* The largest zoom whose panel fits w x h; 0 when not even 1 does. */
static int fv_card_zoom(int w, int h, int s)
{
    int zoom = 1, cw, ch;
    fv_card_size(1, s, &cw, &ch);
    if (cw > w || ch > h) return 0;
    for (;;) {
        fv_card_size(zoom + 1, s, &cw, &ch);
        if (cw > w || ch > h) return zoom;
        zoom++;
    }
}

static void fv_card_draw(int id, int x, int y, int zoom, int s)
{
    char name[64], line[64], kind[32];
    int w, h, inner, at, big, room;
    const char *type;

    fv_card_size(zoom, s, &w, &h);
    inner = w - 8 * s;
    host->fill(host, x, y, w, h, fv_frame_rgb(id), 255);
    host->fill(host, x + 2 * s, y + 2 * s, w - 4 * s, h - 4 * s, 0x000000, 70);

    at = y + 4 * s;
    host->fill(host, x + 4 * s, at, inner, 16 * s, 0x10141C, 235);
    fv_card_name(id, name, sizeof(name));
    fv_fit(name, inner - 6 * s, s);
    host->draw_text(host, x + 7 * s, at + 8 * s, name, 0xFFFFFF, s);

    at += 16 * s + 3 * s;
    fv_art_draw(id, x + 4 * s, at, zoom);

    at += FV_ART_H * zoom + 3 * s;
    host->fill(host, x + 4 * s, at, inner, 32 * s, 0x10141C, 235);
    /* The type icon: twice its size at the box's right end when the card is
       large enough, else small in front of the second line. */
    big = zoom >= 2 * s && fv_type_icon(id) >= 0;
    room = big ? 2 * FV_ICON * s + 2 * s : 0;
    if (big) fv_icon_draw(id, x + 4 * s + inner - 2 * FV_ICON * s, at, 2 * s);
    if (fv_is_monster(id)) {
        snprintf(line, sizeof(line), "ATK %d  DEF %d", (int)fv_stat(id, 0),
                 (int)fv_stat(id, 1));
        type = Cards_TypeName(Cards_Type(id));
        snprintf(kind, sizeof(kind), "%s  Lv %d", type ? type : "", Cards_Level(id));
    } else {
        fv_stats(id, line, sizeof(line));
        kind[0] = 0;
    }
    fv_fit(line, inner - 6 * s - room, s);
    host->draw_text(host, x + 7 * s, at + 8 * s, line, 0xF0D070, s);
    {
        char number[48];
        snprintf(number, sizeof(number), "#%03d  %s", id, kind);
        if (big) {
            fv_fit(number, inner - 6 * s - room, s);
            host->draw_text(host, x + 7 * s, at + 24 * s, number, 0xB8C0D8, s);
        } else {
            fv_fit(number, inner - 6 * s - fv_icon_room(s), s);
            fv_icon_text(id, x + 7 * s, at + 24 * s, number, 0xB8C0D8, s);
        }
    }
}

/* --- drop pools --------------------------------------------------------------- */

#define FV_POOLS 3 /* S/A-POW, B/C/D, S/A-TEC: Duel_SelectCardDrop's order */
/* After the drop pools, the opponent's deck pool (the weights its 40 cards are
 * drawn by): not a drop, so FV_POOLS leaves it out; drop-tables shows it. */
#define FV_POOL_DECK 3
#define FV_ALL_POOLS 4
#define FV_DROPS_SECTOR 7478 /* 0xE9B000 / 2048 */
#define FV_DROPS_SECTORS 3
#define FV_DROP_ROW 1460 /* 722 weights and 16 bytes of padding */
#define FV_RETAIL_DUELISTS 39

static const char *const fv_pool_names[FV_ALL_POOLS] = {"S/A-POW", "B/C/D", "S/A-TEC", "MAZZO"};
static unsigned short *fv_weights; /* [duelist][pool][card id], 0 unused; pool FV_POOL_DECK is the deck */
static int fv_duelists, fv_cards, fv_drops_state; /* state: 0 unread, 1 read, -1 unreadable */

/* Every duelist's three drop pools and deck pool, once a session: the disc's files and the
 * mods' tables are fixed once the game has started. */
static int fv_drops_load(void)
{
    static unsigned short retail[FV_RETAIL_DUELISTS][FV_ALL_POOLS][CARD_COUNT];
    static unsigned char block[FV_DROPS_SECTORS * 2048];
    int duelist, pool, id;

    if (fv_drops_state) return fv_drops_state > 0;
    fv_drops_state = -1;
    if (fv_wa() < 0) return 0;
    for (duelist = 1; duelist <= FV_RETAIL_DUELISTS; duelist++) {
        if (host->disc_read(host, fv_wa() + FV_DROPS_SECTOR + (duelist - 1) * FV_DROPS_SECTORS, FV_DROPS_SECTORS,
                            block) != FV_DROPS_SECTORS)
            return 0;
        for (pool = 0; pool < FV_ALL_POOLS; pool++) { /* the disc's rows: deck, then the drops */
            const unsigned char *row = block + (pool + 1) % FV_ALL_POOLS * FV_DROP_ROW;
            for (id = 0; id < CARD_COUNT; id++)
                retail[duelist - 1][pool][id] = (unsigned short)(row[id * 2] | (unsigned)row[id * 2 + 1] << 8);
        }
    }
    fv_duelists = Duelists_Count();
    fv_cards = gCard_nCount;
    fv_weights = calloc((size_t)fv_duelists * FV_ALL_POOLS * (size_t)(fv_cards + 1), sizeof(*fv_weights));
    if (!fv_weights) return 0;
    for (duelist = 1; duelist < fv_duelists; duelist++) {
        int base = Duelists_Valid(duelist) ? Duelists_BaseId(duelist) : 0;
        if (base < 1 || base > FV_RETAIL_DUELISTS) continue;
        for (pool = 0; pool < FV_ALL_POOLS; pool++) { /* TABLES_POOL_DECK is 0, the drops 1-3 */
            const unsigned short *row = retail[base - 1][pool];
            const unsigned short *edited = Tables_PoolFor(duelist, (pool + 1) % FV_ALL_POOLS, row);
            unsigned short *out = fv_weights + ((size_t)duelist * FV_ALL_POOLS + pool) * (size_t)(fv_cards + 1);
            for (id = 1; id <= fv_cards; id++) out[id] = edited ? edited[id] : id <= CARD_COUNT ? row[id - 1] : 0;
        }
    }
    fv_drops_state = 1;
    return 1;
}

/* `card`'s weight (out of 2048) in `duelist`'s pool; 0 when it is not there
 * or the pools are not read. */
static unsigned fv_weight(int duelist, int pool, int card)
{
    if (fv_drops_state <= 0 || duelist < 1 || duelist >= fv_duelists || pool < 0 || pool >= FV_ALL_POOLS || card < 1 ||
        card > fv_cards)
        return 0;
    return fv_weights[((size_t)duelist * FV_ALL_POOLS + pool) * (size_t)(fv_cards + 1) + card];
}

/* The rarity of a weight out of 2048, by fixed thresholds on its chance in
 * that one table (never relative to the cards beside it): 2% and up common
 * (white), 1-2% blue, 0.2-1% purple, under 0.2% gold. 0 (not in the table):
 * the list's own grey. */
#define FV_RARITY_NONE 0xB8C0D8
#define FV_CURSOR_RGB 0x485070 /* the cursor's row: neutral, so every rarity reads on it */
static uint32_t fv_rarity_rgb(unsigned weight)
{
    if (!weight) return FV_RARITY_NONE;
    if (weight >= 41) return 0xF4F4F4; /* 40.96 is 2% */
    if (weight >= 21) return 0x58A8FF;  /* 20.48 is 1% */
    if (weight >= 5) return 0xC47CFF;   /* 4.096 is 0.2% */
    return 0xFFC830;
}

/* The wish list the Enciclopedia mod keeps (one byte a card id, nonzero for
 * a card the player wants), shared with host->provide; NULL while that mod
 * is not loaded. */
#define FV_WISH_MAX 4096
static const unsigned char *fv_wishes;

static int fv_wished(int card)
{
    if (!fv_wishes) fv_wishes = host->find(host, "enciclopedia:wishlist");
    return fv_wishes && card > 0 && card < FV_WISH_MAX && fv_wishes[card];
}

/* --- layout and pad -------------------------------------------------------------- */

/* Where a panel may go: under the menu bar, between the touch pad's columns
 * when it shows, with a margin; and the text scale, made smaller when the
 * space is narrow. */
typedef struct {
    int x, y, w, h, s, width, height;
} FvArea;

static void fv_area(FvArea *area, int min_w)
{
    int width, height, scale, left = 0, right, bar = Menu_Height(), margin;

    host->overlay_size(host, &width, &height, &scale);
    if (!TouchPad_FreeSpan(&left, &right) || right <= left) {
        left = 0;
        right = width;
    }
    area->s = scale < 1 ? 1 : scale;
    while (area->s > 1 && right - left < min_w * area->s) area->s--;
    /* Tests only (MEMORIES_MOD_<ID>_PROVA_SCALA, not in mod.json): a phone's bigger text on a PC window. */
    if (host->setting(host, "prova_scala", 0) > 0) area->s = host->setting(host, "prova_scala", 0);
    margin = 6 * area->s;
    area->x = left + margin;
    area->y = bar + margin;
    area->w = right - left - 2 * margin;
    area->h = height - bar - 2 * margin;
    area->width = width;
    area->height = height;
}

/* A framed box like the draft's. */
static void fv_box(int x, int y, int w, int h, int s)
{
    host->fill(host, x, y, w, h, 0x10183A, 238);
    host->fill(host, x, y, w, 2 * s, 0xC8A040, 255);
    host->fill(host, x, y + h - 2 * s, w, 2 * s, 0xC8A040, 255);
}

/* Pressed this frame, and held long enough to repeat (every 70 ms after
 * 300 ms), from host->pad, which the managed input hooks do not touch. */
typedef struct {
    unsigned short held;
    uint64_t since, last;
} FvPad;

static void fv_pad_read(FvPad *pad, unsigned short *pressed, unsigned short *repeat)
{
    unsigned short now = host->pad(host, 0);
    uint64_t clock = host->now_us(host);

    *pressed = now & ~pad->held;
    *repeat = *pressed;
    if (*pressed) {
        pad->since = pad->last = clock;
    } else if (now && clock - pad->since > 300000 && clock - pad->last > 70000) {
        *repeat = now;
        pad->last = clock;
    }
    pad->held = now;
}

#endif
