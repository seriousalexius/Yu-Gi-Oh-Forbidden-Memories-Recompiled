/* Draft dei drop: a won duel offers several cards from the opponent's own
 * drop table (the same pool and rank the game rolls SPOILS from) and the
 * player keeps one. The others are never awarded.
 *
 * The results screen is the game's DuelScene_UpdateResultRewards, called once
 * a frame. Its first frame rolls SPOILS' card (and, with Game > Card drops
 * above 1, the extra ones in gCardDrops). This mod wraps it: after that first
 * frame it gathers those cards, rolls the rest with the game's own
 * Duel_SelectCardDrop, and holds the screen's input while its panel is up.
 * The chosen card becomes SPOILS' card, which the game then awards as
 * always when the player leaves the screen.
 *
 * The panel lists the cards with their chance in that pool and how many the
 * player owns, each name in its power tier's color and with a star when it is on
 * the Enciclopedia's wish list, and draws the one under the cursor
 * (shared/fmview.h). */
#define D_8009B360_AS_SIDE_ARRAY
#include "types.h"
#include "pc/mods/modapi.h"

static const MemoriesModHost *host;

#include "../../shared/fmview.h"
#include "game/ai_opponent_data.h"
#include "pc/cards/drops.h"
#include "game/duel_result_display.h"
#include "game/duel_rewards.h"
#include "game/duel_scene_state.h"
#include "game/duel_side_state.h"
#include "game/duel_effect.h"
#include "game/text_staging.h"
#include "game/input.h"
#include "game/card_constants.h"
#include "pc/cards/cards.h"
#include <stdlib.h>

#define DRAFT_MAX 10
/* Regola bloccata (22/10): 1 carta su 3, doppioni tenuti come nel gioco. */
#define DRAFT_CHOICES 3
#define SCENE_STARTED 0x8000 /* gDuel_wSceneStateFlags: the results screen's first frame ran */

static void *original;

/* No pointers: a save state made while the panel is up keeps it. */
static struct {
    s32 picking;            /* the panel is up */
    s32 count;              /* cards offered */
    s32 cursor;
    u16 cards[DRAFT_MAX];
    u8 owned[DRAFT_MAX];    /* copies in deck and chest before the duel */
} state;

/* Triangle: the card under the cursor in big, as the arena's sheet (its
 * picture, ATK/DEF, kind, tier and the card's text in Italian, testi.txt
 * as the arena's, from tools/testi_carte.py); 0 while the list is up. */
static int sheet;
static char text_buf[96 * 1024];
static int text_at[CARD_TABLE_ID_END + 1]; /* 1 + where in text_buf, 0 none */
static int texts_loaded;

static void load_texts(void)
{
    FILE *file;
    char line[1024];
    size_t used = 0;
    texts_loaded = 1;
    if (!(file = host->open_asset(host, "testi.txt"))) {
        host->log(host, "draft: testi.txt non trovato: la scheda resta senza testo");
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
    }
    fclose(file);
}

static const char *card_text(int id)
{
    if (!texts_loaded) load_texts();
    return id > 0 && id <= CARD_TABLE_ID_END && text_at[id] ? text_buf + text_at[id] - 1 : NULL;
}

static int offered(int id)
{
    int i;
    for (i = 0; i < state.count; i++) {
        if (state.cards[i] == id) return 1;
    }
    return 0;
}

static void offer(int id)
{
    state.owned[state.count] = (u8)fv_clamp(fv_owned(id), 0, 255);
    state.cards[state.count++] = (u16)id;
}

/* The pool SPOILS was rolled from, as the game picks it: B/C/D, S/A-POW or
 * S/A-TEC. */
static int drop_pool(void)
{
    if (D_8009B1E8->rank_tier < 3) return 1;
    return D_8009B1E8->is_tec_rank ? 2 : 0;
}

/* Only a duel the player won against the CPU gives a card. */
static int player_won(void)
{
    return D_8009B360[0] < 0 && gDuel_bOpponentID >= 0 && !gDuel_bWinnerSide;
}

