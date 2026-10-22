/* Online duels in lockstep (netplay.h). */
#include "pc/compat/fs.h"
#include "netplay.h"
#include "netplay_net.h"
#include "pc/cards/fusion_helper.h"
#include "pc/debug/monitor.h"
#include "pc/guest/state.h"
#include "pc/memory_map.h"
#include "pc/platform/menu.h"
#include "pc/platform/paths.h"
#include "pc/platform/platform.h"
#include "pc/platform/title_jump.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PROTOCOL 1
#define SAVE_SIZE 0x680
#define SLOT_STATE_OFFSET 0x200 /* save_slots.h: the header, then the state and its duplicate */
#define RING 256                /* VBlanks of input kept; the other game is at most 2 * delay ahead */
#define CHECKS 64
#define WAIT_WARN_MS 5000
#define WAIT_GIVE_UP_MS 60000
/* The sound driver's work area (g_SDValue), which the real-time mixer
 * writes: the same game puts other values there (notes/agent-control.md). */
#define SOUND_FROM 0x001E0384u
#define SOUND_TO 0x001E1B44u

/* The messages, each a type byte and a 16-bit little-endian length. */
enum { MSG_HELLO = 'H', MSG_SAVE = 'S', MSG_INPUT = 'I', MSG_CHECK = 'K', MSG_BYE = 'B' };

typedef struct {
    uint32_t index, frame;
    uint64_t hash;
} Check;

static int active, side, peer_gone, jump_asked, desync_seen, hide = 1, check_ram, websocket;
static unsigned delay = 4, check_every = NETPLAY_CHECK_EVERY, desyncs;
static unsigned waits;     /* VBlanks that waited for the other game's input */
static uint64_t waited_ms, longest_wait_ms, active_since; /* is the delay enough? */
static uint32_t seed, peer_build;
static unsigned vblank_index, next_check;
static unsigned char saves[2][SAVE_SIZE];
static int have_save[2], have_hello;
static char peer_facts[2048];
static uint16_t local_bits[RING], remote_bits[RING];
static uint32_t remote_index[RING];
static uint8_t remote_have[RING];
static Check own_checks[CHECKS], peer_checks[CHECKS];
static unsigned own_count, peer_count;
static unsigned char inbox[16384];
static size_t inbox_length;

static uint64_t now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void put32(unsigned char *at, uint32_t value)
{
    at[0] = (unsigned char)value, at[1] = (unsigned char)(value >> 8);
    at[2] = (unsigned char)(value >> 16), at[3] = (unsigned char)(value >> 24);
}

static uint32_t get32(const unsigned char *at)
{
    return at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
}

static void lost(const char *why);

static void send_message(int type, const void *payload, size_t length)
{
    unsigned char head[3];
    if (peer_gone) return;
    head[0] = (unsigned char)type;
    head[1] = (unsigned char)length, head[2] = (unsigned char)(length >> 8);
    if (NetplayNet_Send(head, sizeof(head)) || (length && NetplayNet_Send(payload, length))) lost("the connection is gone");
}

/* What send_message gathered, out as one WebSocket message (the server's
 * plan counts them); a plain connection has sent it already. */
static void flush(void)
{
    if (!peer_gone && NetplayNet_Flush()) lost("the connection is gone");
}

static uint64_t mix(uint64_t hash, uint32_t address, size_t size)
{
    const unsigned char *at = (const unsigned char *)(uintptr_t)address;
    while (size--) {
        hash ^= *at++;
        hash *= 0x100000001b3ull;
    }
    return hash;
}

/* What the duel's rules run on, the same in both games even when each draws
 * the other's hand as backs (Cheats_CardViewMode): the cards' records (from
 * their ids, the display pointers before them left out, and without the
 * display marker the hand's view sets, duel_card_layout.h), the decks, the
 * two sides (life points...), the random seed, the turn, the duel's phase
 * and the main mode. */
