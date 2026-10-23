/* fork: the bet before an online duel (bet.h). */
#include "bet.h"
#include "button_layout.h"
#include "menu.h"
#include "paths.h"
#include "pc/cards/art.h"
#include "pc/cards/cards.h"
#include "pc/cards/disc_art.h"
#include "pc/cards/deck_tiers.h"
#include "pc/cards/tables.h"
#include "pc/mods/json.h"
#include "pc/saves/save_menu.h"
#include "pc/saves/save_slots.h"
#include "pc/text/glyphs.h"
#include "pc/text/text.h"
#include "game/card_constants.h"
#include "game/duel_card.h"
#include "game/save_data.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RETAIL_CARDS 722
#define DECK_CARDS 40
#define CHEST_OFFSET 0x50
#define STARCHIPS_OFFSET 0x5E0
#define CHEST_MAX 250
#define STARCHIPS_MAX 999999
#define DECK_COPIES 3
#define NAME 13
#define PAGE 18 /* cards a page of the collection shows, three to a row */

typedef struct {
    int card, deck, starchips, ready; /* deck: the card is the deck's (none of it in the trunk) */
    char choosing[12];                /* "carta", "starchip" or "" */
} Stake;

typedef struct {
    unsigned short id;
    unsigned char deck;
} Owned;

enum { VIEW_MAIN, VIEW_CARDS, VIEW_CHIPS };

static struct {
    int active, side, slot, test, view, page, focus, owned_count, chest_total;
    int asking; /* "Vuoi annullare la sfida?" is up */
    unsigned starchips; /* the save's */
    char room[40], mode[8], rival[NAME];
    Stake mine, theirs;
    void (*send)(const char *line);
    void (*back)(void);
} bet;
static Owned owned[RETAIL_CARDS];
static unsigned char state[SAVE_SLOT_STATE_SIZE];
static char settled[600]; /* the settled bet's notice, until shown */

static int check(unsigned char *at) { return SaveData_ValidateIntegrity(at) != 0; }

static int bet_path(char *path, size_t size) { return Paths_User(path, size, "scommessa.txt"); }

/* --- the cards ------------------------------------------------------- */

static int monster(int id)
{
    int cls = Cards_Class(id);
    return cls == CARD_CLASS_MONSTER || cls == CARD_CLASS_EFFECT_MONSTER;
}

static int stat(int id, int defense)
{
    int value = (int)((gDuel_adwCardStats[id - 1] >> (defense ? CARD_STAT_DEFENSE_SHIFT : 0)) & CARD_STAT_VALUE_MASK) *
                CARD_STAT_SCALE;
    return value > Tables_StatCap(defense) ? Tables_StatCap(defense) : value;
}

static char tier(int id) { return id >= 1 && id < (int)sizeof(deck_tiers) - 1 ? deck_tiers[id] : 'C'; }

static const char *tier_name(int id)
{
    switch (tier(id)) {
    case 'L': return "Leggendaria";
    case 'E': return "Epica";
    case 'R': return "Rara";
    default: return "Comune";
    }
}

/* The strongest first: monsters by attack, then defense; the others by tier. */
static int power(int id)
{
    static const char order[] = "CRELV";
    const char *at = strchr(order, tier(id));
    if (monster(id)) return 100000 + stat(id, 0) * 10 + stat(id, 1) / 1000;
    return (at ? (int)(at - order) : 0) * 1000 + id % 1000;
}

static int stronger(const void *a, const void *b)
{
    const Owned *x = a, *y = b;
    int p = power(x->id), q = power(y->id);
    return p != q ? q - p : (int)x->id - (int)y->id;
}

static void card_name(int id, char *out, size_t size)
{
    if (!id || Cards_NameUtf8(id, out, size) <= 0) snprintf(out, size, id ? "carta %d" : "niente", id);
}

/* "Drago Bianco Occhi Blu (3000/2500, Leggendaria)" */
static void card_full(int id, char *out, size_t size)
{
    char name[64];
    card_name(id, name, sizeof(name));
    if (!id) snprintf(out, size, "nessuna carta");
    else if (monster(id)) snprintf(out, size, "%s (%d/%d, %s)", name, stat(id, 0), stat(id, 1), tier_name(id));
    else snprintf(out, size, "%s (%s)", name, tier_name(id));
}

/* The save the duel loads (lobby.c's go): its cards, one row each. */
static void read_collection(void)
{
    int id, i, count[RETAIL_CARDS + 1] = {0};
    bet.owned_count = bet.chest_total = 0;
    bet.starchips = 0;
    if (SaveSlots_ReadState(bet.slot, state, check)) {
        fprintf(stderr, "memories-pc: bet: save slot %d unreadable: nothing to put up\n", bet.slot + 1);
        return;
    }
    for (i = 0; i < DECK_CARDS; i++) {
        id = state[2 * i] | state[2 * i + 1] << 8;
        if (id >= 1 && id <= RETAIL_CARDS) count[id]++;
    }
    for (id = 1; id <= RETAIL_CARDS; id++) {
        int chest = state[CHEST_OFFSET + id - 1];
        bet.chest_total += chest;
        if (!chest && !count[id]) continue;
        owned[bet.owned_count].id = (unsigned short)id;
        owned[bet.owned_count].deck = !chest;
        bet.owned_count++;
    }
    qsort(owned, (size_t)bet.owned_count, sizeof(owned[0]), stronger);
    memcpy(&bet.starchips, state + STARCHIPS_OFFSET, sizeof(bet.starchips));
    if (bet.starchips > STARCHIPS_MAX) bet.starchips = STARCHIPS_MAX;
}

/* --- to the server --------------------------------------------------- */

static void send_stake(void)
{
    char line[200];
    const char *choosing = bet.view == VIEW_CARDS ? "carta" : bet.view == VIEW_CHIPS ? "starchip" : "";
    snprintf(line, sizeof(line),
             "{\"t\":\"puntata\",\"carta\":%d,\"starchip\":%d,\"pronto\":%s,\"vedo\":[%d,%d],\"sceglie\":\"%s\"}",
             bet.mine.card, bet.mine.starchips, bet.mine.ready ? "true" : "false", bet.theirs.card,
             bet.theirs.starchips, choosing);
    if (bet.send) bet.send(line);
}

/* --- the screens ----------------------------------------------------- */

static void show(void);

static void close_bet(void)
{
    bet.active = 0;
    Menu_CloseNotice();
}

static void back_closed(int button, int *quit)
{
    (void)button, (void)quit;
    if (bet.back) bet.back();
}

