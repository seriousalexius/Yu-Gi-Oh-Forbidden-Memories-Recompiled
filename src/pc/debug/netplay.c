/* Online duels in lockstep (netplay.h). */
#include "pc/compat/fs.h"
#include "netplay.h"
#include "netplay_net.h"
#include "pc/cards/deck_rules.h"
#include "pc/cards/fusion_helper.h"
#include "pc/debug/monitor.h"
#include "pc/guest/state.h"
#include "pc/memory_map.h"
#include "pc/mods/events.h"
#include "pc/platform/bet.h"
#include "pc/platform/menu.h"
#include "pc/platform/paths.h"
#include "pc/platform/platform.h"
#include "pc/platform/title_jump.h"
#include "pc/saves/online_deck.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef MEMORIES_IOS
#include <unistd.h>
#define BACK_LABEL "Chiudi il gioco" /* the app closes: restart_or_close */
#else
#define BACK_LABEL "Torna al gioco"
#endif

#define PROTOCOL 2
#define SAVE_SIZE 0x680
#define RING 256                /* VBlanks of input kept; the other game is at most 2 * GAP_FAST ahead */
/* How far ahead of its VBlank an input goes (Netplay_Pads), which bounds
 * how fast the two games can go: GAP_FAST VBlanks in the duel's animations,
 * which run flat out, so that the network keeps up with them; while someone
 * chooses, at 100%, the agreed delay, longer (up to GAP_SLOW_MAX) when the
 * waits say the network wants it. Each game picks its own: every input says
 * the VBlank it is for. */
#define GAP_FAST 48
#define GAP_SLOW_MAX 12
#define FLUSH_MS 20 /* what is kept goes out at least this often */
#define CHECKS 64
#define WAIT_WARN_MS 5000
#define WAIT_GIVE_UP_MS 60000
/* The sound driver's work area (g_SDValue), which the real-time mixer
 * writes: the same game puts other values there (notes/agent-control.md). */
#define SOUND_FROM 0x001E0384u
#define SOUND_TO 0x001E1B44u

/* The messages, each a type byte and a 16-bit little-endian length. */
enum { MSG_HELLO = 'H', MSG_SAVE = 'S', MSG_INPUT = 'I', MSG_CHECK = 'K', MSG_BYE = 'B', MSG_RESIGN = 'R', MSG_DECK = 'D' };
#define DECK_BYTES 80 /* the save state's first 80 bytes: the deck, 40 halfwords */

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
static int from_lobby; /* asked by online.cfg, which the lobby wrote (read_asked) */
static char opponent[64]; /* its "contro=" line */
static int over, test_back; /* the duel's result said (duel_over) */
static unsigned test_resign;
static uint64_t over_at;
/* The arena online (netplay.h): the flags that ride with the input (1 =
 * ready for the next duel), the jump both flags ask for, the duel running. */
static NetplayArena arena;
static int arena_mode, arena_ready, arena_deck_sent, arena_both, arena_go, arena_in_duel, arena_live, arena_waiting;
static unsigned arena_go_index, test_series;
static uint64_t arena_notice_at;
static uint8_t local_flags[RING], remote_flags[RING];
/* The gap (GAP_FAST): the last VBlank our input went out for, the buttons
 * kept for the next while the gap shrinks, the gap while choosing and its
 * window of waits; the rate the duel runs at (-1 flat out) and the phase
 * log (MEMORIES_NETPLAY_PHASES=1). */
