/* Premi della storia: StarChips for the campaign's big steps, once a save.
 *
 * premi.txt lists them ("duelist|starchips|what the banner says"): the first
 * campaign win against one of them adds its starchips to the duel's own
 * prize. "Once" is the save's own record, so nothing else is kept: a disc
 * duelist beaten in the story gets its Free Duel flag (0x6E0 + id, set by the
 * dialogue after the duel, and never cleared, not even by the ending), so a
 * duelist whose flag is already up has paid. The story's text sets each one
 * right after that duelist's duel, Nitemare's too ({f8 19 id} after
 * {f8 0D id} in languages/*.txt), and the ending, which starts the story over
 * on the same save, clears only the flags below 0x120: a second time through
 * pays nothing. A save that was not written after the win loses the prize
 * with the win itself, as it loses the duel's own starchips.
 *
 * The prize is decided on the results screen's first frame (the hook on
 * DuelScene_UpdateResultRewards, as diario does), shown there as a banner of
 * the overlay, and added in MEMORIES_EVENT_STARCHIP as the game pays the
 * duel's own (Free Duel, the arena and the online duels pay nothing extra:
 * only a duel the campaign started counts).
 *
 * Tests only (not in mod.json): MEMORIES_MOD_PREMI_STORIA_PROVA=1 takes any duel
 * but the arena's for the campaign's (the flag check as in the story); =2 keeps
 * the campaign check and adds Simon Muran, the story's first duel. The log's
 * "premi-storia: ..." lines tell each prize. */
#define D_8009B360_AS_SIDE_ARRAY
#include "types.h"
#include "pc/mods/modapi.h"

static const MemoriesModHost *host;

#include "game/main_modes.h"
#include "game/save_data.h"
#include "game/campaign_flags.h"
#include "game/card_constants.h"
#include "game/ai_opponent_data.h"
#include "game/duel_result_display.h"
#include "game/duel_rewards.h"
#include "game/duel_scene_state.h"
#include "game/duel_side_state.h"
#include "pc/cards/tables.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern unsigned char D_8009B26C; /* main_mode_state.h: the running mode */

enum { MODE_MASK = 0x1F };
#define SCENE_STARTED 0x8000 /* gDuel_wSceneStateFlags: the results screen's first frame ran */
#define PRIZES_MAX 40
#define BANNER_FRAMES 240 /* the banner stays this long once the prize is paid */
#define WAIT_FRAMES 36000 /* results up this long without the payment: the prize is dropped */

static struct {
    int duelist;
    long starchips;
    char what[64];
    int any_result; /* "|fine": paid when the story moves past the duel, won or lost (the prologue's Heishin) */
} prizes[PRIZES_MAX];
static int prize_count;

/* The "|fine" prizes paid, as "<duelist>:<the save's name in hex>" lines in
 * mod-data (pagati.txt): one per name, also for a save that went past the
 * duel before this version (paid the first time it is played). */
#define PAID_FILE "pagati.txt"
#define PAID_MAX 64
static char paid[PAID_MAX][48];
static int paid_count, check_wait;

static void *original;
static int menu_mode; /* the last mode outside a duel */
static int pending;   /* the prize (an index + 1) the results screen showed, not paid yet */
static int shown;     /* the banner's prize (an index + 1) */
static int banner;    /* frames left once paid; -1 while the results screen is up */

