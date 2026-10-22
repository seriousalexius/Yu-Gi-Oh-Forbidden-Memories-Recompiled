/* The Game list's actions do what their keys did (host_actions.c). */
#include "pc/platform/host_actions.h"
#include "pc/platform/controls_runtime.h"
#include "pc/platform/menu.h"
#include "pc/platform/platform.h"
#include "pc/platform/settings.h"
#include "pc/audio/spu.h"
#include "pc/debug/log.h"
#include "pc/guest/state.h"
#include "pc/saves/deck_menu.h"
#include <assert.h>
#include <stdio.h>

static uint32_t presses, held;
static int shift, settings[SET_COUNT], saved, applied, rate = 100, stepped, muted, slot = 1, state_what, state_slot,
           deck, shot = -1;
uint32_t ControlsRuntime_TakeHost(void)
{
    uint32_t out = presses;
    presses = 0;
    return out;
}
uint32_t ControlsRuntime_HostHeld(void) { return held; }
int ControlsRuntime_KeyDown(int key) { return shift && key == CTRL_KEY_RIGHT_SHIFT; }
int Settings_Get(SettingId id) { return settings[id]; }
int Netplay_AskResign(void) { return 0; } /* no online duel here */
void Settings_Set(SettingId id, int value) { settings[id] = value; }
int Settings_Save(void) { return ++saved; }
int Settings_Min(SettingId id) { return id == SET_MASTER_VOLUME ? 0 : 0; }
int Settings_Max(SettingId id) { return id == SET_MASTER_VOLUME ? 100 : 1; }
int Platform_HasWindowModes(void) { return 1; }
void Platform_ApplyDisplaySettings(void) { applied++; }
void Platform_Screenshot(int window_image) { shot = window_image; }
int Platform_ClockRate(void) { return rate; }
void Platform_SetClockRate(int percent) { rate = percent; }
void Platform_StepFrame(void) { stepped++; }
int Platform_StateSlot(void) { return slot; }
void Platform_SetStateSlot(int value) { slot = value; }
int Spu_Muted(void) { return muted; }
void Spu_SetMuted(int value) { muted = value; }
void Memories_StateRequest(int what, int value)
{
    state_what = what;
    state_slot = value;
}
void DeckMenu_Request(void) { deck++; }
static int notices;
static void (*notice_answer)(int, int *);
void Menu_ShowNotice(const char *title, const char *text, const char *const *buttons, int count, int focus,
                     void (*answer)(int button, int *quit))
{
    (void)title, (void)text, (void)buttons, (void)count, (void)focus;
    notices++;
    notice_answer = answer;
}
void Menu_Open(void) {} /* quit_prompt.c's Back variant; Back is not a host action */
int Log_Wanted(LogChannel channel)
{
    (void)channel;
    return 0;
}
void Log_Printf(LogChannel channel, const char *format, ...)
{
    (void)channel;
    (void)format;
}

static int run(int action)
{
    int quit = 0;
    presses = 1u << action;
    return HostActions_Run(&quit);
}

int main(void)
{
    int quit = 0;
    settings[SET_SPEED] = 100;
    settings[SET_MASTER_VOLUME] = 50;
    assert(!HostActions_Run(&quit)); /* nothing pressed, nothing to repaint */
    presses = 1u << CTRL_HOST_EXIT;
    assert(HostActions_Run(&quit) && quit); /* as File > Exit */
    /* With Confirm before quitting on, Exit asks; only Quit (button 0) quits. */
    quit = 0;
    settings[SET_CONFIRM_QUIT] = 1;
    presses = 1u << CTRL_HOST_EXIT;
    assert(HostActions_Run(&quit) && !quit && notices == 1);
    notice_answer(1, &quit);
    assert(!quit);
    notice_answer(0, &quit);
    assert(quit);
    settings[SET_CONFIRM_QUIT] = 0;
    run(CTRL_HOST_SLOT_3);
    run(CTRL_HOST_SAVE_STATE);
    assert(slot == 3 && state_what == 1 && state_slot == 3);
    run(CTRL_HOST_LOAD_STATE);
    assert(state_what == 2);
    run(CTRL_HOST_FULLSCREEN);
    assert(settings[SET_FULLSCREEN] && applied == 1);
    settings[SET_BORDERLESS] = 1;
    run(CTRL_HOST_FULLSCREEN);
    assert(!settings[SET_FULLSCREEN] && !settings[SET_BORDERLESS]);
    run(CTRL_HOST_SCREENSHOT);
    assert(shot == 0);
    shift = 1;
    run(CTRL_HOST_SCREENSHOT);
    assert(shot == 1); /* with Shift, the whole window */
    run(CTRL_HOST_MUTE);
    assert(muted);
    run(CTRL_HOST_VOLUME_UP);
    run(CTRL_HOST_VOLUME_UP);
    run(CTRL_HOST_VOLUME_DOWN);
    assert(settings[SET_MASTER_VOLUME] == 55);
    settings[SET_MASTER_VOLUME] = 98;
    run(CTRL_HOST_VOLUME_UP);
    assert(settings[SET_MASTER_VOLUME] == 100);
    /* Frame step only while paused; pause toggles back to the set speed. */
    run(CTRL_HOST_FRAME_STEP);
    assert(!stepped);
    run(CTRL_HOST_PAUSE);
    run(CTRL_HOST_FRAME_STEP);
    assert(rate == 0 && stepped == 1);
    run(CTRL_HOST_PAUSE);
    assert(rate == 100);
    run(CTRL_HOST_HUD);
    run(CTRL_HOST_HUD);
    run(CTRL_HOST_HUD);
    assert(settings[SET_SHOW_HUD] == 0);
    run(CTRL_HOST_DECK_SLOTS);
    assert(deck == 1);
    /* Hold actions follow the hold, not presses. */
    held = 1u << CTRL_HOST_TURBO;
    HostActions_Run(&quit);
    assert(rate == 400);
    run(CTRL_HOST_TURBO); /* a press of a hold action does nothing more */
    assert(rate == 400);
    held = 0;
    HostActions_Run(&quit);
    assert(rate == 100);
    /* Letting go of Turbo while paused (the window lost focus) keeps the pause. */
    held = 1u << CTRL_HOST_TURBO;
    HostActions_Run(&quit);
    rate = 0;
    held = 0;
    HostActions_Run(&quit);
    assert(rate == 0);
    rate = 100;
    puts("host actions: every action does what its key did, holds follow the hold passed");
    return 0;
}
