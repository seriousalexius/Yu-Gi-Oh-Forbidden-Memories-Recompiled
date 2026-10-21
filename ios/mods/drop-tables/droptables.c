/* Tabelle dei drop: on the Free Duel grid, a button (Triangle unless the
 * player chose another) opens the drop table of the duelist under the
 * cursor, one pool at a time: it always opens on S/A-POW, Right goes on to
 * S/A-TEC, B/C/D and last MAZZO (the cards its deck is dealt from, by the same
 * weights out of 2048), so Left from S/A-POW is MAZZO too. The rarest card
 * first, each with its chance and how many the player owns, and the card
 * under the cursor drawn beside the list. From there:
 *
 *   X on a card     the card large, and every duelist and pool that drops
 *                   it, the likeliest first; X on one opens that table
 *   Square          every card by number, to look one up
 *   Circle          back; Start closes everything
 *
 * While the panel is up the game gets no input at all (the INPUT event's
 * before-hook clears it), and after it closes none until every button is let
 * go, so the Circle that closed it does not also leave the Free Duel screen.
 *
 * The numbers are the game's own: the pools come off the disc and through
 * Tables_PoolFor, as the roll at the end of a duel takes them
 * (shared/fmview.h). A card's name, and its chance in the list of who drops
 * it, are in its rarity's color; a star marks the Enciclopedia's wishes. */
#include "types.h"
#include "pc/mods/modapi.h"

static const MemoriesModHost *host;

#include "../../shared/fmview.h"
#include "game/main_modes.h"
#include "game/fade.h"
#include "game/file_transfer.h"
#include "overlays/free_duel/free_duel.h"
#include "overlays/free_duel/module_state.h"

extern unsigned char D_8009B26C; /* main_mode_state.h: the running mode */

/* Main_RunFreeDuelMenu sets 0x40 once it has opened the screen. The screen's
 * own flags: 0x20 while a text box is up, 0x40 once it is leaving
 * (free_duel_progress_view.c). */
enum { MODE_MASK = 0x1F, MODE_ENTERED = 0x40, SCREEN_DIALOG = 0x20, SCREEN_LEAVING = 0x40 };

enum { SCREEN_TABLE = 1, SCREEN_CARD, SCREEN_INDEX };

typedef struct {
    int screen, duelist, pool, card, cursor, top;
} View;

typedef struct {
    int duelist, pool, card;
    unsigned weight;
} Row;

#define DEPTH 8
#define ROWS_MAX 4096

static View stack[DEPTH];
static int depth;
static Row rows[ROWS_MAX];
static int row_count;
static View built;           /* what `rows` was made for */
static int visible = 10;     /* list rows the last drawing had room for */
/* The pools in the order Left/Right walks them: the two the player farms
   most first, the deck (not a drop) last. */
static const int page_order[FV_ALL_POOLS] = {0, 2, 1, FV_POOL_DECK};
static FvPad pad;
/* Read by the INPUT hook, which may run from VBlank. */
static volatile int swallow;            /* the game gets no buttons */
static volatile unsigned open_key_held; /* the open key, kept from the grid */

static View *top_view(void)
{
    return depth ? &stack[depth - 1] : NULL;
}

/* --- the duelist under the Free Duel grid's cursor ------------------------ */

static int grid_duelist(void)
{
    int column, row, duelist;

    if ((D_8009B26C & MODE_MASK) != MAIN_MODE_FREE_DUEL || !(D_8009B26C & MODE_ENTERED)) return 0;
    /* Until the module has loaded and the screen faded in, its state is the
     * last screen's. */
    if ((D_8009B0F4 & FILE_TRANSFER_REQUEST_BLOCKED_MASK) | D_8009B134) return 0;
    if (gFade_State.flags & FADE_FLAG_ACTIVE) return 0;
    if (gFreeDuel_bScreenFlags & (SCREEN_DIALOG | SCREEN_LEAVING)) return 0;
    column = gFreeDuel_bTargetColumn;
    row = gFreeDuel_bTargetRow;
    if (column < 0 || column >= FREE_DUEL_GRID_COLUMN_COUNT || row < 0 || row >= FREE_DUEL_GRID_ROW_COUNT) return 0;
    duelist = Duelists_AtCell(row * FREE_DUEL_GRID_COLUMN_COUNT + column);
    if (duelist < FREE_DUEL_STORY_OPPONENT_FIRST_INDEX || !Duelists_Available(duelist)) return 0;
    return duelist;
}

/* --- the rows ------------------------------------------------------------- */

