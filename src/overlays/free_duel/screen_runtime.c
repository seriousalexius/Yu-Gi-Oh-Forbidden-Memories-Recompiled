#define GINPUT_PAD1_PRESSED_IS_VOLATILE
#define GINPUT_PAD1_HELD_IS_VOLATILE
#define MAIN_MODE_STATE_NEXT_AS_SCALAR
#define MAIN_MODE_STATE_ACTIVE_AS_SCALAR
#include "../../types.h"
#include "../../ygo_types.h"
#include "../../unmatched.h"
#include "../../game/card_constants.h"
#include "../../game/duel_effect_mode_7.h"
#include "../../game/campaign_flags.h"
#include "../../game/display_object.h"
#include "../../game/input.h"
#include "../../game/color_constants.h"
#include "../../game/text_box_lifecycle.h"
#include "../../game/text_box_runtime.h"
#include "../../game/text_staging.h"
#include "../../game/duel_effect.h"
#include "../../game/duel_side_state.h"
#include "../../game/graphics_frame.h"
#include "../../game/text_constants.h"
#include "../../psyq/rand.h"
#include "../../game/sound.h"
#include "../../game/save_data.h"
#include "../../game/display_object_helpers.h"
#ifdef MEMORIES_PC
#include "pc/free_duel/duelists.h"
#include "pc/free_duel/page_box.h"
#include "pc/cards/deck_rules.h"
#include "pc/cards/tables.h"
#include "pc/text/language.h"

/* Which page of forty the grid shows, and the duelist a cell stands for on it.
 * The grid holds forty at a time whatever the roster is: page 0 is the disc's
 * own -- Deck Build and the thirty-nine opponents -- and a page past it shows
 * the duelists a mod added (notes/more-duelists.md). Every index the screen
 * takes from the cursor is a cell; everything that means a duelist goes
 * through here, and the arithmetic is the list's so that the port's own
 * screens reach the same answer (Duelists_AtCell). */
extern int gFreeDuel_nPage;

static int cell_duelist(int cell)
{
    return Duelists_AtCell(cell);
}

/* How many pages the roster fills, always at least the disc's own. */
static int page_count(void)
{
    return (Duelists_Count() + FREE_DUEL_GRID_ENTRY_COUNT - 1) / FREE_DUEL_GRID_ENTRY_COUNT;
}

extern void *gFreeDuel_apCells[FREE_DUEL_GRID_ENTRY_COUNT];
extern void *gFreeDuel_pPageBox;
extern void *gFreeDuel_apPageArrows[2];
extern void *gFreeDuel_pPortraits;

#endif
#define DISPLAY_OBJECT_RELEASE_IF_PRESENT_AMBIENT_OBJECT
#include "../../game/display_object_core.h"
#include "../../game/func_80039794.h"
#define DISPLAY_OBJECT_UPDATE_COMMAND_STREAM_AMBIENT_ARGS
#include "../../game/display_object_update_command_stream.h"
#include "../../game/func_80024DC8.h"
#include "../../game/display_object_config.h"
#include "../../psyq/libgte.h"
#include "../../psyq/libgpu.h"
#include "../../psyq/libgs.h"
#include "free_duel.h"
#include "../../game/main_mode_state.h"

/* The Free Duel opponent-select screen in executable order: cursor layout,
   display-object and portrait initialization, sparkle upkeep, input and
   cursor movement, and the overlay entry tick.

   The nine functions are the module's complete text section. They share one
   gcc_2_8_1_g0_split profile and the cursor widgets, target/committed grid
   positions, screen flags and sparkle pool; there is no data or rodata
   boundary between them.

   One unit settles the sparkle path's types from allocation through release:
   FreeDuel_SpawnSparkle returns DisplayObject *, the pool stores those
   pointers, and FreeDuel_GetSparkleSlot returns DisplayObject **. The cursor
   and thumb use the same record; signed coordinate reads are explicit.

   DisplayObject_ReleaseIfPresent uses display_object_core.h's guarded `void (void)` arm. The
   sparkle updater's call passes no argument, so taking the normal
   `void DisplayObject_ReleaseIfPresent(void *)` declaration would make the compiler set up an
   argument retail does not. */

