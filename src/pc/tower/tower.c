/* What the game asks of the tower (tower.h): whether it is open, the rules
 * and the starting LP and deck of the floor being fought, the "tower"
 * conditions of the duelists' unlocks, and the save's progress, beside the
 * memory card's block as the card packs' is (pack_shop.c).
 *
 * The screen itself is tower_screen.c. */
#include "tower.h"
#include "pc/free_duel/duelists.h"
#include "pc/cards/tables.h"
#include "pc/saves/save_menu.h"
#include "pc/saves/save_slots.h"
#include "pc/platform/paths.h"
#include "pc/guest/state.h"
#include "pc/debug/log.h"
#include "pc/compat/fs.h"
#include "pc/platform/menu.h"
#include "types.h"
#include "game/campaign_flags.h"
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The game's bytes this module reads, declared plainly: this unit is
 * native, so the codegen spellings the game's headers select do not matter
 * (as title_jump.c does). */
extern u8 D_8009B26C, D_8009B368, gDuel_bOpponentID;
extern u16 gDuel_awPlayerDeck[];

#define MODE_ANIMATED_BATTLE 1      /* main_modes.h: the duel's 3D battle */
#define MODE_DUEL 3                 /* main_modes.h, MAIN_MODE_DUEL */
#define SAVE_DUELIST_CODE 0x334     /* save_data.h, SAVE_DATA_DUELIST_CODE_OFFSET */
#define SAVE_CREDITS_FLAGS 0x3DE    /* save_data.h, SAVE_DATA_CREDITS_FLAGS_OFFSET */
#define FREE_DUEL_UNLOCK_FLAG 0x6E0 /* + id: the Free Duel grid shows it (Text_UnlockDuelist) */

static uint32_t state_code(const void *state)
{
    uint32_t code;
    memcpy(&code, (const unsigned char *)state + SAVE_DUELIST_CODE, sizeof(code));
    return code;
}

/* The running save's progress, of this run's tower: a progress of another
 * tower (its mod changed) is none. */
static TowerProgress *progress(void)
{
    const TowerConfig *tower = Tower_Config();
    uint32_t identity = Tower_Identity(tower);
    if (tower && gTower.progress.tower != identity)
        Tower_ClearProgress(&gTower.progress, state_code(gDuel_awPlayerDeck), identity);
    return &gTower.progress;
}

/* --- whether it is open -------------------------------------------------------- */

int Tower_EndingSeen(const void *state)
{
    /* Main_RunCredits (main_run_credits.c), as the ending begins:
     * save_state->field_3DE |= 3, then the save prompt writes it. Nothing on
     * the US disc reads it back (the Japanese one opens its PocketStation
     * menu by it), and NEW GAME clears the whole block (NameEntry_Main). */
    return state && (((const unsigned char *)state)[SAVE_CREDITS_FLAGS] & 3) != 0;
}

int Tower_Unlocked(void)
{
    const TowerConfig *tower = Tower_Config();
    const TowerUnlock *unlock;
    const void *save = gDuel_awPlayerDeck;
    if (!tower) return 0;
    unlock = &tower->unlock;
    if (unlock->kind == TOWER_UNLOCK_ALWAYS) return 1;
    if (unlock->kind == TOWER_UNLOCK_NEVER) return 0;
    if (unlock->after_ending && !Tower_EndingSeen(save)) return 0;
    return Duelists_ConditionsMet(save, unlock->beat, unlock->wins, unlock->story, unlock->card, unlock->copies);
}

/* --- the floor being fought ------------------------------------------------------- */

/* The floor whose duel is on: armed by the screen, and the game in that
 * duel (or its 3D battle, mode 1) against the floor's opponent, on its way
 * back to the tower. A jump out of the duel leaves `armed` set, but the
 * next duel is armed by somebody else, who sets its own way back
 * (D_8009B368) and opponent. */
static const TowerFloor *fought(void)
{
    const TowerConfig *tower = Tower_Config();
    const TowerFloor *floor;
    int mode = D_8009B26C & 0x1F;
    if (!tower || gTower.armed <= 0 || gTower.armed > tower->floor_count) return NULL;
    if (D_8009B368 != TOWER_MODE || (mode != MODE_DUEL && mode != MODE_ANIMATED_BATTLE)) return NULL;
    floor = &tower->floors[gTower.armed - 1];
    return gDuel_bOpponentID == (u8)floor->duelist ? floor : NULL;
}