static unsigned last_target, gap_slow, window_frames, phase_key = ~0u;
static uint16_t pending_bits;
static uint64_t last_flush_ms, window_started, window_waited;
static int phase_log;

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
static void lobby_failed_closed(int button, int *quit);
static void duel_over(int won, const char *how);

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
    last_flush_ms = now_ms();
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
    if (over) return;
    if (active) {
        duel_over(1, "disconnect"); /* the one who stays wins */
        return;
    }
    if (from_lobby) {
        /* From the lobby: a restart is the way back to the game as it was. */
        static const char *const buttons[] = {BACK_LABEL, "Resta qui"};
        Menu_ShowNotice("Online", "L'altro giocatore si e' disconnesso.\nTorna al gioco normale (il gioco si riavvia)?",
                        buttons, 2, 0, lobby_failed_closed);
        return;
    }
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
        if (length == 6 || length == 7) {
            uint32_t index = get32(payload);
            remote_bits[index % RING] = (uint16_t)(payload[4] | payload[5] << 8);
            remote_flags[index % RING] = length == 7 ? payload[6] : 0;
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
    case MSG_DECK:
        if (length == DECK_BYTES && arena_mode) {
            memcpy(saves[2 - side], payload, DECK_BYTES);
            fprintf(stderr, "memories-pc: online: arena: the other player's deck came\n");
        }
        break;
    case MSG_BYE:
        lost("the other player left");
        break;
    case MSG_RESIGN:
        if (!over) fprintf(stderr, "memories-pc: online: the other player resigns\n");
        duel_over(1, "resign");
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

/* Where the duel is asked: MEMORIES_NETPLAY (and _SAVE) first, which the
 * tests use; else online.cfg in the user folder, which the lobby writes
 * before it restarts the game (pc/platform/lobby.c): lines "netplay=..."
 * and "save=...". The file is read once and removed at once, so the next
 * start is a normal one. */
static char asked_config[512], asked_save[1024];
static int asked_read;

static void read_asked(void)
{
    const char *config = getenv("MEMORIES_NETPLAY");
    char path[1024], line[1100];
    FILE *file;
    if (asked_read) return;
    asked_read = 1;
    if (config && *config) {
        const char *save = getenv("MEMORIES_NETPLAY_SAVE");
        snprintf(asked_config, sizeof(asked_config), "%s", config);
        snprintf(asked_save, sizeof(asked_save), "%s", save ? save : "");
        arena_mode = getenv("MEMORIES_NETPLAY_ARENA") && !strcmp(getenv("MEMORIES_NETPLAY_ARENA"), "1");
        return;
    }
    if (Paths_User(path, sizeof(path), "online.cfg")) return;
    file = fopen(path, "rb");
    if (!file) return;
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!strncmp(line, "netplay=", 8)) snprintf(asked_config, sizeof(asked_config), "%s", line + 8);
        else if (!strncmp(line, "save=", 5)) snprintf(asked_save, sizeof(asked_save), "%s", line + 5);
        else if (!strncmp(line, "contro=", 7)) snprintf(opponent, sizeof(opponent), "%s", line + 7);
        else if (!strcmp(line, "modo=arena")) arena_mode = 1;
    }
    fclose(file);
    remove(path);
    from_lobby = asked_config[0] != '\0';
    fprintf(stderr, "memories-pc: online: from online.cfg: %s, save %s\n", asked_config, asked_save);
    if (opponent[0]) fprintf(stderr, "memories-pc: online: opponent %s\n", opponent);
}

/* The duel from the lobby did not start: the game goes on, with the game's
 * own title (the mods' comes back on a restart). */
/* An iPhone app cannot start itself again (guest/main.c): it closes, and
 * opening it again is the restart (what the game keeps is on disk). */
static int restart_or_close(void)
{
    if (Platform_RestartGame() >= 0) return 0;
#ifdef MEMORIES_IOS
    fprintf(stderr, "memories-pc: online: closing the app (an iPhone app cannot restart itself)\n");
    fflush(NULL);
    _exit(0);
#endif
    return -1;
}

static void lobby_failed_closed(int button, int *quit)
{
    (void)quit;
    if (button == 0) restart_or_close();
}

static void lobby_failed(void)
{
    static const char *const buttons[] = {"Riavvia", "Resta qui"};
    if (!from_lobby) return;
    Menu_ShowNotice("Online",
                    "Il duello online non e' partito (l'altro giocatore non e' arrivato o manca internet).\n"
                    "Riavvia per tornare al gioco normale.",
                    buttons, 2, 0, lobby_failed_closed);
}

/* The duel's line in lobby.txt (pc/platform/lobby.c), which the lobby tells
 * the server on its next start: "in_corso=<name>" while the duel runs,
 * which a game that never got to the end (closed, crashed) counts as lost,
 * then "risultato=vinto,<name>" or "risultato=perso,<name>" in its place.
 * The account's lines and results not yet told stay. Duels from the lobby
 * only. */
static void note_result(const char *line)
{
    char path[1024], kept[4096], row[256];
    size_t used = 0;
    FILE *file;
    if (!from_lobby || Paths_User(path, sizeof(path), "lobby.txt") || !(file = fopen(path, "rb"))) return;
    kept[0] = '\0';
    while (fgets(row, sizeof(row), file)) {
        row[strcspn(row, "\r\n")] = '\0';
        if (!row[0] || !strncmp(row, "in_corso=", 9) || used + strlen(row) + 2 > sizeof(kept)) continue;
        used += (size_t)snprintf(kept + used, sizeof(kept) - used, "%s\n", row);
    }
    fclose(file);
    if (!(file = fopen(path, "wb"))) {
        fprintf(stderr, "memories-pc: online: cannot write lobby.txt\n");
        return;
    }
    fprintf(file, "%s%s\n", kept, line);
    fclose(file);
}

static void back_to_game(void)
{
    fprintf(stderr, "memories-pc: online: back to the game\n");
    fflush(stderr);
    if (restart_or_close() < 0) fprintf(stderr, "memories-pc: online: the game did not restart\n");
}

static void over_closed(int button, int *quit)
{
    (void)quit;
    if (from_lobby && button == 0) back_to_game();
}

/* The result, once: how is lp, exodia, deck, resign or disconnect. The
 * lockstep goes on behind the notice; from the lobby its button restarts
 * the game, which comes back as it was (online.cfg is gone). */