void Bet_Cancel(const char *why)
{
    static const char *const ok[] = {"OK"};
    if (!bet.active) return;
    fprintf(stderr, "memories-pc: bet: off (%s)\n", why);
    close_bet();
    Menu_ShowNotice("Scommessa", why, ok, 1, 0, back_closed);
}

static void cancel_chosen(int button, int *quit)
{
    static const char *const no[] = {"OK"};
    (void)quit;
    bet.asking = 0;
    if (button == 0) {
        if (bet.send) bet.send("{\"t\":\"annulla_scommessa\"}");
        fprintf(stderr, "memories-pc: bet: cancelled here\n");
        close_bet();
        Menu_ShowNotice("Scommessa", "Sfida annullata.", no, 1, 0, back_closed);
    } else {
        show();
    }
}

static void changed(void)
{
    bet.mine.ready = bet.theirs.ready = 0;
}

static void cards_chosen(int button, int *quit)
{
    int pages = (bet.owned_count + PAGE - 1) / PAGE, shown = bet.owned_count - bet.page * PAGE, card;
    (void)quit;
    if (!bet.active) return;
    if (shown > PAGE) shown = PAGE;
    if (button >= 0 && button < shown) {
        const Owned *pick = &owned[bet.page * PAGE + button];
        card = pick->id;
        if (card != bet.mine.card) changed();
        bet.mine.card = card;
        bet.mine.deck = pick->deck;
        bet.view = VIEW_MAIN;
        bet.focus = 0;
    } else if (button == shown) {
        bet.page = bet.page > 0 ? bet.page - 1 : (pages ? pages - 1 : 0);
    } else if (button == shown + 1) {
        bet.page = bet.page + 1 < pages ? bet.page + 1 : 0;
    } else if (button == shown + 2) {
        if (bet.mine.card) changed();
        bet.mine.card = bet.mine.deck = 0;
        bet.view = VIEW_MAIN;
        bet.focus = 0;
    } else {
        bet.view = VIEW_MAIN;
        bet.focus = 0;
    }
    send_stake();
    show();
}

static void chips_chosen(int button, int *quit)
{
    static const int steps[] = {-1000, -100, -10, -1, 1, 10, 100, 1000};
    long chips = bet.mine.starchips;
    (void)quit;
    if (!bet.active) return;
    if (button >= 0 && button < 8) chips += steps[button];
    else if (button == 8) chips = 0;
    else if (button == 9) chips = (long)bet.starchips;
    if (chips < 0) chips = 0;
    if (chips > (long)bet.starchips) chips = (long)bet.starchips;
    if (chips != bet.mine.starchips) changed();
    bet.mine.starchips = (int)chips;
    if (button < 0 || button >= 10) {
        bet.view = VIEW_MAIN;
        bet.focus = 2;
    } else {
        bet.focus = button;
    }
    send_stake();
    show();
}

static void main_chosen(int button, int *quit)
{
    static const char *const sure[] = {"Si, annulla", "No"};
    (void)quit;
    if (!bet.active) return;
    bet.focus = button;
    if (button == 0) {
        bet.view = VIEW_CARDS;
        bet.page = 0;
        bet.focus = 0;
        /* the card picked shows on its page */
        if (bet.mine.card) {
            int i;
            for (i = 0; i < bet.owned_count; i++)
                if (owned[i].id == bet.mine.card) bet.page = i / PAGE, bet.focus = i % PAGE;
        }
        send_stake();
    } else if (button == 2) {
        bet.view = VIEW_CHIPS;
        bet.focus = 4;
        send_stake();
    } else if (button == 4) {
        bet.mine.ready = !bet.mine.ready;
        fprintf(stderr, "memories-pc: bet: %s (card %d, %d starchips)\n", bet.mine.ready ? "ready" : "not ready",
                bet.mine.card, bet.mine.starchips);
        send_stake();
    } else {
        bet.asking = 1;
        Menu_ShowNotice("Scommessa", "Vuoi annullare la sfida?", sure, 2, 1, cancel_chosen);
        return;
    }
    show();
}

static void show_chips(void)
{
    static const char *const names[] = {"-1000", "-100", "-10", "-1", "+1", "+10", "+100", "+1000",
                                        "Zero", "Tutti", "OK"};
    char text[160];
    snprintf(text, sizeof(text), "Punti %d starchip (ne hai %u).", bet.mine.starchips, bet.starchips);
    Menu_ShowNoticeGrid("Scommessa - starchip", text, names, 11, 4, NULL, bet.focus, chips_chosen);
}

/* --- the screens, drawn here (Menu_ShowScreen) ----------------------- */

/* The menu's colors (menu.c), and the frames' (mods/cornici: L orange, E
 * purple, R blue, C silver). */
#define C_BACK 0x27282cu
#define C_EDGE 0x3f4147u
#define C_BUTTON 0x33353au
#define C_HOVER 0x4c4e55u
#define C_ACCENT 0x3b82f6u
#define C_TEXT 0xe8e8eau
#define C_DIM 0x8b8d93u
#define C_GREEN 0x4ade80u
#define C_GREEN_DARK 0x15803du
#define C_WHITE 0xffffffu
#define C_EMPTY 0x18191cu
#define COLUMNS 6
#define ROWS 3
#define HITS 32

typedef struct {
    int x, y, w, h, id;
} Hit;

static MenuCanvas *paint;
static Hit hits[HITS];
static int hit_count, hover = -1;

static uint32_t tier_rgb(int id)
{
    switch (tier(id)) {
    case 'L': return 0xf59e0bu;
    case 'E': return 0xa855f7u;
    case 'R': return 0x3b82f6u;
    default: return 0xc0c4ccu;
    }
}

static void fill(int x, int y, int w, int h, uint32_t color, unsigned alpha)
{
    int i, j;
    if (x < 0) w += x, x = 0;
    if (y < 0) h += y, y = 0;
    if (x + w > paint->width) w = paint->width - x;
    if (y + h > paint->height) h = paint->height - y;
    for (j = 0; j < h; j++) {
        uint32_t *row = paint->pixels + (size_t)(y + j) * (size_t)paint->stride + (size_t)x;
        for (i = 0; i < w; i++) {
            uint32_t was = row[i];
            if (alpha >= 255) {
                row[i] = color | 0xff000000u;
                continue;
            }
            row[i] = 0xff000000u |
                     (((color >> 16 & 255) * alpha + (was >> 16 & 255) * (255 - alpha)) / 255) << 16 |
                     (((color >> 8 & 255) * alpha + (was >> 8 & 255) * (255 - alpha)) / 255) << 8 |
                     ((color & 255) * alpha + (was & 255) * (255 - alpha)) / 255;
        }
    }
}

