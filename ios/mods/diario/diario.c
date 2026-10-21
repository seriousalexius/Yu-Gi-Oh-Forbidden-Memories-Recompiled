/* Diario del duellista: every duel the player wins against the CPU (Free
 * Duel and campaign alike) goes into the mod's diary, with the rank (S-POW to
 * S-TEC) and the card taken at the end, whatever the draft offered. A
 * button of the mod's own on the title's second menu, STATISTICHE (mod.json,
 * "action": "event"), opens it:
 *
 *   AVVERSARI   each duelist met: the Free Duel wins and losses the save
 *               itself keeps (from before the mod too), then the diary's
 *               wins by rank and the cards taken; X shows that duelist's cards
 *   CARTE PRESE every card taken, the latest first, with whom from, the rank
 *               and its chance in that pool, in rarity colors
 *
 * Left/Right between the two, Circle back, Start closes. While it is up the
 * game gets no input (as in drop-tables).
 *
 * The rank is the results screen's (DuelScene_UpdateResultRewards' first
 * frame: rank_tier 0-4 is D to S, is_tec_rank the TEC side, the pool the
 * game rolls from as the draft takes it), the card the REWARD event's: the
 * one the game awards as the player leaves the results. The diary is
 * mod-data/diario/diario.txt, a line a reward: duelist, tier, tec, card,
 * free duel (1) or campaign (0), and 1 for a second card of the same duel
 * (Game > Card drops above 1). */
#define D_8009B360_AS_SIDE_ARRAY
#include "types.h"
#include "pc/mods/modapi.h"

static const MemoriesModHost *host;

#include "../../shared/fmview.h"
#include "game/main_modes.h"
#include "game/fade.h"
#include "game/file_transfer.h"
#include "game/save_data.h"
#include "game/ai_opponent_data.h"
#include "game/duel_result_display.h"
#include "game/duel_rewards.h"
#include "game/duel_scene_state.h"
#include "game/duel_side_state.h"
#include "overlays/free_duel/free_duel.h"
#include "overlays/free_duel/module_state.h"

extern unsigned char D_8009B26C; /* main_mode_state.h: the running mode */

enum { MODE_MASK = 0x1F, MODE_ENTERED = 0x40, SCREEN_DIALOG = 0x20, SCREEN_LEAVING = 0x40 };
#define SCENE_STARTED 0x8000 /* gDuel_wSceneStateFlags: the results screen's first frame ran */
#define FLUSH_FRAMES 120      /* out of the duel this long: a win that gave no card is written as is */

/* --- the diary ---------------------------------------------------------------- */

typedef struct {
    short duelist, card;
    unsigned char tier, tec, free, extra;
} Entry;

#define ENTRIES_MAX 16384
#define DIARY_FILE "diario.txt"
static Entry entries[ENTRIES_MAX];
static int entry_count;
static unsigned diary_rev;

static struct {
    int active, rewards, away;
    Entry entry;
} pending;
static int menu_mode; /* the last mode outside a duel: Free Duel or the campaign's */
static void *original;

static void diary_load(void)
{
    FILE *file = host->open_data(host, DIARY_FILE, "r");
    char line[64];
    entry_count = 0;
    if (!file) return;
    while (entry_count < ENTRIES_MAX && fgets(line, sizeof(line), file)) {
        long v[6] = {0, 0, 0, 0, 0, 0};
        int n = 0, duelist, tier, tec, card, free, extra;
        char *at = line, *end;
        Entry *e = &entries[entry_count];
        while (n < 6 && (v[n] = strtol(at, &end, 10), end != at)) at = end, n++;
        if (n < 5) continue;
        duelist = (int)v[0], tier = (int)v[1], tec = (int)v[2], card = (int)v[3], free = (int)v[4], extra = (int)v[5];
        e->duelist = (short)duelist;
        e->card = (short)card;
        e->tier = (unsigned char)fv_clamp(tier, 0, 4);
        e->tec = (unsigned char)(tec != 0);
        e->free = (unsigned char)(free != 0);
        e->extra = (unsigned char)(extra != 0);
        entry_count++;
    }
    fclose(file);
}

static void diary_add(const Entry *e)
{
    FILE *file;
    char line[64];
    int n;
    if (entry_count < ENTRIES_MAX) entries[entry_count++] = *e;
    diary_rev++;
    if (!(file = host->open_data(host, DIARY_FILE, "ab"))) return;
    n = snprintf(line, sizeof(line), "%d %d %d %d %d %d\n", e->duelist, e->tier, e->tec, e->card, e->free, e->extra);
    if (n > 0) fwrite(line, 1, (size_t)n, file);
    fclose(file);
    if (host->log_enabled(host)) host->log(host, "diario: %s", line);
}