static void duel_over(int won, const char *how)
{
    static const char *const back[] = {BACK_LABEL}, *const ok[] = {"OK"};
    char text[700], line[96], wager[300];
    const char *said = !strcmp(how, "exodia") ? "Exodia" : !strcmp(how, "deck") ? "mazzo finito"
                     : !strcmp(how, "resign") ? (won ? "si e' arreso" : "ti sei arreso")
                     : !strcmp(how, "disconnect") ? "si e' disconnesso" : "punti vita a zero";
    if (over) return;
    over = 1;
    over_at = now_ms();
    arena.over = 1;
    arena_waiting = 0;
    fprintf(stderr, "memories-pc: online: duel over: you %s (%s)%s\n", won ? "won" : "lost", how,
            arena_mode ? " (the arena's series)" : "");
    snprintf(line, sizeof(line), "risultato=%s,%s", won ? "vinto" : "perso", opponent);
    if (!arena_mode) {
        /* fork: the legendary cards each deck had, for the server's 1v1
         * statistics (how much they weigh in who wins) */
        unsigned short decks[2][40];
        size_t at = strlen(line);
        memcpy(decks[0], saves[side - 1], sizeof(decks[0]));
        memcpy(decks[1], saves[2 - side], sizeof(decks[1]));
        snprintf(line + at, sizeof(line) - at, ";%d;%d", DeckRules_Legendary(decks[0]), DeckRules_Legendary(decks[1]));
    }
    note_result(line);
    /* fork: the bet's stakes, which the next start puts in the save (bet.h) */
    if (from_lobby) Bet_Describe(won, wager, sizeof(wager));
    else wager[0] = '\0';
    if (arena_mode)
        snprintf(text, sizeof(text), "%s la serie d'arena contro %s%s (%s; serie %d-%d)%s%s",
                 won ? "Hai vinto" : "Hai perso", opponent[0] ? opponent : "l'avversario", won ? "!" : ".", said,
                 arena.wins[0], arena.wins[1], wager,
                 from_lobby ? "\n\nTorna al gioco normale (il gioco si riavvia)." : "");
    else
        snprintf(text, sizeof(text), "%s %s%s (%s)%s%s", won ? "Hai vinto contro" : "Hai perso contro",
                 opponent[0] ? opponent : "l'avversario", won ? "!" : ".", said, wager,
                 from_lobby ? "\n\nTorna al gioco normale (il gioco si riavvia)." : "");
    Menu_ShowNotice("Online", text, from_lobby ? back : ok, 1, 0, over_closed);
}

/* This player gives up: the other game hears it, and both say the result. */
static void resign(void)
{
    if (!active || over) return;
    fprintf(stderr, "memories-pc: online: I resign\n");
    send_message(MSG_RESIGN, NULL, 0);
    flush();
    duel_over(0, "resign");
}

static void resign_chosen(int button, int *quit)
{
    (void)quit;
    if (button == 0) resign();
}

int Netplay_AskResign(void)
{
    static const char *const buttons[] = {"Arrenditi", "Continua"};
    if (!active || over) return 0;
    /* Continua is focused and last: Enter, Esc and Circle keep playing. */
    Menu_ShowNotice("Online", arena_mode ? "Vuoi arrenderti? Perdi la serie d'arena." : "Vuoi arrenderti? Conta come sconfitta.",
                    buttons, 2, 1, resign_chosen);
    return 1;
}

/* --- the arena online (netplay.h) --- */

static void arena_series_notice(const char *how_text);

static void arena_waiting_chosen(int button, int *quit)
{
    (void)quit;
    (void)button;
    if (arena_waiting && !over) resign(); /* its one button: Ritirati */
}

static void arena_wait_notice(void)
{
    static const char *const buttons[] = {"Ritirati"};
    char text[200];
    snprintf(text, sizeof(text), "Pronto. Aspetto %s...\n\nSerie: tu %d - %d", opponent[0] ? opponent : "l'avversario",
             arena.wins[0], arena.wins[1]);
    arena_waiting = 1;
    Menu_ShowNotice("Arena online", text, buttons, 1, 0, arena_waiting_chosen);
}

static void arena_next(void)
{
    arena_ready = 1;
    fprintf(stderr, "memories-pc: online: arena: ready for duel %d\n", arena.duel + 1);
    arena_wait_notice();
}

static void arena_quit_chosen(int button, int *quit)
{
    (void)quit;
    if (over) return;
    if (button == 0) resign();
    else arena_series_notice(NULL);
}

static void arena_series_chosen(int button, int *quit)
{
    static const char *const buttons[] = {"Si, ritirati", "No"};
    (void)quit;
    if (over) return;
    if (button == 0) arena_next();
    else Menu_ShowNotice("Arena online", "Vuoi ritirarti? Perdi la serie.", buttons, 2, 1, arena_quit_chosen);
}