int Tower_DuelRules(void)
{
    return fought() ? gTower.rules : 0;
}

int Tower_Active(void)
{
    return Tower_Config() && ((D_8009B26C & 0x1F) == TOWER_MODE || fought());
}

int Tower_CardDrops(int wanted)
{
    const TowerConfig *tower = Tower_Config();
    if (!tower || !fought() || tower->drops == TOWER_DROPS_PLAYER) return wanted;
    return tower->drops;
}

int Tower_CheatsOff(void)
{
    return 1;   /* fork: the rules are the same for every player, cheats are always off */
}

int Tower_Guard(int what, int tell, char *why, size_t size)
{
    static const char *const ok[] = {"OK"};
    static const char *const titles[] = {"No save states here", "No save states here", "No cheats here",
                                         "Not in this duel"};
    const TowerConfig *tower = Tower_Config();
    const char *text;
    /* fork: no save states and no cheats anywhere, so every player plays by
     * the same rules (the game's own save and the mods' autosave stay). */
    if (what != TOWER_GUARD_JUMP) {
        text = what == TOWER_GUARD_CHEAT ? "In questa versione i trucchi non ci sono: le regole sono uguali per tutti."
                                         : "In questa versione gli stati rapidi non ci sono: le regole sono uguali per "
                                           "tutti. Il salvataggio del gioco e l'autosalvataggio funzionano come sempre.";
        if (why && size) snprintf(why, size, "%s", text);
        LOG(LOG_MODS, "locked rules: refused (%s)", titles[what]);
        if (tell) Menu_ShowNotice(what == TOWER_GUARD_CHEAT ? "Niente trucchi" : "Niente stati rapidi", text, ok, 1, 0,
                                  NULL);
        return 1;
    }
    if (!tower || !Tower_Active()) return 0;
    if (what == TOWER_GUARD_SAVE_STATE || what == TOWER_GUARD_LOAD_STATE) {
        if (tower->save_states) return 0;
        text = "This tower's mod turns save states off while you are in it: no state is saved or loaded until "
               "you leave the tower.";
    } else {
        if (tower->cheats) return 0;
        /* A jump from the tower's screen is a way out as LEAVE is (and
         * banked as it is, "on_leave"); only a floor's duel is kept from
         * being left without its loss. */
        if (what == TOWER_GUARD_JUMP && !fought()) return 0;
        text = what == TOWER_GUARD_JUMP ? "This tower's mod lets no duel of its own be left by a jump or a restart: "
                                          "finish the duel, then leave the tower from its own screen."
                                        : "This tower's mod turns the Cheats menu off while you are in it.";
    }
    if (why && size) snprintf(why, size, "%s", text);
    LOG(LOG_MODS, "tower: refused (%s)", titles[what]);
    fprintf(stderr, "memories-pc: %s\n", text);
    if (tell) Menu_ShowNotice(titles[what], text, ok, 1, 0, NULL);
    return 1;
}

int Tower_Refuses(int rule, int a, int b, int result)
{
    static int told;
    if (!(Tower_DuelRules() & rule)) return 0;
    /* The CPU asks of every pair it holds, many times a turn: the first
       few say so (MEMORIES_TRACE=mods), which is what the tests look for. */
    if (told < 64) {
        told++;
        LOG(LOG_MODS, "tower: %s refused: %d with %d would make %d", rule == TOWER_RULE_NO_FUSIONS ? "fusion" : "equip",
            a, b, result);
    }
    return 1;
}

int Tower_StartingLifePoints(int side, int usual)
{
    const TowerFloor *floor = fought();
    if (!floor || side < 0 || side > 1 || !floor->life_points[side]) return usual;
    return floor->life_points[side];
}

int Tower_OpponentDeck(unsigned short cards[TOWER_DECK])
{
    const TowerFloor *floor = fought();
    if (!floor || !floor->deck_given) return 0;
    memcpy(cards, floor->deck, sizeof(floor->deck));
    return 1;
}

/* --- the duelists' unlocks -------------------------------------------------------- */

