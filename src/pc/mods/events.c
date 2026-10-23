/* Managed callbacks are ordered by priority, then registration order. A
 * snapshot makes subscribing/unsubscribing during dispatch well-defined. */
#include "events.h"
#include "../../types.h"
#include "mods.h"
#include "hooks.h"
#include <string.h>

typedef struct {
    int owner, token, priority;
    unsigned event;
    MemoriesModCallback callback;
} Hook;
typedef struct {
    void *data;
    size_t size;
    unsigned version;
} State;
/* Input can arrive from VBlank: dispatch must never allocate. Distinct event
 * kinds have separate snapshots; same-kind recursion is explicitly bypassed. */
#define HOOK_LIMIT 4096
static Hook hooks[HOOK_LIMIT], snapshots[MEMORIES_EVENT_COUNT][HOOK_LIMIT];
static int count, serial;
static volatile int mutating;
static State states[MODS_MAX];
static unsigned dispatching;
#define PROVIDED_MAX 1024
static struct { int owner; char name[64]; void *pointer; } provided[PROVIDED_MAX];
static int provided_count;
/* What the port itself shares, as "port:<name>" (Mods_ProvidePort). */
#define PORT_PROVIDED_MAX 16
static struct { char name[64]; void *pointer; } port_provided[PORT_PROVIDED_MAX];
static int port_count;

void Mods_ProvidePort(const char *name, void *pointer)
{
    int i;
    for (i = 0; i < port_count; i++)
        if (!strcmp(port_provided[i].name, name)) {
            port_provided[i].pointer = pointer;
            return;
        }
    if (port_count == PORT_PROVIDED_MAX || strlen(name) >= sizeof(port_provided[0].name))
        return;
    strcpy(port_provided[port_count].name, name);
    port_provided[port_count++].pointer = pointer;
}

int Mods_Subscribe(int owner, unsigned event, int priority, MemoriesModCallback callback)
{
    int i;
    Hook hook;
    if (owner < 0 || owner >= Mods_Count() || !callback || event >= MEMORIES_EVENT_COUNT || serial == 0x7fffffff)
        return 0;
    if (count == HOOK_LIMIT)
        return 0;
    mutating = 1;
    hook.owner = owner;
    hook.event = event;
    hook.priority = priority;
    hook.callback = callback;
    hook.token = ++serial;
    for (i = count; i > 0 && hooks[i - 1].priority < priority; i--)
        hooks[i] = hooks[i - 1];
    hooks[i] = hook;
    count++;
    __asm__ volatile("" ::: "memory");
    mutating = 0;
    return hook.token;
}
void Mods_Unsubscribe(int owner, int token)
{
    int i;
    for (i = 0; i < count; i++)
        if (hooks[i].owner == owner && hooks[i].token == token) {
            mutating = 1;
            memmove(hooks + i, hooks + i + 1, (size_t)(--count - i) * sizeof(*hooks));
            __asm__ volatile("" ::: "memory");
            mutating = 0;
            return;
        }
}
void Mods_ClearHooks(int owner)
{
    int i;
    for (i = count - 1; i >= 0; i--)
        if (hooks[i].owner == owner)
            Mods_Unsubscribe(owner, hooks[i].token);
    Hooks_Clear(owner);
    memset(&states[owner], 0, sizeof(states[owner]));
    for (i = provided_count - 1; i >= 0; i--)
        if (provided[i].owner == owner)
            provided[i] = provided[--provided_count];
}
int Mods_Provide(int owner, const char *name, void *pointer)
{
    int i;
    if (owner < 0 || owner >= Mods_Count() || !Mods_SettingKeyValid(name) || strlen(name) >= sizeof(provided[0].name))
        return 0;
    for (i = 0; i < provided_count; i++)
        if (provided[i].owner == owner && !strcmp(provided[i].name, name)) {
            provided[i].pointer = pointer;
            return 1;
        }
    if (provided_count == PROVIDED_MAX)
        return 0;
    provided[provided_count].owner = owner;
    strcpy(provided[provided_count].name, name);
    provided[provided_count++].pointer = pointer;
    return 1;
}
void *Mods_Find(const char *qualified)
{
    const char *colon = qualified ? strchr(qualified, ':') : NULL;
    int i;
    if (!colon)
        return NULL;
    if (!strncmp(qualified, "port:", 5)) {
        for (i = 0; i < port_count; i++)
            if (!strcmp(port_provided[i].name, qualified + 5))
                return port_provided[i].pointer;
        return NULL;
    }
    for (i = 0; i < provided_count; i++) {
        const char *id = Mods_Id(provided[i].owner);
        if (strlen(id) == (size_t)(colon - qualified) && !strncmp(id, qualified, (size_t)(colon - qualified)) &&
            !strcmp(provided[i].name, colon + 1))
            return provided[i].pointer;
    }
    return NULL;
}
int Mods_RegisterState(int owner, void *data, size_t size, unsigned version)
{
    if (owner < 0 || owner >= Mods_Count() || !data || !size || size > (1u << 20))
        return 0;
    states[owner].data = data;
    states[owner].size = size;
    states[owner].version = version;
    return 1;
}
void Mods_VisitState(void (*visit)(int, void *, size_t, unsigned, void *), void *context)
{
    int i;
    for (i = 0; i < Mods_Count(); i++)
        if (Mods_Active(i) && states[i].data)
            visit(i, states[i].data, states[i].size, states[i].version, context);
}
void Mods_Dispatch(MemoriesModEvent *event)
{
    Hook *snapshot;
    int i, n = count;
    unsigned bit;
    if (!event || event->type >= MEMORIES_EVENT_COUNT || !n || mutating)
        return;
    bit = 1u << event->type;
    if (dispatching & bit)
        return; /* Calling the original from a replacement cannot recurse. */
    snapshot = snapshots[event->type];
    memcpy(snapshot, hooks, (size_t)n * sizeof(*snapshot));
    dispatching |= bit;
    for (i = 0; i < n; i++)
        if (snapshot[i].event == event->type && Mods_Active(snapshot[i].owner)) {
            int j;
            MemoriesModEvent before = *event;
            for (j = 0; j < count; j++)
                if (hooks[j].token == snapshot[i].token)
                    break;
            if (j == count)
                continue;
            snapshot[i].callback(event);
            event->type = before.type;
            event->phase = before.phase;
            if (before.phase == MEMORIES_AFTER)
                *event = before; /* after hooks observe */
            if (event->handled && event->phase == MEMORIES_BEFORE)
                break; /* first replacement wins */
        }
    dispatching &= ~bit;
}
int Mods_Notify(unsigned type, int a, int b, int c)
{
    MemoriesModEvent event = {type, MEMORIES_BEFORE, a, b, c, 0, 0};
    Mods_Dispatch(&event);
    return event.handled;
}