static void arena_series_notice(const char *how_text)
{
    static const char *const buttons[] = {"Prossimo duello", "Ritirati"};
    static char last[160];
    char text[400];
    if (how_text) snprintf(last, sizeof(last), "%s", how_text);
    snprintf(text, sizeof(text), "%s\n\nSerie: tu %d - %d %s (al meglio di 3: chi arriva a 2 vince).", last,
             arena.wins[0], arena.wins[1], opponent[0] ? opponent : "l'avversario");
    arena_notice_at = now_ms();
    Menu_ShowNotice("Arena online", text, buttons, 2, 0, arena_series_chosen);
}

/* A duel of the series is over, in both games at the same point. */
static void arena_count(int won, const char *how)
{
    char text[160];
    const char *said = !strcmp(how, "exodia") ? "Exodia" : !strcmp(how, "deck") ? "mazzo finito"
                     : !strcmp(how, "test") ? "prova" : "punti vita a zero";
    arena_live = arena_in_duel = 0;
    arena.wins[won ? 0 : 1]++;
    fprintf(stderr, "memories-pc: online: arena: duel %d: you %s (%s), series %d-%d\n", arena.duel,
            won ? "won" : "lost", how, arena.wins[0], arena.wins[1]);
    if (arena.wins[0] >= 2 || arena.wins[1] >= 2) {
        duel_over(arena.wins[0] >= 2, how);
        return;
    }
    snprintf(text, sizeof(text), "Duello %d: %s (%s).", arena.duel, won ? "hai vinto" : "hai perso", said);
    arena_series_notice(text);
}

/* At every point of an arena session: the deck out once the mod has it, the
 * jump both flags asked for (Netplay_Pads), the tests' shortcuts. */
static void arena_point(void)
{
    if (arena.deck_ready && !arena_deck_sent) {
        memcpy(saves[side - 1], arena.deck, DECK_BYTES);
        send_message(MSG_DECK, saves[side - 1], DECK_BYTES);
        arena_deck_sent = 1;
        fprintf(stderr, "memories-pc: online: arena: my deck is ready and sent\n");
        arena_next();
    }
    if (arena_go && !over) {
        char why[160];
        arena_go = 0;
        arena_ready = 0;
        arena.duel++;
        arena_in_duel = 1;
        arena_live = 0;
        arena_go_index = vblank_index;
        if (arena_waiting) {
            arena_waiting = 0;
            Menu_CloseNotice();
        }
        fprintf(stderr, "memories-pc: online: arena: duel %d starts (VBlank %u)\n", arena.duel, vblank_index);
        if (TitleJump_RequestTo(JUMP_DUEL2P, 0, NULL, why, sizeof(why)))
            fprintf(stderr, "memories-pc: online: no jump to the duel: %s\n", why);
    }
    if (test_series && !over) {
        if (arena_live && vblank_index >= arena_go_index + test_series)
            arena_count((arena.duel % 2 == 1) == (side == 1), "test");
        else if (!arena_in_duel && arena_deck_sent && !arena_ready && Menu_NoticeShown() &&
                 now_ms() - arena_notice_at > 2000) {
            Menu_CloseNotice();
            arena_next();
        }
    }
}


/* The end of a two-player duel (main mode 3, a negative gDuel_bOpponentID),
 * from what both games hold the same: Exodia's phase (14, whose player is
 * the one in turn, D_8009B1D5), else the result phase (12, then 13) with
 * gDuel_bWinnerSide and the winner's rank adjustment (D_800E9FF0[side], the
 * first byte: 40 Exodia, -40 the other's deck ran out, else life points). */
static void check_end(void)
{
    int phase = *(volatile uint16_t *)(uintptr_t)0x8009B23Au & 0xF, winner;
    const char *how = "lp";
    if ((*(volatile uint8_t *)(uintptr_t)0x8009B26Cu & 31) != 3 || *(volatile int8_t *)(uintptr_t)0x8009B361u >= 0)
        return;
    if (phase == 14) {
        winner = *(volatile uint8_t *)(uintptr_t)0x8009B1D5u & 1;
        how = "exodia";
    } else if (phase == 12 || phase == 13) {
        int8_t adjustment;
        winner = *(volatile uint8_t *)(uintptr_t)0x8009B165u & 1;
        adjustment = *(volatile int8_t *)(uintptr_t)(0x800E9FF0u + (uint32_t)winner * 0x20u);
        if (adjustment == 40) how = "exodia";
        else if (adjustment == -40) how = "deck";
    } else {
        arena_live = arena_in_duel; /* the arena's duel runs: its end counts once */
        return;
    }
    if (!arena_mode) duel_over(winner == side - 1, how);
    else if (arena_live) arena_count(winner == side - 1, how);
}