/* Rarest first in a table; likeliest first in a card's list of duelists. */
static int before(const Row *a, const Row *b, int screen)
{
    if (a->weight != b->weight) return screen == SCREEN_TABLE ? a->weight < b->weight : a->weight > b->weight;
    if (a->card != b->card) return a->card < b->card;
    if (a->duelist != b->duelist) return a->duelist < b->duelist;
    return a->pool < b->pool;
}

static void sort_rows(int screen)
{
    int i, j;
    for (i = 1; i < row_count; i++) {
        Row row = rows[i];
        for (j = i; j > 0 && before(&row, &rows[j - 1], screen); j--) rows[j] = rows[j - 1];
        rows[j] = row;
    }
}

static void add_row(int duelist, int pool, int card, unsigned weight)
{
    if (row_count >= ROWS_MAX) return;
    rows[row_count].duelist = duelist;
    rows[row_count].pool = pool;
    rows[row_count].card = card;
    rows[row_count].weight = weight;
    row_count++;
}

static void build_rows(const View *view)
{
    int duelist, pool, card;
    unsigned weight;

    row_count = 0;
    switch (view->screen) {
    case SCREEN_TABLE:
        for (card = 1; card <= fv_cards; card++) {
            if ((weight = fv_weight(view->duelist, view->pool, card))) add_row(view->duelist, view->pool, card, weight);
        }
        break;
    case SCREEN_CARD:
        for (duelist = 1; duelist < fv_duelists; duelist++) {
            for (pool = 0; pool < FV_POOLS; pool++) {
                if ((weight = fv_weight(duelist, pool, view->card))) add_row(duelist, pool, view->card, weight);
            }
        }
        break;
    default:
        /* The index is every card by number: row i is card i + 1. */
        row_count = fv_cards;
        break;
    }
    sort_rows(view->screen);
    built = *view;
}

/* The rows match the view on top, made again when it changed. */
static void rows_for(const View *view)
{
    if (view->screen != built.screen || view->duelist != built.duelist || view->pool != built.pool ||
        view->card != built.card)
        build_rows(view);
}

static int row_card(const View *view, int index)
{
    if (index < 0 || index >= row_count) return 0;
    return view->screen == SCREEN_INDEX ? index + 1 : rows[index].card;
}

static int find_card(int card)
{
    int i;
    for (i = 0; i < row_count; i++) {
        if (rows[i].card == card) return i;
    }
    return 0;
}

/* --- opening and closing ---------------------------------------------------- */

static void push(int screen, int duelist, int pool, int card, int cursor)
{
    View *view;
    if (depth == DEPTH) {
        memmove(stack, stack + 1, sizeof(stack[0]) * (DEPTH - 1));
        depth--;
    }
    view = &stack[depth++];
    view->screen = screen;
    view->duelist = duelist;
    view->pool = pool;
    view->card = card;
    view->cursor = cursor;
    view->top = 0;
    built.screen = 0;
    swallow = 1;
}

static void open_table(int duelist, int pool, int card)
{
    View *view;
    push(SCREEN_TABLE, duelist, pool, 0, 0);
    view = top_view();
    rows_for(view);
    view->cursor = card ? find_card(card) : 0;
}

static void pop(void)
{
    if (depth) depth--;
    built.screen = 0;
}

static void close_all(void)
{
    depth = 0;
    built.screen = 0;
}

/* --- input -------------------------------------------------------------------- */

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

static int next_card(int card, int step)
{
    int i;
    for (i = 0; i < fv_cards; i++) {
        card += step;
        if (card < 1) card = fv_cards;
        if (card > fv_cards) card = 1;
        if (Cards_Valid(card)) return card;
    }
    return card;
}

static void table_input(View *view, unsigned short pressed, unsigned short repeat)
{
    int card = row_card(view, view->cursor);

    if (repeat & FV_UP) move(view, -1, 1);
    if (repeat & FV_DOWN) move(view, 1, 1);
    if (repeat & FV_L1) move(view, -visible, 0);
    if (repeat & FV_R1) move(view, visible, 0);
    if (pressed & (FV_LEFT | FV_RIGHT)) {
        int page = 0;
        while (page < FV_ALL_POOLS - 1 && page_order[page] != view->pool) page++;
        view->pool = page_order[(page + (pressed & FV_LEFT ? FV_ALL_POOLS - 1 : 1)) % FV_ALL_POOLS];
        rows_for(view);
        view->cursor = find_card(card);
        view->top = 0;
    }
    if ((pressed & FV_CROSS) && card) push(SCREEN_CARD, 0, 0, card, 0);
    if (pressed & FV_SQUARE) push(SCREEN_INDEX, 0, 0, 0, card ? card - 1 : 0);
}

