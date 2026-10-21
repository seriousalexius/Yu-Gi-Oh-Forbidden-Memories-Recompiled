/* Turbo: a pad button (L2 unless the player chose another) turns flat out
 * on, and a second press off, so a phone, which has no Tab key, can hurry
 * the long animations along. It is the host's own
 * Turbo (host_actions.c): Platform_SetClockRate, and back to the player's
 * Game > Game speed when it ends. Never while the game is paused (a clock
 * rate of 0), which is left alone.
 *
 * The button is kept from the game while the mod is on, so pressing it does
 * nothing else. A small ">> MAX" in the corner says it is on.
 *
 * In a duel (main mode 3, up to the result screen: scene phase 12 on) the
 * speed also follows the turn by itself: "mine" for the player's turns
 * (400% unless chosen otherwise), "opponent" for the opponent's (flat out
 * unless chosen otherwise), told apart by D_8009B1D5, the acting side (1 the
 * opponent). The button, when on, goes over both. Out of a duel it is
 * Game > Game speed.
 *
 * Flat out is measured, not assumed. The game moves everything by its
 * per-frame step (D_8009B0D8, graphics_frame.h), which counts the VBlanks
 * since the last frame: at 400% a device too slow for 240 frames a second
 * still gets 240 steps a second, as dropped frames move things farther;
 * the uncapped clock (-1) gives one step a frame, so there it is only as
 * fast as the device draws. Which one wins depends on the phone, so flat out
 * runs the one that gave more steps a second, tries the other for half a
 * second every few seconds, and keeps the faster. */
#include "types.h"
#include "pc/mods/modapi.h"
#include "pc/platform/settings.h"
#include <stdio.h>

void Platform_SetClockRate(int percent);
int Platform_ClockRate(void);
int Menu_Height(void);
int TouchPad_FreeSpan(int *left, int *right);

extern unsigned char D_8009B26C;      /* main_mode_state.h: the running mode */
extern unsigned char D_8009B1D5;      /* duel_side_state.h: the acting side, 1 the opponent */
extern unsigned short gDuel_wSceneStateFlags; /* duel_scene_state.h: the phase in the low nibble */
extern int D_8009B0D8;                /* graphics_frame.h: the per-frame step */

enum { MODE_MASK = 0x1F, MODE_DUEL = 3, PHASE_RESULT = 12 };
enum { FLAT_OUT = -1, TOP_FIXED = 400 };
enum { TRY_US = 500000, KEEP_US = 4000000 };
enum { BY_NONE, BY_BUTTON, BY_MINE, BY_OPPONENT };

static const MemoriesModHost *host;
static int level, applied_rate;     /* level: 1 while the button has it on; applied_rate: the rate last applied, 0 none */
static int target, target_by;       /* this frame's wanted rate (flat out resolved) and what asked for it */
static unsigned short was;
static volatile unsigned key_bits;  /* read by the INPUT hook, which may run from VBlank */

/* Flat out's measuring: the rate it runs (FLAT_OUT or TOP_FIXED), whether the
 * other is being tried, and the steps a second each gave last ([1] 400%). */
static int flat_on, flat_best, flat_trying;
static uint64_t flat_since, flat_until;
static unsigned flat_steps, flat_speed[2];

static int in_duel(void)
{
    return (D_8009B26C & MODE_MASK) == MODE_DUEL && (gDuel_wSceneStateFlags & 0xF) < PHASE_RESULT;
}

/* The player's turns: 0 Game > Game speed, else the chosen rate. */
static int mine_rate(void)
{
    static const int rates[] = {0, 200, 300, 400};
    int at = host->setting(host, "mine", 3);
    return at >= 0 && at <= 3 ? rates[at] : 0;
}

/* The opponent's turns: 0 as the player's, 1 flat out (2, the retired fixed
 * speed, kept as 400%). */
static int opponent_rate(void)
{
    int at = host->setting(host, "avversario", 1);
    return at == 1 ? FLAT_OUT : at == 2 ? TOP_FIXED : mine_rate();
}

static int other(int rate) { return rate == FLAT_OUT ? TOP_FIXED : FLAT_OUT; }