/* fork: the turn timer (Alessio's). A player in turn has TURN_SECONDS of
 * the game waiting on them (the hand, the field, the guardian star: phases
 * 4, 5 and 8 of gDuel_wSceneStateFlags, with no transition counting down in
 * D_8009B162); the last TURN_SHOWN of them count down on screen. At zero the
 * game plays the turn for them, as the shortest way through: unmark a lone
 * marked card's partner, play a card (face down when the face-up one is
 * refused), take the default zone and star, end the turn with Start. Both
 * games count and press the same at the same VBlank, from the same state
 * (the lockstep), so nothing but the inputs crosses. MEMORIES_NETPLAY_TURN
 * (seconds) shortens it for the tests. */
#define TURN_SECONDS 90
#define TURN_SHOWN 30
#define RAW_START 0x0008
#define RAW_UP 0x0010
#define RAW_RIGHT 0x0020
#define RAW_CIRCLE 0x2000
#define RAW_CROSS 0x4000
static unsigned turn_vblanks = TURN_SECONDS * 60, turn_used;
static int turn_side = -1, turn_auto, auto_flip, auto_toggle;
static unsigned auto_step, auto_tries, auto_state;
static uint16_t auto_button;

static int turn_duel(void)
{
    return active && !over && (*(volatile uint8_t *)(uintptr_t)0x8009B26Cu & 31) == 3 &&
           *(volatile int8_t *)(uintptr_t)0x8009B361u < 0;
}

/* The button the auto play presses now, from the duel's state; 0 to wait. */
static uint16_t auto_choose(int acting)
{
    int phase = *(volatile uint16_t *)(uintptr_t)0x8009B23Au & 0xF;
    int sub = *(volatile uint8_t *)(uintptr_t)0x8009B174u & 0xF;
    unsigned state = (unsigned)phase << 4 | (unsigned)sub;
    auto_tries = state == auto_state ? auto_tries + 1 : 0;
    auto_state = state;
    if (*(volatile uint8_t *)(uintptr_t)0x8009B164u) return RAW_CIRCLE;                 /* the quit dialog: no */
    if ((*(volatile uint8_t *)(uintptr_t)0x8009B254u & 0xF) == 2) return RAW_CIRCLE;    /* the card viewer */
    if (phase == 4) {
        if (sub == 1) {
            /* one marked card alone cannot be played: mark a second (Up on
             * an unmarked one), moving on when the cursor is on it */
            int marked = *(volatile uint8_t *)(uintptr_t)(0x800E9F10u + (uint32_t)acting * 0x70u + 0x15u);
            if (marked == 1) return (auto_toggle = !auto_toggle) ? RAW_UP : RAW_RIGHT;
            return RAW_CROSS;
        }
        if (sub == 3) {
            if (auto_flip) {
                auto_flip = 0;
                return RAW_RIGHT; /* face down: an equip with no monster cannot go face up */
            }
            return RAW_CROSS;
        }
        if (sub == 4 && auto_tries >= 3) {
            auto_flip = 1;
            return RAW_CIRCLE; /* the zone refused it: back, and face down */
        }
        return RAW_CROSS;
    }
    if (phase == 5) {
        if (sub == 3) return RAW_START;
        if (sub == 6 || sub == 7) return RAW_CIRCLE;
        return 0;
    }
    if (phase >= 6 && phase <= 8) return RAW_CROSS;
    return 0;
}

/* Each VBlank of a two-player duel: the side in turn's time, and its pad
 * replaced by the auto play once the time is up. */
static void turn_timer(uint16_t both[2])
{
    int acting, phase;
    if (!turn_duel()) {
        turn_side = -1;
        return;
    }
    acting = *(volatile uint8_t *)(uintptr_t)0x8009B1D5u & 1;
    phase = *(volatile uint16_t *)(uintptr_t)0x8009B23Au & 0xF;
    if (acting != turn_side) {
        turn_side = acting;
        turn_used = 0;
        turn_auto = auto_flip = auto_toggle = 0;
        auto_step = auto_tries = auto_state = 0;
    }
    if (!turn_auto) {
        if ((phase == 4 || phase == 5 || phase == 8) && !*(volatile uint16_t *)(uintptr_t)0x8009B162u &&
            !(phase == 5 && ((*(volatile uint8_t *)(uintptr_t)0x8009B174u & 0xF) >= 0xA)))
            turn_used++;
        if (turn_used < turn_vblanks) return;
        turn_auto = 1;
        fprintf(stderr, "memories-pc: online: turn timer: player %d's time is up, the turn plays itself\n", acting + 1);
    }
    /* A press every 16 VBlanks, held 4: the game reads presses as edges. */
    if (auto_step % 16 == 0) auto_button = auto_choose(acting);
    both[acting] = auto_step % 16 < 4 ? auto_button : 0;
    auto_step++;
}

