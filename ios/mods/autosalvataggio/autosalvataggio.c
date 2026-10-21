/* Autosalvataggio: after a Free Duel the player won, once the game is back
 * on the Free Duel grid (the card, the StarChips and the win already in the
 * save state), the game is saved by itself to the slot loaded or saved last
 * (SaveMenu_CurrentSlot), exactly as the slot menu saves it: the state copied
 * out of the live one and sealed by the game (SaveData_RequestWrite,
 * SaveData_BuildPayload), the slot file written beside and renamed over
 * (SaveSlots_WriteFile: a failed write leaves the old save), then what the
 * dialog does once a save is done (mem_card_dialog_runtime.c: the sequence
 * one up, SaveCards_Saved for what the port keeps beside the save).
 *
 * Nothing when no slot was loaded or saved yet, or when that slot holds
 * another duelist's game. Before each write the slot's previous save goes to
 * mod-data/autosalvataggio/riserva-slotNN.sav, a whole slot file: copied
 * over saves/slotNN.sav it is that save again.
 *
 * The deck slots (deck_menu.c) are stored only by the slot menu's saves, so
 * they still wait for a save by hand. */
#include "types.h"
#include "pc/mods/modapi.h"
#include "game/save_data.h"
#include <stdio.h>
#include <string.h>

int SaveMenu_CurrentSlot(void);
int SaveMenu_Active(void);
int DeckMenu_Active(void);
int SaveSlots_ReadState(int slot, unsigned char *state, int (*check)(unsigned char *));
int SaveSlots_WriteFile(int slot, const unsigned char *image, size_t bytes);
const char *SaveSlots_LastError(void);
void SaveCards_Saved(const void *state, unsigned sequence);
int Menu_Height(void);
int TouchPad_FreeSpan(int *left, int *right);

extern unsigned char D_8009B26C; /* main_mode_state.h: the running mode, flags above the low five bits */

enum { MODE_MASK = 0x1F, MODE_DUEL = 3, MODE_FREE_DUEL = 6 };
enum { SETTLE_FRAMES = 30, SLOT_FILE = 0x2000, MESSAGE_US = 4000000 };

static const MemoriesModHost *host;
static int last_mode = -1, from_free_duel, armed, settled;
static unsigned wins_before;
static unsigned char image[SLOT_FILE], backup[SLOT_FILE], old[SAVE_DATA_STATE_SIZE];
static char message[64];
static unsigned long long message_until;

static SaveDataState *live(void) { return (SaveDataState *)gDuel_awPlayerDeck; }

/* Every win the save counts: the total and each Free Duel opponent's. */
static unsigned wins(void)
{
    const unsigned char *records = (const unsigned char *)live()->duelist_records;
    unsigned total = live()->duel_wins;
    for (int i = 0; i < FREE_DUEL_GRID_ENTRY_COUNT; i++)
        total += records[i * 4] | records[i * 4 + 1] << 8;
    return total;
}

static int check(unsigned char *state) { return SaveData_ValidateIntegrity(state); }

static void say(const char *text, int slot)
{
    snprintf(message, sizeof(message), text, slot + 1);
    message_until = host->now_us(host) + MESSAGE_US;
    host->log(host, "%s\n", message);
}

/* The slot's save as it is, a whole slot file, before it is replaced. */
static int keep_old(int slot)
{
    char name[32];
    FILE *file;
    size_t done;
    memset(backup, 0, sizeof(backup));
    memcpy(backup, gSaveData_aHeaderTemplate, SAVE_DATA_HEADER_SIZE);
    memcpy(backup + SAVE_DATA_HEADER_SIZE, old, SAVE_DATA_STATE_SIZE);
    memcpy(backup + SAVE_DATA_DUPLICATE_STATE_OFFSET, old, SAVE_DATA_STATE_SIZE);
    snprintf(name, sizeof(name), "riserva-slot%02d.sav", slot + 1);
    if (!(file = host->open_data(host, name, "wb"))) return -1;
    done = fwrite(backup, 1, sizeof(backup), file);
    return fclose(file) == 0 && done == sizeof(backup) ? 0 : -1;
}

