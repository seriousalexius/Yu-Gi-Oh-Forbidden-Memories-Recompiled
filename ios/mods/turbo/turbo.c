/* Turbo: a pad button (L2 unless the player chose another) runs the game
 * flat out, so a phone, which has no Tab key, can hurry the long animations
 * along. "uso" picks how: a press turns it on and a second press off, or it
 * is on only while the button is held. It is the host's own Turbo
 * (host_actions.c): Platform_SetClockRate, and back to the player's
 * Game > Game speed when it ends. Never while the game is paused (a clock
 * rate of 0), which is left alone.
 *
 * The button is kept from the game while the mod is on, so pressing it does
 * nothing else. A small ">> MAX" in the corner says it is on; in a duel with
 * it off, "L2 = veloce" there says which button it is.
 *
 * In a duel (main mode 3, up to the result screen: scene phase 12 on) the
 * opponent's turns, told apart by D_8009B1D5 (the acting side, 1 the
 * opponent), run flat out by themselves unless "avversario" says Game speed.
 * The player's own turns, and everything out of a duel, are Game > Game
 * speed unless the button is on.
 *
 * Online (the fork's Netplay_Active) the mod stands aside: the game itself
 * runs the duel's animations flat out and the choices at 100%, the same in
 * both games (pc/debug/netplay.c), and the button reaches the game.
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
extern signed char D_8009B360[2];     /* the sides' duelists: negative for a person (two-player duels) */

enum { MODE_MASK = 0x1F, MODE_DUEL = 3, PHASE_RESULT = 12 };
enum { FLAT_OUT = -1, TOP_FIXED = 400 };
enum { TRY_US = 500000, KEEP_US = 4000000 };
enum { BY_NONE, BY_BUTTON, BY_OPPONENT };

static const MemoriesModHost *host;
static int level, applied_rate;     /* level: 1 while the button has it on; applied_rate: the rate last applied, 0 none */
static int target, target_by;       /* this frame's wanted rate (flat out resolved) and what asked for it */
static unsigned short was;
static volatile unsigned key_bits;  /* read by the INPUT hook, which may run from VBlank */
static int (*online_fn)(void);      /* the fork's Netplay_Active, else none */

/* Flat out's measuring: the rate it runs (FLAT_OUT or TOP_FIXED), whether the
 * other is being tried, and the steps a second each gave last ([1] 400%). */
static int flat_on, flat_best, flat_trying;
static uint64_t flat_since, flat_until;
static unsigned flat_steps, flat_speed[2];

static int in_duel(void)
{
    return (D_8009B26C & MODE_MASK) == MODE_DUEL && (gDuel_wSceneStateFlags & 0xF) < PHASE_RESULT;
}

/* The opponent's turns: 0 Game > Game speed, else flat out (2, a retired
 * choice, too). */
static int opponent_rate(void)
{
    return host->setting(host, "avversario", 1) ? FLAT_OUT : 0;
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

/* This frame's rate and what asked for it: the button's, else the
 * opponent's turn in a duel, else none (Game > Game speed). */
static void choose(void)
{
    int rate = 0;
    target_by = BY_NONE;
    if (level) {
        rate = FLAT_OUT;
        target_by = BY_BUTTON;
    } else if (in_duel() && D_8009B1D5 == 1 && D_8009B360[1] >= 0) {
        /* the computer's turn only: in a two-player duel (online) side 2 is a person */
        rate = opponent_rate();
        target_by = rate ? BY_OPPONENT : BY_NONE;
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
                  target_by == BY_OPPONENT ? " (avversario)" : "");
    applied_rate = want;
}

static void frame(void)
{
    unsigned key = (unsigned)host->setting(host, "key", 0x0100);
    unsigned short pad = host->pad(host, 0);
    int down = key && (pad & key) == key, held_before = key && (was & key) == key;

    if (online_fn && online_fn()) {
        level = target = 0;
        target_by = BY_NONE;
        flat_on = 0;
        key_bits = 0;
        was = pad;
        return;
    }
    key_bits = key;
    if (host->setting(host, "uso", 0) == 1) level = down;
    else if (down && !held_before) level = !level;
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

/* In a duel with the turbo off: the button that turns it on, said small
 * (Alessio's rule: a new button shows itself before it is used). */
static int hint_shown(void) { return target_by == BY_NONE && in_duel() && key_bits; }

static const char *key_name(unsigned key)
{
    static const char *const names[16] = {"Select", "L3", "R3", "Start", "Su", "Destra", "Giu", "Sinistra",
                                          "L2", "R2", "L1", "R1", "Triangolo", "Cerchio", "X", "Quadrato"};
    int bit;
    for (bit = 0; bit < 16; bit++)
        if (key & (1u << bit)) return names[bit];
    return "?";
}

/* The label: the button's, or the opponent's turn; Game speed has only the
 * hint, in a duel. */
static void draw(void)
{
    int width, height, scale, left = 0, right, w, rate = shown_rate();
    char text[40];

    if (hint_shown()) {
        host->overlay_size(host, &width, &height, &scale);
        if (scale < 1) scale = 1;
        right = width;
        if (!TouchPad_FreeSpan(&left, &right) || right <= left) right = width;
        snprintf(text, sizeof(text), "%s = veloce", key_name(key_bits));
        scale *= 2; /* a phone's menu text is small */
        w = host->text_width(host, text, scale) + 8 * scale;
        host->fill(host, right - w - 4 * scale, Menu_Height() + 4 * scale, w, 16 * scale, 0x000000, 110);
        host->draw_text(host, right - w, Menu_Height() + 12 * scale, text, 0xB0A070, scale);
        return;
    }
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
    if (hint_shown()) return 3u + (unsigned)right * 131u + (unsigned)Menu_Height() + key_bits * 7u;
    if (target_by != BY_BUTTON && target_by != BY_OPPONENT) return 1u;
    return (unsigned)target_by + (unsigned)(rate + 1) * 7u + (unsigned)right * 131u + (unsigned)Menu_Height();
}

int MemoriesModInit(const MemoriesModHost *from, MemoriesMod *mod)
{
    if (from->api < 13) return 0;
    host = from;
    mod->api = 13;
    online_fn = (int (*)(void))host->symbol(host, "Netplay_Active");
    mod->frame = frame;
    mod->applied = applied;
    mod->overlay = draw;
    mod->overlay_signature = signature;
    return host->subscribe(host, MEMORIES_EVENT_INPUT, 0, input) != 0;
}
