/* The online lobby (lobby.h). The thread owns the socket (netplay_net.h)
 * and moves whole lines: the main thread's messages out through `outbox`,
 * the server's in through `inbox`, both under `lock`. Every message of the
 * server is one JSON line; the thread adds its own, whose "t" starts with
 * "_": "_up" (connected), "_down" (the link is lost), "_none" (no link
 * could be made). Everything else, the account, the players and the
 * notices, is the main thread's. */
#define _GNU_SOURCE
#include "pc/compat/fs.h"
#include "cloud.h"
#include "lobby.h"
#include "menu.h"
#include "paths.h"
#include "platform.h"
#include "pc/compat/signal.h"
#include "pc/debug/netplay.h"
#include "pc/debug/netplay_net.h"
#include "pc/mods/json.h"
#include "pc/saves/save_menu.h"
#include "pc/saves/save_slots.h"
#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define LOBBY_HOST "yfm-online.alessio-scuola.workers.dev"
#define PING_MS 30000
#define SILENCE_MS 75000 /* the server answers every ping: this long without a word, the link is gone */
#define PLAYERS (MENU_NOTICE_GRID - 2) /* then "Salvataggi" and "Chiudi" */
#define NAME 13

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char outbox[65536], inbox[65536]; /* lines; under lock (a slot file in base64 is 11 KB) */
static size_t outbox_length, inbox_length;
static char shared_token[40];           /* under lock: what the thread says "entra" with */
static int thread_running, paused, wake; /* paused and wake: atomic */

static char host[256];
static unsigned port = 80;

/* --- the thread ------------------------------------------------------ */

static uint64_t now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void nap(int ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec span = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&span, NULL);
#endif
}

static void push_in(const char *data, size_t size)
{
    pthread_mutex_lock(&lock);
    if (inbox_length + size <= sizeof(inbox)) {
        memcpy(inbox + inbox_length, data, size);
        inbox_length += size;
    }
    pthread_mutex_unlock(&lock);
}

static void push_note(const char *line) { push_in(line, strlen(line)); }

/* Waits ms, or less when Lobby_Open asks for a try now. */
static void wait_retry(int ms)
{
    int waited;
    for (waited = 0; waited < ms; waited += 200) {
        if (__atomic_exchange_n(&wake, 0, __ATOMIC_ACQ_REL)) return;
        nap(200);
    }
}

/* What the main thread queued, sent one line per text frame. -1 when the
 * connection is gone. */
static int send_queued(void)
{
    static char lines[sizeof(outbox) + 1];
    char *line, *end;
    pthread_mutex_lock(&lock);
    memcpy(lines, outbox, outbox_length);
    lines[outbox_length] = '\0';
    outbox_length = 0;
    pthread_mutex_unlock(&lock);
    for (line = lines; *line; line = end + 1) {
        end = strchr(line, '\n');
        if (!end) break;
        *end = '\0';
        if (NetplayNet_SendText(line)) return -1;
    }
    return 0;
}

static void *run(void *unused)
{
    unsigned failures = 0;
    char buffer[4096];
    (void)unused;
    for (;;) {
        uint64_t last_in, last_ping;
        char mine[40], hello[96];
        if (__atomic_load_n(&paused, __ATOMIC_ACQUIRE)) {
            wait_retry(1000);
            continue;
        }
        if (NetplayNet_ConnectWebSocket(host, port, "/lobby", 10000)) {
            push_note("{\"t\":\"_none\"}\n");
            failures++;
            wait_retry(failures <= 1 ? 3000 : 60000);
            continue;
        }
        failures = 0;
        push_note("{\"t\":\"_up\"}\n");
        pthread_mutex_lock(&lock);
        snprintf(mine, sizeof(mine), "%s", shared_token);
        pthread_mutex_unlock(&lock);
        last_in = last_ping = now_ms();
        if (mine[0]) {
            snprintf(hello, sizeof(hello), "{\"t\":\"entra\",\"gettone\":\"%s\"}", mine);
            if (NetplayNet_SendText(hello)) goto lost;
        }
        while (!__atomic_load_n(&paused, __ATOMIC_ACQUIRE)) {
            long got;
            uint64_t now;
            if (send_queued()) break;
            got = NetplayNet_Receive(buffer, sizeof(buffer), 200);
            if (got < 0) break;
            now = now_ms();
            if (got > 0) {
                push_in(buffer, (size_t)got);
                last_in = now;
            }
            if (now - last_ping >= PING_MS) {
                if (NetplayNet_SendText("{\"t\":\"ping\"}")) break;
                last_ping = now;
            }
            if (now - last_in > SILENCE_MS) break;
        }
    lost:
        NetplayNet_Close();
        pthread_mutex_lock(&lock);
        outbox_length = 0; /* said to a server that is gone */
        pthread_mutex_unlock(&lock);
        push_note("{\"t\":\"_down\"}\n");
        wait_retry(3000);
    }
    return NULL;
}