static void start_draft(void)
{
    int wanted = DRAFT_CHOICES;
    int no_repeats = 0;
    int spoils = D_8009B1E8->dropped_card_id, pool = drop_pool(), i, tries;

    state.count = 0;
    state.cursor = 0;
    if (!Cards_Valid(spoils)) return;
    offer(spoils);
    /* Cards Game > Card drops (or a tower) already dealt count as offered:
       none of them is awarded unless chosen. */
    for (i = 0; i < gCardDrops.count && state.count < wanted; i++) {
        if (no_repeats && offered(gCardDrops.cards[i])) continue;
        offer(gCardDrops.cards[i]);
    }
    gCardDrops.count = 0;
    /* A table with fewer different cards than asked for stops short. */
    for (tries = 0; state.count < wanted && tries < 64 * DRAFT_MAX; tries++) {
        int card = Duel_SelectCardDrop(pool);
        if (!Cards_Valid(card) || (no_repeats && offered(card))) continue;
        offer(card);
    }
    state.picking = state.count > 1;
    fv_drops_load();
    for (i = 0; i < state.count; i++) fv_art_load(state.cards[i]);
}

static void choose(int index)
{
    int card = state.cards[index];

    state.picking = 0;
    D_8009B1E8->dropped_card_id = (s16)card;
    gDuel_wSelectedCardID = (s16)card;
    D_801D56A8[0] = card;
    D_8009B1E8->page_index = 0;
    Duel_ShowResultPage(0);
}

static void read_input(void)
{
    u16 pressed = gInput_wPad1Pressed, repeat = gInput_wPad1Repeat;

    /* The results screen must not see them: Cross would leave it. */
    gInput_wPad1Pressed = 0;
    gInput_wPad1Repeat = 0;
    if (sheet) {
        if (pressed & (PAD_BUTTON_TRIANGLE | PAD_BUTTON_CROSS | PAD_BUTTON_CIRCLE)) sheet = 0;
        return;
    }
    if (pressed & PAD_BUTTON_TRIANGLE) {
        sheet = state.cards[state.cursor];
        return;
    }
    if (repeat & PAD_DIRECTION_UP) state.cursor = (state.cursor + state.count - 1) % state.count;
    if (repeat & PAD_DIRECTION_DOWN) state.cursor = (state.cursor + 1) % state.count;
    if (pressed & PAD_BUTTON_CROSS) choose(state.cursor);
}

/* A duel of the arena mod gives nothing of the normal game (arena:duello). */
static int arena_duel(void)
{
    volatile int *duel = (volatile int *)host->find(host, "arena:duello");
    return duel && *duel;
}

static void results_screen(void)
{
    int started = gDuel_wSceneStateFlags & SCENE_STARTED;

    if (state.picking) read_input();
    ((void (*)(void))original)();
    if (!started && (gDuel_wSceneStateFlags & SCENE_STARTED)) {
        state.picking = 0;
        sheet = 0;
        if (player_won() && !arena_duel()) start_draft();
    }
    /* A state loaded on this screen has the cards but not their pictures. */
    if (state.picking) fv_art_load(state.cards[state.cursor]);
}

/* --- the panel ---------------------------------------------------------- */

static void row_texts(int i, char *name, size_t name_size, char *stat, size_t stat_size, char *chance,
                      size_t chance_size, char *own, size_t own_size)
{
    unsigned weight = fv_weight(gDuel_bOpponentID, drop_pool(), state.cards[i]);
    char plain[64];
    fv_card_name(state.cards[i], plain, sizeof(plain));
    snprintf(name, name_size, "%s%s", fv_wished(state.cards[i]) ? "* " : "", plain);
    fv_stats(state.cards[i], stat, stat_size);
    if (weight) fv_percent(weight, chance, chance_size);
    else snprintf(chance, chance_size, "-");
    snprintf(own, own_size, state.owned[i] ? "hai %d" : "NUOVA", state.owned[i]);
}

/* As many words a row as fit in w, rows down to bottom (the arena's). */
static void draw_wrapped(const char *text, int x, int mid, int w, int bottom, int line, uint32_t rgb, int s)
{
    char row[256];
    while (*text == ' ') text++;
    while (*text && mid + line / 2 <= bottom) {
        int end = 0, fit = 0, last = mid + line + line / 2 > bottom;
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
            snprintf(row, sizeof(row), "%s", text);
            fv_fit(row, w, s);
            host->draw_text(host, x, mid, row, rgb, s);
            return;
        }
        memcpy(row, text, (size_t)end);
        row[end] = 0;
        host->draw_text(host, x, mid, row, rgb, s);
        text += end;
        while (*text == ' ') text++;
        mid += line;
    }
}