int Tower_ConditionMet(int floor)
{
    const TowerConfig *tower = Tower_Config();
    const TowerProgress *done;
    if (!tower) return 0;
    done = progress();
    if (floor <= 0) return done->completions > 0;
    return Tower_FloorCleared(done, floor - 1);
}

int Tower_FreesDuelist(int duelist)
{
    const TowerConfig *tower = Tower_Config();
    const TowerProgress *done;
    int floor, i;
    if (!tower || duelist <= 0) return 0;
    done = progress();
    for (floor = 0; floor < tower->floor_count; floor++) {
        const TowerFloor *one = &tower->floors[floor];
        if (!Tower_FloorCleared(done, floor)) continue;
        /* Its opponent, beaten, unless the floor says "unlock_opponent": false. */
        if (one->duelist == duelist && one->unlock_opponent != 0) return 1;
        for (i = 0; i < one->reward_duelist_count; i++) {
            if (one->reward_duelists[i] == duelist) return 1;
        }
    }
    return 0;
}

int Tower_OpensDuelist(int duelist)
{
    const TowerConfig *tower = Tower_Config();
    const TowerProgress *done;
    int floor, i;
    if (!tower || duelist <= 0) return 0;
    done = progress();
    for (floor = 0; floor < tower->floor_count; floor++) {
        const TowerFloor *one = &tower->floors[floor];
        if (!Tower_FloorCleared(done, floor)) continue;
        if (one->duelist == duelist && one->unlock_opponent == 1) return 1;
        for (i = 0; i < one->reward_duelist_count; i++) {
            if (one->reward_duelists[i] == duelist) return 1;
        }
    }
    return 0;
}

void Tower_UnlockRetailDuelists(const TowerFloor *floor)
{
    int i;
    for (i = -1; i < floor->reward_duelist_count; i++) {
        /* The floor's own opponent first, when it says "unlock_opponent": true. */
        int id = i < 0 ? (floor->unlock_opponent == 1 ? floor->duelist : 0) : floor->reward_duelists[i];
        if (id <= 0 || id >= DUELISTS_RETAIL_COUNT - 1) continue;   /* 39, Duel Master K, is always there */
        /* The grid's own flag, which the campaign's "you won" text sets
         * (Text_UnlockDuelist). Not its other one, 0x1F + id, which the
         * campaign's event script tests to choose a scene. */
        Library_UpdateCardUsedFlag(FREE_DUEL_UNLOCK_FLAG + id);
    }
}

/* --- beside the save ---------------------------------------------------------------- */

/* The other towers' sections of the save's file: written back with it. */
static char *foreign;
/* The slot token and duelist code of the save loaded last, whose file a
 * save written while no tower mod is on carries over (Tower_SaveWritten). */
static unsigned loaded_token;
static uint32_t loaded_code;

static int progress_path(char *out, size_t size, unsigned token)
{
    char relative[64];
    snprintf(relative, sizeof(relative), "tower/%08X.txt", token);
    return Paths_User(out, size, relative);
}

static unsigned slot_token(void)
{
    int slot = SaveMenu_CurrentSlot();
    return slot >= 0 ? SaveSlots_Token(slot) : 0;
}

/* Whether `file` is a progress file, "XXXXXXXX.txt", and its token. */
static int token_named(const char *file, unsigned *token)
{
    char tail[8];
    return strlen(file) == 12 && sscanf(file, "%8x%7s", token, tail) == 2 && !strcmp(tail, ".txt") &&
           strspn(file, "0123456789ABCDEF") == 8;
}

void Tower_SaveLoaded(const void *state)
{
    const TowerConfig *tower = Tower_Config();
    char path[1024];
    unsigned token;
    uint32_t code, identity = Tower_Identity(tower);
    FILE *file;
    int found;
    loaded_token = slot_token();
    loaded_code = state_code(state);
    if (!tower) return;
    free(foreign);
    foreign = NULL;
    code = state_code(state);
    Tower_ClearProgress(&gTower.progress, code, identity);
    token = slot_token();
    if (!token || progress_path(path, sizeof(path), token)) return;
    file = fopen(path, "r");
    if (!file) return;
    found = Tower_ReadProgress(file, tower->mod, identity, &gTower.progress, &foreign);
    fclose(file);
    Tower_Bank(tower, &gTower.progress);   /* "on_leave": a file of another rule kept no further */
    if (!found) {
        gTower.progress.owner = code;   /* this tower not climbed yet; the others' sections kept */
    } else if (gTower.progress.owner != code) {
        /* The slot's file is of a save it held before: none of this one's. */
        Tower_ClearProgress(&gTower.progress, code, identity);
        free(foreign);
        foreign = NULL;
    }
    LOG(LOG_MODS, "tower: progress read from %s (floor %u, checkpoint %u)", path, (unsigned)gTower.progress.floor,
        (unsigned)gTower.progress.checkpoint);
}

