#ifndef MEMORIES_PC_SAVE_SLOTS_H
#define MEMORIES_PC_SAVE_SLOTS_H
/* Save slots: the port's saves, one file per slot, instead of memory cards.
 *
 * Slot n is saves/slotNN.sav in the user directory (paths.h). A slot file is
 * byte for byte the 8 KiB block the game writes to a memory card: the
 * 0x200-byte title/icon header, the 0x680-byte save state and its 0x680-byte
 * duplicate, then zeros. So a slot can go back onto a card image with any
 * memory card manager, and a card's save can become a slot.
 *
 * After both copies, at SAVE_SLOT_TAG_OFFSET, the port keeps a token of its
 * own in what is otherwise zero padding: a number drawn afresh each time the
 * slot is saved. What a save holds of the cards mods add is kept beside it
 * under that token (cards.h), so two slots of one duelist never share it.
 *
 * The first time the saves directory is created, the save on each memory
 * card image the older builds used (memcard1.mcd, memcard2.mcd) is copied
 * into slots 1 and 2. The card images are only read.
 *
 * A memory card file from an emulator or a card manager put in the saves
 * folder is imported whenever the slot menu opens (SaveSlots_ImportFolder):
 * each save of the game on it goes into an empty slot, and the file is
 * renamed so it is not imported twice.
 *
 * Nothing here touches game memory; the caller passes the game's integrity
 * check in, so the storage can be tested on its own. */
#include <stddef.h>

#define SAVE_SLOT_COUNT 10
#define SAVE_SLOT_FILE_SIZE 0x2000
#define SAVE_SLOT_HEADER_SIZE 0x200
#define SAVE_SLOT_STATE_SIZE 0x680
#define SAVE_SLOT_DUPLICATE_OFFSET (SAVE_SLOT_HEADER_SIZE + SAVE_SLOT_STATE_SIZE)
#define SAVE_SLOT_TAG_OFFSET (SAVE_SLOT_DUPLICATE_OFFSET + SAVE_SLOT_STATE_SIZE)

/* Damaged: the file is there but neither copy passes the game's check.
 * Unreadable: the file, or the saves folder, could not be opened at all
 * (SaveSlots_ReadError says why); the save itself may well be sound. */
typedef enum { SAVE_SLOT_EMPTY, SAVE_SLOT_USED, SAVE_SLOT_DAMAGED, SAVE_SLOT_UNREADABLE } SaveSlotStatus;

/* What a slot shows in the menu, read from its save state. */
typedef struct SaveSlotInfo {
    SaveSlotStatus status;
    int from_duplicate; /* the first copy failed its check; the duplicate is used */
    char name[16];      /* player name in ASCII */
    int duelist_code;
    unsigned sequence;
    unsigned starchips;
    int wins, losses, cards; /* wins and losses: Free Duel's, all opponents */
    /* The file's modification time, seconds since 1970; aligned so i386
     * Linux lays it out as Windows does. */
    long long saved_at __attribute__((aligned(8)));
} SaveSlotInfo;

/* SaveData_ValidateIntegrity: nonzero when a 0x680-byte state is sound. */
typedef int (*SaveSlotCheck)(unsigned char *state);

/* Adds what a save's Free Duel record holds outside its state (the added
 * duelists', pc/free_duel/duelists.h) to the wins and losses a slot shows.
 * None by default. */
typedef void (*SaveSlotRecordReader)(const unsigned char *state, int *wins, int *losses);
void SaveSlots_SetRecordReader(SaveSlotRecordReader reader);

/* Called after each slot file written (any of the writes below), with the
 * whole file as it is now on the disk: the saves on the online account
 * (pc/platform/cloud.h). None by default. */
typedef void (*SaveSlotWriteListener)(int slot, const unsigned char *image);
void SaveSlots_SetWriteListener(SaveSlotWriteListener listener);
/* Replace a slot's file with these exact bytes (a save from the account),
 * the same safe way as the writes below. 0 on success. */
int SaveSlots_WriteImage(int slot, const unsigned char image[SAVE_SLOT_FILE_SIZE]);

/* Slots are numbered from 0 here; the menu shows them from 1. */
int SaveSlots_Path(int slot, char *out, size_t size);
void SaveSlots_Scan(SaveSlotInfo out[SAVE_SLOT_COUNT], SaveSlotCheck check);
/* Read a slot's sound state (the duplicate when the first copy is damaged)
 * into `state`. 0 on success, -1 when the slot is empty or both copies fail. */
int SaveSlots_ReadState(int slot, unsigned char state[SAVE_SLOT_STATE_SIZE], SaveSlotCheck check);
/* Replace a slot with `bytes` of file image (header first), padded with
 * zeros to a whole block, under a new token. Written beside the slot and
 * renamed over it, so a failed write leaves the old save. 0 on success. */
int SaveSlots_WriteFile(int slot, const unsigned char *image, size_t bytes);
/* Patch `bytes` at `offset` of an existing slot, the same way; the token
 * stays. */
int SaveSlots_WriteAt(int slot, long offset, const unsigned char *data, size_t bytes);
/* Replace both state copies together, preserving the existing header and
 * padding. Used after a trade so backup recovery retains the traded cards. */
int SaveSlots_WriteState(int slot, const unsigned char state[SAVE_SLOT_STATE_SIZE]);
/* Slot files written so far, by any path: the menu, an autosave, the
 * account's copy. */
unsigned SaveSlots_WriteCount(void);
/* Why the last write failed, "<path>: <reason>." (Paths_WriteError). */
const char *SaveSlots_LastError(void);
/* Why the last unreadable slot SaveSlots_Scan met could not be read,
 * "<path>: <reason>.", or "" when it met none. */
const char *SaveSlots_ReadError(void);
/* The slot's token, or 0 when it has none (empty, or saved by an older
 * build). */
unsigned SaveSlots_Token(int slot);
/* The same, -1 when the slot's file is there but cannot be read (its token
 * is then not known), 0 otherwise. */
int SaveSlots_ReadToken(int slot, unsigned *token);
/* Copy the save named `name` off the memory card images into slots 1 and
 * 2, once: only when the saves directory does not exist yet. */
void SaveSlots_ImportMemoryCards(const char *name);
/* Where in `data` the next save of the game starts, at or after `from`: a
 * block that opens with the "SC" title header and holds a state that passes
 * `check` (either copy) and has a deck (zeros pass the check). -1 when there is none. Every format the emulators
 * and card managers use (raw .mcr/.mcd/.srm images, .gme, .vgs, .vmp, and
 * the single saves .mcs/.psx/.psv) keeps the game's one block whole, after
 * a header of its own size, so this finds it without knowing the format. */
long SaveSlots_FindSave(unsigned char *data, size_t size, size_t from, SaveSlotCheck check);
/* Nonzero for a file name the import takes: a memory card or single-save
 * extension (SaveSlots_ImportFolder), in any case. */
int SaveSlots_ImportName(const char *name);
/* Import the saves in the memory card files in the saves folder into empty
 * slots, the same block the game would read off a card. A file whose saves
 * are all in slots now (copied, or there already) is renamed to
 * <name>.imported. `message` gets what to tell the player, or "" when there
 * was no such file. Returns the first slot filled, or -1. */
int SaveSlots_ImportFolder(SaveSlotCheck check, char *message, size_t size);
/* The player name of a state, in ASCII (full-width letters, digits and the
 * usual punctuation; anything else becomes '?'). */
void SaveSlots_StateName(const unsigned char *state, char *out, size_t size);
/* One full-width Shift-JIS character (or ASCII) as that name reads it. */
char SaveSlots_Ascii(unsigned sjis);

#endif