int Mods_DamageLife(int side, int life, int damage, int kind)
{
    MemoriesModEvent event = {MEMORIES_EVENT_DAMAGE, MEMORIES_BEFORE, side, damage, kind, life, 0};
    Mods_Dispatch(&event);
    if (!event.handled) {
        int amount = event.b < 0 ? 0 : event.b;
        event.result = amount > life ? 0 : life - amount;
    }
    if (event.result < 0)
        event.result = 0;
    if (event.result > 32767)
        event.result = 32767;
    event.phase = MEMORIES_AFTER;
    Mods_Dispatch(&event);
    return event.result;
}

/* End-of-duel StarChip award (func_800218F0). Before hooks may edit a or
 * handle to skip; after sees result as the amount actually credited. */
int Mods_AwardStarchips(unsigned *balance, int prize)
{
    MemoriesModEvent event = {MEMORIES_EVENT_STARCHIP, MEMORIES_BEFORE, prize, 0, 0, 0, 0};
    unsigned before;

    if (!balance)
        return 0;
    before = *balance;
    Mods_Dispatch(&event);
    if (!event.handled) {
        unsigned long long total;

        if (event.a < 0)
            event.a = 0;
        total = (unsigned long long)*balance + (unsigned long long)event.a;
        /* 999999, or a mod's "limits" (pc/cards/tables.h). A prize never
           takes starchips away: a balance a mod let past the cap, played
           without it (or with a lower one), stays, as the chest's count does. */
        unsigned long long cap = (unsigned long long)Mods_Limit("starchips", 999999);
        if (cap < *balance)
            cap = *balance;
        *balance = total > cap ? (unsigned)cap : (unsigned)total;
    }
    event.result = (int)(*balance - before);
    event.phase = MEMORIES_AFTER;
    Mods_Dispatch(&event);
    return (int)*balance;
}

int Mods_SubscriptionAt(int index, int *owner, unsigned *event)
{
    if (index < 0 || index >= count) return 0;
    *owner = hooks[index].owner;
    *event = hooks[index].event;
    return 1;
}
int Mods_HasSubscribers(unsigned event)
{
    int i;
    for (i = 0; i < count; i++)
        if (hooks[i].event == event && Mods_Active(hooks[i].owner)) return 1;
    return 0;
}