/* The progress files no slot's save goes with any more: the one the slot
 * just written had before, and that of a slot saved over from another's
 * load. None while a slot cannot be read (its token is not known). */
static void forget_progress(unsigned written)
{
    unsigned live[SAVE_SLOT_COUNT], token;
    char directory[1024], path[1024], *slash;
    struct dirent *item;
    DIR *folder;
    int slot, i;
    for (slot = 0; slot < SAVE_SLOT_COUNT; slot++) {
        if (SaveSlots_ReadToken(slot, &live[slot])) return;
    }
    if (progress_path(directory, sizeof(directory), 1)) return;
    slash = strrchr(directory, '/');
    if (!slash) return;
    *slash = '\0';
    if (!(folder = opendir(directory))) return;
    while ((item = readdir(folder)) != NULL) {
        int still = token_named(item->d_name, &token) ? token == written : 1;
        for (i = 0; i < SAVE_SLOT_COUNT && !still; i++) still = live[i] == token;
        if (!still && !progress_path(path, sizeof(path), token)) remove(path);
    }
    closedir(folder);
}

static int nothing_done(const TowerProgress *done)
{
    size_t i;
    if (done->floor || done->checkpoint || done->completions || done->flags) return 0;
    for (i = 0; i < sizeof(done->cleared); i++) {
        if (done->cleared[i]) return 0;
    }
    return 1;
}

/* A save written while no tower mod is on: every slot write is a new token,
 * so the loaded save's file goes with it whole, every section of it a tower
 * not here this run, rather than be forgotten as the old token's. */
static void carry_over(const void *state, unsigned token)
{
    char from[1024], to[1024], temporary[1040], directory[1024], *slash, buffer[4096];
    FILE *in, *out;
    size_t n;
    int failed = 0;
    if (!token || !loaded_token || token == loaded_token || state_code(state) != loaded_code) return;
    if (progress_path(from, sizeof(from), loaded_token) || progress_path(to, sizeof(to), token)) return;
    if (!(in = fopen(from, "rb"))) return;
    snprintf(directory, sizeof(directory), "%s", to);
    slash = strrchr(directory, '/');
    if (slash) { *slash = '\0'; Paths_MakeDirs(directory); }
    snprintf(temporary, sizeof(temporary), "%s.tmp", to);
    if (!(out = fopen(temporary, "wb"))) {
        fclose(in);
        return;
    }
    while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (fwrite(buffer, 1, n, out) != n) failed = 1;
    }
    fclose(in);
    if (ferror(out)) failed = 1;
    if (fclose(out)) failed = 1;
    if (failed || rename(temporary, to)) {
        remove(temporary);
        return;
    }
    loaded_token = token;
    forget_progress(token);
}