static uint64_t duel_hash(void)
{
    uint64_t hash = 0xcbf29ce484222325ull;
    unsigned record;
    for (record = 0; record < 30; record++) {
        uint32_t at = 0x801A7AD8u + record * 0x1C;
        unsigned char flags_high = *(const unsigned char *)(uintptr_t)(at + 0x17) & (unsigned char)~0x20;
        hash = mix(hash, at + 0x08, 0x0F);
        hash ^= flags_high, hash *= 0x100000001b3ull;
        hash = mix(hash, at + 0x18, 4);
    }
    hash = mix(hash, 0x801A7E20u, 0x801A8000u - 0x801A7E20u); /* gDuel_aDeckCardRecords */
    hash = mix(hash, 0x800E9FF0u, 0x40);                       /* D_800E9FF0[2] */
    hash = mix(hash, 0x800FE6F8u, 4);                          /* gRand_dwSeed */
    hash = mix(hash, 0x8009B1D5u, 1);                          /* the turn */
    hash = mix(hash, 0x8009B23Au, 2);                          /* gDuel_wSceneStateFlags */
    return mix(hash, 0x8009B26Cu, 1);                          /* the main mode */
}

/* Guest RAM, word by word (FNV-1a over 32-bit words), but the sound area:
 * only while both games draw the same picture (MEMORIES_NETPLAY_HIDE=0),
 * which the display lists in RAM follow. */