static int start_thread(void)
{
    pthread_t thread;
    sigset_t all, previous;
    int error;
    if (thread_running) return 0;
    /* The game's clock signal belongs to the main thread (platform_common.c). */
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &previous);
    error = pthread_create(&thread, NULL, run, NULL);
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    if (error) {
        fprintf(stderr, "memories-pc: lobby: no thread\n");
        return -1;
    }
    pthread_detach(thread);
    thread_running = 1;
    return 0;
}

static void send_line(const char *line)
{
    size_t length = strlen(line);
    pthread_mutex_lock(&lock);
    if (outbox_length + length + 1 <= sizeof(outbox)) {
        memcpy(outbox + outbox_length, line, length);
        outbox[outbox_length + length] = '\n';
        outbox_length += length + 1;
    }
    pthread_mutex_unlock(&lock);
}

void Lobby_Send(const char *line) { send_line(line); }

/* --- the account and the players (main thread) ----------------------- */

typedef struct {
    char name[NAME];
    int online, won, lost;
} Player;

#define RESULTS 8

static char my_name[NAME], token[40];
static char results[RESULTS][NAME + 1]; /* lobby.txt's duels not told yet: 'v' or 'p', then the other's name */
static int result_count;
static char accepted[NAME];             /* the player whose challenge this one accepted */
static Player players[PLAYERS];
static int player_count, connected, in_lobby;
static char challenged[NAME];   /* the player this one challenged, waiting for an answer */
static char challenger[NAME];   /* the player whose challenge is on screen */
static char test_challenge[NAME];
static int test_accept;

enum { SCREEN_NONE, SCREEN_NAME, SCREEN_INVITE, SCREEN_JOINING, SCREEN_LIST, SCREEN_PLAYER, SCREEN_WAIT,
       SCREEN_INCOMING, SCREEN_INFO, SCREEN_START, SCREEN_PASSWORD, SCREEN_ASK_PASSWORD, SCREEN_SAVES };
static int screen;
static char typed[16], typed_name[NAME], typed_invite[16], picked[NAME];
static int grid_focus;
/* What the letter grids are for: a new account (name, invite, password),
 * "Ho gia' un account" (name, password) or the password of an account made
 * before there were any. */
enum { FLOW_MAKE, FLOW_LOGIN, FLOW_PASSWORD };
static int flow;
static int ask_password, password_asked; /* the account has none: asked once a run */
static const char *test_password;

static int account_path(char *path, size_t size) { return Paths_User(path, size, "lobby.txt"); }

static void read_account(void)
{
    char path[1024], line[128];
    FILE *file;
    my_name[0] = token[0] = '\0';
    result_count = 0;
    if (account_path(path, sizeof(path)) || !(file = fopen(path, "rb"))) return;
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!strncmp(line, "nome=", 5)) snprintf(my_name, sizeof(my_name), "%s", line + 5);
        else if (!strncmp(line, "gettone=", 8)) snprintf(token, sizeof(token), "%s", line + 8);
        /* The online duels' results (pc/debug/netplay.c); a duel still
         * "in_corso" never got to its end here: lost. */
        else if (result_count < RESULTS && !strncmp(line, "in_corso=", 9))
            snprintf(results[result_count++], NAME + 1, "p%s", line + 9);
        else if (result_count < RESULTS && !strncmp(line, "risultato=vinto,", 16))
            snprintf(results[result_count++], NAME + 1, "v%s", line + 16);
        else if (result_count < RESULTS && !strncmp(line, "risultato=perso,", 16))
            snprintf(results[result_count++], NAME + 1, "p%s", line + 16);
    }
    fclose(file);
    if (!my_name[0]) token[0] = '\0';
}

static void write_account(void)
{
    char path[1024];
    FILE *file;
    if (account_path(path, sizeof(path)) || !(file = fopen(path, "wb"))) {
        fprintf(stderr, "memories-pc: lobby: cannot write lobby.txt\n");
        return;
    }
    fprintf(file, "nome=%s\ngettone=%s\n", my_name, token);
    fclose(file);
}

static void forget_account(void)
{
    char path[1024];
    my_name[0] = token[0] = '\0';
    if (!account_path(path, sizeof(path))) remove(path);
    pthread_mutex_lock(&lock);
    shared_token[0] = '\0';
    pthread_mutex_unlock(&lock);
}

static void share_token(void)
{
    pthread_mutex_lock(&lock);
    snprintf(shared_token, sizeof(shared_token), "%s", token);
    pthread_mutex_unlock(&lock);
}

/* JSON text of a name or code: only what the grid types, so no escapes. */
static void clean(char *out, size_t size, const char *text)
{
    size_t used = 0;
    for (; *text && used + 1 < size; text++)
        if ((unsigned char)*text >= ' ' && *text != '"' && *text != '\\') out[used++] = *text;
    out[used] = '\0';
}

