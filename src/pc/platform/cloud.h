#ifndef MEMORIES_PC_PLATFORM_CLOUD_H
#define MEMORIES_PC_PLATFORM_CLOUD_H
/* The saves on the online account (server/worker.js, Lobby.salvataggi),
 * over the lobby's connection (lobby.h). Every slot file the game writes
 * (the slot menu, the autosave mod, anything through save_slots.h) goes to
 * the server in the background; the server numbers the versions and keeps
 * the last ten of each slot. When the lobby joins, each slot is compared:
 * a newer version on the account replaces a local save that did not change
 * since this device last sent or took one (the local one first copied to
 * saves/backup/), and when both changed the player chooses. What this device
 * last sent or took, per slot, is in saves/cloud.txt (the account's version
 * and the file's hash): a save written without internet differs from it, so
 * it still goes once there is a link, after a restart too.
 *
 * For tests, MEMORIES_CLOUD_TEST_CHOICE=local|server answers a conflict by
 * itself, two seconds after the question is up. */

#include "pc/mods/json.h"
#include <stddef.h>

/* The account `name` is known (start, made, logged in): watches the slot
 * writes and reads saves/cloud.txt. */
void Cloud_Start(const char *name);
/* The lobby joined (its first player list on a connection), or lost it. */
void Cloud_Joined(void);
void Cloud_Lost(void);
/* One of the server's messages of the saves; 1 when it was one. */
int Cloud_Handle(const char *type, const JsonValue *root);
/* Every frame, on the main thread, while the lobby runs. */
void Cloud_Frame(void);
/* "Salvataggi sull'account": each slot on the account and what is left to
 * send. */
void Cloud_Status(char *text, size_t size);

#endif
