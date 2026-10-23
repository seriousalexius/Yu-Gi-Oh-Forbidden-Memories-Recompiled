/* Debug > Jump to > Title Screen and Game > Restart game: the game's own way
 * back, taken from anywhere. Restart game is the player's version: it asks
 * first, then makes the same request, like a console's soft reset.
 *
 * Retail already has one: when a campaign loss is over, Main_RunGameOver
 * (src/game/main_mode_runners.c) fades the music and the screen out, asks
 * for the title menu (D_8009B268 = 1, D_8009B26D = 0, mode 8) and longjmps
 * to the point Main_Init set up after the boot sequence
 * (src/game/main_init.c), which resets the frontend runtime, loads the main
 * menu package and runs the title. The debug menu's exit (DebugMenu_Exit)
 * does the same with DisplayObject_Reset and func_80035A64 first.
 *
 * A request is only taken between two mode runners, from the Main_Loop in
 * src/game/main_loop.c: no runner is half way through a step and no
 * nested frame loop (a fade, a disc wait) is on the stack, the state the
 * retail longjmp leaves from. A new game's name entry is a frame loop of its
 * own before Main_Loop (NameEntry_Main), which polls too: nothing there is
 * half done between its frames. The disc is left idle first, so no
 * transfer the old mode asked for lands on the title's package. While the
 * save slot menu (src/pc/saves) is open the request waits for it to close,
 * so it is never left drawn over the title. Progress not saved is lost, as
 * with a reset.
 *
 * From the credits the jump is a real restart (Platform_RestartGame, in
 * src/pc/overrides/title_jump.c): retail never leaves them but by a reset,
 * and their presentation takes over what only the boot before Main_Init's
 * setjmp sets up. Its first step moves the music buffer into its own area
 * (SD_SetMusicTrackBuffer(D_80010034)) and only its last step puts it back,
 * so a jump in between left the sound driver's VAB header in memory the
 * next screens load over: the first Free Duel after it crashed in
 * func_8004ADE8. It also uploads over resident VRAM palettes the boot
 * uploaded once (y 240-248 from x 512, y 248 from x 0, x 480-511 from y 256),
 * and the Build Deck trunk came out green. The game starts again instead,
 * with the settings file and the saves as they are.
 *
 * MEMORIES_TITLE_AT=N[,N...] requests it at presented frames N (checks). */
#include "pc/compat/fs.h"
#include "pc/platform/title_jump.h"
#include "pc/platform/menu.h"
#include "pc/saves/save_menu.h"
#include "pc/guest/state.h"
#include "pc/free_duel/duelists.h"
#include "pc/debug/cheats.h"
#include "pc/tower/tower.h"
#include "pc/debug/netplay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Main_Loop is running: the title's own loop (Main_RunFrontendLoop, before
 * Main_Loop and again after each jump) is where the request would go anyway,
 * so the item is off there and a request made there is not kept for later. */
static int active, requested;

void TitleJump_SetActive(int enabled)
{
    active = !!enabled;
    if (!active) requested = 0;
    Menu_SetItemEnabled(MENU_ITEM_TITLE, active);
    Menu_SetItemEnabled(MENU_ITEM_RESTART, active);
}

/* A tower whose mod turns cheats off: no leaving its duel this way
 * (tower.h, Tower_Guard); `tell` says so in a notice. */
/* Online the two games must go the same way: no jump either takes alone
 * (pc/debug/netplay.h). */
static int online_refuses(int tell)
{
    static const char *const ok[] = {"OK"};
    if (!Netplay_Active()) return 0;
    if (tell) Menu_ShowNotice("Online", "Durante un duello online non si puo'.", ok, 1, 0, NULL);
    return 1;
}

static void request(int tell)
{
    if (active && !online_refuses(tell) && !Tower_Guard(TOWER_GUARD_JUMP, tell, NULL, 0)) requested = 1;
}

void TitleJump_Request(void)
{
    request(1);
}

/* Yes is taken only if the item is still on: the game may have reached the
 * title by itself while the question was up. */
static void restart_chosen(int button, int *quit)
{
    (void)quit;
    if (button == 0) TitleJump_Request();
}

void TitleJump_Confirm(void)
{
    static const char *const buttons[] = {"Yes", "No"};
    if (!active) return;
    if (online_refuses(1)) return;
    if (Tower_Guard(TOWER_GUARD_JUMP, 1, NULL, 0)) return;   /* asked before the question */
    /* No is focused and last, so Enter and Escape both keep playing. */
    Menu_ShowNotice("Restart game", "Restart the game? Unsaved progress is lost.", buttons, 2, 1, restart_chosen);
}