static void send_to(const char *type, const char *name, const char *rest)
{
    char line[160], who[NAME];
    clean(who, sizeof(who), name);
    snprintf(line, sizeof(line), "{\"t\":\"%s\",\"a\":\"%s\"%s}", type, who, rest);
    send_line(line);
}

/* Once in the lobby: each duel's result to the server, which counts this
 * account's wins and losses, then out of lobby.txt. */
static void tell_results(void)
{
    char line[160], who[NAME];
    int i;
    if (!result_count) return;
    for (i = 0; i < result_count; i++) {
        int won = results[i][0] == 'v';
        clean(who, sizeof(who), results[i] + 1);
        snprintf(line, sizeof(line), "{\"t\":\"risultato\",\"contro\":\"%s\",\"esito\":\"%s\"}", who,
                 won ? "vinto" : "perso");
        send_line(line);
        fprintf(stderr, "memories-pc: lobby: result sent: %s against %s\n", won ? "won" : "lost", results[i] + 1);
    }
    result_count = 0;
    write_account();
}

static void join(void)
{
    char line[96];
    snprintf(line, sizeof(line), "{\"t\":\"entra\",\"gettone\":\"%s\"}", token);
    send_line(line);
}

/* --- the screens ----------------------------------------------------- */

static void show_list(void);
static void show_grid(int invite);

static void info_closed(int button, int *quit)
{
    (void)button, (void)quit;
    screen = SCREEN_NONE;
}

static void info(const char *text)
{
    static const char *const ok[] = {"OK"};
    screen = SCREEN_INFO;
    Menu_ShowNotice("Online", text, ok, 1, 0, info_closed);
}

/* The letter grid: A-Z, 0-9, then a space (the name) or a dash (the
 * invite code, the password), Canc, OK and Annulla. */
#define LETTERS 36
enum { KEY_EXTRA = LETTERS, KEY_DELETE, KEY_OK, KEY_CANCEL, KEYS };
enum { GRID_NAME, GRID_INVITE, GRID_PASSWORD };
#define PASSWORD_MIN 4

/* The grids' end: the account made, logged into, or its password set. */
static void send_account(const char *password, int quiet)
{
    static const char *const cancel[] = {"Annulla"};
    char line[192], name[NAME], code[sizeof(typed_invite)], secret[sizeof(typed)];
    clean(name, sizeof(name), typed_name);
    clean(code, sizeof(code), typed_invite);
    clean(secret, sizeof(secret), password ? password : "");
    if (flow == FLOW_MAKE && !password) /* the test's account with no password, as made before the passwords */
        snprintf(line, sizeof(line), "{\"t\":\"registra\",\"nome\":\"%s\",\"invito\":\"%s\"}", name, code);
    else if (flow == FLOW_MAKE)
        snprintf(line, sizeof(line), "{\"t\":\"registra\",\"nome\":\"%s\",\"invito\":\"%s\",\"password\":\"%s\"}", name,
                 code, secret);
    else if (flow == FLOW_LOGIN)
        snprintf(line, sizeof(line), "{\"t\":\"accedi\",\"nome\":\"%s\",\"password\":\"%s\"}", name, secret);
    else
        snprintf(line, sizeof(line), "{\"t\":\"password\",\"gettone\":\"%s\",\"password\":\"%s\"}", token, secret);
    send_line(line);
    if (quiet) return;
    screen = SCREEN_JOINING;
    Menu_ShowNotice("Online",
                    flow == FLOW_MAKE    ? "Creo l'account..." :
                    flow == FLOW_LOGIN   ? "Entro nell'account..." :
                                           "Salvo la password...",
                    cancel, 1, 0, info_closed);
}

static void grid_chosen(int button, int *quit)
{
    int mode = screen == SCREEN_INVITE ? GRID_INVITE : screen == SCREEN_PASSWORD ? GRID_PASSWORD : GRID_NAME;
    size_t length = strlen(typed), limit = mode == GRID_NAME ? NAME - 1 : sizeof(typed) - 1;
    (void)quit;
    grid_focus = button;
    if (button < LETTERS || button == KEY_EXTRA) {
        char letter = button < 26 ? (char)('A' + button) : button < LETTERS ? (char)('0' + button - 26)
                                                       : mode == GRID_NAME ? ' ' : '-';
        if (length < limit && !(letter == ' ' && (!length || typed[length - 1] == ' '))) {
            typed[length] = letter;
            typed[length + 1] = '\0';
        }
    } else if (button == KEY_DELETE) {
        if (length) typed[length - 1] = '\0';
    } else if (button == KEY_OK) {
        while (length && typed[length - 1] == ' ') typed[--length] = '\0';
        if (length < (size_t)(mode == GRID_NAME ? 3 : mode == GRID_PASSWORD ? PASSWORD_MIN : 0)) {
            show_grid(mode);
            return;
        }
        if (mode == GRID_PASSWORD) {
            send_account(typed, 0);
            typed[0] = '\0';
            return;
        }
        if (mode == GRID_NAME) snprintf(typed_name, sizeof(typed_name), "%s", typed);
        else snprintf(typed_invite, sizeof(typed_invite), "%s", typed);
        typed[0] = '\0';
        grid_focus = 0;
        show_grid(mode == GRID_NAME && flow == FLOW_MAKE ? GRID_INVITE : GRID_PASSWORD);
        return;
    } else {
        screen = SCREEN_NONE;
        return;
    }
    show_grid(mode);
}