static void frame(int x, int y, int w, int h, int thick, uint32_t color)
{
    fill(x, y, w, thick, color, 255);
    fill(x, y + h - thick, w, thick, color, 255);
    fill(x, y, thick, h, color, 255);
    fill(x + w - thick, y, thick, h, color, 255);
}

static void text(int x, int middle, const char *line, uint32_t color) { Menu_DrawText(paint, x, middle, line, color); }

/* `line` cut to `width` pixels, "..." at the end when it had to be. */
static void fit(char *line, int width)
{
    size_t length = strlen(line); /* room for 3 more */
    if (Menu_TextWidth(line) <= width) return;
    while (length > 0) {
        length = Menu_TextBack(line, length);
        strcpy(line + length, "...");
        if (Menu_TextWidth(line) <= width) return;
    }
}

static void centred(int x, int w, int middle, const char *line, uint32_t color)
{
    char cut[204];
    snprintf(cut, 200, "%s", line);
    fit(cut, w);
    text(x + (w - Menu_TextWidth(cut)) / 2, middle, cut, color);
}

static void add_hit(int x, int y, int w, int h, int id)
{
    if (hit_count < HITS) hits[hit_count].x = x, hits[hit_count].y = y, hits[hit_count].w = w,
                          hits[hit_count].h = h, hits[hit_count++].id = id;
}

static int hit_at(int x, int y)
{
    int i;
    for (i = 0; i < hit_count; i++)
        if (x >= hits[i].x && x < hits[i].x + hits[i].w && y >= hits[i].y && y < hits[i].y + hits[i].h)
            return hits[i].id;
    return -1;
}

static void button(int x, int y, int w, int h, const char *label, int id, int focus, uint32_t back)
{
    fill(x, y, w, h, focus ? C_ACCENT : id == hover ? C_HOVER : back, 255);
    if (!focus) frame(x, y, w, h, 1, C_EDGE);
    centred(x + 4, w - 8, y + h / 2, label, focus || back != C_BUTTON ? C_WHITE : C_TEXT);
    add_hit(x, y, w, h, id);
}

/* A card's picture (its art record off the disc, as the card view has it),
 * the last few kept. */
#define ART_CACHE 24
static struct {
    int card;
    unsigned used;
    DiscArt art;
    uint32_t pixels[CARD_ART_WIDTH * CARD_ART_HEIGHT];
} arts[ART_CACHE];
static unsigned art_clock;

static const DiscArt *card_art(int id)
{
    static unsigned char record[7 * 2048];
    int i, base, slot = 0;
    const unsigned char *pixels = record + CARD_ART_PIXELS, *clut = record + CARD_ART_CLUT;
    if (!id || !Cards_Valid(id)) return NULL;
    for (i = 0; i < ART_CACHE; i++) {
        if (arts[i].card == id) {
            arts[i].used = ++art_clock;
            return &arts[i].art;
        }
        if (arts[i].used < arts[slot].used) slot = i;
    }
    base = Cards_BaseId(id);
    if (base < 1 || base > RETAIL_CARDS || !DiscArt_Read(RETAIL_CARDS + (base - 1) * 7, record, (int)sizeof(record)))
        return NULL;
    Cards_PatchArtRecord(id, record);
    for (i = 0; i < CARD_ART_WIDTH * CARD_ART_HEIGHT; i++) {
        unsigned color = clut[pixels[i] * 2] | (unsigned)clut[pixels[i] * 2 + 1] << 8;
        unsigned r = (color & 31) << 3, g = (color >> 5 & 31) << 3, b = (color >> 10 & 31) << 3;
        arts[slot].pixels[i] = 0xff000000u | (r | r >> 5) << 16 | (g | g >> 5) << 8 | (b | b >> 5);
    }
    arts[slot].art.width = CARD_ART_WIDTH;
    arts[slot].art.height = CARD_ART_HEIGHT;
    arts[slot].art.pixels = arts[slot].pixels;
    arts[slot].card = id;
    arts[slot].used = ++art_clock;
    return &arts[slot].art;
}

/* The picture in a frame of its tier's color, as big as fits w x h. */
static void draw_art(int id, int x, int y, int w, int h, int thick, uint32_t edge)
{
    const DiscArt *art = card_art(id);
    int aw = w - 2 * thick, ah = h - 2 * thick;
    if (aw * CARD_ART_HEIGHT > ah * CARD_ART_WIDTH) aw = ah * CARD_ART_WIDTH / CARD_ART_HEIGHT;
    else ah = aw * CARD_ART_HEIGHT / CARD_ART_WIDTH;
    x += (w - aw - 2 * thick) / 2;
    y += (h - ah - 2 * thick) / 2;
    fill(x, y, aw + 2 * thick, ah + 2 * thick, edge, 255);
    if (art) DiscArt_Draw(paint, art, x + thick, y + thick, aw, ah);
    else fill(x + thick, y + thick, aw, ah, C_EMPTY, 255);
}

static const char *const type_names[] = {
    "Drago", "Incantatore", "Zombie", "Guerriero", "Guerriero-Bestia", "Bestia", "Bestia Alata", "Demone",
    "Fata", "Insetto", "Dinosauro", "Rettile", "Pesce", "Serpente Marino", "Macchina", "Tuono", "Acqua",
    "Pyro", "Roccia", "Pianta", "Magia", "Trappola", "Rituale", "Equipaggiamento"};

static const char *type_name(int id)
{
    int type = Cards_Type(id);
    return type >= 0 && type < (int)(sizeof(type_names) / sizeof(type_names[0])) ? type_names[type]
                                                                                : Cards_TypeName(type);
}

/* What the card does, in the language shown (a translation's text), on one line. */
static void card_text(int id, char *out, size_t size)
{
    const unsigned char *codes = Cards_DescriptionText(id);
    size_t n = 0;
    if (!codes) codes = Text_Resolve(0xD100 + id, NULL);
    while (codes && *codes != 0xFF && n + 5 < size) {
        int code = *codes++;
        uint32_t c;
        if (code >= 0xF6 && code != 0xFE) continue;
        if (code >= 0xF0 && code < 0xF6) code = ((code - 0xF0) << 8) | *codes++;
        c = code == 0xFE || !code ? ' ' : Glyphs_Character(code);
        if (!c) c = '?';
        if (c == ' ' && (!n || out[n - 1] == ' ')) continue;
        if (c < 0x80) out[n++] = (char)c;
        else if (c < 0x800) out[n++] = (char)(0xC0 | c >> 6), out[n++] = (char)(0x80 | (c & 0x3F));
        else out[n++] = (char)(0xE0 | (c >> 12 & 15)), out[n++] = (char)(0x80 | (c >> 6 & 0x3F)),
             out[n++] = (char)(0x80 | (c & 0x3F));
    }
    out[n] = '\0';
}

