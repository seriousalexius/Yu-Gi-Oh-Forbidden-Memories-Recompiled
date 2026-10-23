#ifndef MEMORIES_MOD_EVENTS_H
#define MEMORIES_MOD_EVENTS_H
#include "modapi.h"
int Mods_Subscribe(int owner, unsigned event, int priority, MemoriesModCallback callback);
void Mods_Unsubscribe(int owner, int token);
void Mods_ClearHooks(int owner);
int Mods_RegisterState(int owner, void *data, size_t size, unsigned version);
/* What mods share with each other (host->provide/find). */
int Mods_Provide(int owner, const char *name, void *pointer);
void *Mods_Find(const char *qualified);
/* The port's own, which mods find as "port:<name>" (the arena online's
 * session, pc/debug/netplay.h); NULL takes it back. */
void Mods_ProvidePort(const char *name, void *pointer);
void Mods_VisitState(void (*visit)(int owner, void *data, size_t size, unsigned version, void *context), void *context);
/* Any applied mod observes or replaces this gameplay event. */
int Mods_HasSubscribers(unsigned event);
/* Subscription `index` (from 0) of any mod: 0 past the last, else 1 with its
 * mod and event (the Mods window's overlaps). */
int Mods_SubscriptionAt(int index, int *owner, unsigned *event);
#endif