static void autosave(void)
{
    SaveDataPayload *payload = (SaveDataPayload *)image;
    int slot = SaveMenu_CurrentSlot(), had_old = 0;
    /* Only the tests set it (MEMORIES_MOD_AUTOSALVATAGGIO_PROVA_SLOT): a slot
     * from 1 for a game that was never loaded nor saved. */
    if (slot < 0) slot = host->setting(host, "prova_slot", 0) - 1;
    if (slot < 0) {
        say("Autosalvataggio: salva una volta a mano per scegliere lo slot", slot);
        return;
    }
    if (SaveSlots_ReadState(slot, old, check) == 0) {
        if (((SaveDataState *)old)->duelist_code != live()->duelist_code) {
            say("Autosalvataggio: lo slot %d e' di un'altra partita, non salvo", slot);
            return;
        }
        if (keep_old(slot)) {
            say("Autosalvataggio: copia di riserva non riuscita, non salvo", slot);
            return;
        }
        had_old = 1;
    }
    /* SaveData_RequestWrite, into a buffer of our own. */
    memset(image, 0, sizeof(image));
    memcpy(&payload->state, gDuel_awPlayerDeck, SAVE_DATA_STATE_SIZE);
    SaveData_BuildPayload(payload);
    if (SaveSlots_WriteFile(slot, image, SAVE_DATA_HEADER_SIZE + SAVE_DATA_REPLICATED_STATE_SIZE)) {
        say("Autosalvataggio NON riuscito (slot %d)", slot);
        host->log(host, "%s\n", SaveSlots_LastError());
        return;
    }
    /* Read back through the game's own check, as the slot menu will read it;
     * anything amiss and the save that was there goes back. */
    if (SaveSlots_ReadState(slot, old, check) || memcmp(old, &payload->state, SAVE_DATA_STATE_SIZE)) {
        if (had_old) SaveSlots_WriteFile(slot, backup, SAVE_DATA_HEADER_SIZE + SAVE_DATA_REPLICATED_STATE_SIZE);
        say("Autosalvataggio NON riuscito (slot %d): tenuto quello di prima", slot);
        return;
    }
    gSaveDataSequence = (s32)((u32)gSaveDataSequence + 1);
    SaveCards_Saved(&payload->state, payload->state.save_sequence);
    say("Salvato nello slot %d", slot);
}

static void frame(void)
{
    int mode = D_8009B26C & MODE_MASK;
    if (mode == MODE_DUEL && last_mode != MODE_DUEL) {
        armed = from_free_duel;
        wins_before = wins();
        settled = 0;
    }
    if (mode != MODE_DUEL) from_free_duel = mode == MODE_FREE_DUEL;
    if (armed && mode == MODE_FREE_DUEL) {
        /* The win, the card and the StarChips are in the state before the grid
         * comes back; its mode byte keeps 0x40 up there after a duel, so only
         * the menus of the port are waited out. */
        settled = SaveMenu_Active() || DeckMenu_Active() ? 0 : settled + 1;
        if (settled >= SETTLE_FRAMES) {
            armed = 0;
            if (wins() != wins_before) autosave();
        }
    } else if (mode != MODE_DUEL) {
        armed = 0;
    }
    if (mode != last_mode && host->log_enabled(host))
        host->log(host, "modo %d -> %d (byte %02X), armato %d, vittorie %u (prima %u)\n", last_mode, mode, D_8009B26C,
                  armed, wins(), wins_before);
    last_mode = mode;
}

static void applied(int is_on)
{
    if (!is_on) armed = 0, message[0] = '\0';
}

static void draw(void)
{
    int width, height, scale, left = 0, right, w;
    if (!message[0]) return;
    if (host->now_us(host) >= message_until) {
        message[0] = '\0';
        return;
    }
    host->overlay_size(host, &width, &height, &scale);
    if (scale < 1) scale = 1;
    right = width;
    if (!TouchPad_FreeSpan(&left, &right) || right <= left) left = 0;
    w = host->text_width(host, message, scale) + 8 * scale;
    host->fill(host, left + 4 * scale, Menu_Height() + 4 * scale, w, 16 * scale, 0x000000, 150);
    host->draw_text(host, left + 8 * scale, Menu_Height() + 12 * scale, message, 0x90E090, scale);
}

static unsigned signature(void)
{
    int left = 0, right = 0;
    if (!message[0]) return 1u;
    if (host->now_us(host) >= message_until) return 2u; /* draw clears it */
    TouchPad_FreeSpan(&left, &right);
    return 3u + (unsigned)message_until + (unsigned)left * 131u + (unsigned)Menu_Height();
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
    return 1;
}