void TitleJump_Frame(unsigned presented)
{
    static const char *at;
    if (!at) at = getenv("MEMORIES_TITLE_AT") ? getenv("MEMORIES_TITLE_AT") : "";
    while (*at) {
        char *end;
        unsigned long frame = strtoul(at, &end, 10);
        if (end == at) {
            at = "";
        } else if (presented >= frame) {
            at = *end == ',' ? end + 1 : end;
            request(0);
        } else {
            break;
        }
    }
}

/* A jump to another screen (title_jump.h): the target, and whether the
 * title has already given way to the debug menu for it. */
static int jump_target = -1, jump_opponent, handed;
static unsigned jump_count; /* jumps taken, for the control channel's `info` */
static char jump_deck[256];
static const char *const target_names[JUMP_COUNT] = {
    "title", "debug", "duel", "free_duel", "build_deck", "library", "password", "map", "credits", "options", "duel2p"};

const char *TitleJump_TargetName(int target)
{
    return target >= 0 && target < JUMP_COUNT ? target_names[target] : "?";
}

int TitleJump_TargetByName(const char *name)
{
    int target;
    for (target = 0; target < JUMP_COUNT; target++) {
        if (!strcmp(name, target_names[target])) return target;
    }
    return -1;
}

int TitleJump_RequestTo(int target, int opponent, const char *deck, char *why, unsigned why_size)
{
    if (target < 0 || target >= JUMP_COUNT) {
        snprintf(why, why_size, "no such screen");
        return -1;
    }
    /* The caller (Debug > Jump to, the control channel) tells the player. */
    if (Tower_Guard(TOWER_GUARD_JUMP, 0, why, why_size)) return -1;
    if (Netplay_Active() && target != JUMP_DUEL2P) {
        snprintf(why, why_size, "not during an online duel");
        return -1;
    }
    if (target == JUMP_TITLE) {
        if (!active) jump_count++; /* already at the title (or not past it yet): nothing to do */
        request(0);
        return 0;
    }
    if (target == JUMP_DUEL && (opponent < 1 || !Duelists_Valid(opponent))) {
        snprintf(why, why_size, "a duel needs an opponent: no duelist %d (1 to %d)", opponent, Duelists_Count() - 1);
        return -1;
    }
    if (deck && *deck && !Cheats_DeckCount(deck)) {
        snprintf(why, why_size, "no card in the deck '%.40s' (ids and ranges, \"723-762\", \"1,2,3\")", deck);
        return -1;
    }
    if (target == JUMP_DUEL2P && !Netplay_Save(2)) {
        snprintf(why, why_size, "a two-player duel is started by an online session only");
        return -1;
    }
    if (target == JUMP_DUEL && !(deck && *deck) && !Cheats_SaveLoaded()) {
        snprintf(why, why_size, "the save has no deck (none loaded): give one");
        return -1;
    }
    jump_target = target;
    jump_opponent = opponent;
    snprintf(jump_deck, sizeof(jump_deck), "%s", deck ? deck : "");
    handed = 0;
    fprintf(stderr, "memories-pc: jump to %s requested\n", TitleJump_TargetName(target));
    return 0;
}

int TitleJump_Pending(void)
{
    return jump_target >= 0 && !handed;
}

void TitleJump_TitleGaveWay(void)
{
    handed = 1;
}

void TitleJump_Poll(void)
{
    if (!active) TitleJump_SetActive(1);
    if (jump_target >= 0) {
        /* Through the debug menu: from it once it is idle; from anywhere
         * else, by way of the title, which gives way to it. */
        if (handed || TitleJump_InDebugMenu()) {
            if (TitleJump_DebugMenuIdle()) {
                int target = jump_target;
                jump_target = -1;
                handed = 0;
                TitleJump_EnterTarget(target, jump_opponent, jump_deck);
                jump_count++;
            }
            return;
        }
        requested = 1;
    }
    if (!requested) return;
    if (SaveMenu_Active()) {
        if (requested == 1) fprintf(stderr, "memories-pc: back to the title screen once the save menu closes\n");
        requested = 2;
        return;
    }
    /* Disc waits and fades present frames and accept menu input. Disable
     * now, so another click during the jump cannot reset the next game. */
    TitleJump_SetActive(0);
    if (jump_target < 0) jump_count++; /* a jump to the title: taken (Execute does not return) */
    TitleJump_Execute();
}

unsigned TitleJump_Count(void)
{
    return jump_count;
}

void TitleJump_State(MemoriesState *state)
{
    MemoriesStateField field = {&active, sizeof(active)};
    if (Memories_StateLoading(state)) {
        active = 0;
        Memories_StateChunk(state, "title-jump", &field, 1);
        /* Requests belong to the UI's current timeline, not the save. */
        requested = 0;
        jump_target = -1;
        handed = 0;
        TitleJump_SetActive(active);
    } else {
        Memories_StateChunk(state, "title-jump", &field, 1);
    }
}