/* The countdown's seconds while it shows, else -1. */
static int turn_left(void)
{
    unsigned left;
    if (!turn_duel() || turn_side < 0) return -1;
    if (turn_auto) return 0;
    left = turn_vblanks - turn_used;
    return left <= TURN_SHOWN * 60u ? (int)((left + 59) / 60) : -1;
}

/* The relay's answer, a line: "WAIT" while alone in the room, then
 * "GO <seed>" once both are in. What follows it is the other game's. From
 * the lobby the other is already on the way: a shorter wait. */
static int wait_for_pair(void)
{
    char line[128];
    size_t length = 0;
    uint64_t started = now_ms();
    while (now_ms() - started < (from_lobby ? 90000u : 600000u)) {
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
    int slot;
    if (OnlineDeck_ReadSave(path, saves[side - 1])) {
        fprintf(stderr, "memories-pc: online: cannot read the save %s, or it has no whole deck\n",
                path ? path : "(none)");
        return -1;
    }
    have_save[side - 1] = 1;
    /* fork: the 1v1 may take one of the save's deck slots (online_deck.h) */
    if (!arena_mode && (slot = OnlineDeck_Apply(saves[side - 1])) > 0)
        fprintf(stderr, "memories-pc: online: the deck of slot %d\n", slot);
    return 0;
}

static void start(void)
{
    const char *config = asked_config, *wanted_delay = getenv("MEMORIES_NETPLAY_DELAY");
    char host[256], room[64];
    unsigned port = 0;
    unsigned char hello[12 + 1024];
    size_t facts_length;
    uint64_t started;
    if (!*config) return;
    if (!strncmp(config, "ws://", 5)) {
        /* ws://host[:port]/room/side: the server (server/worker.js), which
         * adds about 60 ms each way, more on a phone, hence a longer
         * delay. */
        const char *rest = config + 5;
        char *end;
        websocket = 1;
        delay = 8;
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
    if (getenv("MEMORIES_NETPLAY_TURN") && atoi(getenv("MEMORIES_NETPLAY_TURN")) > 0)
        turn_vblanks = (unsigned)atoi(getenv("MEMORIES_NETPLAY_TURN")) * 60u;
    if (getenv("MEMORIES_NETPLAY_TEST_RESIGN")) test_resign = (unsigned)strtoul(getenv("MEMORIES_NETPLAY_TEST_RESIGN"), NULL, 10);
    test_back = getenv("MEMORIES_NETPLAY_TEST_BACK") && !strcmp(getenv("MEMORIES_NETPLAY_TEST_BACK"), "1");
    /* At least 2: the input goes out in pairs of VBlanks (Netplay_Pads). */
    if (delay < 2) delay = 2;
    if (delay >= RING / 4) delay = RING / 4 - 1;
    if (read_save(asked_save)) return;
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
    /* The VBlanks before the delay have no input; ours go out from there. */
    last_target = delay - 1;
    gap_slow = delay;
    phase_log = getenv("MEMORIES_NETPLAY_PHASES") && strcmp(getenv("MEMORIES_NETPLAY_PHASES"), "0");
    if (arena_mode) {
        arena.version = NETPLAY_ARENA_VERSION;
        arena.side = side;
        arena.seed = seed;
        snprintf(arena.rival, sizeof(arena.rival), "%s", opponent);
        if (getenv("MEMORIES_NETPLAY_TEST_SERIES"))
            test_series = (unsigned)strtoul(getenv("MEMORIES_NETPLAY_TEST_SERIES"), NULL, 10);
        Mods_ProvidePort("netplay_arena", &arena);
        fprintf(stderr, "memories-pc: online: arena: the draft first (seed %u), then best of three\n", (unsigned)seed);
    }
    {
        char line[96];
        snprintf(line, sizeof(line), "in_corso=%s", opponent);
        note_result(line);
    }
    fprintf(stderr, "memories-pc: online: player %d of room %s (%s), input delay %u, seed %u, %s, checks of %s\n",
            side, room, websocket ? "WebSocket" : "relay", delay, (unsigned)seed,
            hide ? "the other's hand hidden" : "both hands shown", check_ram ? "all RAM" : "the duel");
}

void Netplay_Init(void)
{
    read_asked();
    if (!asked_config[0]) return;
    start();
    if (!active) lobby_failed();
}

int Netplay_Active(void)
{
    return active;
}

int Netplay_Asked(void)
{
    read_asked();
    return asked_config[0] != '\0';
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
    int x, y, w, h, what = covering(), left = turn_left();
    if (!what && left < 0) return 0;
    FusionHelper_GetViewport(&x, &y, &w, &h);
    return ((((unsigned)x * 31u + (unsigned)y) * 31u + (unsigned)w * 7u + (unsigned)h) * 3u + (unsigned)what) * 97u +
           (unsigned)(left + 2) * 5u + (unsigned)Menu_Scale() + (unsigned)(turn_side == side - 1);
}

/* The turn timer's countdown, at the top of the picture: whose turn, the
 * seconds left, or that the time is up. */
static void draw_timer(MenuCanvas *canvas, int *x, int *y, int *w, int *h)
{
    char text[96];
    int vx, vy, vw, vh, scale = Menu_Scale() * 2, width, left = turn_left(), mine = turn_side == side - 1;
    int box_x, box_y, box_w, box_h, row, column;
    *x = *y = *w = *h = 0;
    if (left < 0) return;
    FusionHelper_GetViewport(&vx, &vy, &vw, &vh);
    if (vw <= 0 || vh <= 0) return;
    if (!left) snprintf(text, sizeof(text), "Tempo scaduto: il turno %s gioca da solo", mine ? "tuo" : "avversario");
    else snprintf(text, sizeof(text), "%s: %d s", mine ? "Il tuo turno" : "Turno avversario", left);
    width = Menu_TextWidthScaled(text, scale);
    if (width > vw - 8 * scale) {
        scale = Menu_Scale();
        width = Menu_TextWidthScaled(text, scale);
    }
    box_w = width + 12 * scale, box_h = 16 * scale;
    box_x = vx + (vw - box_w) / 2, box_y = vy + vh / 6;
    if (box_x < 0) box_x = 0;
    if (box_y < 0) box_y = 0;
    if (box_x + box_w > canvas->width) box_w = canvas->width - box_x;
    if (box_y + box_h > canvas->height) box_h = canvas->height - box_y;
    if (box_w <= 0 || box_h <= 0) return;
    for (row = box_y; row < box_y + box_h; row++)
        for (column = box_x; column < box_x + box_w; column++)
            canvas->pixels[row * canvas->stride + column] = 0xFF101830u;
    Menu_DrawTextScaled(canvas, box_x + 6 * scale, box_y + box_h / 2, text,
                        left && left > 10 ? 0xFFF0D070u : 0xFFFF8080u, scale);
    *x = box_x, *y = box_y, *w = box_w, *h = box_h;
}

static void draw_cover(MenuCanvas *canvas, int *x, int *y, int *w, int *h)
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

void Netplay_Draw(MenuCanvas *canvas, int *x, int *y, int *w, int *h)
{
    int tx, ty, tw, th;
    draw_cover(canvas, x, y, w, h);
    draw_timer(canvas, &tx, &ty, &tw, &th);
    if (!tw || !th) return;
    if (!*w || !*h) {
        *x = tx, *y = ty, *w = tw, *h = th;
        return;
    }
    {
        int x1 = *x + *w > tx + tw ? *x + *w : tx + tw, y1 = *y + *h > ty + th ? *y + *h : ty + th;
        if (tx < *x) *x = tx;
        if (ty < *y) *y = ty;
        *w = x1 - *x, *h = y1 - *y;
    }
}

unsigned Netplay_LifePoints(void)
{
    const char *test = getenv("MEMORIES_NETPLAY_TEST_LP");
    unsigned value = test ? (unsigned)strtoul(test, NULL, 10) : 0;
    return value >= 1 && value <= NETPLAY_LIFE_POINTS ? value : NETPLAY_LIFE_POINTS;
}

int Netplay_FirstSide(void)
{
    if (!active)
        return 0;
    /* The arena: the duels of the series take turns. Otherwise a coin toss
     * both games make alike, from the seed they share. */
    if (arena_mode)
        return arena.duel > 0 ? (arena.duel - 1) & 1 : 0;
    const char *test = getenv("MEMORIES_NETPLAY_TEST_FIRST");
    if (test && (*test == '1' || *test == '2'))
        return *test - '1';
    return (int)((seed * 0x9E3779B9u) >> 31);
}

uint32_t Netplay_Seed(void)
{
    /* The arena: another seed for each duel of the series. */
    return arena_mode ? seed + (uint32_t)arena.duel * 0x9E3779B9u : seed;
}

NetplayArena *Netplay_Arena(void)
{
    return active && arena_mode ? &arena : NULL;
}

const unsigned char *Netplay_Save(int player)
{
    return active && (player == 1 || player == 2) && have_save[player - 1] ? saves[player - 1] : NULL;
}

/* Our input for the VBlank target, kept and sent. */
static void emit(unsigned target, uint16_t bits)
{
    unsigned char message[7];
    local_bits[target % RING] = bits;
    local_flags[target % RING] = (uint8_t)arena_ready;
    put32(message, target);
    message[4] = (unsigned char)bits, message[5] = (unsigned char)(bits >> 8);
    message[6] = (unsigned char)arena_ready;
    send_message(MSG_INPUT, message, arena_mode ? 7 : 6);
}

/* The duel's animations, where nobody chooses (the draw, the battle, the
 * field turning at the turn's end, the results): flat out in both games.
 * The choices, the phases the turn timer counts, at 100%, so that the
 * cursor moves as it always has. The fusions and the card going down
 * (phases 6 and 7): fast but not flat out, FUSION_RATE (Alessio's), so a
 * fusion can still be followed; animation_now() says 2 there. Out of the
 * duel (the start after the restart, the way into each duel) flat out too,
 * but for the arena's draft and its notices between the duels, which the
 * mod's own panels run at 100%. The same in both games, which are at the
 * same state at the same VBlank. */
#define FUSION_RATE 250
static int animation_now(void)
{
    int phase, sub;
    if (!turn_duel()) return active && !over && !(arena_mode && !arena_in_duel);
    phase = *(volatile uint16_t *)(uintptr_t)0x8009B23Au & 0xF;
    sub = *(volatile uint8_t *)(uintptr_t)0x8009B174u & 0xF;
    if (phase_log && (unsigned)(phase << 4 | sub) != phase_key) {
        phase_key = (unsigned)(phase << 4 | sub);
        fprintf(stderr, "memories-pc: online: phase %d.%d at VBlank %u, %llu ms\n", phase, sub, vblank_index,
                (unsigned long long)(now_ms() - active_since));
    }
    if (phase == 6 || phase == 7) return 2;
    if (phase == 4 || phase == 8) return 0;
    if (phase == 5) return sub >= 0xA;
    return 1;
}

void Netplay_Pads(uint16_t local, uint16_t both[2])
{
    unsigned index = vblank_index++, want;
    uint16_t mine, theirs = 0;
    uint8_t theirs_flags = 0;
    int animation = animation_now(), rate;
    /* The arena's draft and the series' notices: no buttons to the game. */
    if (arena_mode && !arena_in_duel) local = 0;
    /* Ours for the VBlank the gap says. A longer gap fills the VBlanks in
     * between with no buttons; a shorter one keeps the buttons for the next
     * VBlank still free, so that no press is lost. */
    want = index + (animation ? GAP_FAST : gap_slow);
    pending_bits |= local;
    if (want > last_target) {
        int jumped = want > last_target + 1;
        while (last_target + 1 < want) emit(++last_target, 0);
        emit(++last_target, pending_bits);
        pending_bits = 0;
        if (jumped) flush();
    }
    /* What is kept goes out every FLUSH_MS (the check of Netplay_Point
     * rides along): a VBlank or two at 100%, many flat out. */
    if (now_ms() - last_flush_ms >= FLUSH_MS) flush();
    /* Flat out in the animations, and on until the gap has shrunk back
     * (choosing at 100% with the long gap would feel slow); paused stays
     * paused. Game speed and the turbo come back after the duel. */
    rate = animation == 2 ? FUSION_RATE : animation || last_target > index + gap_slow ? -1 : 100;
    if (Platform_ClockRate() != 0 && Platform_ClockRate() != rate) Platform_SetClockRate(rate);
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
        /* Choosing, every 240 VBlanks: waits over an eighth of the time
         * say the network is slower than the gap, which grows by one. */
        if (animation || !window_started) {
            window_frames = 0, window_started = now_ms(), window_waited = waited_ms;
        } else if (++window_frames >= 240) {
            uint64_t now = now_ms();
            if (gap_slow < GAP_SLOW_MAX && (waited_ms - window_waited) * 8 > now - window_started) {
                gap_slow++;
                fprintf(stderr, "memories-pc: online: waited %llu ms of %llu, the input goes %u VBlanks ahead\n",
                        (unsigned long long)(waited_ms - window_waited), (unsigned long long)(now - window_started),
                        gap_slow);
            }
            window_frames = 0, window_started = now, window_waited = waited_ms;
        }
        if (remote_have[index % RING] && remote_index[index % RING] == index) {
            theirs = remote_bits[index % RING];
            theirs_flags = remote_flags[index % RING];
            remote_have[index % RING] = 0;
        }
    }
    /* Both ready at this VBlank, and not at the one before: the jump, at
     * the same VBlank in both games (each sees the same pair of flags). */
    if (arena_mode && index >= delay) {
        int both_ready = (local_flags[index % RING] & 1) && (theirs_flags & 1);
        if (both_ready && !arena_both) arena_go = 1;
        arena_both = both_ready;
    }
    both[side - 1] = mine;
    both[2 - side] = theirs;
    if (index >= delay) turn_timer(both);
}

void Netplay_Point(unsigned frame)
{
    if (!active) return;
    if (arena_mode) arena_point();
    else if (!jump_asked) {
        char why[160];
        jump_asked = 1;
        if (TitleJump_RequestTo(JUMP_DUEL2P, 0, NULL, why, sizeof(why)))
            fprintf(stderr, "memories-pc: online: no jump to the duel: %s\n", why);
    }
    if (!over) {
        check_end();
        if (!over && test_resign && vblank_index >= test_resign) resign();
    } else if (test_back && from_lobby && now_ms() - over_at > 2000) {
        test_back = 0;
        Menu_CloseNotice();
        back_to_game();
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