static void show_grid(int mode)
{
    static char letters[LETTERS][2];
    const char *keys[KEYS];
    char text[256];
    int i;
    for (i = 0; i < LETTERS; i++) {
        letters[i][0] = i < 26 ? (char)('A' + i) : (char)('0' + i - 26);
        keys[i] = letters[i];
    }
    keys[KEY_EXTRA] = mode == GRID_NAME ? "Spazio" : "-";
    keys[KEY_DELETE] = "Canc";
    keys[KEY_OK] = "OK";
    keys[KEY_CANCEL] = "Annulla";
    if (mode == GRID_INVITE)
        snprintf(text, sizeof(text), "Nome: %s\nScrivi il codice d'invito (te lo da' chi ti ha invitato):\n\n%s_",
                 typed_name, typed);
    else if (mode == GRID_NAME && flow == FLOW_LOGIN)
        snprintf(text, sizeof(text), "Entra nel tuo account online.\nScrivi il nome dell'account:\n\n%s_", typed);
    else if (mode == GRID_NAME)
        snprintf(text, sizeof(text),
                 "Crea il tuo account online (una volta sola).\nScegli il nome (da 3 a 12 lettere o cifre):\n\n%s_",
                 typed);
    else if (flow == FLOW_LOGIN)
        snprintf(text, sizeof(text), "Nome: %s\nScrivi la password dell'account:\n\n%s_", typed_name, typed);
    else
        snprintf(text, sizeof(text),
                 "%s: scegli una password (almeno %d lettere o cifre).\nTi serve per entrare nell'account, con i "
                 "tuoi salvataggi, da un altro telefono o dal PC.\n\n%s_",
                 flow == FLOW_MAKE ? typed_name : my_name, PASSWORD_MIN, typed);
    screen = mode == GRID_INVITE ? SCREEN_INVITE : mode == GRID_PASSWORD ? SCREEN_PASSWORD : SCREEN_NAME;
    Menu_ShowNoticeGrid(mode == GRID_INVITE   ? "Online - codice d'invito" :
                        mode == GRID_PASSWORD ? "Online - password" :
                        flow == FLOW_LOGIN    ? "Online - entra" :
                                                "Online - nuovo account",
                        text, keys, KEYS, 10, NULL, grid_focus, grid_chosen);
}

/* No account here yet: a new one, or the one made elsewhere. */
static void start_chosen(int button, int *quit)
{
    (void)quit;
    screen = SCREEN_NONE;
    if (button > 1) return;
    flow = button == 0 ? FLOW_MAKE : FLOW_LOGIN;
    typed[0] = '\0';
    grid_focus = 0;
    show_grid(GRID_NAME);
}

static void show_start(void)
{
    static const char *const buttons[] = {"Crea account", "Ho gia' un account", "Annulla"};
    screen = SCREEN_START;
    Menu_ShowNotice("Online",
                    "Per giocare online serve un account. I tuoi salvataggi stanno anche li': cambiando telefono, "
                    "passando dal PC al telefono o reinstallando il gioco non perdi niente.",
                    buttons, 3, 0, start_chosen);
}

/* An account made before the passwords: one, now or the next time. */
static void ask_password_chosen(int button, int *quit)
{
    (void)quit;
    screen = SCREEN_NONE;
    if (button != 0) {
        fprintf(stderr, "memories-pc: lobby: password put off\n");
        return;
    }
    flow = FLOW_PASSWORD;
    typed[0] = '\0';
    grid_focus = 0;
    show_grid(GRID_PASSWORD);
}

static void ask_for_password(void)
{
    static const char *const buttons[] = {"Scegli la password", "Piu' tardi"};
    ask_password = 0;
    password_asked = 1;
    fprintf(stderr, "memories-pc: lobby: account without password, asking\n");
    if (test_password) {
        flow = FLOW_PASSWORD;
        send_account(test_password, 0);
        return;
    }
    screen = SCREEN_ASK_PASSWORD;
    Menu_ShowNotice("Online - password",
                    "Scegli una password per il tuo account.\n\nTi serve per ritrovarlo, con i tuoi salvataggi, su "
                    "un altro telefono, sul PC o se reinstalli il gioco.",
                    buttons, 2, 0, ask_password_chosen);
}

static void saves_closed(int button, int *quit)
{
    (void)button, (void)quit;
    screen = SCREEN_NONE;
    show_list();
}