/* A win whose card never came (none awarded): written without one. */
static void flush_pending(void)
{
    if (pending.active && !pending.rewards) diary_add(&pending.entry);
    pending.active = 0;
}

/* Only a duel the player won against the CPU gives a card (as the draft). */
static int player_won(void)
{
    return D_8009B360[0] < 0 && gDuel_bOpponentID >= 0 && !gDuel_bWinnerSide;
}

static void results_screen(void)
{
    int started = gDuel_wSceneStateFlags & SCENE_STARTED;
    ((void (*)(void))original)();
    if (started || !(gDuel_wSceneStateFlags & SCENE_STARTED)) return;
    flush_pending();
    if (!player_won()) return;
    memset(&pending, 0, sizeof(pending));
    pending.active = 1;
    pending.entry.duelist = (short)gDuel_bOpponentID;
    pending.entry.tier = (unsigned char)fv_clamp(D_8009B1E8->rank_tier, 0, 4);
    pending.entry.tec = (unsigned char)(D_8009B1E8->is_tec_rank != 0);
    pending.entry.free = (unsigned char)(menu_mode == MAIN_MODE_FREE_DUEL);
}

static void reward(MemoriesModEvent *event)
{
    Entry e;
    if (event->phase != MEMORIES_AFTER || event->handled || !pending.active || !Cards_Valid(event->a)) return;
    e = pending.entry;
    e.card = (short)event->a;
    e.extra = (unsigned char)(pending.rewards > 0);
    pending.rewards++;
    diary_add(&e);
}

/* --- what the diary says ------------------------------------------------------- */

/* The rank's column: S-POW, A-POW, B/C/D, A-TEC, S-TEC. */
static int rank_column(const Entry *e)
{
    if (e->tier < 3) return 2;
    return e->tec ? (e->tier == 4 ? 4 : 3) : (e->tier == 4 ? 0 : 1);
}

static const char *const rank_names[5] = {"S-POW", "A-POW", "B/C/D", "A-TEC", "S-TEC"};
static const char *const tier_names[5] = {"D", "C", "B", "A", "S"};

static void rank_text(const Entry *e, char *out, size_t size)
{
    if (e->tier < 3) snprintf(out, size, "%s", tier_names[e->tier]);
    else snprintf(out, size, "%s-%s", tier_names[e->tier], e->tec ? "TEC" : "POW");
}

/* The pool the card came from, as the game rolls it. */
static int entry_pool(const Entry *e)
{
    return e->tier < 3 ? 1 : e->tec ? 2 : 0;
}

/* The save's own Free Duel record: wins and losses. */
static void free_record(int duelist, int *wins, int *losses)
{
    *wins = *losses = 0;
    if (duelist < 0 || duelist >= FREE_DUEL_GRID_ENTRY_COUNT) return;
    *wins = gFreeDuel_aDuelistRecords[duelist].result.wins;
    *losses = gFreeDuel_aDuelistRecords[duelist].result.losses;
}

/* --- the screens --------------------------------------------------------------- */

enum { SCREEN_DUELISTS = 1, SCREEN_CARDS, SCREEN_ONE };

typedef struct {
    int screen, duelist, cursor, top;
} View;

typedef struct {
    int duelist, wins, losses, cards;
    int ranks[5];
    const Entry *entry; /* the card rows */
} Row;

#define DEPTH 4
#define ROWS_MAX ENTRIES_MAX
static View stack[DEPTH];
static int depth;
static Row rows[ROWS_MAX];
static int row_count;
static View built;
static unsigned built_rev;
static int visible = 10;
static FvPad pad;
static volatile int swallow; /* read by the INPUT hook, which may run from VBlank */

static View *top_view(void)
{
    return depth ? &stack[depth - 1] : NULL;
}