void FreeDuel_UpdateScrollbar(void)
{
    DisplayObject *cursor = gFreeDuel_pCursorWidget;
    s32 relative = (s16)cursor->field_30.h.field_32 - gGraphics_sViewportY;

    if (relative < 0x28) {
        gGraphics_sViewportY = (s16)cursor->field_30.h.field_32 - 0x28;
    }
    if (relative >= 0x91) {
        gGraphics_sViewportY = (s16)cursor->field_30.h.field_32 - 0x90;
    }
    gFreeDuel_pThumbWidget->field_30.h.field_32 =
        ((s16)cursor->field_30.h.field_32 - 0x28) * 72 / 364 + 7;
}

void FreeDuel_PlaceCursor(DisplayObject *w, s32 arm)
{
    s32 col;
    s32 index;
    s32 param;
    s16 trunc;
    DuelEffectChannel *panel;
    SaveDataWorkspace *base;
    u16 *slot;

    col = gFreeDuel_bCursorColumn;
    panel = D_800EB0F8;
    w->field_30.h.field_30 = col * 56 + 20;
    w->field_30.h.field_32 = gFreeDuel_bCursorRow * 52 + 40;
    TextBox_Destroy(panel);
    if (arm == 0) {
        return;
    }
    index = gFreeDuel_bTargetColumn +
            gFreeDuel_bCursorRow * FREE_DUEL_GRID_COLUMN_COUNT;
#ifdef MEMORIES_PC
    if (!Duelists_Available(cell_duelist(index))) {
        return;
    }
#else
    if (gFreeDuel_abGridAvailable[index] == 0) {
        return;
    }
#endif
    slot = &D_8009B32E;
#ifdef MEMORIES_PC
    /* The string that names the duelist, which is the duelist's and not the
     * cell's: on a later page they are not the same. An added duelist is named
     * through a range of its own, since 0x8328 + its id is a location name
     * (duelists.h). */
    trunc = (s16)Duelists_NameTextId(cell_duelist(index));
#else
    trunc = index - 31960;
#endif
    *slot = trunc;
    param = trunc;
    /* Deck Build is duelist 0, and only it gets the bare name; everyone else
     * gets the template with the win and loss counts. On a later page cell 0
     * is a duelist, so the test is on the duelist. */
#ifdef MEMORIES_PC
    if (cell_duelist(index) != 0) {
#else
    if (index != 0) {
#endif
        param = 12;
        base = (SaveDataWorkspace *)D_801D0000;
#ifdef MEMORIES_PC
        /* A duelist a mod added keeps its record beside the save rather than
         * in the block; the slot is the same either way (duelists.h). */
        {
            const u16 *record = Duelists_RecordSlot(&base->state, cell_duelist(index));
            D_801D5608[0].pair.lo = (s16)record[0];
            D_801D5608[0].pair.hi = (s16)record[1];
        }
#else
        D_801D5608[0].pair.lo =
            (s16)base->state.duelist_records[index].result.wins;
        D_801D5608[0].pair.hi =
            (s16)base->state.duelist_records[index].result.losses;
#endif
    }
#ifdef MEMORIES_PC
    /* Game > Language's European text has the record on a line of its own
     * under the name (string 12, notes/translation.md "Frames"): no language
     * fits its words and the longest names on the US line. The PAL's box is
     * as high as two of its lines, 32, from the same corner; the US one's
     * height stays for English (US) and its one line. */
    TextBox_Create(0, param, 16, 204, 288, Language_Current() != LANGUAGE_US ? 32 : 16);
#else
    TextBox_Create(0, param, 16, 204, 288, 16);
#endif
    func_80039A60((struct DuelEffectChannel *)panel);
}

DisplayObject *FreeDuel_SpawnSparkle(void)
{
    DisplayObject *x;

    x = DisplayObject_AcquireSlot(DisplayObject_FindFreeGeneralSlot(), 2);
    DisplayObject_ConfigureSpriteAtPositionWithResource(x, 0, 0, 0, 0, 3, 0x11, 3, D_801AF000);
    ((u8 *)&x->field_5E)[1] = 0x80;
    x->field_48.word = 0x180018;
    DisplayObject_SetDepthOffset(x, 5);
    x->flags |= 0x20;
    return x;
}

#ifdef MEMORIES_PC
/* Show a page in the forty cells.
 *
 * A cell's texture slot and palette are fixed by the cell, so the sprites
 * built once in FreeDuel_Init stand for whatever is put in those slots: only
 * the texels change, and a cell the page does not fill is simply not drawn.
 * That is what makes a page cost nothing in video memory over the grid the
 * disc already had, which is the whole reason for paging.
 *
 * The portraits come from the forty records the screen was opened with, taken
 * by base: a duelist a mod added has its base's face until it is given one.
 */
static void FreeDuel_ShowPage(int page)
{
    RECT slot;
    int cell;

    if (page < 0) page = 0;
    if (page >= page_count()) page = page_count() - 1;
    gFreeDuel_nPage = page;

    /* Which page this is, in the game's own letters on the title's row: the
     * one band of the screen the scrolling grid never reaches. */
    FreeDuelPage_Compose(page, page_count());
    if (gFreeDuel_pPageBox) {
        TextBox_Destroy((struct DuelEffectChannel *)gFreeDuel_pPageBox);
        gFreeDuel_pPageBox = 0;
    }
    for (cell = 0; cell < 2; cell++) {
        if (gFreeDuel_apPageArrows[cell]) {
            DisplayObject_ReleaseIfPresent(gFreeDuel_apPageArrows[cell]);
            gFreeDuel_apPageArrows[cell] = 0;
        }
    }
    if (page_count() > 1) {
        /* Channel 2: the screen's own name box is 0 and its "no deck" message
         * is 1, and there are four. Channel 3 is Build Deck's and the
         * Library's, and a box left there by either would be what this screen
         * found, so the whole line goes in one box here. Creating a box only
         * arms it -- it draws once it has been pumped, which is what the name
         * box does too.
         *
         * Along the top of the picture, above the FREE DUEL artwork, which
         * begins around 27 down. The string places its three runs itself
         * (page_box.h). */
        gFreeDuel_pPageBox = TextBox_Create(2, FREE_DUEL_PAGE_TEXT_ID, FREE_DUEL_PAGE_BOX_X, 0x01,
                                            FREE_DUEL_PAGE_BOX_WIDTH, 0x10);
        if (gFreeDuel_pPageBox) {
            func_80039A60((struct DuelEffectChannel *)gFreeDuel_pPageBox);
        }
        /* The red arrow either side, which is the game's own: the card viewer
         * puts the same one at the foot of its page, and the hand's card
         * cycling puts the pair around a card. The last operand before the
         * color is which way it faces. */
        for (cell = 0; cell < 2; cell++) {
            DisplayObject *arrow =
                DisplayObject_AcquireSlot(DisplayObject_FindFreeGeneralSlot(), 2);
            if (!arrow) break;
            /* The pair duel_scene_hand_actions.c puts either side of a card:
             * it places this one on the left and the other on the right, so
             * they are a matched set and face the way their side wants. The
             * card viewer's arrow is red but has no left-facing sibling in
             * the sheet -- the two ends would not match. */
            DisplayObject_ConfigureSpriteAtPosition(arrow, cell ? 0x120 : 0x1E, 0x01,
                                                    3, 1, cell ? 0 : 2, 0xB, 0x20C);
            DisplayObject_SelectOrderingTable1(arrow);
            DisplayObject_SetDepthOffset(arrow, 0xA);
            arrow->flags |= 0x28;
            gFreeDuel_apPageArrows[cell] = arrow;
        }
    }

    for (cell = 0; cell < FREE_DUEL_GRID_ENTRY_COUNT; cell++) {
        DisplayObject *obj = (DisplayObject *)gFreeDuel_apCells[cell];
        const int duelist = cell_duelist(cell);
        const int shown = Duelists_Valid(duelist) && Duelists_Available(duelist);
        const u8 *record;

        if (obj) {
            if (shown) obj->flags |= DISPLAY_OBJECT_FLAG_RENDERABLE;
            else obj->flags &= ~DISPLAY_OBJECT_FLAG_RENDERABLE;
        }
        if (!shown || !gFreeDuel_pPortraits) continue;

        /* Its own face when it has one, its base's otherwise. A record a mod
         * made is not the disc's bytes, so the texture pack knows it by those
         * bytes alone (Duelists_Portrait) and never puts the pack's picture of
         * whoever it copies over it. */
        record = Duelists_Portrait(duelist);
        if (!record) {
            record = (const u8 *)gFreeDuel_pPortraits +
                     Duelists_BaseId(duelist) * FREE_DUEL_PORTRAIT_RECORD_SIZE;
        }
        /* The image, then its palette, into the slot this cell draws from --
         * the same places FreeDuel_Init put the disc's forty. */
        slot.x = (s16)((cell < 25 ? 128 : 256) +
                       (cell % FREE_DUEL_GRID_COLUMN_COUNT) * 24);
        slot.y = (s16)(256 + ((cell < 25 ? cell : cell - 25) /
                              FREE_DUEL_GRID_COLUMN_COUNT) * 48);
        slot.w = 24;
        slot.h = 48;
        LoadImage2(&slot, (u32 *)record);
        slot.x = (s16)((cell / 16) * 64 + 128);
        slot.y = (s16)((cell & 15) + 496);
        slot.w = 64;
        slot.h = 1;
        LoadImage2(&slot, (u32 *)(record + FREE_DUEL_PORTRAIT_IMAGE_SIZE));
    }
    DrawSync(0);
}
#endif

void FreeDuel_Init(u8 *src)
{
    s32 i;
    s32 one;
    s32 k;
    s32 row;
    s32 col;
    s32 count;
    u16 *rec;
    DisplayObject *TRANSLATED_G32 *slot;
    u8 *cell;
    DisplayObject *obj;
    RECT *clut;

#ifdef MEMORIES_PC
    /* Kept before anything else: the upload loops below walk `src` along the
     * block as they go, so the start of it has to be taken now for a page
     * turn to find the records again. */
    gFreeDuel_pPortraits = src;
    /* The screen's display objects do not outlive it -- the grid's forty are
     * acquired again below, every visit -- so the page line's from the last
     * one are gone, and the pointers kept to them name whatever holds those
     * slots now. FreeDuel_ShowPage releases what it is about to remake, which
     * would take somebody else's object with it: the "select opponent" box's
     * backdrop is the one that shows. They are forgotten here instead, before
     * anything of this visit is acquired. */
    {
        DuelEffectChannel *box = &D_800EB0F8[2];
        box->field_28 = 0;
        box->field_2C = 0;
        box->field_30 = 0;
        gFreeDuel_pPageBox = 0;
        gFreeDuel_apPageArrows[0] = 0;
        gFreeDuel_apPageArrows[1] = 0;
    }
#endif
    if (gFreeDuel_bReturnFlags & 0x80) {
#ifdef MEMORIES_PC
        {
            /* The disc's forty through the game's own table, the save block's
             * records at 0x51C (and a name code mods bind to); an added
             * duelist's beside the save (duelists.h). */
            const int duelist = cell_duelist(gFreeDuel_bCursorRow * FREE_DUEL_GRID_COLUMN_COUNT +
                                             gFreeDuel_bCursorColumn);
            rec = duelist < FREE_DUEL_GRID_ENTRY_COUNT
                      ? gFreeDuel_aDuelistRecords[duelist].counts
                      : Duelists_RecordSlot(&((SaveDataWorkspace *)D_801D0000)->state, duelist);
        }
#else
        rec = gFreeDuel_aDuelistRecords[
            gFreeDuel_bCursorRow * FREE_DUEL_GRID_COLUMN_COUNT +
            gFreeDuel_bCursorColumn].counts;
#endif
        if (D_8009B362 == 1) {
            rec++;
        }
#ifdef MEMORIES_PC
        /* 999, or a mod's "limits" (tables.h); a cap of 32767 must not
           wrap the halfword negative. */
        if ((s16)*rec < Tables_FreeDuelRecordCap()) {
            *rec = *rec + 1;
        } else {
            *rec = Tables_FreeDuelRecordCap();
        }
#else
        *rec = *rec + 1;
        if ((s16)*rec >= FREE_DUEL_RECORD_MAX + 1) {
            *rec = FREE_DUEL_RECORD_MAX;
        }
#endif
    }
    gGraphics_sViewportY = 0;
    gGraphics_sViewportX = 0;
    gFreeDuel_bScreenFlags = 0;
    if (gFreeDuel_bReturnFlags == 0) {
        gFreeDuel_bTargetRow = 0;
        gFreeDuel_bTargetColumn = 0;
        gFreeDuel_bCursorRow = 0;
        gFreeDuel_bCursorColumn = 0;
        TextBox_CreateFlagged(1, 13, 48, 108, 224, 16, 4136);
        func_80039794();
        gFreeDuel_bScreenFlags |= 0x20;
    }
    i = FREE_DUEL_SPARKLE_POOL_CAPACITY - 1;
    slot = gFreeDuel_apSparklePool + i;
    do {
        *slot = 0;
        i--;
        slot--;
    } while (i >= 0);
    one = 1;
    i = FREE_DUEL_GRID_ENTRY_COUNT - 1;
    cell = gFreeDuel_abGridAvailable + i;
    do {
        *cell = one;
        i--;
        cell--;
    } while (i >= 0);
    for (i = FREE_DUEL_STORY_OPPONENT_FIRST_INDEX;
         i < FREE_DUEL_STORY_OPPONENT_INDEX_END; i++) {
        if (Campaign_TestStoryFlag(FREE_DUEL_UNLOCK_FLAG_BASE + i) == 0) {
            gFreeDuel_abGridAvailable[i] = 0;
        }
    }
#ifdef MEMORIES_PC
    /* A duelist a mod added has no campaign flag to unlock it: it appears when
     * the running save meets the conditions its mod wrote, and from the start
     * when it wrote none. A stock duelist a mod gave conditions to answers to
     * those in place of its flag, which is why this comes after the flags and
     * not before them (duelists.h). Read afresh every time the screen opens,
     * so a win in the duel just left opens up what it was the condition for. */
    for (i = 1; i < Duelists_Count(); i++) {
        if (i < FREE_DUEL_GRID_ENTRY_COUNT && !Duelists_HasUnlock(i)) continue;
        Duelists_SetAvailable(i, Duelists_Unlocked(&((SaveDataWorkspace *)D_801D0000)->state, i));
    }
#endif
    do {
    } while (IsIdleGPU(10) != 0);
    count = 0;
    clut = &D_800E9D70[1];
    clut->x = 128;
    clut->y = 496;
    clut->w = 64;
    clut->h = 1;
    for (row = 0; row < 5; row++) {
        D_800E9D70[0].x = 128;
        D_800E9D70[0].y = row * 48 + 256;
        D_800E9D70[0].w = 24;
        D_800E9D70[0].h = 48;
        for (col = 0; col < FREE_DUEL_GRID_COLUMN_COUNT; col++) {
            LoadImage2(&D_800E9D70[0], (u32 *)src);
            LoadImage2(&D_800E9D70[1],
                       (u32 *)(src + FREE_DUEL_PORTRAIT_IMAGE_SIZE));
            D_800E9D70[0].x += 24;
            D_800E9D70[1].y++;
            if ((s16)D_800E9D70[1].y >= 512) {
                D_800E9D70[1].y = 496;
                D_800E9D70[1].x += 64;
            }
            count++;
            src += FREE_DUEL_PORTRAIT_RECORD_SIZE;
        }
    }
    for (row = 0; row < 5; row++) {
        D_800E9D70[0].x = 256;
        D_800E9D70[0].y = row * 48 + 256;
        D_800E9D70[0].w = 24;
        D_800E9D70[0].h = 48;
        for (col = 0; col < FREE_DUEL_GRID_COLUMN_COUNT; col++) {
            LoadImage2(&D_800E9D70[0], (u32 *)src);
            LoadImage2(&D_800E9D70[1],
                       (u32 *)(src + FREE_DUEL_PORTRAIT_IMAGE_SIZE));
            count++;
            if (count >= FREE_DUEL_GRID_ENTRY_COUNT) {
                goto done;
            }
            D_800E9D70[0].x += 24;
            D_800E9D70[1].y++;
            src += FREE_DUEL_PORTRAIT_RECORD_SIZE;
            if ((s16)D_800E9D70[1].y >= 512) {
                D_800E9D70[1].y = 496;
                D_800E9D70[1].x += 64;
            }
        }
    }
done:
    for (i = 0; i < 25; i++) {
#ifdef MEMORIES_PC
        /* Every cell gets its sprite, shown or not: turning the page changes
         * which duelists are there, and a cell with no sprite could not be
         * made to appear without building one. */
        if (1) {
#else
        if (gFreeDuel_abGridAvailable[i] != 0) {
#endif
            obj = DisplayObject_AcquireSlot(DisplayObject_FindFreeGeneralSlot(), 1);
            DisplayObject_ConfigureScreenSprite(obj,
                          (i % FREE_DUEL_GRID_COLUMN_COUNT) * 56 + 20,
                          (i / FREE_DUEL_GRID_COLUMN_COUNT) * 52 + 40, 48, 48,
                          (i % FREE_DUEL_GRID_COLUMN_COUNT) * 48,
                          (i / FREE_DUEL_GRID_COLUMN_COUNT) * 48, 18,
                          (i / 16) * 64 + 128, (i & 15) + 496);
            obj->attribute |= 0x1000000;
            obj->flags &= ~DISPLAY_OBJECT_FLAG_SCREEN_SPACE;
#ifdef MEMORIES_PC
            gFreeDuel_apCells[i] = obj;
#endif
        }
    }
    for (k = 25, i = 0; i < 15; i++, k++) {
#ifdef MEMORIES_PC
        if (1) {
#else
        if (gFreeDuel_abGridAvailable[k] != 0) {
#endif
            obj = DisplayObject_AcquireSlot(DisplayObject_FindFreeGeneralSlot(), 1);
            DisplayObject_ConfigureScreenSprite(obj,
                          (i % FREE_DUEL_GRID_COLUMN_COUNT) * 56 + 20,
                          (k / FREE_DUEL_GRID_COLUMN_COUNT) * 52 + 40, 48, 48,
                          (i % FREE_DUEL_GRID_COLUMN_COUNT) * 48,
                          (i / FREE_DUEL_GRID_COLUMN_COUNT) * 48, 20,
                          (k / 16) * 64 + 128, (k & 15) + 496);
            obj->attribute |= 0x1000000;
            obj->flags &= ~DISPLAY_OBJECT_FLAG_SCREEN_SPACE;
#ifdef MEMORIES_PC
            gFreeDuel_apCells[k] = obj;
#endif
        }
    }
#ifdef MEMORIES_PC
    /* The portraits the page wants, and which cells it shows. */
    FreeDuel_ShowPage(gFreeDuel_nPage);
#endif
    obj = DisplayObject_AcquireSlot(DisplayObject_FindFreeGeneralSlot(), 2);
    DisplayObject_ConfigureSpriteAtPositionWithResource(obj, 0, 0, 0, 0, 0, 16, 0, D_801AF000);
    DisplayObject_SetDepthOffset(obj, 10);
    obj->attribute |= 0x1000000;
    obj->flags |= DISPLAY_OBJECT_FLAG_SCREEN_SPACE;
    obj = DisplayObject_AcquireSlot(DisplayObject_FindFreeGeneralSlot(), 2);
    DisplayObject_ConfigureSpriteAtPositionWithResource(obj, 0, 0, 0, 0, 1, 16, 0, D_801AF000);
    DisplayObject_SetDepthOffset(obj, -10);
    obj->attribute |= 0x1000000;
    obj->flags |= DISPLAY_OBJECT_FLAG_TEXTURE_CELL_OFFSET |
                  DISPLAY_OBJECT_FLAG_SCREEN_SPACE;
    obj = DisplayObject_AcquireSlot(DisplayObject_FindFreeGeneralSlot(), 2);
    DisplayObject_ConfigureSpriteAtPositionWithResource(obj, 0, 0, 0, 0, 2, 17, 3, D_801AF000);
    ((u8 *)&obj->field_5E)[1] = 128;
    DisplayObject_SetDepthOffset(obj, 15);
    obj->flags |= DISPLAY_OBJECT_FLAG_TEXTURE_CELL_OFFSET |
                  DISPLAY_OBJECT_FLAG_SCREEN_SPACE;
    gFreeDuel_pThumbWidget = obj;
    obj = FreeDuel_SpawnSparkle();
    gFreeDuel_pCursorWidget = obj;
    obj->attribute &= ~GsROTOFF;
    if (gFreeDuel_bReturnFlags == 0) {
        obj->flags &= ~DISPLAY_OBJECT_FLAG_RENDERABLE;
        FreeDuel_PlaceCursor(obj, 0);
    } else {
        FreeDuel_PlaceCursor(obj, 1);
    }
    FreeDuel_UpdateScrollbar();
    SD_BGMPlay(29376);
}

DisplayObject *TRANSLATED_G32 *FreeDuel_GetSparkleSlot(void)
{
    s32 i;

    for (i = FREE_DUEL_SPARKLE_POOL_CAPACITY - 1; i >= 0; i--) {
        if (gFreeDuel_apSparklePool[i] == 0) {
            return &gFreeDuel_apSparklePool[i];
        }
    }
    return 0;
}

void FreeDuel_UpdateSparkle(void)
{
    DisplayObject *obj;
    s32 level;
    s16 timer;
    s32 i;

    for (i = FREE_DUEL_SPARKLE_POOL_CAPACITY - 1; i >= 0; i--) {
        obj = gFreeDuel_apSparklePool[i];
        if (obj != 0 && (obj->field_6C & 0xF) == 1) {
            if (!(obj->field_6C & 0x80)) {
                obj->field_6C |= 0x80;
                obj->field_60 = 16;
                obj->field_0C = COLOR_RGB24_DIM_GREY;
                obj->attribute |= (GsALON | GsAONE);
            }
            level = ((u8 *)&obj->field_0C)[0] - 4;
            ((u8 *)&obj->field_0C)[2] = level;
            ((u8 *)&obj->field_0C)[1] = level;
            ((u8 *)&obj->field_0C)[0] = level;
            timer = obj->field_60 - 1;
            obj->field_60 = timer;
            if (timer == 0) {
#ifdef MEMORIES_PC
                DisplayObject_ReleaseIfPresent(obj); /* retail leaves it in $a0 */
#else
                DisplayObject_ReleaseIfPresent();
#endif
                gFreeDuel_apSparklePool[i] = 0;
            }
        }
    }
}

void FreeDuel_UpdateCursorTween(void)
{
    DisplayObject *widget = gFreeDuel_pCursorWidget;
    DisplayObject *TRANSLATED_G32 *slot;
    DisplayObject *sparkle;
    s32 tx;
    s32 ty;
    s32 sx;
    s32 d;
    s16 left;

    if ((gFreeDuel_bScreenFlags & 0x40) == 0) {
        if (gFreeDuel_bCursorColumn == gFreeDuel_bTargetColumn && gFreeDuel_bCursorRow == gFreeDuel_bTargetRow) {
            return;
        }
        gFreeDuel_bScreenFlags |= 0x40;
        widget->field_60 = 8;
        DisplayObject_ResetVelocity((DisplayObjectVelocity *)widget);

        d = gFreeDuel_bTargetColumn;
        tx = d * 56 + 20;
        d = (s16)widget->field_30.h.field_30;
        d = tx - d;
        sx = (d << 8) / 8;
        d = gFreeDuel_bTargetRow;
        ty = d * 52 + 40;
        widget->field_34.h.field_36 = sx;
        d = (s16)widget->field_30.h.field_32;
        d = ty - d;
        widget->field_38.h.field_38 = (d << 8) / 8;
    }

    DisplayObject_StepPositionXY((DisplayObjectVelocity *)widget);
    left = (u16)widget->field_60 - 1;
    widget->field_60 = left;
    if (left == 0) {
        gFreeDuel_bCursorColumn = gFreeDuel_bTargetColumn;
        gFreeDuel_bCursorRow = gFreeDuel_bTargetRow;
        FreeDuel_PlaceCursor(widget, 1);
        gFreeDuel_bScreenFlags &= ~0x40;
        SD_SEPlayFull(47);
    } else {
        slot = FreeDuel_GetSparkleSlot();
        sparkle = FreeDuel_SpawnSparkle();
        if (sparkle != 0 && slot != 0) {
            sparkle->field_30.word = widget->field_30.word;
            DisplayObject_SetDepthOffset(sparkle, (s8)((u8)widget->field_16 - 1));
            DisplayObject_UpdateCommandStream(sparkle);
            sparkle->field_4C = widget->field_4C;
            sparkle->field_6C = 1;
            sparkle->flags |= 1;
            *slot = sparkle;
        }
    }
}

void FreeDuel_UpdateScreen(void)
{
    DuelEffectChannel *panel;
    u16 *entry;
    s32 index;

    if ((gFreeDuel_bScreenFlags & 0x20) != 0) {
        func_80039794();
        panel = &D_800EB15C;
        if ((panel->flags_34 & 8) == 0) {
            gFreeDuel_bScreenFlags &= 0xDF;
            TextBox_Destroy(panel);
            gFreeDuel_pCursorWidget->flags |=
                DISPLAY_OBJECT_FLAG_RENDERABLE;
            FreeDuel_PlaceCursor(
                gFreeDuel_pCursorWidget, 1
            );
        }
        return;
    }

    FreeDuel_UpdateCursorTween();
    FreeDuel_UpdateScrollbar();
    if ((gFreeDuel_bScreenFlags & 0x40) != 0) {
        return;
    }

#ifdef MEMORIES_PC
    /* L1 and R1 turn the page. The screen reads only the pad's directions,
     * Cancel and Confirm, so the shoulder buttons are free here, and they
     * already mean "by a page" on the Library's grid. */
    if (page_count() > 1 && (gInput_wPad1Pressed & PAD_BUTTON_L1_R1_MASK) != 0) {
        const int want = gFreeDuel_nPage + ((gInput_wPad1Pressed & PAD_BUTTON_R1) ? 1 : -1);
        if (want >= 0 && want < page_count()) {
            FreeDuel_ShowPage(want);
            /* The name and the win/loss box are only rebuilt when the cursor
             * arrives somewhere, so the page asks for that itself. */
            if (gFreeDuel_pCursorWidget) {
                FreeDuel_PlaceCursor(gFreeDuel_pCursorWidget, 1);
            }
            SD_SEPlayFull(6);
        } else {
            SD_SEPlayFull(9);
        }
        return;
    }
#endif

    if ((gInput_wPad1Held & PAD_DIRECTION_MASK) != 0) {
        if ((gInput_wPad1Held & PAD_DIRECTION_RIGHT) != 0) {
            if (++gFreeDuel_bTargetColumn >= FREE_DUEL_GRID_COLUMN_COUNT) {
                gFreeDuel_bTargetColumn = FREE_DUEL_GRID_COLUMN_COUNT - 1;
            }
        }
        if ((gInput_wPad1Held & PAD_DIRECTION_LEFT) != 0) {
            if (--gFreeDuel_bTargetColumn < 0) {
                gFreeDuel_bTargetColumn = 0;
            }
        }
        if ((gInput_wPad1Held & PAD_DIRECTION_DOWN) != 0) {
            if (++gFreeDuel_bTargetRow >= FREE_DUEL_GRID_ROW_COUNT) {
                gFreeDuel_bTargetRow = FREE_DUEL_GRID_ROW_COUNT - 1;
            }
        }
        if ((gInput_wPad1Held & PAD_DIRECTION_UP) != 0) {
            if (--gFreeDuel_bTargetRow <= 0) {
                gFreeDuel_bTargetRow = 0;
            }
        }
    } else {
        if ((gInput_wPad1Pressed & PAD_BUTTON_CANCEL) != 0) {
            SD_SEPlayFull(8);
            D_8009B26C = 8;
            return;
        }
        if ((gInput_wPad1Pressed & PAD_BUTTON_CONFIRM_MASK) == 0) {
            return;
        }
#ifdef MEMORIES_PC
        if (!Duelists_Available(cell_duelist(gFreeDuel_bCursorRow * FREE_DUEL_GRID_COLUMN_COUNT +
                                             gFreeDuel_bCursorColumn))) {
            return;
        }
        /* Deck Build is duelist 0, which is the top left of the first page
         * only: on a later page that cell is somebody to duel. */
        if (cell_duelist(gFreeDuel_bCursorRow * FREE_DUEL_GRID_COLUMN_COUNT +
                         gFreeDuel_bCursorColumn) == 0) {
#else
        if (gFreeDuel_abGridAvailable[
                gFreeDuel_bCursorRow * FREE_DUEL_GRID_COLUMN_COUNT +
                gFreeDuel_bCursorColumn] == 0) {
            return;
        }
        if ((gFreeDuel_bCursorColumn | gFreeDuel_bCursorRow) == 0) {
#endif
            BuildDeck_EnterNarrowConfirmMode();
            D_8009B269 = 6;
            gFreeDuel_bReturnFlags = 0x40;
            SD_SEPlayFull(0x30);
            return;
        }
        entry = gDuel_awPlayerDeck;
        for (index = 0; index < DECK_SIZE; index++) {
            if (*entry == 0) {
                SD_SEPlayFull(9);
                TextBox_CreateFlagged(1, 8, 0x30, 0x6C, 0xE0, 0x10, 0x1028);
                gFreeDuel_bScreenFlags |= 0x20;
                return;
            }
            entry++;
        }
#ifdef MEMORIES_PC
        /* fork: a deck out of the rules does not duel; the notice says why (deck_rules.h). */
        if (!DeckRules_DeckOk(gDuel_awPlayerDeck)) {
            SD_SEPlayFull(9);
            return;
        }
#endif
        SD_SEPlayFull(0x30);
        gFreeDuel_bReturnFlags = 0x80;
        func_80024DC8(
            -1,
#ifdef MEMORIES_PC
            /* The opponent, which is the cell only on the first page. */
            cell_duelist(gFreeDuel_bCursorRow * FREE_DUEL_GRID_COLUMN_COUNT +
                         gFreeDuel_bCursorColumn),
#else
            gFreeDuel_bCursorRow * FREE_DUEL_GRID_COLUMN_COUNT +
                gFreeDuel_bCursorColumn,
#endif
            0x6000, 0x6000);
        D_8009B368 = 6;
        D_8009B26C = 3;
    }
}

void FreeDuel_Entry(void)
{
    s32 phase;

    rand();
    FreeDuel_UpdateScreen();
    phase = D_8009B0CC & 0x7F;
    if (phase < 0x10) {
        if (phase >= 8) {
            phase = 0xF - phase;
        }
        gFreeDuel_pCursorWidget->field_44.h.field_46 =
            phase * 48 + 0x1000;
        gFreeDuel_pCursorWidget->field_44.h.field_44 =
            phase * 48 + 0x1000;
    }
    FreeDuel_UpdateSparkle();
}