static void load_prizes(void)
{
    FILE *file = host->open_asset(host, "premi.txt");
    char line[128];
    prize_count = 0;
    if (!file) {
        host->log(host, "premi-storia: premi.txt non trovato");
        return;
    }
    while (fgets(line, sizeof(line), file) && prize_count < PRIZES_MAX) {
        char *end, *at, *nl;
        long id, n;
        if (line[0] == '#') continue;
        id = strtol(line, &end, 10);
        if (end == line || *end != '|') continue;
        at = end + 1;
        n = strtol(at, &end, 10);
        if (end == at || *end != '|' || id < 1 || n <= 0) continue;
        for (nl = end + 1; *nl && *nl != '\r' && *nl != '\n'; nl++) {}
        *nl = 0;
        prizes[prize_count].duelist = (int)id;
        prizes[prize_count].starchips = n;
        prizes[prize_count].any_result = 0;
        if (nl - (end + 1) >= 5 && !strcmp(nl - 5, "|fine")) {
            nl[-5] = 0;
            prizes[prize_count].any_result = 1;
        }
        snprintf(prizes[prize_count].what, sizeof(prizes[prize_count].what), "%s", end + 1);
        prize_count++;
    }
    fclose(file);
    /* PROVA=2: Simon Muran, the story's first duel, pays too (the campaign's own mode check stays) */
    if (host->setting(host, "prova", 0) == 2 && prize_count < PRIZES_MAX) {
        prizes[prize_count].duelist = 1;
        prizes[prize_count].starchips = 50;
        snprintf(prizes[prize_count].what, sizeof(prizes[prize_count].what), "Prova: hai battuto Simon Muran");
        prize_count++;
    }
    host->log(host, "premi-storia: %d premi in premi.txt", prize_count);
}

/* Paid already, on this save: the duelist's Free Duel flag. */
static int paid_already(int duelist)
{
    return Campaign_TestStoryFlag(FREE_DUEL_UNLOCK_FLAG_BASE + duelist) != 0;
}

static int campaign_duel(void)
{
    if (menu_mode == MAIN_MODE_CAMPAIGN || menu_mode == MAIN_MODE_CAMPAIGN_MAP) return 1;
    return host->setting(host, "prova", 0) == 1 && menu_mode != MAIN_MODE_UNUSED_DEVELOPER;
}

static void results_screen(void)
{
    int started = gDuel_wSceneStateFlags & SCENE_STARTED, i;
    ((void (*)(void))original)();
    if (started || !(gDuel_wSceneStateFlags & SCENE_STARTED)) return;
    pending = 0;
    host->log(host, "premi-storia: risultati, avversario %d, vincitore %d, lato %d, modo %d", gDuel_bOpponentID,
              gDuel_bWinnerSide, D_8009B360[0], menu_mode);
    if (D_8009B360[0] >= 0 || gDuel_bOpponentID < 0 || gDuel_bWinnerSide || !campaign_duel()) return;
    for (i = 0; i < prize_count; i++) {
        if (prizes[i].duelist != gDuel_bOpponentID || prizes[i].any_result) continue;
        if (paid_already(prizes[i].duelist)) {
            host->log(host, "premi-storia: %s, gia' preso su questo salvataggio", prizes[i].what);
            return;
        }
        pending = shown = i + 1;
        banner = -1;
        host->log(host, "premi-storia: %s, +%ld starchip in arrivo", prizes[i].what, prizes[i].starchips);
        return;
    }
}

static void starchips(MemoriesModEvent *event)
{
    if (!pending) return;
    if (event->phase == MEMORIES_BEFORE) {
        if (!event->handled) event->a += (int)prizes[pending - 1].starchips;
        return;
    }
    host->log(host, "premi-storia: pagati %d starchip col duello (premio %ld)", event->result,
              prizes[pending - 1].starchips);
    pending = 0;
    banner = BANNER_FRAMES;
}

static void load_paid(void)
{
    FILE *file = host->open_data(host, PAID_FILE, "r");
    char line[64];
    paid_count = 0;
    if (!file) return;
    while (paid_count < PAID_MAX && fgets(line, sizeof(line), file)) {
        char *end = line;
        while (*end && *end != '\r' && *end != '\n') end++;
        *end = 0;
        if (line[0]) snprintf(paid[paid_count++], sizeof(paid[0]), "%s", line);
    }
    fclose(file);
}

static void paid_key(int duelist, char *out, size_t size)
{
    size_t at = (size_t)snprintf(out, size, "%d:", duelist);
    int i;
    for (i = 0; i < SAVE_DATA_PLAYER_NAME_SIZE && at + 2 < size; i++, at += 2)
        snprintf(out + at, size - at, "%02X", gSaveData_aPlayerNameSjis[i]);
}

/* The "|fine" prizes: in the story (the campaign or its map), a save whose
 * flag says the duel is behind it and whose name has not had it yet. */
