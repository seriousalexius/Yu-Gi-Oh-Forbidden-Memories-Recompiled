/* The retail game-over return sequence, called only between mode runners. */
#include "pc/platform/title_jump.h"
#include "pc/platform/title_screen.h"
#include "pc/platform/platform.h"
#include "types.h"
#include "game/display_object_core.h"
#include "game/fade.h"
#include "game/file_transfer.h"
#include "game/func_80035A64.h"
#include "game/main_modes.h"
#include "game/sound.h"
#include "game/func_80024DC8.h"
#include "pc/debug/cheats.h"
#include "pc/debug/netplay.h"
#include "pc/rng.h"
#include "game/text_sjis_to_glyph_codes.h"
#include <stdio.h>
#include <string.h>

extern u8 D_8009B268, D_8009B26C, D_8009B26D;
extern int D_800E9DC0[];
void Psx_longjmp(int *env, int value);
/* The debug menu's state (frontend_debug_state.h, frontend_debug_tables.h)
 * and the Free Duel return byte (free_duel.h), declared plainly here: this
 * unit is native, so the codegen spellings those headers select do not
 * matter. */
extern u8 D_8009B2EB, gDebugMenu_bPage, gFreeDuel_bReturnFlags, D_8009B368, D_8009B269;
extern s8 gDebugMenu_bCursor;
extern u16 gDuel_awPlayerDeck[];
/* The two-player slots and their names (save_data.h): each slot's state,
 * the SJIS name in it, and the glyph codes the duel draws the name from. */
extern u8 D_801D1200[], D_801D2200[], D_801D160C[], D_801B122B[], D_801B1238[];

/* The debug menu (mode 0) has run its first frame and runs no entry. */
int TitleJump_DebugMenuIdle(void)
{
    return D_8009B26C == (MAIN_MODE_DEBUG | 0xC0) && D_8009B2EB == 0;
}

int TitleJump_InDebugMenu(void)
{
    return (D_8009B26C & 0x9F) == (MAIN_MODE_DEBUG | 0x80); /* Main_Loop runs it (0 alone: before Main_Loop) */
}

/* The debug menu's entry for each target (frontend_debug_tables.h: Detail is
 * the Library, 3D MAP the campaign map); -1 for those set up here instead. */
static int entry_of(int target)
{
    switch (target) {
    case JUMP_FREE_DUEL: return 8;
    case JUMP_BUILD_DECK: return 7;
    case JUMP_LIBRARY: return 3;
    case JUMP_PASSWORD: return 11;
    case JUMP_MAP: return 6;
    case JUMP_OPTIONS: return 16;
    default: return -1;
    }
}

/* From the idle debug menu, between two mode runners. An entry is taken as
 * the menu takes Cross on it: the cursor on it and its step asked for
 * (D_8009B2EB = entry + 1), which the menu's next frame runs. A duel is armed
 * as the Free Duel screen arms one (screen_runtime.c), against `opponent`,
 * with the deck set first through MEMORIES_DEBUG_DECK's path; it comes back
 * to the Free Duel menu. The credits have no entry: their mode is set, as
 * MEMORIES_MODE_AT does. */
int TitleJump_EnterTarget(int target, int opponent, const char *deck)
{
    int entry = entry_of(target);
    if (deck && *deck && !Cheats_SetDeck(deck)) {
        fprintf(stderr, "memories-pc: jump: no card in the deck '%s'\n", deck);
        return -1;
    }
    fprintf(stderr, "memories-pc: jump: from the debug menu to %s\n", TitleJump_TargetName(target));
    if (entry >= 0) {
        gDebugMenu_bPage = 0;
        gDebugMenu_bCursor = (s8)entry;
        D_8009B2EB = (u8)(entry + 1);
        return 0;
    }
    switch (target) {
    case JUMP_DUEL:
        if (!gDuel_awPlayerDeck[0]) {
            fprintf(stderr, "memories-pc: jump: the deck is empty (no save loaded); give one\n");
            return -1;
        }
        gFreeDuel_bReturnFlags = 0x80;
        func_80024DC8(-1, opponent, 0x6000, 0x6000);
        D_8009B368 = MAIN_MODE_FREE_DUEL;
        D_8009B269 = MAIN_MODE_DEBUG;
        return 0;
    case JUMP_DUEL2P:
        /* What the debug menu's 2P duel entry does once the memory card
         * pair has loaded (debug_menu_two_player_entry.c and
         * SaveData_UpdateDuelLoad), with the saves the two games swapped
         * and the seed they share. */
        if (!Netplay_Save(1) || !Netplay_Save(2)) return -1;
        memcpy(D_801D1200, Netplay_Save(1), 0x680);
        memcpy(D_801D2200, Netplay_Save(2), 0x680);
        Text_SjisToGlyphCodes(D_801B122B, D_801D160C, 6);
        Text_SjisToGlyphCodes(D_801B1238, D_801D160C + 0x1000, 6);
        gRand_dwSeed = Netplay_Seed();
        File_RequestMainMenuPackage();
        File_WaitForTransfers();
        D_8009B2EB = 0;
        D_8009B26C = MAIN_MODE_TWO_PLAYER_DUEL_SETUP;
        D_8009B269 = MAIN_MODE_DEBUG;
        return 0;
    case JUMP_CREDITS:
        D_8009B26C = MAIN_MODE_CREDITS;
        D_8009B269 = MAIN_MODE_DEBUG;
        return 0;
    default:
        return 0; /* the debug menu itself */
    }
}

void TitleJump_Execute(void)
{
    /* The credits are the one mode retail never leaves but by a console
     * reset: their presentation takes over what only the boot sets up (the
     * music buffer, the resident VRAM palettes; platform/title_jump.c), so
     * leaving them is a reset here too. */
    if ((D_8009B26C & 0x1F) == MAIN_MODE_CREDITS) {
        fprintf(stderr, "memories-pc: leaving the credits; restarting the game\n");
        Platform_RestartGame();
        fprintf(stderr, "memories-pc: the restart failed; back to the title screen instead\n");
    }
    fprintf(stderr, "memories-pc: back to the title screen from mode %u\n", D_8009B26C & 0x1F);
    File_WaitForTransfers();
    SD_BGMFadeOut();
    Fade_WaitOut();
    DisplayObject_Reset();
    func_80035A64();
    /* Out past MainMenu_DestroyFrontendMenu, if it is the title we leave. */
    TitleScreen_Closed();
    D_8009B268 = 1;
    D_8009B26D = 0;
    D_8009B26C = MAIN_MODE_MENU;
    Psx_longjmp(D_800E9DC0, 1);
}
