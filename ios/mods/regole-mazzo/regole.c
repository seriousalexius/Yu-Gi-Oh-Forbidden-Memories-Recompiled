/* Regole del mazzo: in Build Deck a strip in the top left corner says at a
 * glance whether the deck being built keeps Alessio's rules, for each way of
 * playing, and how many cards it has ("Campagna OK  1v1 NO (3)  38/40
 * Select: regole"). Select (which the game only uses there to step the sort
 * backwards; Start still steps it forwards) opens the panel: each mode's
 * verdict, what breaks it, its rules, and the cards still missing for 40.
 * Select or Circle closes it; while it is up the game gets no input.
 *
 * The rules are the fork's (src/pc/cards/deck_rules.c, the same the duels and
 * the lobby check), looked up by name: on a game without them the mod shows
 * nothing. The deck is Build Deck's own count of each card
 * (gBuildDeck_aWorkspace[0], live while cards go in and out). */
#include "types.h"
#include "pc/mods/modapi.h"

static const MemoriesModHost *host;

#include "../../shared/fmview.h"
#include "game/build_deck_transition_state.h"

extern unsigned char D_8009B26C; /* main_mode_state.h: the running mode */
extern BuildDeckTransitionState gBuildDeck_aWorkspace[2];
int Menu_NoticeShown(void);

enum { MODE_MASK = 0x1F, MODE_BUILD_DECK = 7 };
enum { RULES_CAMPAIGN, RULES_ONLINE }; /* deck_rules.h */

typedef int (*ProblemsFn)(int mode, const unsigned short *deck, char *out, size_t size);
typedef const char *(*TextFn)(int mode);
static ProblemsFn problems_fn;
static TextFn text_fn;

static int open, in_build;
static volatile int swallow, eat_select; /* the game gets no buttons / no Select */
static FvPad pad;

/* What the strip and the panel show, worked out once a frame. */
static struct {
    int cards, broken[2];
    char lines[2][768];
    unsigned sum;
} now;

static void refresh(void)
{
    unsigned short deck[DECK_SIZE];
    const u8 *copies = gBuildDeck_aWorkspace[0].deck_card_quantities;
    int id, n = 0, i, mode;
    unsigned sum = 0;
    memset(deck, 0, sizeof(deck));
    for (id = 1; id < CARD_TABLE_ID_END && n < DECK_SIZE; id++)
        for (i = 0; i < copies[id] && n < DECK_SIZE; i++) {
            deck[n++] = (unsigned short)id;
            sum = sum * 31u + (unsigned)id;
        }
    now.cards = n;
    now.sum = sum + (unsigned)n;
    for (mode = 0; mode < 2; mode++)
        now.broken[mode] = problems_fn(mode, deck, now.lines[mode], sizeof(now.lines[mode]));
}

static void frame(void)
{
    unsigned short pressed, repeat;
    int build = problems_fn && (D_8009B26C & MODE_MASK) == MODE_BUILD_DECK;
    if (!build) {
        open = in_build = 0;
        swallow = eat_select = 0;
        return;
    }
    in_build = 1;
    eat_select = 1;
    refresh();
    fv_pad_read(&pad, &pressed, &repeat);
    if (Menu_NoticeShown()) return; /* the port's own notice has the buttons */
    if (!open && (pressed & FV_SELECT)) {
        open = 1;
        swallow = 1;
    } else if (open && (pressed & (FV_SELECT | FV_CIRCLE | FV_START))) {
        open = 0; /* swallow stays until the buttons are let go */
    }
    if (!open && swallow && !pad.held) swallow = 0;
}

static void input(MemoriesModEvent *event)
{
    if (event->a != 0) return;
    if (swallow) event->b = 0;
    else if (eat_select) event->b &= ~(int)FV_SELECT;
}

static void applied(int is_on)
{
    if (!is_on) open = in_build = swallow = eat_select = 0;
}

/* --- drawing ----------------------------------------------------------- */

#define LINE(s) (14 * (s))

/* `text` in lines at most `w` wide (words kept whole), each drawn at
 * x, *y (moved down) when `draw`; the lines it took. */
static int wrapped(const char *text, int x, int *y, int w, uint32_t rgb, int s, int draw)
{
    char ascii[1024], line[256];
    const char *at;
    int lines = 0;
    fv_ascii(text, ascii, sizeof(ascii));
    at = ascii;
    while (*at) {
        size_t used = 0, last_space = 0;
        const char *end = strchr(at, '\n');
        size_t length = end ? (size_t)(end - at) : strlen(at);
        while (used < length && used < sizeof(line) - 1) {
            line[used] = at[used];
            line[used + 1] = '\0';
            if (at[used] == ' ') last_space = used;
            if (host->text_width(host, line, s) > w && used) {
                if (last_space) used = last_space;
                break;
            }
            used++;
        }
        line[used] = '\0';
        if (draw) host->draw_text(host, x, *y + LINE(s) / 2, line, rgb, s);
        *y += LINE(s);
        lines++;
        at += used;
        while (*at == ' ') at++;
        if (*at == '\n') at++;
    }
    return lines;
}

/* The rules of `mode` without their title line. */
static const char *rules(int mode)
{
    const char *text = text_fn ? text_fn(mode) : "", *nl = strchr(text, '\n');
    return nl ? nl + 1 : text;
}

