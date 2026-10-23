/* fork: the update notice (news.h). The thread owns `answer` until it
 * publishes `done`; the main thread reads it only after that. */
#define _GNU_SOURCE
#include "pc/compat/fs.h"
#include "news.h"
#include "menu.h"
#include "paths.h"
#include "platform.h"
#include "pc/compat/signal.h"
#include "pc/debug/netplay_net.h"
#include "pc/mods/json.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NEWS_LINES 10
#define NEWS_WAIT_MS 10000

enum { NOTICE_NONE, NOTICE_NEWER, NOTICE_NEWS };

static char host[256], answer[8192];
static unsigned port;
static int started, done; /* done: 1 answered, -1 failed (atomic) */
static int pending;
static char page[512], text[1024];

static void *run(void *unused)
{
    (void)unused;
    __atomic_store_n(&done, NetplayNet_HttpGet(host, port, "/versione", answer, sizeof(answer), NEWS_WAIT_MS) < 0 ? -1 : 1,
                     __ATOMIC_RELEASE);
    return NULL;
}

void News_Start(const char *from, unsigned at)
{
    pthread_t thread;
    sigset_t all, previous;
    int error;
    if (started) return;
    snprintf(host, sizeof(host), "%s", from);
    port = at;
    /* The game's clock signal belongs to the main thread (platform_common.c). */
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &previous);
    error = pthread_create(&thread, NULL, run, NULL);
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    if (error) return;
    pthread_detach(thread);
    started = 1;
}

static long seen_version(void)
{
    char path[1024], line[32];
    FILE *file;
    long seen = 0;
    snprintf(path, sizeof(path), "%s/updates/news-seen.txt", Paths_UserDir()); /* nothing created */
    if ((file = fopen(path, "r"))) {
        if (fgets(line, sizeof(line), file)) seen = strtol(line, NULL, 10);
        fclose(file);
    }
    return seen;
}

static void write_seen(long version)
{
    char path[1024];
    FILE *file;
    if (Paths_User(path, sizeof(path), "updates/news-seen.txt")) return;
    if ((file = fopen(path, "w"))) {
        fprintf(file, "%ld\n", version);
        fclose(file);
    }
}

/* What the server said, as the notice to show (pending), or nothing. */
static void read_answer(void)
{
    char error[128];
    JsonDocument *document = Json_Parse(answer, error, sizeof(error));
    const JsonValue *root = document ? Json_Root(document) : NULL;
    const JsonValue *lines = root ? Json_Member(root, "novita") : NULL;
    const char *given = root ? Json_String(Json_Member(root, "link"), "") : "";
    long newest = root ? Json_Number(Json_Member(root, "versione"), 0) : 0;
    int i, count = lines ? Json_Count(lines) : 0;
    size_t at;
    fprintf(stderr, "memories-pc: news: version %ld is the newest, this is %d\n", newest, NEWS_VERSION);
    if (newest > NEWS_VERSION) {
        pending = NOTICE_NEWER;
        snprintf(text, sizeof(text), "C'e' una versione nuova del gioco.%s\n", count ? " Novita':" : "");
    } else if (newest == NEWS_VERSION && count > 0 && seen_version() < NEWS_VERSION) {
        pending = NOTICE_NEWS;
        snprintf(text, sizeof(text), "In questa versione:\n");
    }
    if (pending) {
        /* Only a web page: the server never hands the system anything else to open. */
        snprintf(page, sizeof(page), "%s", strncmp(given, "https://", 8) ? "" : given);
        for (i = 0; i < count && i < NEWS_LINES; i++) {
            at = strlen(text);
            snprintf(text + at, sizeof(text) - at, "- %s\n", Json_String(Json_At(lines, i), ""));
        }
        if (pending == NOTICE_NEWER) {
            at = strlen(text);
            snprintf(text + at, sizeof(text) - at,
                     "\nScaricala e installala sopra questa: salvataggi e impostazioni restano.%s",
                     page[0] ? "" : "\nLa trovi dove hai preso il gioco.");
        }
    }
    if (document) Json_Free(document);
}

static void newer_chosen(int button, int *quit)
{
    (void)quit;
    if (button == 0 && page[0] && Platform_OpenUrl(page))
        fprintf(stderr, "memories-pc: news: could not open %s\n", page);
}

static void news_chosen(int button, int *quit)
{
    (void)button, (void)quit;
    write_seen(NEWS_VERSION);
}

void News_Frame(void)
{
    static const char *const update[] = {"Aggiorna", "Piu' tardi"};
    static const char *const ok[] = {"OK"};
    static int finished;
    int answered;
    if (!started || finished) return;
    if (!pending) {
        answered = __atomic_load_n(&done, __ATOMIC_ACQUIRE);
        if (!answered) return;
        if (answered > 0) read_answer();
        else fprintf(stderr, "memories-pc: news: no answer from %s\n", host);
        if (!pending) {
            finished = 1;
            return;
        }
    }
    /* After any other notice (the lobby's password question, say). */
    if (Menu_NoticeShown()) return;
    if (pending == NOTICE_NEWS) Menu_ShowNotice("Cosa c'e' di nuovo", text, ok, 1, 0, news_chosen);
    else if (page[0]) Menu_ShowNotice("Aggiornamento disponibile", text, update, 2, 0, newer_chosen);
    else Menu_ShowNotice("Aggiornamento disponibile", text, ok, 1, 0, NULL);
    pending = NOTICE_NONE;
    finished = 1;
}
