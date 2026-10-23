#ifndef MEMORIES_PC_PLATFORM_TITLE_JUMP_H
#define MEMORIES_PC_PLATFORM_TITLE_JUMP_H
/* Back to the title screen from anywhere (title_jump.c). */
void TitleJump_Request(void);
/* Game > Restart game: asks first in a notice (Menu_ShowNotice); Yes makes
 * the same request as Debug > Jump to > Title Screen. */
void TitleJump_Confirm(void);
/* Main_RunFrontendLoop disables requests on every entry to the title. */
void TitleJump_SetActive(int enabled);
/* Consume scripted requests at the actual presented frame, even at the
 * title or inside a fade. Execution still waits for TitleJump_Poll. */
void TitleJump_Frame(unsigned frame);
/* Called by Main_Loop between two mode runners; does not return when a jump
 * was requested. */
void TitleJump_Poll(void);
/* Game-ABI implementation; called with requests disabled at a safe point. */
void TitleJump_Execute(void);

/* Debug > Jump to's other screens, and the control channel's `jump`: one
 * path for both (notes/agent-control.md, step 6). The game leaves what it
 * runs for the title as Title Screen does, the title gives way to the
 * game's own debug menu (mode 0), and the debug menu takes the target's
 * entry as if Cross had chosen it; a duel is armed against `opponent` with
 * `deck` (MEMORIES_DEBUG_DECK's syntax) as the Free Duel screen arms one. */
enum {
    JUMP_TITLE, JUMP_DEBUG_MENU, JUMP_DUEL, JUMP_FREE_DUEL, JUMP_BUILD_DECK, JUMP_LIBRARY, JUMP_PASSWORD,
    JUMP_MAP, JUMP_CREDITS, JUMP_OPTIONS,
    JUMP_DUEL2P, /* an online duel's start (pc/debug/netplay.h): its two saves, then the 2P duel setup */
    JUMP_COUNT
};
const char *TitleJump_TargetName(int target); /* "title", "debug", "duel"... */
int TitleJump_TargetByName(const char *name); /* -1 for none */
/* 0 when taken (it happens at the next point between two screens' frames),
 * -1 with the reason in `why`. */
int TitleJump_RequestTo(int target, int opponent, const char *deck, char *why, unsigned why_size);
/* The title's menu (title_screen.c): a jump waits to go through the debug
 * menu, so the title gives way at once, skipping the opening movie. */
int TitleJump_Pending(void);
/* Jumps taken so far (Title Screen's and the others'), which the control
 * channel's `info` reports, so a client knows its own has landed. */
unsigned TitleJump_Count(void);
void TitleJump_TitleGaveWay(void);
/* Game-ABI side (src/pc/overrides/title_jump.c). */
int TitleJump_InDebugMenu(void);
int TitleJump_DebugMenuIdle(void);
int TitleJump_EnterTarget(int target, int opponent, const char *deck);
#endif