static void show_saves(void)
{
    static const char *const back[] = {"Indietro"};
    char text[900];
    Cloud_Status(text, sizeof(text));
    screen = SCREEN_SAVES;
    Menu_ShowNotice("Salvataggi sull'account", text, back, 1, 0, saves_closed);
}

static void wait_chosen(int button, int *quit)
{
    (void)button, (void)quit;
    if (challenged[0]) {
        send_to("annulla", challenged, "");
        fprintf(stderr, "memories-pc: lobby: challenge to %s cancelled\n", challenged);
        challenged[0] = '\0';
    }
    screen = SCREEN_NONE;
    show_list();
}

static void challenge(const char *name)
{
    static const char *const cancel[] = {"Annulla"};
    char text[256];
    snprintf(challenged, sizeof(challenged), "%s", name);
    send_to("sfida", name, ",\"modo\":\"1v1\"");
    fprintf(stderr, "memories-pc: lobby: challenge to %s (1v1)\n", name);
    snprintf(text, sizeof(text),
             "Hai sfidato %s (1v1). Aspetto la risposta...\n\n"
             "Se accetta, il gioco si riavvia nel duello online: i progressi NON SALVATI si perdono.",
             name);
    screen = SCREEN_WAIT;
    Menu_ShowNotice("Online - sfida", text, cancel, 1, 0, wait_chosen);
}

static void player_chosen(int button, int *quit)
{
    (void)quit;
    screen = SCREEN_NONE;
    if (button == 0) challenge(picked);
    else show_list();
}

static void list_chosen(int button, int *quit)
{
    static const char *const buttons[] = {"Sfida 1v1", "Arena (presto)", "Indietro"};
    static const signed char styles[] = {MENU_NOTICE_PLAIN, MENU_NOTICE_DIM, MENU_NOTICE_PLAIN};
    char text[128];
    (void)quit;
    screen = SCREEN_NONE;
    if (button == player_count) {
        grid_focus = button;
        show_saves();
        return;
    }
    if (button < 0 || button >= player_count) return; /* Chiudi */
    snprintf(picked, sizeof(picked), "%s", players[button].name);
    grid_focus = button;
    snprintf(text, sizeof(text), "%s e' online. Che sfida?", picked);
    screen = SCREEN_PLAYER;
    Menu_ShowNoticeGrid("Online", text, buttons, 3, 1, styles, 0, player_chosen);
}

static void show_list(void)
{
    static char labels[PLAYERS][40];
    const char *names[PLAYERS + 2];
    signed char styles[PLAYERS + 2];
    char text[256];
    int i, online = 0, columns = 3;
    if (!in_lobby) {
        static const char *const cancel[] = {"Chiudi"};
        screen = SCREEN_JOINING;
        Menu_ShowNotice("Online", connected ? "Entro nella lobby..." : "Collegamento alla lobby...", cancel, 1, 0,
                        info_closed);
        return;
    }
    for (i = 0; i < player_count; i++) {
        int me = !strcmp(players[i].name, my_name);
        /* The wins (V) and losses (S) the server counts, once there are some;
         * two columns when a cell would grow too wide. */
        if (players[i].won || players[i].lost)
            snprintf(labels[i], sizeof(labels[i]), "%s %dV %dS", players[i].name, players[i].won, players[i].lost);
        else
            snprintf(labels[i], sizeof(labels[i]), "%s", players[i].name);
        if (strlen(labels[i]) > 15) columns = 2;
        names[i] = labels[i];
        styles[i] = players[i].online && !me ? MENU_NOTICE_GREEN : MENU_NOTICE_DIM;
        online += players[i].online;
    }
    names[player_count] = "Salvataggi";
    styles[player_count] = MENU_NOTICE_PLAIN;
    names[player_count + 1] = "Chiudi";
    styles[player_count + 1] = MENU_NOTICE_PLAIN;
    snprintf(text, sizeof(text), "Sei %s. Online: %d di %d.\nVerde = online: scegline uno per sfidarlo.", my_name,
             online, player_count);
    screen = SCREEN_LIST;
    Menu_ShowNoticeGrid("Online - giocatori", text, names, player_count + 2, columns, styles,
                        grid_focus <= player_count ? grid_focus : player_count + 1, list_chosen);
}

/* "via": the duel's room. online.cfg for pc/debug/netplay.c, then a
 * restart into it. The save: the game's slot, else the first; "contro=",
 * the other player, for the result. */
static void go(const char *room, int side, const char *rival)
{
    char path[1024], save[1024];
    FILE *file;
    int slot = SaveMenu_CurrentSlot();
    if (slot < 0) slot = 0;
    fprintf(stderr, "memories-pc: lobby: go room %s side %d\n", room, side);
    if (SaveSlots_Path(slot, save, sizeof(save)) || Paths_User(path, sizeof(path), "online.cfg") ||
        !(file = fopen(path, "wb"))) {
        info("Non riesco a scrivere online.cfg nella cartella del gioco: il duello non parte.");
        return;
    }
    if (port == 80) fprintf(file, "netplay=ws://%s/%s/%d\nsave=%s\n", host, room, side, save);
    else fprintf(file, "netplay=ws://%s:%u/%s/%d\nsave=%s\n", host, port, room, side, save);
    if (*rival) fprintf(file, "contro=%s\n", rival);
    fclose(file);
    fprintf(stderr, "memories-pc: lobby: wrote online.cfg (slot %d), restarting\n", slot + 1);
    fflush(stderr);
    Platform_RestartGame();
    remove(path);
    info("Il gioco non si e' riavviato: il duello non parte.");
}