void Tower_SaveWritten(const void *state)
{
    const TowerConfig *tower = Tower_Config();
    char path[1024], temporary[1040], directory[1024], *slash;
    unsigned token = slot_token();
    uint32_t code;
    FILE *file;
    int failed;
    if (!tower) {
        carry_over(state, token);
        return;
    }
    if (!token) return;
    code = state_code(state);
    if (progress()->owner != code) {
        Tower_ClearProgress(&gTower.progress, code, Tower_Identity(tower));
        free(foreign);
        foreign = NULL;
    }
    if (progress_path(path, sizeof(path), token)) return;
    if (nothing_done(&gTower.progress) && !(foreign && *foreign)) {
        remove(path);   /* nothing climbed: no file */
        forget_progress(token);
        return;
    }
    snprintf(directory, sizeof(directory), "%s", path);
    slash = strrchr(directory, '/');
    if (slash) { *slash = '\0'; Paths_MakeDirs(directory); }
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    file = fopen(temporary, "w");
    if (!file) {
        fprintf(stderr, "memories-pc: cannot write %s\n", temporary);
        return;
    }
    {   /* "on_leave": a save keeps the climb no further than the checkpoint
         * (or the first floor), whatever is in memory now (Tower_Bank). */
        TowerProgress banked = gTower.progress;
        Tower_Bank(tower, &banked);
        Tower_WriteProgress(file, tower->mod, &banked, foreign);
    }
    failed = ferror(file);
    if (fclose(file)) failed = 1;
    if (failed || rename(temporary, path)) {
        remove(temporary);
        fprintf(stderr, "memories-pc: cannot replace %s\n", path);
        return;
    }
    {
        TowerProgress banked = gTower.progress;
        Tower_Bank(tower, &banked);
        LOG(LOG_MODS, "tower: progress written to %s (floor %u, checkpoint %u)", path, (unsigned)banked.floor,
            (unsigned)banked.checkpoint);
    }
    loaded_token = token;
    loaded_code = code;
    forget_progress(token);
}

/* "save_states": false. The climb is in the game's memory, so a state saved
 * outside the tower would bring an earlier climb back (a loss undone, a
 * checkpoint regained): the climb as it was before a state is loaded is
 * kept here and put back after it (Tower_State marks the load). */
static TowerProgress shadow;
static int shadow_kept, shadow_wanted;

void Tower_Frame(void)
{
    const TowerConfig *tower = Tower_Config();
    uint32_t code;
    if (!tower) return;
    if (!tower->save_states) {
        if (shadow_wanted && shadow_kept) {
            gTower.progress = shadow;
            LOG(LOG_MODS, "tower: a state was loaded; the climb kept (floor %u, checkpoint %u)",
                (unsigned)gTower.progress.floor + 1, (unsigned)gTower.progress.checkpoint + 1);
        }
        shadow = gTower.progress;
        shadow_kept = 1;
    }
    shadow_wanted = 0;
    code = state_code(gDuel_awPlayerDeck);
    if (gTower.progress.owner == code && gTower.progress.tower == Tower_Identity(tower)) return;
    /* NEW GAME (or the save's code otherwise changed): a save with no climb
     * yet, until a load reads one. */
    Tower_ClearProgress(&gTower.progress, code, Tower_Identity(tower));
    free(foreign);
    foreign = NULL;
}

/* --- save states -------------------------------------------------------------------- */

/* The progress and the screen are the game's data (zz_tower_storage.c),
 * which every state holds; the other towers' sections of the save's file
 * are this module's, and go with the state rather than stay those of the
 * save loaded since: else a save written after the load would be given
 * another save's. Their size first, then the text.
 *
 * The save loaded last (whose file a save with no tower on carries over)
 * goes with the state too, tower or not: a state of another slot's line
 * would else have that slot's climb copied to the one it is saved to. A
 * state from before this knew none, and carries nothing over. */
void Tower_State(MemoriesState *state)
{
    uint32_t length = foreign ? (uint32_t)strlen(foreign) : 0;
    MemoriesStateField count = {&length, sizeof(length)};
    MemoriesStateField loaded[] = {{&loaded_token, sizeof(loaded_token)}, {&loaded_code, sizeof(loaded_code)}};
    if (!Memories_StateChunk(state, "tower-loaded", loaded, 2) && Memories_StateLoading(state)) loaded_token = 0;
    if (Memories_StateLoading(state)) shadow_wanted = 1;   /* Tower_Frame: the climb from before, "save_states" false */
    if (!Tower_Config()) return;
    if (!Memories_StateLoading(state)) {
        MemoriesStateField text = {foreign, length};
        Memories_StateChunk(state, "tower-foreign-n", &count, 1);
        if (length) Memories_StateChunk(state, "tower-foreign", &text, 1);
        return;
    }
    free(foreign);
    foreign = NULL;
    if (Memories_StateChunk(state, "tower-foreign-n", &count, 1) && length) {
        char *read = malloc(length + 1u);
        MemoriesStateField text = {read, length};
        if (read && Memories_StateChunk(state, "tower-foreign", &text, 1)) {
            read[length] = '\0';
            foreign = read;
        } else {
            free(read);
        }
    }
}