/* `all` wrapped to `width`: up to `most` lines, the last cut with "...". */
static int wrap(const char *all, int width, char lines[][168], int most)
{
    int count = 0;
    while (*all && count < most) {
        char *line = lines[count];
        size_t best = 0, next = 0;
        for (;;) {
            while (all[next] && all[next] != ' ') next++;
            if (next > 160) break;
            memcpy(line, all, next);
            line[next] = '\0';
            if (best && Menu_TextWidth(line) > width) break;
            best = next;
            if (!all[next]) break;
            next++;
        }
        if (!best) best = Menu_TextFit(all, 160);
        memcpy(line, all, best);
        line[best] = '\0';
        all += best;
        while (*all == ' ') all++;
        if (++count == most && *all) snprintf(line + strlen(line), 161 - strlen(line), " %s", all);
        fit(line, width);
    }
    return count;
}

/* The lines under a card: its name, what it is, its tier; then its text. */
static int card_lines(int id, int x, int w, int top, int line, int text_lines)
{
    char name[96], what[96], words[600], lines[4][168];
    int i, count;
    card_name(id, name, sizeof(name));
    centred(x, w, top + line / 2, name, C_WHITE);
    if (monster(id))
        snprintf(what, sizeof(what), "ATK %d  DEF %d  -  %s", stat(id, 0), stat(id, 1), type_name(id));
    else snprintf(what, sizeof(what), "%s", type_name(id));
    centred(x, w, top + line + line / 2, what, C_TEXT);
    centred(x, w, top + 2 * line + line / 2, tier_name(id), tier_rgb(id));
    if (text_lines <= 0) return 3;
    card_text(id, words, sizeof(words));
    count = wrap(words, w, lines, text_lines > 4 ? 4 : text_lines);
    for (i = 0; i < count; i++) centred(x, w, top + (3 + i) * line + line / 2, lines[i], C_DIM);
    return 3 + count;
}

/* The box the screens are drawn in, and its title. */
static void backdrop(int x, int y, int w, int h, int ui, const char *title, const char *right)
{
    int line = 19 * ui;
    hit_count = 0;
    fill(x, y, w, h, C_BACK, 255);
    fill(x, y, w, 3 * ui, C_ACCENT, 255);
    text(x + 12 * ui, y + 3 * ui + line / 2 + 4 * ui, title, C_WHITE);
    if (right) text(x + w - 12 * ui - Menu_TextWidth(right), y + 3 * ui + line / 2 + 4 * ui, right, C_DIM);
}

/* Main: the two stakes side by side, the starchips, Pronto and Annulla.
 * Buttons as main_chosen numbers them: 0 the card, 2 the starchips, 4 Pronto,
 * 5 Annulla. */
static void draw_main(MenuCanvas *canvas, int x, int y, int w, int h)
{
    int ui = Menu_Scale(), line = 19 * ui, pad = 12 * ui, row = 30 * ui, half, top, card_h, side, bottom;
    char label[160], status[96];
    paint = canvas;
    if (row < Menu_TouchTarget()) row = Menu_TouchTarget();
    snprintf(label, sizeof(label), "Scommessa con %s", bet.rival);
    backdrop(x, y, w, h, ui, label, !strcmp(bet.mode, "arena") ? "vale per tutta la serie" : NULL);
    half = (w - 3 * pad) / 2;
    top = y + 3 * ui + line + 10 * ui;
    bottom = y + h - pad - row;                /* Pronto, Annulla */
    bottom -= row + 6 * ui;                    /* the starchips */
    bottom -= line * (bet.mine.deck ? 2 : 1); /* the rule, the deck's */
    for (side = 0; side < 2; side++) {
        const Stake *stake = side ? &bet.theirs : &bet.mine;
        int cx = x + pad + side * (half + pad), at = top, focus = !side && bet.focus == 0, text_lines;
        if (side) {
            if (!strcmp(bet.theirs.choosing, "carta")) snprintf(status, sizeof(status), "sta scegliendo la carta...");
            else if (!strcmp(bet.theirs.choosing, "starchip"))
                snprintf(status, sizeof(status), "sta scegliendo gli starchip...");
            else snprintf(status, sizeof(status), bet.theirs.ready ? "PRONTO" : "non ancora pronto");
            snprintf(label, sizeof(label), "%s: %s", bet.rival, status);
        } else {
            snprintf(label, sizeof(label), "Tu: %s", bet.mine.ready ? "PRONTO" : "tocca la carta per cambiarla");
        }
        centred(cx, half, at + line / 2, label,
                stake->ready ? C_GREEN : side && stake->choosing[0] ? C_ACCENT : C_TEXT);
        at += line + 4 * ui;
        text_lines = stake->card && !monster(stake->card) ? 2 : 0;
        card_h = bottom - at - (3 + text_lines) * line - 6 * ui;
        if (card_h < 3 * line) card_h = 3 * line, text_lines = 0;
        if (!side) add_hit(cx, at - line - 4 * ui, half, bottom - at + line + 4 * ui, 0);
        if (focus || (!side && hover == 0)) fill(cx, at - 2 * ui, half, card_h + 4 * ui, focus ? C_ACCENT : C_HOVER, 90);
        if (stake->card) {
            draw_art(stake->card, cx, at, half, card_h, 3 * ui, tier_rgb(stake->card));
            card_lines(stake->card, cx, half, at + card_h + 6 * ui, line, text_lines);
        } else {
            int bw = card_h * CARD_ART_WIDTH / CARD_ART_HEIGHT;
            if (bw > half) bw = half;
            fill(cx + (half - bw) / 2, at, bw, card_h, C_EMPTY, 255);
            frame(cx + (half - bw) / 2, at, bw, card_h, ui, side ? C_EDGE : focus ? C_WHITE : C_DIM);
            centred(cx, half, at + card_h / 2, side ? "nessuna carta" : "Nessuna carta: tocca qui per sceglierla",
                    side ? C_DIM : C_TEXT);
        }
    }
    top = bottom;
    centred(x + pad, w - 2 * pad, top + line / 2,
            "Chi vince prende tutte e due le puntate. Niente da tutti e due = si gioca senza scommessa.", C_DIM);
    top += line;
    if (bet.mine.deck) {
        centred(x + pad, w - 2 * pad, top + line / 2,
                "La tua carta e' nel mazzo: se la perdi, al suo posto va la piu' debole del baule.", C_DIM);
        top += line;
    }
    top += 6 * ui;
    snprintf(label, sizeof(label), "Starchip: %d  (cambia)", bet.mine.starchips);
    button(x + pad, top, half, row, label, 2, bet.focus == 2, C_BUTTON);
    snprintf(label, sizeof(label), "%s: %d starchip", bet.rival, bet.theirs.starchips);
    fill(x + 2 * pad + half, top, half, row, C_EMPTY, 255);
    centred(x + 2 * pad + half + 4, half - 8, top + row / 2, label, C_TEXT);
    top += row + 6 * ui;
    button(x + pad, top, half, row, bet.mine.ready ? "PRONTO!  (tocca per toglierlo)" : "Pronto", 4, bet.focus == 4,
           bet.mine.ready ? C_GREEN_DARK : C_BUTTON);
    button(x + 2 * pad + half, top, half, row, "Annulla la sfida", 5, bet.focus == 5, C_BUTTON);
}