static void draw_sheet(void)
{
    static const char *const attributes[6] = {"Luce", "Oscurita'", "Terra", "Acqua", "Fuoco", "Vento"};
    static const char *const types[20] = {"Drago",   "Incantatore", "Zombie",    "Guerriero", "Guerriero-Bestia",
                                          "Bestia",  "Bestia Alata", "Demone",   "Fata",      "Insetto",
                                          "Dinosauro", "Rettile",   "Pesce",     "Serpente Marino", "Macchina",
                                          "Tuono",   "Acqua",       "Pyro",      "Roccia",    "Pianta"};
    FvArea area;
    char name[64], kind[96], text[96];
    int id = sheet, s, line, pad, gap, panel_w, panel_h, x, y, top, bottom, zoom, cw = 0, ch = 0, tx, tw, mid;
    char tier = id > 0 && id < (int)sizeof(fv_fasce) - 1 ? fv_fasce[id] : 0;
    const char *tier_name = tier == 'L' ? "Leggendaria" : tier == 'E' ? "Epica" : tier == 'R' ? "Rara" :
                            tier == 'C' ? "Comune" : "";
    fv_area(&area, 360);
    s = area.s;
    line = 18 * s;
    pad = 10 * s;
    gap = 12 * s;
    panel_w = area.w;
    panel_h = area.h;
    x = area.x;
    y = area.y;
    host->fill(host, 0, 0, area.width, area.height, 0x000000, 150);
    fv_box(x, y, panel_w, panel_h, s);
    fv_card_name(id, name, sizeof(name));
    if (fv_is_monster(id)) {
        int type_id = Cards_Type(id), attribute = Cards_Attribute(id);
        snprintf(kind, sizeof(kind), "Mostro %s - Livello %d%s%s", type_id >= 0 && type_id < 20 ? types[type_id] : "",
                 Cards_Level(id), attribute >= 0 && attribute < 6 ? " - " : "",
                 attribute >= 0 && attribute < 6 ? attributes[attribute] : "");
    } else {
        fv_stats(id, kind, sizeof(kind));
    }
    fv_fit(name, panel_w - 2 * pad, s);
    host->draw_text(host, x + pad, y + pad + line / 2, name, fv_tier_rgb(id), s);
    top = y + pad + line + 6 * s;
    bottom = y + panel_h - pad - line - 4 * s;
    zoom = fv_card_zoom((panel_w - 2 * pad) / 2, bottom - top, s);
    if (zoom) {
        fv_card_size(zoom, s, &cw, &ch);
        fv_card_draw(id, x + pad, top, zoom, s);
    }
    tx = x + pad + (zoom ? cw + gap : 0);
    tw = x + panel_w - pad - tx;
    mid = top + line / 2;
    fv_fit(kind, tw, s);
    host->draw_text(host, tx, mid, kind, 0xB8C0D8, s);
    mid += line;
    if (fv_is_monster(id)) {
        snprintf(text, sizeof(text), "ATK %d   DEF %d", fv_stat(id, 0), fv_stat(id, 1));
        host->draw_text(host, tx, mid, text, 0xF0D070, s);
        mid += line;
    }
    if (tier_name[0]) {
        snprintf(text, sizeof(text), "Fascia: %s", tier_name);
        host->draw_text(host, tx, mid, text, fv_tier_rgb(id), s);
        mid += line;
    }
    mid += 4 * s;
    if (card_text(id)) draw_wrapped(card_text(id), tx, mid, tw, bottom, line, 0xF4F4F4, s);
    host->draw_text(host, x + pad, y + panel_h - pad - line / 2, "/\\, X o O: torna alle 3 carte", 0x8890A8, s);
}