static void incoming_chosen(int button, int *quit)
{
    (void)quit;
    screen = SCREEN_NONE;
    if (!challenger[0]) return;
    send_to("risposta", challenger, button == 0 ? ",\"si\":true" : ",\"si\":false");
    if (button == 0) snprintf(accepted, sizeof(accepted), "%s", challenger);
    fprintf(stderr, "memories-pc: lobby: %s the challenge from %s\n", button == 0 ? "accepted" : "refused",
            challenger);
    if (button == 0) {
        static const char *const none[] = {"Attendi"};
        screen = SCREEN_JOINING;
        Menu_ShowNotice("Online", "Si parte...", none, 1, 0, info_closed);
    }
    challenger[0] = '\0';
}

static void incoming(const char *from, const char *mode)
{
    static const char *const buttons[] = {"Si", "No"};
    char text[400];
    if (challenged[0] || challenger[0] || strcmp(mode, "1v1") != 0) {
        /* Busy with another challenge, or a kind this game does not know. */
        send_to("risposta", from, ",\"si\":false");
        fprintf(stderr, "memories-pc: lobby: challenge from %s (%s) refused: busy\n", from, mode);
        return;
    }
    snprintf(challenger, sizeof(challenger), "%s", from);
    fprintf(stderr, "memories-pc: lobby: challenge from %s (%s)\n", from, mode);
    if (test_accept) {
        incoming_chosen(0, NULL);
        return;
    }
    snprintf(text, sizeof(text),
             "%s ti sfida (%s).\n\nSe accetti il gioco si riavvia nel duello online: i progressi NON SALVATI "
             "si perdono.\n\nAccetti?",
             from, mode);
    screen = SCREEN_INCOMING;
    Menu_ShowNotice("Sfida online", text, buttons, 2, 1, incoming_chosen);
}

/* --- what came in ---------------------------------------------------- */

static void read_list(const JsonValue *list)
{
    int i, count = Json_Count(list);
    player_count = 0;
    for (i = 0; i < count && player_count < PLAYERS; i++) {
        const JsonValue *player = Json_At(list, i);
        const char *name = Json_String(Json_Member(player, "nome"), NULL);
        if (!name) continue;
        snprintf(players[player_count].name, NAME, "%s", name);
        players[player_count].online = Json_Bool(Json_Member(player, "online"), 0);
        players[player_count].won = (int)Json_Number(Json_Member(player, "vinte"), 0);
        players[player_count].lost = (int)Json_Number(Json_Member(player, "perse"), 0);
        player_count++;
    }
}

static int player_online(const char *name)
{
    int i;
    for (i = 0; i < player_count; i++)
        if (!strcmp(players[i].name, name)) return players[i].online;
    return 0;
}