/* Flat out: the faster of the uncapped clock and 400%, kept up to date. */
static int flat_out(void)
{
    uint64_t now = host->now_us(host);
    if (!flat_on) {
        flat_on = 1;
        flat_best = FLAT_OUT;
        flat_trying = 0;
        flat_since = now;
        flat_until = now + TRY_US;
        flat_steps = 0;
    }
    flat_steps += D_8009B0D8 > 0 && D_8009B0D8 < 16 ? (unsigned)D_8009B0D8 : 1u;
    if (now >= flat_until) {
        int ran = flat_trying ? other(flat_best) : flat_best;
        flat_speed[ran == TOP_FIXED] = (unsigned)((uint64_t)flat_steps * 1000000u / (now - flat_since + 1));
        if (flat_trying) {
            /* 400% wins a near tie: the same speed for less heat. */
            unsigned uncapped = flat_speed[0], fixed = flat_speed[1] * 105u / 100u;
            int best = fixed >= uncapped ? TOP_FIXED : FLAT_OUT;
            if (host->log_enabled(host))
                host->log(host, "MAX: senza limite %u, 400%% %u passi/s -> %s\n", flat_speed[0], flat_speed[1],
                          best == FLAT_OUT ? "senza limite" : "400%");
            flat_best = best;
        }
        flat_trying = !flat_trying;
        flat_since = now;
        flat_steps = 0;
        flat_until = now + (flat_trying ? TRY_US : KEEP_US);
    }
    return flat_trying ? other(flat_best) : flat_best;
}

/* This frame's rate and what asked for it: the button's, else the turn's in
 * a duel, else none (Game > Game speed). */
static void choose(void)
{
    int rate = 0;
    target_by = BY_NONE;
    if (level) {
        rate = FLAT_OUT;
        target_by = BY_BUTTON;
    } else if (in_duel()) {
        int theirs = D_8009B1D5 == 1;
        rate = theirs ? opponent_rate() : mine_rate();
        target_by = !rate ? BY_NONE : theirs ? BY_OPPONENT : BY_MINE;
    }
    if (rate == FLAT_OUT) rate = flat_out();
    else flat_on = 0;
    target = rate;
}

/* The rate the label names: flat out stays "MAX" while it measures. */
static int shown_rate(void)
{
    return flat_on ? FLAT_OUT : target;
}

static void apply(void)
{
    int rate = Platform_ClockRate(), want = target;
    if (rate == 0) return;
    if (want && rate != want) {
        Platform_SetClockRate(want);
    } else if (!want && applied_rate) {
        Platform_SetClockRate(Settings_Get(SET_SPEED));
    }
    if ((want != 0) != (applied_rate != 0) && host->log_enabled(host))
        host->log(host, "%d%% -> %d%%%s\n", rate, Platform_ClockRate(),
                  target_by == BY_OPPONENT ? " (avversario)" : target_by == BY_MINE ? " (tuo turno)" : "");
    applied_rate = want;
}

static void frame(void)
{
    unsigned key = (unsigned)host->setting(host, "key", 0x0100);
    unsigned short pad = host->pad(host, 0);
    int down = key && (pad & key) == key, held_before = key && (was & key) == key;

    key_bits = key;
    if (down && !held_before) level = !level;
    was = pad;
    choose();
    apply();
}

static void input(MemoriesModEvent *event)
{
    if (event->a == 0) event->b &= ~(int)key_bits;
}

static void applied(int is_on)
{
    if (!is_on) {
        level = target = 0;
        target_by = BY_NONE;
        flat_on = 0;
        apply();
        key_bits = 0;
    }
}

/* The label: the button's level, or the opponent's turn. The player's own
 * turn has none, its speed is the duel's normal one. */
static void draw(void)
{
    int width, height, scale, left = 0, right, w, rate = shown_rate();
    char text[16];

    if (target_by != BY_BUTTON && target_by != BY_OPPONENT) return;
    host->overlay_size(host, &width, &height, &scale);
    if (scale < 1) scale = 1;
    right = width;
    if (!TouchPad_FreeSpan(&left, &right) || right <= left) right = width;
    if (target_by == BY_OPPONENT) snprintf(text, sizeof(text), ">> AVVERSARIO");
    else if (rate < 0) snprintf(text, sizeof(text), ">> MAX");
    else snprintf(text, sizeof(text), ">> %d%%", rate);
    w = host->text_width(host, text, scale) + 8 * scale;
    host->fill(host, right - w - 4 * scale, Menu_Height() + 4 * scale, w, 16 * scale, 0x000000, 150);
    host->draw_text(host, right - w, Menu_Height() + 12 * scale, text, 0xF0D070, scale);
}

static unsigned signature(void)
{
    int left = 0, right = 0, rate = shown_rate();
    TouchPad_FreeSpan(&left, &right);
    if (target_by != BY_BUTTON && target_by != BY_OPPONENT) return 1u;
    return (unsigned)target_by + (unsigned)(rate + 1) * 7u + (unsigned)right * 131u + (unsigned)Menu_Height();
}

int MemoriesModInit(const MemoriesModHost *from, MemoriesMod *mod)
{
    if (from->api < 13) return 0;
    host = from;
    mod->api = 13;
    mod->frame = frame;
    mod->applied = applied;
    mod->overlay = draw;
    mod->overlay_signature = signature;
    return host->subscribe(host, MEMORIES_EVENT_INPUT, 0, input) != 0;
}