static void build_rows(const View *view)
{
    int i, duelist;
    row_count = 0;
    if (view->screen == SCREEN_DUELISTS) {
        for (duelist = 1; duelist < fv_duelists && row_count < ROWS_MAX; duelist++) {
            Row *row = &rows[row_count];
            memset(row, 0, sizeof(*row));
            row->duelist = duelist;
            free_record(duelist, &row->wins, &row->losses);
            for (i = 0; i < entry_count; i++) {
                if (entries[i].duelist != duelist) continue;
                if (!entries[i].extra) row->ranks[rank_column(&entries[i])]++;
                if (entries[i].card) row->cards++;
            }
            if (row->wins || row->losses || row->cards || row->ranks[0] + row->ranks[1] + row->ranks[2] + row->ranks[3] +
                                                                row->ranks[4])
                row_count++;
        }
    } else {
        for (i = entry_count - 1; i >= 0 && row_count < ROWS_MAX; i--) {
            if (!entries[i].card || (view->screen == SCREEN_ONE && entries[i].duelist != view->duelist)) continue;
            memset(&rows[row_count], 0, sizeof(rows[0]));
            rows[row_count].duelist = entries[i].duelist;
            rows[row_count++].entry = &entries[i];
        }
    }
    built = *view;
    built_rev = diary_rev;
}

static void rows_for(const View *view)
{
    if (view->screen != built.screen || view->duelist != built.duelist || built_rev != diary_rev) build_rows(view);
}