/* The collection: a page of cards (three rows of six) with the one under the
 * cursor big on the right. Ids: a card is its place on the page, then
 * PAGE + 0 "<", 1 ">", 2 Nessuna carta, 3 Indietro, 4 Punta questa. */
enum { HIT_PREVIOUS = PAGE, HIT_NEXT, HIT_NONE, HIT_BACK, HIT_PICK };

static int pages(void) { return bet.owned_count ? (bet.owned_count + PAGE - 1) / PAGE : 1; }

static int shown_cards(void)
{
    int shown = bet.owned_count - bet.page * PAGE;
    return shown < 0 ? 0 : shown > PAGE ? PAGE : shown;
}

/* A card of the deck goes only when the trunk has one to take its place. */
static int takeable(const Owned *card) { return !card->deck || bet.chest_total > 0; }

static void draw_cards(MenuCanvas *canvas, int x, int y, int w, int h)
{
    int ui = Menu_Scale(), line = 19 * ui, pad = 10 * ui, row = 30 * ui, shown, i, grid_w, side_w, cell_w, cell_h,
        art_h, top, bottom;
    char label[160];
    paint = canvas;
    if (row < Menu_TouchTarget()) row = Menu_TouchTarget();
    if (bet.page >= pages()) bet.page = 0;
    shown = shown_cards();
    snprintf(label, sizeof(label), "Pagina %d di %d", bet.page + 1, pages());
    backdrop(x, y, w, h, ui, "Scegli la carta da puntare", label);
    top = y + 3 * ui + line + 10 * ui;
    bottom = y + h - pad - row;
    side_w = (w - 3 * pad) * 3 / 10;
    grid_w = w - 3 * pad - side_w;
    cell_w = (grid_w - (COLUMNS - 1) * 6 * ui) / COLUMNS;
    cell_h = (bottom - pad - top - (ROWS - 1) * 6 * ui) / ROWS;
    art_h = cell_h - line;
    if (art_h * CARD_ART_WIDTH > cell_w * CARD_ART_HEIGHT) art_h = cell_w * CARD_ART_HEIGHT / CARD_ART_WIDTH;
    if (!shown) centred(x + pad, grid_w, top + cell_h / 2, "Nel salvataggio non ci sono carte.", C_DIM);
    for (i = 0; i < shown; i++) {
        const Owned *card = &owned[bet.page * PAGE + i];
        int cx = x + pad + i % COLUMNS * (cell_w + 6 * ui), cy = top + i / COLUMNS * (cell_h + 6 * ui);
        int chosen = card->id == bet.mine.card, focus = i == bet.focus;
        if (focus || i == hover) fill(cx - 3 * ui, cy - 3 * ui, cell_w + 6 * ui, cell_h + 6 * ui, focus ? C_ACCENT : C_HOVER, 255);
        draw_art(card->id, cx, cy, cell_w, art_h, 2 * ui, chosen ? C_GREEN : tier_rgb(card->id));
        if (card->deck) {
            int bw = Menu_TextWidth("MAZZO") + 6 * ui;
            if (bw > cell_w) bw = cell_w;
            fill(cx + 2 * ui, cy + 2 * ui, bw, line, 0x000000u, 190);
            text(cx + 5 * ui, cy + 2 * ui + line / 2, bw < Menu_TextWidth("MAZZO") ? "M" : "MAZZO", C_WHITE);
        }
        if (!takeable(card)) fill(cx, cy, cell_w, art_h, 0x000000u, 150);
        if (monster(card->id)) snprintf(label, sizeof(label), "%d", stat(card->id, 0));
        else snprintf(label, sizeof(label), "%s", type_name(card->id));
        centred(cx, cell_w, cy + art_h + line / 2, label, chosen ? C_GREEN : focus ? C_WHITE : C_TEXT);
        add_hit(cx, cy, cell_w, cell_h, i);
    }
    /* the card under the cursor, big */
    if (bet.focus < shown) {
        const Owned *card = &owned[bet.page * PAGE + bet.focus];
        int sx = x + 2 * pad + grid_w, big_h = bottom - pad - top - 5 * line - row - 8 * ui;
        if (big_h * CARD_ART_WIDTH > side_w * CARD_ART_HEIGHT) big_h = side_w * CARD_ART_HEIGHT / CARD_ART_WIDTH;
        draw_art(card->id, sx, top, side_w, big_h, 3 * ui, tier_rgb(card->id));
        i = card_lines(card->id, sx, side_w, top + big_h + 6 * ui, line, monster(card->id) ? 0 : 2);
        if (card->deck) centred(sx, side_w, top + big_h + 6 * ui + i * line + line / 2,
                                takeable(card) ? "nel tuo MAZZO" : "nel MAZZO (baule vuoto: non si puo')", C_DIM);
        if (takeable(card)) button(sx, bottom - pad - row, side_w, row,
                                   card->id == bet.mine.card ? "Puntata (OK)" : "Punta questa", HIT_PICK, 0,
                                   C_GREEN_DARK);
    }
    {
        int bw = (w - 2 * pad - 3 * 6 * ui) / 4, bx = x + pad;
        static const char *const names[] = {"< Pagina", "Pagina >", "Nessuna carta", "Indietro"};
        for (i = 0; i < 4; i++, bx += bw + 6 * ui)
            button(bx, bottom, bw, row, names[i], HIT_PREVIOUS + i, bet.focus == PAGE + i, C_BUTTON);
    }
}