static uint64_t ram_hash(void)
{
    const uint32_t *ram = (const uint32_t *)(uintptr_t)MEMORIES_GUEST_RAM;
    uint64_t hash = 0xcbf29ce484222325ull;
    uint32_t at;
    for (at = 0; at < MEMORIES_GUEST_RAM_SIZE; at += 4) {
        if (at >= SOUND_FROM && at < SOUND_TO) continue;
        hash ^= ram[at / 4];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

/* The first mismatch: said once, with this game's RAM beside the log, so
 * the two dumps can be compared. */
static void desync(const Check *mine, uint64_t theirs)
{
    char path[600], name[64];
    FILE *file;
    if (desyncs++ < 20)
        fprintf(stderr, "memories-pc: online: DESYNC at VBlank %u frame %u: ours %016llx, theirs %016llx\n",
                (unsigned)mine->index, (unsigned)mine->frame, (unsigned long long)mine->hash,
                (unsigned long long)theirs);
    if (desync_seen) return;
    desync_seen = 1;
    snprintf(name, sizeof(name), "netplay-desync-p%d.ram", side);
    if (Paths_User(path, sizeof(path), name) == 0 && (file = fopen(path, "wb")) != NULL) {
        fwrite((const void *)(uintptr_t)MEMORIES_GUEST_RAM, 1, MEMORIES_GUEST_RAM_SIZE, file);
        fclose(file);
        fprintf(stderr, "memories-pc: online: RAM written to %s\n", path);
    }
}

static void compare_checks(void)
{
    unsigned i, j;
    for (i = 0; i < peer_count; i++) {
        const Check *theirs = &peer_checks[i];
        for (j = 0; j < own_count; j++) {
            const Check *mine = &own_checks[j];
            if (mine->index != theirs->index || mine->frame != theirs->frame) continue;
            if (mine->hash != theirs->hash) desync(mine, theirs->hash);
            else if (mine->index % (check_every * 10) < check_every)
                fprintf(stderr,
                        "memories-pc: online: in step at VBlank %u after %llu ms (waited at %u VBlanks, %llu ms in all, "
                        "%llu at most)\n",
                        (unsigned)mine->index, (unsigned long long)(now_ms() - active_since), waits,
                        (unsigned long long)waited_ms, (unsigned long long)longest_wait_ms);
            /* Matched: drop it from the other game's list. */
            peer_checks[i] = peer_checks[--peer_count];
            i--;
            break;
        }
    }
}

static void notice_closed(int button, int *quit)
{
    (void)button, (void)quit;
}

static void lost(const char *why)
{
    static const char *const ok[] = {"OK"};
    if (peer_gone) return;
    peer_gone = 1;
    NetplayNet_Close();
    fprintf(stderr, "memories-pc: online: %s; the other player's pad stays idle\n", why);
    Menu_ShowNotice("Online", "L'altro giocatore si e' disconnesso.", ok, 1, 0, notice_closed);
}

static void handle(int type, const unsigned char *payload, size_t length)
{
    switch (type) {
    case MSG_HELLO:
        if (length >= 12) {
            unsigned their_delay = get32(payload + 8);
            if (get32(payload) != PROTOCOL) fprintf(stderr, "memories-pc: online: the other game speaks another protocol\n");
            peer_build = get32(payload + 4);
            if (their_delay > delay && their_delay < RING / 4) delay = their_delay;
            snprintf(peer_facts, sizeof(peer_facts), "%.*s", (int)(length - 12), (const char *)payload + 12);
            have_hello = 1;
        }
        break;
    case MSG_SAVE:
        if (length == SAVE_SIZE) {
            memcpy(saves[2 - side], payload, SAVE_SIZE);
            have_save[2 - side] = 1;
        }
        break;
    case MSG_INPUT:
        if (length == 6) {
            uint32_t index = get32(payload);
            remote_bits[index % RING] = (uint16_t)(payload[4] | payload[5] << 8);
            remote_index[index % RING] = index;
            remote_have[index % RING] = 1;
        }
        break;
    case MSG_CHECK:
        if (length == 16 && peer_count < CHECKS) {
            Check *check = &peer_checks[peer_count++];
            check->index = get32(payload);
            check->frame = get32(payload + 4);
            check->hash = get32(payload + 8) | (uint64_t)get32(payload + 12) << 32;
            compare_checks();
        }
        break;
    case MSG_BYE:
        lost("the other player left");
        break;
    default:
        break;
    }
}

/* Take what came in, waiting up to timeout_ms for something. -1 when the
 * connection is gone. */
static int receive(int timeout_ms)
{
    long got;
    size_t at = 0;
    if (peer_gone) return -1;
    got = NetplayNet_Receive(inbox + inbox_length, sizeof(inbox) - inbox_length, timeout_ms);
    if (got < 0) {
        lost("the connection is gone");
        return -1;
    }
    inbox_length += (size_t)got;
    while (inbox_length - at >= 3) {
        size_t length = inbox[at + 1] | (size_t)inbox[at + 2] << 8;
        if (inbox_length - at < 3 + length) break;
        handle(inbox[at], inbox + at + 3, length);
        at += 3 + length;
    }
    memmove(inbox, inbox + at, inbox_length - at);
    inbox_length -= at;
    return 0;
}

/* The relay's answer, a line: "WAIT" while alone in the room, then
 * "GO <seed>" once both are in. What follows it is the other game's. */
static int wait_for_pair(void)
{
    char line[128];
    size_t length = 0;
    uint64_t started = now_ms();
    while (now_ms() - started < 600000u) {
        long got = NetplayNet_Receive(line + length, 1, 100);
        if (got < 0) return -1;
        Platform_ControlIdle();
        Platform_PumpEvents();
        if (Platform_ShouldQuit()) return -1;
        if (!got) continue;
        if (line[length] != '\n') {
            if (length + 1 < sizeof(line)) length++;
            continue;
        }
        line[length] = '\0';
        length = 0;
        if (!strncmp(line, "GO ", 3)) {
            seed = (uint32_t)strtoul(line + 3, NULL, 10);
            return 0;
        }
        if (!strcmp(line, "WAIT")) fprintf(stderr, "memories-pc: online: waiting for the other player\n");
        else fprintf(stderr, "memories-pc: online: the relay says: %s\n", line);
    }
    return -1;
}

static int read_save(const char *path)
{
    unsigned char slot[SLOT_STATE_OFFSET + 2 * SAVE_SIZE];
    FILE *file = path && *path ? fopen(path, "rb") : NULL;
    size_t got = file ? fread(slot, 1, sizeof(slot), file) : 0;
    int copy, card;
    if (file) fclose(file);
    if (got != sizeof(slot)) {
        fprintf(stderr, "memories-pc: online: cannot read the save %s\n", path ? path : "(none)");
        return -1;
    }
    /* The first copy whose deck is whole (40 cards, as the 2P load wants). */
    for (copy = 0; copy < 2; copy++) {
        const unsigned char *state = slot + SLOT_STATE_OFFSET + copy * SAVE_SIZE;
        for (card = 0; card < 40 && (state[2 * card] | state[2 * card + 1]); card++) {}
        if (card == 40) {
            memcpy(saves[side - 1], state, SAVE_SIZE);
            have_save[side - 1] = 1;
            return 0;
        }
    }
    fprintf(stderr, "memories-pc: online: the save %s has no whole deck\n", path);
    return -1;
}

void Netplay_Init(void)
{
    const char *config = getenv("MEMORIES_NETPLAY"), *wanted_delay = getenv("MEMORIES_NETPLAY_DELAY");
    char host[256], room[64];
    unsigned port = 0;
    unsigned char hello[12 + 1024];
    size_t facts_length;
    uint64_t started;
    if (!config || !*config) return;
    if (!strncmp(config, "ws://", 5)) {
        /* ws://host[:port]/room/side: the server (server/worker.js), which
         * adds about 60 ms each way, hence a longer delay. */
        const char *rest = config + 5;
        char *end;
        websocket = 1;
        delay = 6;
        port = 80;
        host[0] = '\0';
        if (sscanf(rest, "%255[^:/]", host) == 1) rest += strlen(host);
        if (*rest == ':') {
            port = (unsigned)strtoul(rest + 1, &end, 10);
            rest = end;
        }
        if (!host[0] || !port || sscanf(rest, "/%63[^/]/%d", room, &side) != 2 || (side != 1 && side != 2)) {
            fprintf(stderr, "memories-pc: online: MEMORIES_NETPLAY wants ws://host[:port]/room/side, not %s\n", config);
            return;
        }
    } else if (sscanf(config, "%255[^:]:%u/%63[^/]/%d", host, &port, room, &side) != 4 || (side != 1 && side != 2)) {
        fprintf(stderr, "memories-pc: online: MEMORIES_NETPLAY wants host:port/room/side, not %s\n", config);
        return;
    }
    if (wanted_delay && *wanted_delay) delay = (unsigned)strtoul(wanted_delay, NULL, 10);
    if (getenv("MEMORIES_NETPLAY_HIDE")) hide = strcmp(getenv("MEMORIES_NETPLAY_HIDE"), "0") != 0;
    check_ram = !hide;
    if (getenv("MEMORIES_NETPLAY_CHECK_EVERY")) check_every = (unsigned)strtoul(getenv("MEMORIES_NETPLAY_CHECK_EVERY"), NULL, 10);
    if (check_every < 1) check_every = 1;
    /* At least 2: the input goes out in pairs of VBlanks (Netplay_Pads). */
    if (delay < 2) delay = 2;
    if (delay >= RING / 4) delay = RING / 4 - 1;
    if (read_save(getenv("MEMORIES_NETPLAY_SAVE"))) return;
    if (websocket) {
        /* The room is in the path: no "YFM-NET" line. */
        char path[128];
        snprintf(path, sizeof(path), "/stanza/%s?lato=%d", room, side);
        if (NetplayNet_ConnectWebSocket(host, port, path, 10000)) return;
    } else {
        char line[160];
        if (NetplayNet_Connect(host, port, 10000)) return;
        snprintf(line, sizeof(line), "YFM-NET %d %s %d\n", PROTOCOL, room, side);
        if (NetplayNet_Send(line, strlen(line))) return;
    }
    if (wait_for_pair()) {
        fprintf(stderr, "memories-pc: online: no other player came\n");
        NetplayNet_Close();
        return;
    }
    /* Hello: the protocol, the build, the delay this game wants and its
     * facts (settings and mods), which the other compares. */
    put32(hello, PROTOCOL);
    put32(hello + 4, Memories_StateBuildId());
    put32(hello + 8, delay);
    {
        static char facts[4096];
        char *line, *next;
        size_t used = 0;
        Monitor_Facts(facts, sizeof(facts));
        for (line = facts; line && *line; line = next) {
            next = strchr(line, '\n');
            if (next) *next++ = '\0';
            if (strncmp(line, "settings: ", 10) && strncmp(line, "mods: ", 6)) continue;
            used += (size_t)snprintf((char *)hello + 12 + used, sizeof(hello) - 12 - used, "%s\n", line);
            if (used >= sizeof(hello) - 12) used = sizeof(hello) - 13;
        }
        facts_length = used;
    }
    send_message(MSG_HELLO, hello, 12 + facts_length);
    send_message(MSG_SAVE, saves[side - 1], SAVE_SIZE);
    flush();
    started = now_ms();
    while (!(have_hello && have_save[2 - side])) {
        if (receive(100) < 0 || now_ms() - started > 30000u || Platform_ShouldQuit()) {
            fprintf(stderr, "memories-pc: online: the other game did not say hello\n");
            NetplayNet_Close();
            return;
        }
        Platform_ControlIdle();
        Platform_PumpEvents();
    }
    if (peer_build != Memories_StateBuildId())
        fprintf(stderr, "memories-pc: online: the other game is another build (%08x, ours %08x): expect a desync\n",
                (unsigned)peer_build, (unsigned)Memories_StateBuildId());
    {
        char mine[1024];
        snprintf(mine, sizeof(mine), "%.*s", (int)facts_length, (const char *)hello + 12);
        if (strcmp(mine, peer_facts))
            fprintf(stderr, "memories-pc: online: the settings or mods differ:\n--- ours\n%s--- theirs\n%s", mine,
                    peer_facts);
    }
    active = 1;
    active_since = now_ms();
    fprintf(stderr, "memories-pc: online: player %d of room %s (%s), input delay %u, seed %u, %s, checks of %s\n",
            side, room, websocket ? "WebSocket" : "relay", delay, (unsigned)seed,
            hide ? "the other's hand hidden" : "both hands shown", check_ram ? "all RAM" : "the duel");
}

int Netplay_Active(void)
{
    return active;
}

int Netplay_Asked(void)
{
    const char *config = getenv("MEMORIES_NETPLAY");
    return config && *config;
}

int Netplay_Side(void)
{
    return active ? side : 0;
}

int Netplay_HidesOtherHand(void)
{
    return active && hide;
}

/* What the other player looks at in their turn (D_8009B1D5) of a
 * two-player duel (main mode 3, a negative gDuel_bOpponentID): COVER_STAR
 * while they choose a guardian star (phase 8 of gDuel_wSceneStateFlags; the
 * dialog names the two stars of a card that may go down face down),
 * COVER_VIEWER while they hold a card up with Triangle (gDuel_bEffectState
 * 2, DUEL_EFFECT_STATE_CARD_VIEWER: the picture is a back, but the text
 * beside it gives the name, the stars and the description). */
enum { COVER_NONE, COVER_STAR, COVER_VIEWER };

static int covering(void)
{
    if (!active || !hide || (*(volatile uint8_t *)(uintptr_t)0x8009B26Cu & 31) != 3) return COVER_NONE;
    if (*(volatile int8_t *)(uintptr_t)0x8009B361u >= 0) return COVER_NONE;
    if (*(volatile uint8_t *)(uintptr_t)0x8009B1D5u == side - 1) return COVER_NONE;
    if ((*(volatile uint8_t *)(uintptr_t)0x8009B254u & 0x0F) == 2) return COVER_VIEWER;
    return (*(volatile uint16_t *)(uintptr_t)0x8009B23Au & 0xF) == 8 ? COVER_STAR : COVER_NONE;
}

unsigned Netplay_DrawSignature(void)
{
    int x, y, w, h, what = covering();
    if (!what) return 0;
    FusionHelper_GetViewport(&x, &y, &w, &h);
    return (((unsigned)x * 31u + (unsigned)y) * 31u + (unsigned)w * 7u + (unsigned)h) * 3u + (unsigned)what;
}

void Netplay_Draw(MenuCanvas *canvas, int *x, int *y, int *w, int *h)
{
    int vx, vy, vw, vh, wide, left, top, right, bottom, row, column, scale, width, what = covering();
    const char *text = what == COVER_STAR ? "L'avversario sceglie la stella guardiana" : "L'avversario guarda una carta";
    *x = *y = *w = *h = 0;
    if (!what) return;
    FusionHelper_GetViewport(&vx, &vy, &vw, &vh);
    if (vw <= 0 || vh <= 0) return;
    /* The dialog's box, in the game's 320x240 (centred when widescreen), or
     * the whole picture for the card viewer. */
    wide = Platform_Widescreen() ? 426 : 320;
    if (what == COVER_STAR) {
        left = vx + vw / 2 + (24 - 160) * vw / wide;
        right = vx + vw / 2 + (296 - 160) * vw / wide;
        top = vy + 102 * vh / 240;
        bottom = vy + 174 * vh / 240;
    } else {
        left = vx, right = vx + vw, top = vy, bottom = vy + vh;
    }
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > canvas->width) right = canvas->width;
    if (bottom > canvas->height) bottom = canvas->height;
    if (right <= left || bottom <= top) return;
    for (row = top; row < bottom; row++)
        for (column = left; column < right; column++) canvas->pixels[row * canvas->stride + column] = 0xFF18181Cu;
    scale = Menu_Scale();
    width = Menu_TextWidthScaled(text, scale);
    Menu_DrawTextScaled(canvas, left + (right - left - width) / 2, (top + bottom) / 2 - 6 * scale, text, 0xFFFFFFFFu,
                        scale);
    *x = left, *y = top, *w = right - left, *h = bottom - top;
}

uint32_t Netplay_Seed(void)
{
    return seed;
}

const unsigned char *Netplay_Save(int player)
{
    return active && (player == 1 || player == 2) && have_save[player - 1] ? saves[player - 1] : NULL;
}

void Netplay_Pads(uint16_t local, uint16_t both[2])
{
    unsigned index = vblank_index++, ahead = index + delay;
    uint16_t mine, theirs = 0;
    unsigned char message[6];
    local_bits[ahead % RING] = local;
    put32(message, ahead);
    message[4] = (unsigned char)local, message[5] = (unsigned char)(local >> 8);
    send_message(MSG_INPUT, message, sizeof(message));
    /* Two VBlanks to a message (half the WebSocket messages; the check of
     * Netplay_Point rides along): the even one waits for the odd one, which
     * the delay of 2 or more leaves time for. */
    if (ahead % 2) flush();
    mine = index < delay ? 0 : local_bits[index % RING];
    if (index >= delay && !peer_gone) {
        uint64_t started = 0, entered = now_ms();
        int warned = 0;
        while (!(remote_have[index % RING] && remote_index[index % RING] == index)) {
            uint64_t now;
            /* Before any wait, out with what is kept: the other game may be
             * waiting for it in turn. */
            flush();
            if (receive(2) < 0) break;
            if (remote_have[index % RING] && remote_index[index % RING] == index) break;
            /* Waiting on purpose: a present while waiting services the
             * clock, and the virtual clock would take a wait of a second
             * for a game that spins and step itself (platform_common.c). */
            Platform_ControlIdle();
            Platform_PumpEvents();
            if (Platform_ShouldQuit()) break;
            now = now_ms();
            if (!started) started = now;
            if (!warned && now - started > WAIT_WARN_MS) {
                warned = 1;
                fprintf(stderr, "memories-pc: online: waiting for the other player at VBlank %u\n", index);
            }
            if (now - started > WAIT_GIVE_UP_MS) {
                lost("the other player stopped answering");
                break;
            }
        }
        if (started) {
            uint64_t waited = now_ms() - entered;
            waits++, waited_ms += waited;
            if (waited > longest_wait_ms) longest_wait_ms = waited;
        }
        if (remote_have[index % RING] && remote_index[index % RING] == index) {
            theirs = remote_bits[index % RING];
            remote_have[index % RING] = 0;
        }
    }
    both[side - 1] = mine;
    both[2 - side] = theirs;
}

void Netplay_Point(unsigned frame)
{
    if (!active) return;
    if (!jump_asked) {
        char why[160];
        jump_asked = 1;
        if (TitleJump_RequestTo(JUMP_DUEL2P, 0, NULL, why, sizeof(why)))
            fprintf(stderr, "memories-pc: online: no jump to the duel: %s\n", why);
    }
    if (vblank_index >= next_check) {
        Check *check;
        unsigned char message[16];
        next_check = (vblank_index / check_every + 1) * check_every;
        if (own_count == CHECKS) memmove(own_checks, own_checks + 1, --own_count * sizeof(*own_checks));
        check = &own_checks[own_count++];
        check->index = vblank_index;
        check->frame = frame;
        check->hash = check_ram ? ram_hash() : duel_hash();
        put32(message, check->index);
        put32(message + 4, check->frame);
        put32(message + 8, (uint32_t)check->hash);
        put32(message + 12, (uint32_t)(check->hash >> 32));
        send_message(MSG_CHECK, message, sizeof(message));
        compare_checks();
    }
    if (!peer_gone) receive(0);
}