static void draw_panel(void)
{
    FvArea area;
    int s, line, pad, gap, name_w = 0, stat_w, chance_w, own_w, list_w, list_h, card_w = 0, card_h = 0, zoom;
    int panel_w, panel_h, x, y, top, i;
    char name[64], stat[24], chance[16], own[24], title[48], subtitle[96], duelist[48];

    if (!state.picking) return;
    if (sheet) {
        draw_sheet();
        return;
    }
    fv_area(&area, 360);
    s = area.s;
    line = 18 * s;
    pad = 10 * s;
    gap = 10 * s;
    stat_w = host->text_width(host, "0000/0000", s);
    chance_w = host->text_width(host, "00.00%", s);
    own_w = host->text_width(host, "hai 99", s);
    if (host->text_width(host, "NUOVA", s) > own_w) own_w = host->text_width(host, "NUOVA", s);
    for (i = 0; i < state.count; i++) {
        int w;
        row_texts(i, name, sizeof(name), stat, sizeof(stat), chance, sizeof(chance), own, sizeof(own));
        if ((w = host->text_width(host, name, s) + fv_icon_room(s)) > name_w) name_w = w;
    }
    list_h = state.count * line;
    /* The card beside the list, as large as the space allows but no larger
       than three times the picture's own size at this scale. */
    zoom = fv_card_zoom(area.w * 2 / 5, area.h - 4 * line - 2 * pad, s);
    if (zoom > 3 * s) zoom = 3 * s;
    if (zoom) fv_card_size(zoom, s, &card_w, &card_h);
    list_w = name_w + stat_w + chance_w + own_w + 3 * gap;
    panel_w = list_w + (zoom ? card_w + gap : 0) + 2 * pad;
    if (panel_w > area.w) {
        name_w -= panel_w - area.w;
        list_w -= panel_w - area.w;
        panel_w = area.w;
    }
    panel_h = 2 * line + (list_h > card_h ? list_h : card_h) + line + 2 * pad + 4 * s;
    if (panel_h > area.h) panel_h = area.h;
    x = area.x + (area.w - panel_w) / 2;
    y = area.y + (area.h - panel_h) / 2;

    host->fill(host, 0, 0, area.width, area.height, 0x000000, 110);
    fv_box(x, y, panel_w, panel_h, s);
    snprintf(title, sizeof(title), "SCEGLI UNA CARTA");
    host->draw_text(host, x + pad, y + pad + line / 2, title, 0xF0D070, s);
    fv_duelist_name(gDuel_bOpponentID, duelist, sizeof(duelist));
    snprintf(subtitle, sizeof(subtitle), "%s - %s: probabilita di ognuna", duelist, fv_pool_names[drop_pool()]);
    fv_fit(subtitle, panel_w - 2 * pad, s);
    host->draw_text(host, x + pad, y + pad + line + line / 2, subtitle, 0xB8C0D8, s);
    top = y + pad + 2 * line + 4 * s;
    if (zoom) fv_card_draw(state.cards[state.cursor], x + panel_w - pad - card_w, top, zoom, s);
    for (i = 0; i < state.count; i++) {
        int mid = top + i * line + line / 2, chosen = i == state.cursor, at = x + pad;
        u32 color = chosen ? 0xFFFFFF : 0xB8C0D8;
        if (chosen) host->fill(host, at - pad / 2, mid - line / 2, list_w + pad, line, FV_CURSOR_RGB, 230);
        row_texts(i, name, sizeof(name), stat, sizeof(stat), chance, sizeof(chance), own, sizeof(own));
        fv_fit(name, name_w - fv_icon_room(s), s);
        fv_icon_text(state.cards[i], at, mid, name, fv_tier_rgb(state.cards[i]), s);
        at += name_w + gap;
        host->draw_text(host, at, mid, stat, color, s);
        at += stat_w + gap;
        host->draw_text(host, at + chance_w - host->text_width(host, chance, s), mid, chance, 0xF0D070, s);
        host->draw_text(host, x + pad + list_w - host->text_width(host, own, s), mid, own,
                        state.owned[i] ? color : 0x70E070, s);
    }
    host->draw_text(host, x + pad, y + panel_h - pad - line / 2, "Su/Giu: scorri   X: prendi   /\\: scheda", 0x8890A8, s);
}

static unsigned panel_signature(void)
{
    int left = 0, right = 0;
    if (!state.picking) return 1;
    TouchPad_FreeSpan(&left, &right);
    return 2u + (unsigned)state.cursor * 131u + (unsigned)state.count * 7u + (unsigned)state.cards[0] * 1031u +
           (fv_art_find(state.cards[state.cursor]) ? 1u : 0u) * 5u + (unsigned)sheet * 7919u + (unsigned)left * 17u + (unsigned)right * 19u;
}

int MemoriesModInit(const MemoriesModHost *from, MemoriesMod *mod)
{
    if (from->api < 13) return 0;
    host = from;
    mod->api = 13;
    mod->overlay = draw_panel;
    mod->overlay_signature = panel_signature;
    return host->register_state(host, &state, sizeof(state), 1) &&
           host->hook(host, (void *)DuelScene_UpdateResultRewards, (void *)results_screen, &original) != 0;
}