static void story_prizes(int mode)
{
    SaveDataState *save = (SaveDataState *)gDuel_awPlayerDeck;
    char key[48];
    unsigned cap, was;
    int i, k, named = 0;
    FILE *file;
    if (++check_wait < 30 || shown) return;
    check_wait = 0;
    if (mode != MAIN_MODE_CAMPAIGN && mode != MAIN_MODE_CAMPAIGN_MAP &&
        !(host->setting(host, "prova", 0) == 3 && mode != MAIN_MODE_DUEL))
        return;
    for (i = 0; i < SAVE_DATA_PLAYER_NAME_SIZE; i++) named |= gSaveData_aPlayerNameSjis[i];
    if (!named) return;
    for (i = 0; i < prize_count; i++) {
        if (!prizes[i].any_result || !paid_already(prizes[i].duelist)) continue;
        paid_key(prizes[i].duelist, key, sizeof(key));
        for (k = 0; k < paid_count && strcmp(paid[k], key); k++) {}
        if (k < paid_count || paid_count >= PAID_MAX) continue;
        cap = Tables_StarchipCap();
        was = save->starchips;
        save->starchips = was + (unsigned)prizes[i].starchips > cap ? cap : was + (unsigned)prizes[i].starchips;
        snprintf(paid[paid_count++], sizeof(paid[0]), "%s", key);
        if ((file = host->open_data(host, PAID_FILE, "a"))) {
            fwrite(key, 1, strlen(key), file);
            fwrite("\n", 1, 1, file);
            fclose(file);
        }
        shown = i + 1;
        banner = BANNER_FRAMES;
        host->log(host, "premi-storia: %s, +%ld starchip (da %u a %u), modo %d", prizes[i].what, prizes[i].starchips,
                  was, (unsigned)save->starchips, mode);
        return;
    }
}

static void frame(void)
{
    /* A campaign duel may run in the campaign's own mode: the mode is only
     * remembered, the prize waits for the payment (or WAIT_FRAMES). */
    int mode = D_8009B26C & MODE_MASK;
    if (mode != MAIN_MODE_DUEL) menu_mode = mode;
    story_prizes(mode);
    if (banner < 0 && --banner < -WAIT_FRAMES) pending = shown = banner = 0;
    if (banner > 0 && --banner == 0) shown = 0;
}

static void draw(void)
{
    char top[96], bottom[96];
    int w, h, s, bw, bh, x, y, line;
    if (!shown) return;
    host->overlay_size(host, &w, &h, &s);
    snprintf(top, sizeof(top), "PREMIO DELLA STORIA: +%ld STARCHIP", prizes[shown - 1].starchips);
    snprintf(bottom, sizeof(bottom), "%s", prizes[shown - 1].what);
    line = 20 * s;
    bw = host->text_width(host, top, s);
    if (host->text_width(host, bottom, s) > bw) bw = host->text_width(host, bottom, s);
    bw += 24 * s;
    bh = 2 * line + 12 * s;
    x = (w - bw) / 2;
    y = 30 * s;
    host->fill(host, x, y, bw, bh, 0x101018, 225);
    host->fill(host, x, y, bw, 2 * s, 0xF0C040, 255);
    host->fill(host, x, y + bh - 2 * s, bw, 2 * s, 0xF0C040, 255);
    host->draw_text(host, (w - host->text_width(host, top, s)) / 2, y + 6 * s + line / 2, top, 0xF0C040, s);
    host->draw_text(host, (w - host->text_width(host, bottom, s)) / 2, y + 6 * s + line + line / 2, bottom,
                    0xF4F4F4, s);
}

static unsigned signature(void) { return (unsigned)shown; }

static void reset(void)
{
    pending = shown = banner = 0;
}

int MemoriesModInit(const MemoriesModHost *from, MemoriesMod *mod)
{
    if (from->api < 13) return 0;
    host = from;
    mod->api = 13;
    mod->frame = frame;
    mod->reset = reset;
    mod->overlay = draw;
    mod->overlay_signature = signature;
    load_prizes();
    load_paid();
    return host->subscribe(host, MEMORIES_EVENT_STARCHIP, 0, starchips) != 0 &&
           host->hook(host, (void *)DuelScene_UpdateResultRewards, (void *)results_screen, &original) != 0;
}