static void push(int screen, int duelist)
{
    View *view;
    if (depth == DEPTH) {
        memmove(stack, stack + 1, sizeof(stack[0]) * (DEPTH - 1));
        depth--;
    }
    view = &stack[depth++];
    memset(view, 0, sizeof(*view));
    view->screen = screen;
    view->duelist = duelist;
    built.screen = 0;
    swallow = 1;
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

static int shown_card(const View *view)
{
    if (view->screen == SCREEN_DUELISTS || view->cursor >= row_count || !rows[view->cursor].entry) return 0;
    return rows[view->cursor].entry->card;
}

static void open_diary(int duelist)
{
    int i;
    diary_load();
    fv_drops_load();
    push(SCREEN_DUELISTS, 0);
    build_rows(top_view());
    for (i = 0; i < row_count; i++) {
        if (rows[i].duelist == duelist) top_view()->cursor = i;
    }
}

static void frame(void)
{
    unsigned short pressed, repeat;
    int mode = D_8009B26C & MODE_MASK;
    View *view;

    if (mode != MAIN_MODE_DUEL) {
        menu_mode = mode;
        if (pending.active && ++pending.away > FLUSH_FRAMES) flush_pending();
    } else {
        pending.away = 0;
    }

    fv_pad_read(&pad, &pressed, &repeat);
    if (!depth) {
        if (swallow && !pad.held) swallow = 0;
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
    if (repeat & FV_UP) move(view, -1, 1);
    if (repeat & FV_DOWN) move(view, 1, 1);
    if (repeat & FV_L1) move(view, -visible, 0);
    if (repeat & FV_R1) move(view, visible, 0);
    if (view->screen != SCREEN_ONE && (pressed & (FV_LEFT | FV_RIGHT))) {
        view->screen = view->screen == SCREEN_DUELISTS ? SCREEN_CARDS : SCREEN_DUELISTS;
        view->cursor = view->top = 0;
    } else if (view->screen == SCREEN_DUELISTS && (pressed & FV_CROSS) && view->cursor < row_count) {
        push(SCREEN_ONE, rows[view->cursor].duelist);
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
    if (event->a == 0 && swallow) event->b = 0;
}

/* The title's STATISTICHE button: the diary over the menu, which stays. */
static void menu(MemoriesModEvent *event)
{
    const char *name;
    if (event->phase != MEMORIES_BEFORE || event->handled || depth) return;
    if (!(name = host->menu_item(host, event->a)) || strcmp(name, "diario:statistiche") != 0) return;
    event->handled = 1;
    event->result = -1;
    open_diary(0);
}

/* --- drawing ------------------------------------------------------------------- */

#define LINE 16
#define GAP 10
#define TEXT_RGB 0xB8C0D8
#define DIM_RGB 0x8890A8
#define GOLD_RGB 0xF0D070
#define WIN_RGB 0x70E070
#define LOSS_RGB 0xF08060

static void right_text(int right, int mid, const char *text, uint32_t rgb, int s)
{
    host->draw_text(host, right - host->text_width(host, text, s), mid, text, rgb, s);
}

static void draw(void)
{
    const View *view = top_view();
    FvArea area;
    char title[96], subtitle[128], text[96], name[64];
    int s, line, pad_, gap, w, h, x, y, top, list_h, i, cw = 0, ch, zoom, list_w, card = 0;

    if (!view || built.screen != view->screen) return;
    fv_area(&area, 420);
    s = area.s;
    line = LINE * s;
    pad_ = 8 * s;
    gap = GAP * s;
    w = area.w > 960 * s ? 960 * s : area.w;
    h = area.h;
    x = area.x + (area.w - w) / 2;
    y = area.y;
    host->fill(host, 0, 0, area.width, area.height, 0x000000, 120);
    fv_box(x, y, w, h, s);

    if (view->screen == SCREEN_DUELISTS) {
        int wins = 0, losses = 0, taken = 0;
        for (i = 0; i < row_count; i++) wins += rows[i].wins, losses += rows[i].losses;
        for (i = 0; i < entry_count; i++) taken += entries[i].card != 0;
        snprintf(title, sizeof(title), "DIARIO DEL DUELLISTA - AVVERSARI");
        snprintf(subtitle, sizeof(subtitle), "Free Duel: %d vinte, %d perse   Carte prese col diario: %d", wins, losses,
                 taken);
    } else if (view->screen == SCREEN_CARDS) {
        snprintf(title, sizeof(title), "DIARIO DEL DUELLISTA - CARTE PRESE");
        snprintf(subtitle, sizeof(subtitle), "%d carte, dall'ultima", row_count);
    } else {
        fv_duelist_name(view->duelist, name, sizeof(name));
        snprintf(title, sizeof(title), "DIARIO - %s", name);
        snprintf(subtitle, sizeof(subtitle), "%d carte prese contro di lui, dall'ultima", row_count);
    }
    fv_fit(title, w - 2 * pad_, s);
    host->draw_text(host, x + pad_, y + pad_ + line / 2, title, GOLD_RGB, s);
    fv_fit(subtitle, w - 2 * pad_, s);
    host->draw_text(host, x + pad_, y + pad_ + line + line / 2, subtitle, TEXT_RGB, s);
    top = y + pad_ + 2 * line + 4 * s;
    list_h = y + h - pad_ - line - 2 * s - top;

    if (view->screen == SCREEN_DUELISTS) {
        /* name | V P | the five ranks | cards */
        int col = host->text_width(host, "S-POW", s) + gap / 2, num = host->text_width(host, "999", s) + gap / 2;
        int cards_w = host->text_width(host, "Carte", s) + gap;
        int name_w = w - 2 * pad_ - 2 * num - 5 * col - cards_w - 2 * gap, at;
        int head = top + line / 2;
        list_w = w - 2 * pad_;
        host->draw_text(host, x + pad_, head, "Avversario", DIM_RGB, s);
        at = x + pad_ + name_w + gap;
        right_text(at + num, head, "V", WIN_RGB, s);
        right_text(at + 2 * num, head, "P", LOSS_RGB, s);
        at += 2 * num + gap;
        for (i = 0; i < 5; i++) right_text(at + (i + 1) * col, head, rank_names[i], DIM_RGB, s);
        right_text(x + pad_ + list_w, head, "Carte", DIM_RGB, s);
        top += line;
        list_h -= line;
        visible = list_h / line;
        if (!row_count)
            host->draw_text(host, x + pad_, top + line / 2, "Ancora nessun duello: vinci un duello e torna qui.", DIM_RGB, s);
        for (i = 0; i < visible && view->top + i < row_count; i++) {
            const Row *row = &rows[view->top + i];
            int mid = top + i * line + line / 2, k;
            if (view->top + i == view->cursor)
                host->fill(host, x + pad_ / 2, mid - line / 2, list_w + pad_, line, FV_CURSOR_RGB, 230);
            fv_duelist_name(row->duelist, name, sizeof(name));
            fv_fit(name, name_w, s);
            host->draw_text(host, x + pad_, mid, name, TEXT_RGB, s);
            at = x + pad_ + name_w + gap;
            snprintf(text, sizeof(text), "%d", row->wins);
            right_text(at + num, mid, text, row->wins ? WIN_RGB : DIM_RGB, s);
            snprintf(text, sizeof(text), "%d", row->losses);
            right_text(at + 2 * num, mid, text, row->losses ? LOSS_RGB : DIM_RGB, s);
            at += 2 * num + gap;
            for (k = 0; k < 5; k++) {
                snprintf(text, sizeof(text), "%d", row->ranks[k]);
                right_text(at + (k + 1) * col, mid, row->ranks[k] ? text : "-", row->ranks[k] ? 0xF4F4F4 : DIM_RGB, s);
            }
            snprintf(text, sizeof(text), "%d", row->cards);
            right_text(x + pad_ + list_w, mid, row->cards ? text : "-", row->cards ? GOLD_RGB : DIM_RGB, s);
        }
    } else {
        int w_rank = host->text_width(host, "S-POW", s), w_chance = host->text_width(host, "00.00%", s);
        int w_who = view->screen == SCREEN_CARDS ? host->text_width(host, "High Mage Secmeton", s) : 0, name_w;
        card = shown_card(view);
        zoom = fv_card_zoom((w - 2 * pad_) / 3, list_h, s);
        if (zoom && card) {
            fv_card_size(zoom, s, &cw, &ch);
            fv_card_draw(card, x + w - pad_ - cw, top, zoom, s);
        } else {
            cw = 0;
        }
        list_w = w - 2 * pad_ - (cw ? cw + gap : 0);
        if (w_who > list_w / 3) w_who = list_w / 3;
        name_w = list_w - (w_who ? w_who + gap : 0) - w_rank - w_chance - 2 * gap;
        visible = list_h / line;
        if (!row_count)
            host->draw_text(host, x + pad_, top + line / 2, "Ancora nessuna carta nel diario.", DIM_RGB, s);
        for (i = 0; i < visible && view->top + i < row_count; i++) {
            const Entry *e = rows[view->top + i].entry;
            int mid = top + i * line + line / 2, at = x + pad_ + name_w + gap;
            unsigned weight = fv_weight(e->duelist, entry_pool(e), e->card);
            if (view->top + i == view->cursor)
                host->fill(host, x + pad_ / 2, mid - line / 2, list_w + pad_, line, FV_CURSOR_RGB, 230);
            fv_card_name(e->card, name, sizeof(name));
            snprintf(text, sizeof(text), "%s%s", fv_wished(e->card) ? "* " : "", name);
            fv_fit(text, name_w - fv_icon_room(s), s);
            fv_icon_text(e->card, x + pad_, mid, text, fv_rarity_rgb(weight), s);
            if (w_who) {
                fv_duelist_name(e->duelist, name, sizeof(name));
                fv_fit(name, w_who, s);
                host->draw_text(host, at, mid, name, TEXT_RGB, s);
                at += w_who + gap;
            }
            rank_text(e, text, sizeof(text));
            host->draw_text(host, at, mid, text, e->free ? TEXT_RGB : GOLD_RGB, s);
            at += w_rank + gap;
            if (weight) fv_percent(weight, text, sizeof(text));
            else snprintf(text, sizeof(text), "-");
            right_text(at + w_chance, mid, text, fv_rarity_rgb(weight), s);
        }
    }
    snprintf(text, sizeof(text), "%s",
             view->screen == SCREEN_DUELISTS ? "Su/Giu scorri  X le sue carte  Sx/Dx carte prese  O esci"
             : view->screen == SCREEN_CARDS  ? "Su/Giu scorri  Sx/Dx avversari  O esci  (rank in oro = campagna)"
                                             : "Su/Giu scorri  O indietro  Start chiudi  (rank in oro = campagna)");
    fv_fit(text, w - 2 * pad_, s);
    host->draw_text(host, x + pad_, y + h - pad_ - line / 2, text, DIM_RGB, s);
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
    hash = hash * 1031u + (unsigned)view->cursor;
    hash = hash * 1031u + (unsigned)view->top;
    hash = hash * 1031u + (unsigned)row_count;
    hash = hash * 3u + (unsigned)(built.screen == view->screen);
    hash = hash * 131u + diary_rev;
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
}

static void reset(void)
{
    close_all();
    pending.active = 0;
}

int MemoriesModInit(const MemoriesModHost *from, MemoriesMod *mod)
{
    if (from->api < 13 || !from->menu_item) return 0;
    host = from;
    mod->api = 13;
    mod->frame = frame;
    mod->applied = applied;
    mod->reset = reset;
    mod->overlay = draw;
    mod->overlay_signature = signature;
    diary_load();
    return host->subscribe(host, MEMORIES_EVENT_INPUT, 0, input) != 0 &&
           host->subscribe(host, MEMORIES_EVENT_MENU, 0, menu) != 0 &&
           host->subscribe(host, MEMORIES_EVENT_REWARD, 0, reward) != 0 &&
           host->hook(host, (void *)DuelScene_UpdateResultRewards, (void *)results_screen, &original) != 0;
}