/* --- the screens' input ----------------------------------------------- */

static void screen_pressed(int id, int *quit)
{
    if (bet.view == VIEW_MAIN) {
        if (id == 0 || id == 2 || id == 4 || id == 5) main_chosen(id, quit);
        return;
    }
    if (id >= 0 && id < shown_cards()) {
        if (id != bet.focus) {
            bet.focus = id; /* a first tap shows it big; a second one puts it up */
            Menu_ScreenChanged();
        } else if (takeable(&owned[bet.page * PAGE + id])) {
            cards_chosen(id, quit);
        }
    } else if (id == HIT_PICK) {
        cards_chosen(bet.focus, quit);
    } else if (id >= HIT_PREVIOUS && id <= HIT_BACK) {
        bet.focus = id - HIT_PREVIOUS + PAGE;
        cards_chosen(shown_cards() + id - HIT_PREVIOUS, quit);
    }
}

/* Up, Down, Left, Right as -COLUMNS, +COLUMNS, -1, +1 over the screen's places. */
static void screen_move(int dx, int dy, int *quit)
{
    if (bet.view == VIEW_MAIN) {
        static const int down[] = {2, 0, 4, 0, 4, 5}, up[] = {0, 0, 0, 0, 2, 2};
        if (dy > 0) bet.focus = down[bet.focus];
        else if (dy < 0) bet.focus = up[bet.focus];
        else if (bet.focus >= 4) bet.focus = dx > 0 ? 5 : 4;
        return;
    }
    {
        int shown = shown_cards(), f = bet.focus;
        if (f >= PAGE) { /* the buttons' row */
            if (dx) f = PAGE + (f - PAGE + dx + 4) % 4;
            else if (dy < 0 && shown) f = (shown - 1) / COLUMNS * COLUMNS + (f - PAGE) * COLUMNS / 4;
            if (f < PAGE && f >= shown) f = shown - 1;
        } else if (dx < 0 && f % COLUMNS == 0 && pages() > 1) {
            cards_chosen(shown, quit); /* the page before, from its right */
            bet.focus = f + COLUMNS - 1 < shown_cards() ? f + COLUMNS - 1 : shown_cards() - 1;
            return;
        } else if (dx > 0 && (f % COLUMNS == COLUMNS - 1 || f + 1 >= shown) && pages() > 1) {
            cards_chosen(shown + 1, quit);
            bet.focus = f - f % COLUMNS < shown_cards() ? f - f % COLUMNS : 0;
            return;
        } else if (dx) {
            f = f + dx < 0 ? 0 : f + dx >= shown ? shown - 1 : f + dx;
        } else if (dy > 0) {
            f = f + COLUMNS < shown ? f + COLUMNS : f / COLUMNS == (shown - 1) / COLUMNS ? PAGE + (f % COLUMNS) * 4 / COLUMNS
                                                                                        : shown - 1;
        } else if (dy < 0 && f >= COLUMNS) {
            f -= COLUMNS;
        }
        bet.focus = f;
    }
}

static int screen_event(const MenuEvent *event, int *quit)
{
    int at;
    switch (event->type) {
    case MENU_EVENT_BUTTON_DOWN:
        if (event->button == 1 && (at = hit_at(event->x, event->y)) >= 0) screen_pressed(at, quit);
        return 1;
    case MENU_EVENT_MOTION:
        at = hit_at(event->x, event->y);
        if (at == hover) return 0;
        hover = at;
        return 1;
    case MENU_EVENT_LEAVE:
        if (hover < 0) return 0;
        hover = -1;
        return 1;
    case MENU_EVENT_WHEEL:
        if (bet.view == VIEW_CARDS) cards_chosen(shown_cards() + (event->wheel > 0 ? 0 : 1), quit);
        return 1;
    case MENU_EVENT_KEY_DOWN:
        switch (event->key) {
        case MENU_KEY_LEFT: screen_move(-1, 0, quit); break;
        case MENU_KEY_RIGHT: case MENU_KEY_TAB: screen_move(1, 0, quit); break;
        case MENU_KEY_UP: screen_move(0, -1, quit); break;
        case MENU_KEY_DOWN: screen_move(0, 1, quit); break;
        case MENU_KEY_ENTER:
            screen_pressed(bet.view == VIEW_CARDS && bet.focus >= PAGE ? HIT_PREVIOUS + bet.focus - PAGE
                           : bet.view == VIEW_CARDS ? HIT_PICK : bet.focus, quit);
            break;
        case MENU_KEY_ESCAPE: screen_pressed(bet.view == VIEW_CARDS ? HIT_BACK : 5, quit); break;
        default: break;
        }
        return 1;
    default:
        return 0;
    }
}

static int screen_pad(uint16_t pressed, int *quit)
{
    if (pressed & BUTTON_LAYOUT_CROSS)
        screen_pressed(bet.view == VIEW_CARDS && bet.focus >= PAGE ? HIT_PREVIOUS + bet.focus - PAGE
                       : bet.view == VIEW_CARDS ? (bet.focus < shown_cards() &&
                                                   takeable(&owned[bet.page * PAGE + bet.focus]) ? HIT_PICK : -1)
                                                : bet.focus, quit);
    else if (pressed & BUTTON_LAYOUT_CIRCLE) screen_pressed(bet.view == VIEW_CARDS ? HIT_BACK : 5, quit);
    else if (pressed & 0x0400 && bet.view == VIEW_CARDS) cards_chosen(shown_cards(), quit);     /* L1 */
    else if (pressed & 0x0800 && bet.view == VIEW_CARDS) cards_chosen(shown_cards() + 1, quit); /* R1 */
    else if (pressed & 0x0010) screen_move(0, -1, quit);
    else if (pressed & 0x0040) screen_move(0, 1, quit);
    else if (pressed & 0x0080) screen_move(-1, 0, quit);
    else if (pressed & 0x0020) screen_move(1, 0, quit);
    else return 0;
    return 1;
}

static const MenuScreen main_screen = {draw_main, screen_event, screen_pad};
static const MenuScreen cards_screen = {draw_cards, screen_event, screen_pad};

static void show_main(void)
{
    if (bet.focus != 0 && bet.focus != 2 && bet.focus != 4 && bet.focus != 5) bet.focus = 0;
    hover = -1;
    Menu_ShowScreen(&main_screen);
}

static void show_cards(void)
{
    if (bet.focus >= shown_cards() && bet.focus < PAGE) bet.focus = shown_cards() ? shown_cards() - 1 : PAGE + 3;
    hover = -1;
    Menu_ShowScreen(&cards_screen);
}