static void handle(const char *line)
{
    char error[128];
    JsonDocument *document = Json_Parse(line, error, sizeof(error));
    const JsonValue *root = document ? Json_Root(document) : NULL;
    const char *type = root ? Json_String(Json_Member(root, "t"), "") : "";
    const char *from = root ? Json_String(Json_Member(root, "da"), "") : "";
    char text[256];
    if (!document) return;
    if (Cloud_Handle(type, root)) {
        /* the saves' (cloud.c) */
    } else if (!strcmp(type, "_up")) {
        connected = 1;
    } else if (!strcmp(type, "_down") || !strcmp(type, "_none")) {
        int was = in_lobby;
        Cloud_Lost();
        if (was || connected) fprintf(stderr, "memories-pc: lobby: link lost\n");
        connected = in_lobby = 0;
        challenger[0] = '\0';
        if (challenged[0] || ((screen == SCREEN_LIST || screen == SCREEN_PLAYER || screen == SCREEN_JOINING) &&
                              Menu_NoticeShown())) {
            challenged[0] = '\0';
            info(was ? "Connessione alla lobby persa. Riprovo da solo tra poco." :
                       "Non riesco a collegarmi alla lobby (manca internet?). Riprovo da solo tra poco.");
        }
    } else if (!strcmp(type, "ok")) {
        const char *given = Json_String(Json_Member(root, "gettone"), NULL);
        const char *name = Json_String(Json_Member(root, "nome"), NULL);
        if (given && name) {
            snprintf(my_name, sizeof(my_name), "%s", name);
            snprintf(token, sizeof(token), "%s", given);
            write_account();
            share_token();
            fprintf(stderr, "memories-pc: lobby: account %s %s\n", my_name, flow == FLOW_LOGIN ? "logged in" : "made");
            Cloud_Start(my_name);
            join();
        }
    } else if (!strcmp(type, "account")) {
        if (!Json_Bool(Json_Member(root, "password"), 1) && !password_asked) ask_password = 1;
    } else if (!strcmp(type, "password_ok")) {
        fprintf(stderr, "memories-pc: lobby: password set\n");
        if (screen == SCREEN_JOINING && Menu_NoticeShown())
            info("Password salvata. Per entrare nell'account da un altro telefono o dal PC: Online > Ho gia' un "
                 "account, con il nome e questa password.");
    } else if (!strcmp(type, "lista")) {
        int first = !in_lobby;
        read_list(Json_Member(root, "giocatori"));
        in_lobby = 1;
        if (first) fprintf(stderr, "memories-pc: lobby: connected as %s (%d players)\n", my_name, player_count);
        if (first) tell_results();
        if (first) Cloud_Joined();
        if ((screen == SCREEN_LIST || screen == SCREEN_JOINING) && Menu_NoticeShown()) show_list();
        if (test_challenge[0] && player_online(test_challenge) && !challenged[0]) {
            challenge(test_challenge);
            test_challenge[0] = '\0';
        }
    } else if (!strcmp(type, "sfida")) {
        incoming(from, Json_String(Json_Member(root, "modo"), "1v1"));
    } else if (!strcmp(type, "annullata")) {
        if (!strcmp(challenger, from)) {
            fprintf(stderr, "memories-pc: lobby: %s cancelled the challenge\n", from);
            challenger[0] = '\0';
            snprintf(text, sizeof(text), "%s ha annullato la sfida.", from);
            info(text);
        }
    } else if (!strcmp(type, "rifiutata")) {
        if (!strcmp(challenged, from)) {
            fprintf(stderr, "memories-pc: lobby: %s refused\n", from);
            challenged[0] = '\0';
            snprintf(text, sizeof(text), "%s ha rifiutato la sfida.", from);
            info(text);
        }
    } else if (!strcmp(type, "via")) {
        const char *room = Json_String(Json_Member(root, "stanza"), "");
        int side = (int)Json_Number(Json_Member(root, "lato"), 0);
        int good = (side == 1 || side == 2) && *room && strlen(room) <= 32;
        const char *at;
        for (at = room; *at; at++)
            if (!isalnum((unsigned char)*at)) good = 0;
        char rival[NAME];
        snprintf(rival, sizeof(rival), "%s", challenged[0] ? challenged : accepted);
        challenged[0] = challenger[0] = accepted[0] = '\0';
        if (good) go(room, side, rival);
    } else if (!strcmp(type, "errore")) {
        const char *why = Json_String(Json_Member(root, "motivo"), "");
        fprintf(stderr, "memories-pc: lobby: the server says no: %s\n", why);
        if (!strcmp(why, "invito")) {
            info("Codice d'invito sbagliato. Chiedilo a chi ti ha invitato e riprova da Online.");
        } else if (!strcmp(why, "nome preso")) {
            info("Questo nome e' gia' preso: riprova da Online con un altro.");
        } else if (!strcmp(why, "nome")) {
            info("Nome non valido: da 3 a 12 lettere, cifre o spazi.");
        } else if (!strcmp(why, "password")) {
            info("Nome o password sbagliati. Riprova da Online > Ho gia' un account.");
        } else if (!strcmp(why, "bloccato")) {
            snprintf(text, sizeof(text),
                     "Troppe password sbagliate di fila: per questo account riprova tra %d minuti.",
                     (int)Json_Number(Json_Member(root, "minuti"), 10));
            info(text);
        } else if (!strcmp(why, "senza password")) {
            info("Questo account non ha ancora una password. Sceglila sul gioco dove l'hai creato (Online, te la "
                 "chiede da solo), poi riprova da qui.");
        } else if (!strcmp(why, "password corta")) {
            snprintf(text, sizeof(text), "La password deve avere almeno %d lettere o cifre.", PASSWORD_MIN);
            info(text);
        } else if (!strcmp(why, "gettone")) {
            Cloud_Lost();
            forget_account();
            in_lobby = 0;
            __atomic_store_n(&paused, 1, __ATOMIC_RELEASE);
            info("Il tuo account online non esiste piu'. Creane uno nuovo da Online.");
        } else if (!strcmp(why, "altrove")) {
            Cloud_Lost();
            in_lobby = 0;
            __atomic_store_n(&paused, 1, __ATOMIC_RELEASE);
            info("Il tuo account si e' collegato da un altro gioco: qui la lobby si ferma. Riapri Online per "
                 "tornare qui.");
        } else if (!strcmp(why, "offline")) {
            if (challenged[0]) {
                snprintf(text, sizeof(text), "%s non e' piu' online.", challenged);
                challenged[0] = '\0';
                info(text);
            }
        }
    }
    Json_Free(document);
}