static int panel(int x, int y, int w, int s, int draw)
{
    static const char *const titles[2] = {"CAMPAGNA E FREE DUEL", "1V1 ONLINE"};
    char text[160];
    int mode, top = y;
    snprintf(text, sizeof(text), "REGOLE DEL MAZZO  -  %d/%d carte", now.cards, DECK_SIZE);
    if (draw) host->draw_text(host, x, y + LINE(s) / 2, text, 0xF0D070, s);
    y += LINE(s);
    if (now.cards < DECK_SIZE) {
        snprintf(text, sizeof(text), "Mancano %d carte per arrivare a %d.", DECK_SIZE - now.cards, DECK_SIZE);
        wrapped(text, x, &y, w, 0xF08080, s, draw);
    }
    for (mode = 0; mode < 2; mode++) {
        int bad = now.broken[mode] > 0 || (mode == RULES_ONLINE && now.broken[mode] < 0);
        y += LINE(s) / 2;
        if (draw) host->fill(host, x, y, w, s, 0xC8A040, 200);
        y += LINE(s) / 2;
        snprintf(text, sizeof(text), "%s: %s", titles[mode], bad ? "DA SISTEMARE" : "OK");
        wrapped(text, x, &y, w, bad ? 0xF08080 : 0x80E080, s, draw);
        if (bad) wrapped(now.lines[mode], x, &y, w, 0xFFFFFF, s, draw);
        wrapped(rules(mode), x, &y, w, 0xA0A8C0, s, draw);
    }
    y += LINE(s) / 2;
    wrapped("Per l'1v1 puoi portare anche uno slot di Decks: Online > Mazzo 1v1.", x, &y, w, 0xA0A8C0, s, draw);
    y += LINE(s) / 2;
    wrapped("Select o O chiudi", x, &y, w, 0xF0D070, s, draw);
    return y - top;
}

static void draw_panel(void)
{
    FvArea area;
    int s, w, h;
    fv_area(&area, 260);
    /* the biggest text that fits, up to three times the menus' */
    for (s = 3 * area.s; s > 1; s--) {
        w = area.w - 16 * s;
        if (w >= 260 * s && panel(0, 0, w, s, 0) + 16 * s <= area.h) break;
    }
    if (s < 1) s = 1;
    w = area.w - 16 * s;
    h = panel(0, 0, w, s, 0) + 16 * s;
    if (h > area.h) h = area.h;
    fv_box(area.x, area.y, area.w, h, s);
    panel(area.x + 8 * s, area.y + 8 * s, w, s, 1);
}

static void draw_strip(void)
{
    FvArea area;
    char text[160];
    int s, w, x, y;
    fv_area(&area, 200);
    snprintf(text, sizeof(text), "Campagna OK   1v1 NO (%d)   %d/%d   Select: regole", now.broken[1], now.cards,
             DECK_SIZE);
    /* twice the menus' text when it fits the width: the phone's is small */
    for (s = 2 * area.s; s > area.s && host->text_width(host, text, s) + 8 * s > area.w; s--) {}
    w = host->text_width(host, text, s) + 8 * s;
    x = area.x - 2 * s;
    y = area.y - 2 * s;
    host->fill(host, x, y, w, 14 * s, 0x000000, 170);
    /* each verdict in its colour: drawn piece by piece */
    {
        char piece[64];
        int at = x + 4 * s, mid = y + 7 * s;
        host->draw_text(host, at, mid, "Campagna ", 0xFFFFFF, s);
        at += host->text_width(host, "Campagna ", s);
        snprintf(piece, sizeof(piece), "%s", now.broken[0] ? "NO" : "OK");
        host->draw_text(host, at, mid, piece, now.broken[0] ? 0xF08080 : 0x80E080, s);
        at += host->text_width(host, piece, s);
        host->draw_text(host, at, mid, "   1v1 ", 0xFFFFFF, s);
        at += host->text_width(host, "   1v1 ", s);
        if (now.broken[1] > 0) snprintf(piece, sizeof(piece), "NO (%d)", now.broken[1]);
        else snprintf(piece, sizeof(piece), "%s", now.broken[1] ? "NO" : "OK");
        host->draw_text(host, at, mid, piece, now.broken[1] ? 0xF08080 : 0x80E080, s);
        at += host->text_width(host, piece, s);
        snprintf(piece, sizeof(piece), "   %d/%d   ", now.cards, DECK_SIZE);
        host->draw_text(host, at, mid, piece, now.cards < DECK_SIZE ? 0xF08080 : 0xFFFFFF, s);
        at += host->text_width(host, piece, s);
        host->draw_text(host, at, mid, "Select: regole", 0xF0D070, s);
    }
}

static void draw(void)
{
    if (!in_build || Menu_NoticeShown()) return;
    if (open) draw_panel();
    else draw_strip();
}

static unsigned signature(void)
{
    int left = 0, right = 0, width = 0, height = 0, scale = 0;
    if (!in_build) return 1u;
    TouchPad_FreeSpan(&left, &right);
    host->overlay_size(host, &width, &height, &scale);
    return now.sum * 7u + (unsigned)open * 3u + (unsigned)Menu_NoticeShown() * 5u + (unsigned)right * 131u +
           (unsigned)width * 17u + (unsigned)height * 13u + (unsigned)scale + 2u;
}

int MemoriesModInit(const MemoriesModHost *from, MemoriesMod *mod)
{
    if (from->api < 13) return 0;
    host = from;
    mod->api = 13;
    problems_fn = (ProblemsFn)host->symbol(host, "DeckRules_Problems");
    text_fn = (TextFn)host->symbol(host, "DeckRules_Text");
    if (!problems_fn) host->log(host, "regole-mazzo: questo gioco non ha le regole del mazzo (serve il fork)\n");
    mod->frame = frame;
    mod->applied = applied;
    mod->overlay = draw;
    mod->overlay_signature = signature;
    return host->subscribe(host, MEMORIES_EVENT_INPUT, 0, input) != 0;
}