static void show(void)
{
    if (!bet.active) return;
    if (bet.view == VIEW_CARDS) show_cards();
    else if (bet.view == VIEW_CHIPS) show_chips();
    else show_main();
}

/* --- the lobby's ----------------------------------------------------- */

void Bet_Open(const char *room, int side, const char *mode, const char *rival, int test,
              void (*send)(const char *line), void (*back)(void))
{
    int slot = SaveMenu_CurrentSlot();
    memset(&bet, 0, sizeof(bet));
    bet.active = 1;
    bet.side = side;
    bet.slot = slot < 0 ? 0 : slot;
    bet.send = send;
    bet.back = back;
    snprintf(bet.room, sizeof(bet.room), "%s", room);
    snprintf(bet.mode, sizeof(bet.mode), "%s", mode);
    snprintf(bet.rival, sizeof(bet.rival), "%s", rival);
    read_collection();
    fprintf(stderr, "memories-pc: bet: open against %s (%s), save slot %d: %d cards, %u starchips\n", rival, mode,
            bet.slot + 1, bet.owned_count, bet.starchips);
    bet.test = test || getenv("MEMORIES_BET_TEST");
    if (bet.test) {
        const char *given = getenv("MEMORIES_BET_TEST");
        int card = 0, chips = 0, i;
        if (given) sscanf(given, "%d:%d", &card, &chips);
        for (i = 0; i < bet.owned_count; i++)
            if (owned[i].id == card) bet.mine.card = card, bet.mine.deck = owned[i].deck;
        bet.mine.starchips = chips < 0 ? 0 : chips > (int)bet.starchips ? (int)bet.starchips : chips;
        bet.mine.ready = 1;
    }
    send_stake();
    show();
}

int Bet_Active(void) { return bet.active; }

int Bet_With(const char *name) { return bet.active && !strcmp(bet.rival, name); }

int Bet_Handle(const char *type, const JsonValue *root)
{
    const JsonValue *mine;
    int card, before_card = bet.theirs.card, before_chips = bet.theirs.starchips;
    if (strcmp(type, "puntata") != 0) return 0;
    if (!bet.active) return 1;
    card = (int)Json_Number(Json_Member(root, "carta"), 0);
    bet.theirs.card = card >= 1 && card <= RETAIL_CARDS ? card : 0;
    bet.theirs.starchips = (int)Json_Number(Json_Member(root, "starchip"), 0);
    if (bet.theirs.starchips < 0 || bet.theirs.starchips > STARCHIPS_MAX) bet.theirs.starchips = 0;
    bet.theirs.ready = Json_Bool(Json_Member(root, "pronto"), 0);
    snprintf(bet.theirs.choosing, sizeof(bet.theirs.choosing), "%s", Json_String(Json_Member(root, "sceglie"), ""));
    /* the server's word on this one's "Pronto": off when the other's stake changed */
    mine = Json_Member(root, "tuo_pronto");
    if (mine) bet.mine.ready = Json_Bool(mine, 0);
    if (bet.theirs.card != before_card || bet.theirs.starchips != before_chips) bet.mine.ready = 0;
    if (bet.test && !bet.mine.ready) {
        bet.mine.ready = 1;
        send_stake();
    }
    if (Menu_NoticeShown() && bet.view == VIEW_MAIN && !bet.asking) show();
    return 1;
}

void Bet_Forget(void)
{
    char path[1024];
    if (!bet_path(path, sizeof(path))) remove(path);
}

void Bet_Go(const JsonValue *root)
{
    char path[1024];
    FILE *file;
    int my_card, my_chips, their_card, their_chips;
    if (!bet.active) return;
    bet.active = 0;
    /* the server's stakes, which both games were shown */
    my_card = (int)Json_Number(Json_Member(root, "mia_carta"), 0);
    my_chips = (int)Json_Number(Json_Member(root, "miei_starchip"), 0);
    their_card = (int)Json_Number(Json_Member(root, "sua_carta"), 0);
    their_chips = (int)Json_Number(Json_Member(root, "suoi_starchip"), 0);
    if (my_card < 0 || my_card > RETAIL_CARDS) my_card = 0;
    if (their_card < 0 || their_card > RETAIL_CARDS) their_card = 0;
    if (my_chips < 0 || my_chips > STARCHIPS_MAX) my_chips = 0;
    if (their_chips < 0 || their_chips > STARCHIPS_MAX) their_chips = 0;
    Bet_Forget();
    if (!my_card && !my_chips && !their_card && !their_chips) {
        fprintf(stderr, "memories-pc: bet: nothing put up, a duel with no bet\n");
        return;
    }
    if (bet_path(path, sizeof(path)) || !(file = fopen(path, "wb"))) {
        fprintf(stderr, "memories-pc: bet: cannot write scommessa.txt\n");
        return;
    }
    fprintf(file, "contro=%s\nslot=%d\nmodo=%s\nmia=%d,%d,%d\nsua=%d,%d\n", bet.rival, bet.slot, bet.mode, my_card,
            my_card == bet.mine.card ? bet.mine.deck : 0, my_chips, their_card, their_chips);
    fclose(file);
    fprintf(stderr, "memories-pc: bet: on: card %d + %d starchips against card %d + %d starchips (save slot %d)\n",
            my_card, my_chips, their_card, their_chips, bet.slot + 1);
}

/* --- after the duel -------------------------------------------------- */

typedef struct {
    char rival[NAME], mode[8];
    int slot, my_card, my_deck, my_chips, their_card, their_chips;
} Wager;

static int read_wager(Wager *wager)
{
    char path[1024], line[128];
    FILE *file;
    memset(wager, 0, sizeof(*wager));
    if (bet_path(path, sizeof(path)) || !(file = fopen(path, "rb"))) return -1;
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!strncmp(line, "contro=", 7)) snprintf(wager->rival, sizeof(wager->rival), "%s", line + 7);
        else if (!strncmp(line, "modo=", 5)) snprintf(wager->mode, sizeof(wager->mode), "%s", line + 5);
        else if (!strncmp(line, "slot=", 5)) wager->slot = atoi(line + 5);
        else if (!strncmp(line, "mia=", 4))
            sscanf(line + 4, "%d,%d,%d", &wager->my_card, &wager->my_deck, &wager->my_chips);
        else if (!strncmp(line, "sua=", 4)) sscanf(line + 4, "%d,%d", &wager->their_card, &wager->their_chips);
    }
    fclose(file);
    if (wager->slot < 0 || wager->slot >= SAVE_SLOT_COUNT) wager->slot = 0;
    if (wager->my_card < 0 || wager->my_card > RETAIL_CARDS) wager->my_card = 0;
    if (wager->their_card < 0 || wager->their_card > RETAIL_CARDS) wager->their_card = 0;
    return wager->rival[0] ? 0 : -1;
}