static void card_input(View *view, unsigned short pressed, unsigned short repeat)
{
    if (repeat & FV_UP) move(view, -1, 1);
    if (repeat & FV_DOWN) move(view, 1, 1);
    if (repeat & (FV_LEFT | FV_RIGHT)) {
        view->card = next_card(view->card, repeat & FV_LEFT ? -1 : 1);
        view->cursor = view->top = 0;
    }
    if ((pressed & FV_CROSS) && view->cursor < row_count) {
        const Row row = rows[view->cursor];
        open_table(row.duelist, row.pool, row.card);
    }
}

static void index_input(View *view, unsigned short pressed, unsigned short repeat)
{
    if (repeat & FV_UP) move(view, -1, 1);
    if (repeat & FV_DOWN) move(view, 1, 1);
    if (repeat & FV_LEFT) move(view, -10, 0);
    if (repeat & FV_RIGHT) move(view, 10, 0);
    if (repeat & FV_L1) move(view, -100, 0);
    if (repeat & FV_R1) move(view, 100, 0);
    if ((pressed & FV_CROSS) && row_card(view, view->cursor)) push(SCREEN_CARD, 0, 0, row_card(view, view->cursor), 0);
}

/* The card the screen on top shows large or beside its list. */
static int shown_card(const View *view)
{
    return view->screen == SCREEN_CARD ? view->card : row_card(view, view->cursor);
}