void Lobby_Frame(void)
{
    static char lines[sizeof(inbox) + 1];
    static size_t kept;
    char *line, *end;
    size_t length;
    if (!thread_running) return;
    pthread_mutex_lock(&lock);
    length = inbox_length;
    if (kept + length > sizeof(lines) - 1) kept = 0;
    memcpy(lines + kept, inbox, length);
    inbox_length = 0;
    pthread_mutex_unlock(&lock);
    if (length) {
        length += kept;
        lines[length] = '\0';
        for (line = lines; (end = memchr(line, '\n', length - (size_t)(line - lines))); line = end + 1) {
            *end = '\0';
            if (*line) handle(line);
        }
        /* A line the next frame finishes. */
        kept = length - (size_t)(line - lines);
        memmove(lines, line, kept);
    }
    /* The password question over nothing, or over the lobby's own list. */
    if (ask_password && in_lobby &&
        (!Menu_NoticeShown() || screen == SCREEN_LIST || screen == SCREEN_JOINING))
        ask_for_password();
    Cloud_Frame();
}

/* --- start ----------------------------------------------------------- */

static int flag_off(const char *name)
{
    const char *value = getenv(name);
    return value && !strcmp(value, "0");
}

static void read_host(void)
{
    const char *named = getenv("MEMORIES_LOBBY_HOST");
    char *colon;
    snprintf(host, sizeof(host), "%s", named && *named ? named : LOBBY_HOST);
    if ((colon = strchr(host, ':'))) {
        port = (unsigned)strtoul(colon + 1, NULL, 10);
        *colon = '\0';
        if (!port) port = 80;
    }
}

void Lobby_Start(void)
{
    const char *on = getenv("MEMORIES_LOBBY"), *wanted = getenv("MEMORIES_LOBBY_CHALLENGE");
    if (flag_off("MEMORIES_LOBBY") || Netplay_Asked() || getenv("MEMORIES_HEADLESS")) return;
    /* Scripted runs are tests: online only when told. */
    if (!(on && !strcmp(on, "1")) && (getenv("MEMORIES_SDL_SCRIPT") || getenv("MEMORIES_INPUT"))) return;
    read_host();
    test_accept = getenv("MEMORIES_LOBBY_ACCEPT") && !strcmp(getenv("MEMORIES_LOBBY_ACCEPT"), "1");
    read_account();
    /* The test's challenge once: not again when back from its duel (a
     * result to tell), which the restart's environment cannot say. */
    if (wanted && !result_count) snprintf(test_challenge, sizeof(test_challenge), "%s", wanted);
    test_password = getenv("MEMORIES_LOBBY_TEST_PASSWORD");
    if (!token[0]) {
        /* No account: nothing until Online..., or the test's. */
        const char *made = getenv("MEMORIES_LOBBY_TEST_REGISTER"), *login = getenv("MEMORIES_LOBBY_TEST_LOGIN");
        char fields[3][32] = {"", "", ""};
        const char *given = made && *made ? made : login && *login ? login : NULL;
        int count = 0;
        if (!given) return;
        while (count < 3) {
            size_t span = strcspn(given, ":");
            snprintf(fields[count++], sizeof(fields[0]), "%.*s", (int)(span < 31 ? span : 31), given);
            if (!given[span]) break;
            given += span + 1;
        }
        flow = made && *made ? FLOW_MAKE : FLOW_LOGIN;
        snprintf(typed_name, sizeof(typed_name), "%s", fields[0]);
        snprintf(typed_invite, sizeof(typed_invite), "%s", flow == FLOW_MAKE ? fields[1] : "");
        fprintf(stderr, "memories-pc: lobby: test %s %s\n", flow == FLOW_MAKE ? "register" : "login", typed_name);
        send_account(flow == FLOW_MAKE && count < 3 ? NULL : fields[flow == FLOW_MAKE ? 2 : 1], 1);
        start_thread();
        return;
    }
    share_token();
    Cloud_Start(my_name);
    fprintf(stderr, "memories-pc: lobby: connecting to %s as %s\n", host, my_name);
    start_thread();
}

void Lobby_Open(void)
{
    if (Netplay_Asked()) {
        if (Netplay_AskResign()) return; /* the duel runs: Game > Online... is the way to resign by touch */
        info("Sei in un duello online: la lobby torna quando riavvii il gioco.");
        return;
    }
    if (flag_off("MEMORIES_LOBBY")) {
        info("La lobby online e' spenta (MEMORIES_LOBBY=0).");
        return;
    }
    if (!host[0]) {
        read_host();
        read_account();
        share_token();
    }
    __atomic_store_n(&paused, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&wake, 1, __ATOMIC_RELEASE);
    if (start_thread()) {
        info("La lobby online non parte su questo dispositivo.");
        return;
    }
    if (!token[0]) {
        show_start();
        return;
    }
    Cloud_Start(my_name);
    show_list();
}