static void stake_text(int card, int chips, char *out, size_t size)
{
    char name[96];
    card_full(card, name, sizeof(name));
    if (card && chips) snprintf(out, size, "%s e %d starchip", name, chips);
    else if (card) snprintf(out, size, "%s", name);
    else snprintf(out, size, "%d starchip", chips);
}

void Bet_Describe(int won, char *out, size_t size)
{
    Wager wager;
    char stake[160];
    out[0] = '\0';
    if (read_wager(&wager)) return;
    if (won && (wager.their_card || wager.their_chips)) {
        stake_text(wager.their_card, wager.their_chips, stake, sizeof(stake));
        snprintf(out, size, "\n\nScommessa vinta: prendi %s.", stake);
    } else if (!won && (wager.my_card || wager.my_chips)) {
        stake_text(wager.my_card, wager.my_chips, stake, sizeof(stake));
        snprintf(out, size, "\n\nScommessa persa: dai %s.", stake);
    } else {
        snprintf(out, size, won ? "\n\nScommessa vinta (non puntava niente)." : "\n\nScommessa persa (non hai puntato niente).");
    }
}

/* The trunk's weakest card with a copy to spare for the deck, -1 when none. */
static int weakest(const unsigned short *deck)
{
    int id, best = -1, i;
    for (id = 1; id <= RETAIL_CARDS; id++) {
        int copies = 0;
        if (!state[CHEST_OFFSET + id - 1]) continue;
        for (i = 0; i < DECK_CARDS; i++) copies += deck[i] == id;
        if (copies >= DECK_COPIES) continue;
        if (best < 0 || power(id) < power(best)) best = id;
    }
    return best;
}

void Bet_Settle(int (*result)(const char *rival))
{
    Wager wager;
    unsigned short deck[DECK_CARDS];
    unsigned chips;
    char stake[200], name[96], put[96];
    int won, i;
    settled[0] = '\0';
    if (read_wager(&wager)) return;
    Bet_Forget();
    won = result(wager.rival);
    fprintf(stderr, "memories-pc: bet: against %s: %s\n", wager.rival, won < 0 ? "no duel" : won ? "won" : "lost");
    if (won < 0) {
        snprintf(settled, sizeof(settled), "La scommessa con %s e' annullata: il duello non e' cominciato.",
                 wager.rival);
        return;
    }
    if (won ? !wager.their_card && !wager.their_chips : !wager.my_card && !wager.my_chips) return;
    if (SaveSlots_ReadState(wager.slot, state, check)) {
        snprintf(settled, sizeof(settled), "Non riesco a leggere il salvataggio (slot %d): la scommessa con %s non "
                 "e' stata applicata.", wager.slot + 1, wager.rival);
        return;
    }
    for (i = 0; i < DECK_CARDS; i++) deck[i] = (unsigned short)(state[2 * i] | state[2 * i + 1] << 8);
    memcpy(&chips, state + STARCHIPS_OFFSET, sizeof(chips));
    put[0] = '\0';
    if (won) {
        if (wager.their_card && state[CHEST_OFFSET + wager.their_card - 1] < CHEST_MAX)
            state[CHEST_OFFSET + wager.their_card - 1]++;
        chips = chips + (unsigned)wager.their_chips > STARCHIPS_MAX ? STARCHIPS_MAX : chips + (unsigned)wager.their_chips;
        stake_text(wager.their_card, wager.their_chips, stake, sizeof(stake));
    } else {
        int card = wager.my_card, at = -1;
        if (card) {
            for (i = 0; i < DECK_CARDS; i++)
                if (deck[i] == card) at = i;
            if ((wager.my_deck || !state[CHEST_OFFSET + card - 1]) && at >= 0) {
                /* out of the deck: the weakest of the trunk takes its place */
                int instead;
                deck[at] = 0;
                instead = weakest(deck);
                if (instead > 0) {
                    state[CHEST_OFFSET + instead - 1]--;
                    deck[at] = (unsigned short)instead;
                    state[2 * at] = (unsigned char)(instead & 0xFF);
                    state[2 * at + 1] = (unsigned char)(instead >> 8);
                    card_name(instead, name, sizeof(name));
                    snprintf(put, sizeof(put), "\nEra nel mazzo: al suo posto c'e' %s (dal baule).", name);
                } else {
                    deck[at] = (unsigned short)card;
                    fprintf(stderr, "memories-pc: bet: the trunk is empty: card %d stays in the deck\n", card);
                }
            } else if (state[CHEST_OFFSET + card - 1]) {
                state[CHEST_OFFSET + card - 1]--;
            }
        }
        chips = chips > (unsigned)wager.my_chips ? chips - (unsigned)wager.my_chips : 0;
        stake_text(wager.my_card, wager.my_chips, stake, sizeof(stake));
    }
    memcpy(state + STARCHIPS_OFFSET, &chips, sizeof(chips));
    SaveData_WritePrimarySecondaryIntegrity(state);
    SaveData_WriteTertiaryIntegrity(state);
    if (!check(state) || SaveSlots_WriteState(wager.slot, state)) {
        snprintf(settled, sizeof(settled), "Non riesco a scrivere il salvataggio (slot %d): la scommessa con %s non "
                 "e' stata applicata.", wager.slot + 1, wager.rival);
        fprintf(stderr, "memories-pc: bet: the save was not written\n");
        return;
    }
    fprintf(stderr, "memories-pc: bet: save slot %d: %s card %d, starchips now %u\n", wager.slot + 1,
            won ? "got" : "gave", won ? wager.their_card : wager.my_card, chips);
    snprintf(settled, sizeof(settled), "%s contro %s: %s %s.%s\n\nE' gia' nel salvataggio (slot %d).",
             won ? "Scommessa vinta" : "Scommessa persa", wager.rival, won ? "hai preso" : "hai dato", stake, put,
             wager.slot + 1);
}

static void settled_closed(int button, int *quit) { (void)button, (void)quit; }

void Bet_Frame(void)
{
    static const char *const ok[] = {"OK"};
    if (!settled[0] || Menu_NoticeShown()) return;
    Menu_ShowNotice("Scommessa", settled, ok, 1, 0, settled_closed);
    settled[0] = '\0';
}