static void frame(void)
{
    unsigned short pressed, repeat;
    unsigned key = (unsigned)host->setting(host, "open_key", FV_TRIANGLE);
    View *view;
    int duelist;

    fv_pad_read(&pad, &pressed, &repeat);
    if (!depth) {
        duelist = grid_duelist();
        open_key_held = duelist ? key : 0;
        if (duelist && key && (pressed & key) && fv_drops_load()) open_table(duelist, 0, 0);
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
        pop();
        return;
    }
    switch (view->screen) {
    case SCREEN_TABLE: table_input(view, pressed, repeat); break;
    case SCREEN_CARD: card_input(view, pressed, repeat); break;
    default: index_input(view, pressed, repeat); break;
    }
    view = top_view();
    rows_for(view);
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

/* --- drawing --------------------------------------------------------------------- */

#define LINE 16
#define GAP 10

typedef struct {
    FvArea area;
    int s, line, pad, gap, x, y, w, h;
} Panel;

static void panel_begin(Panel *p, const char *title, const char *subtitle)
{
    char text[96];
    int s;

    fv_area(&p->area, 420);
    p->s = s = p->area.s;
    p->line = LINE * s;
    p->pad = 8 * s;
    p->gap = GAP * s;
    p->w = p->area.w > 900 * s ? 900 * s : p->area.w;
    p->h = p->area.h;
    p->x = p->area.x + (p->area.w - p->w) / 2;
    p->y = p->area.y;
    host->fill(host, 0, 0, p->area.width, p->area.height, 0x000000, 120);
    fv_box(p->x, p->y, p->w, p->h, s);
    snprintf(text, sizeof(text), "%s", title);
    fv_fit(text, p->w - 2 * p->pad, s);
    host->draw_text(host, p->x + p->pad, p->y + p->pad + p->line / 2, text, 0xF0D070, s);
    snprintf(text, sizeof(text), "%s", subtitle);
    fv_fit(text, p->w - 2 * p->pad, s);
    host->draw_text(host, p->x + p->pad, p->y + p->pad + p->line + p->line / 2, text, 0xB8C0D8, s);
}

static void panel_footer(const Panel *p, const char *hint)
{
    char text[128];
    snprintf(text, sizeof(text), "%s", hint);
    fv_fit(text, p->w - 2 * p->pad, p->s);
    host->draw_text(host, p->x + p->pad, p->y + p->h - p->pad - p->line / 2, text, 0x8890A8, p->s);
}

/* The list's room: under the two title lines, over the footer. */
static void list_room(const Panel *p, int *top, int *height)
{
    *top = p->y + p->pad + 2 * p->line + 4 * p->s;
    *height = p->y + p->h - p->pad - p->line - 2 * p->s - *top;
}

/* A card drawn right of the list when there is room for both, at most two
 * fifths of the panel; the list's width is what is left. */
static int side_card(const Panel *p, int card, int list_min, int top, int height)
{
    int inner = p->w - 2 * p->pad, room = inner - list_min - p->gap, zoom, cw, ch;
    if (room > inner * 2 / 5) room = inner * 2 / 5;
    zoom = fv_card_zoom(room, height, p->s);
    if (!zoom || !card) return inner;
    fv_card_size(zoom, p->s, &cw, &ch);
    fv_card_draw(card, p->x + p->w - p->pad - cw, top, zoom, p->s);
    return inner - cw - p->gap;
}

static void owned_text(int card, char *out, size_t size, uint32_t *color)
{
    int have = fv_owned(card);
    snprintf(out, size, have ? "x%d" : "NUOVA", have);
    *color = have ? 0x9098B0 : 0x70E070;
}

static void draw_cursor_row(const Panel *p, int x, int mid, int w)
{
    host->fill(host, x - p->pad / 2, mid - p->line / 2, w + p->pad, p->line, FV_CURSOR_RGB, 230);
}

static void draw_table(const View *view)
{
    char title[96], subtitle[96], name[64], label[64], stat[24], chance[16], own[16];
    int i, have = 0, top, height, list_w, w_stat, w_chance, w_own, name_w, x;
    Panel p;
    uint32_t own_color;

    for (i = 0; i < row_count; i++) have += fv_owned(rows[i].card) > 0;
    fv_duelist_name(view->duelist, name, sizeof(name));
    snprintf(title, sizeof(title), "%s  < %s >", name, fv_pool_names[view->pool]);
    if (view->pool == FV_POOL_DECK)
        snprintf(subtitle, sizeof(subtitle), "%d carte possibili nel suo mazzo, ne hai %d", row_count,
                 have);
    else
        snprintf(subtitle, sizeof(subtitle), "%d carte, ne hai %d - dalla piu rara", row_count, have);
    panel_begin(&p, title, subtitle);
    list_room(&p, &top, &height);
    w_stat = host->text_width(host, "0000/0000", p.s);
    w_chance = host->text_width(host, "00.00%", p.s);
    w_own = host->text_width(host, "NUOVA", p.s);
    list_w = side_card(&p, row_card(view, view->cursor), 12 * host->text_width(host, "M", p.s) + w_stat + w_chance +
                                                             w_own + 3 * p.gap,
                       top, height);
    name_w = list_w - w_stat - w_chance - w_own - 3 * p.gap;
    visible = height / p.line;
    x = p.x + p.pad;
    if (!row_count) host->draw_text(host, x, top + p.line / 2, "Nessuna carta in questo rank.", 0xB8C0D8, p.s);
    for (i = 0; i < visible && view->top + i < row_count; i++) {
        const Row *row = &rows[view->top + i];
        int mid = top + i * p.line + p.line / 2, chosen = view->top + i == view->cursor;
        uint32_t color = chosen ? 0xFFFFFF : 0xB8C0D8;
        if (chosen) draw_cursor_row(&p, x, mid, list_w);
        fv_card_name(row->card, label, sizeof(label));
        snprintf(name, sizeof(name), "%s%s", fv_wished(row->card) ? "* " : "", label);
        fv_fit(name, name_w - fv_icon_room(p.s), p.s);
        fv_stats(row->card, stat, sizeof(stat));
        fv_percent(row->weight, chance, sizeof(chance));
        owned_text(row->card, own, sizeof(own), &own_color);
        fv_icon_text(row->card, x, mid, name, fv_rarity_rgb(row->weight), p.s);
        host->draw_text(host, x + name_w + p.gap, mid, stat, color, p.s);
        host->draw_text(host, x + name_w + w_stat + 2 * p.gap + w_chance - host->text_width(host, chance, p.s), mid,
                        chance, 0xF0D070, p.s);
        host->draw_text(host, x + list_w - w_own, mid, own, chosen ? 0xFFFFFF : own_color, p.s);
    }
    panel_footer(&p, "Su/Giu scorri  L1/R1 pagina  Sx/Dx rank  X carta  [] tutte le carte  O esci");
}

static void draw_card(const View *view)
{
    char subtitle[96], name[64], chance[16];
    int i, top, height, cw, ch, zoom, x, list_w, w_pool, w_chance, name_w, have = fv_owned(view->card);
    Panel p;

    if (row_count) snprintf(subtitle, sizeof(subtitle), "Ne hai %d - la droppano %d tabelle", have, row_count);
    else snprintf(subtitle, sizeof(subtitle), "Ne hai %d - nessun duellante la droppa", have);
    panel_begin(&p, "CHI LA DROPPA", subtitle);
    list_room(&p, &top, &height);
    w_pool = host->text_width(host, "S/A-POW", p.s);
    w_chance = host->text_width(host, "00.00%", p.s);
    /* The card large on the left, at most half the panel. */
    zoom = fv_card_zoom((p.w - 2 * p.pad) / 2, height, p.s);
    if (zoom) {
        fv_card_size(zoom, p.s, &cw, &ch);
        fv_card_draw(view->card, p.x + p.pad, top, zoom, p.s);
    } else {
        cw = 0;
    }
    x = p.x + p.pad + (cw ? cw + p.gap : 0);
    list_w = p.x + p.w - p.pad - x;
    name_w = list_w - w_pool - w_chance - 2 * p.gap;
    visible = height / p.line;
    for (i = 0; i < visible && view->top + i < row_count; i++) {
        const Row *row = &rows[view->top + i];
        int mid = top + i * p.line + p.line / 2, chosen = view->top + i == view->cursor;
        uint32_t color = chosen ? 0xFFFFFF : 0xB8C0D8;
        if (chosen) draw_cursor_row(&p, x, mid, list_w);
        fv_duelist_name(row->duelist, name, sizeof(name));
        fv_fit(name, name_w, p.s);
        fv_percent(row->weight, chance, sizeof(chance));
        host->draw_text(host, x, mid, name, color, p.s);
        host->draw_text(host, x + name_w + p.gap, mid, fv_pool_names[row->pool], color, p.s);
        host->draw_text(host, x + list_w - host->text_width(host, chance, p.s), mid, chance,
                        fv_rarity_rgb(row->weight), p.s);
    }
    panel_footer(&p, "Su/Giu scegli  X la sua tabella  Sx/Dx carta vicina  O indietro");
}

static void draw_index(const View *view)
{
    char subtitle[64], name[64], label[80], stat[24], own[16];
    int i, top, height, list_w, w_stat, w_own, name_w, x;
    Panel p;
    uint32_t own_color;

    snprintf(subtitle, sizeof(subtitle), "%d carte, per numero", fv_cards);
    panel_begin(&p, "TUTTE LE CARTE", subtitle);
    list_room(&p, &top, &height);
    w_stat = host->text_width(host, "0000/0000", p.s);
    w_own = host->text_width(host, "NUOVA", p.s);
    list_w = side_card(&p, row_card(view, view->cursor),
                       16 * host->text_width(host, "M", p.s) + w_stat + w_own + 2 * p.gap, top, height);
    name_w = list_w - w_stat - w_own - 2 * p.gap;
    visible = height / p.line;
    x = p.x + p.pad;
    for (i = 0; i < visible && view->top + i < row_count; i++) {
        int card = row_card(view, view->top + i);
        int mid = top + i * p.line + p.line / 2, chosen = view->top + i == view->cursor;
        uint32_t color = chosen ? 0xFFFFFF : 0xB8C0D8;
        if (chosen) draw_cursor_row(&p, x, mid, list_w);
        fv_card_name(card, name, sizeof(name));
        snprintf(label, sizeof(label), "%03d %s", card, name);
        fv_fit(label, name_w - fv_icon_room(p.s), p.s);
        fv_stats(card, stat, sizeof(stat));
        owned_text(card, own, sizeof(own), &own_color);
        fv_icon_text(card, x, mid, label, color, p.s);
        host->draw_text(host, x + name_w + p.gap, mid, stat, color, p.s);
        host->draw_text(host, x + list_w - w_own, mid, own, chosen ? 0xFFFFFF : own_color, p.s);
    }
    panel_footer(&p, "Su/Giu scorri  Sx/Dx 10  L1/R1 100  X carta  O indietro");
}

static void draw(void)
{
    const View *view = top_view();
    if (!view || built.screen != view->screen) return;
    switch (view->screen) {
    case SCREEN_TABLE: draw_table(view); break;
    case SCREEN_CARD: draw_card(view); break;
    default: draw_index(view); break;
    }
}

static unsigned signature(void)
{
    const View *view = top_view();
    int left = 0, right = 0;
    unsigned hash;

    if (!view) return 1;
    TouchPad_FreeSpan(&left, &right);
    hash = 2u + (unsigned)depth;
    hash = hash * 31u + (unsigned)view->screen;
    hash = hash * 131u + (unsigned)view->duelist;
    hash = hash * 7u + (unsigned)view->pool;
    hash = hash * 1031u + (unsigned)view->card;
    hash = hash * 1031u + (unsigned)view->cursor;
    hash = hash * 1031u + (unsigned)view->top;
    hash = hash * 1031u + (unsigned)row_count;
    hash = hash * 3u + (fv_art_find(shown_card(view)) != NULL);
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
    return host->subscribe(host, MEMORIES_EVENT_INPUT, 0, input) != 0;
}
