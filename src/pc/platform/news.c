/* fork: the update notice (news.h). */
#include "pc/compat/fs.h"
#include "news.h"
#include "menu.h"
#include "paths.h"
#include "platform.h"
#include "pc/mods/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NEWS_LINES 10

enum { NOTICE_NONE, NOTICE_NEWER, NOTICE_NEWS };

static int pending, told;
static char page[512], text[1024];

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

void News_Version(const JsonValue *message)
{
    const JsonValue *lines = Json_Member(message, "novita");
    const char *given = Json_String(Json_Member(message, "link"), "");
    long newest = Json_Number(Json_Member(message, "versione"), 0);
    int i, count = lines ? Json_Count(lines) : 0;
    size_t at;
    /* Once a session: the lobby says it again at each reconnection. */
    if (told || newest <= 0) return;
    if (newest > NEWS_VERSION) {
        pending = NOTICE_NEWER;
        snprintf(text, sizeof(text), "C'e' una versione nuova del gioco. Novita':\n");
    } else if (newest == NEWS_VERSION && seen_version() < NEWS_VERSION && count > 0) {
        pending = NOTICE_NEWS;
        snprintf(text, sizeof(text), "In questa versione:\n");
    } else {
        return;
    }
    told = 1;
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
    fprintf(stderr, "memories-pc: news: version %ld out, this is %d\n", newest, NEWS_VERSION);
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
    if (!pending || Menu_NoticeShown()) return;
    if (pending == NOTICE_NEWER) {
        if (page[0]) Menu_ShowNotice("Aggiornamento disponibile", text, update, 2, 0, newer_chosen);
        else Menu_ShowNotice("Aggiornamento disponibile", text, ok, 1, 0, NULL);
    } else {
        Menu_ShowNotice("Cosa c'e' di nuovo", text, ok, 1, 0, news_chosen);
    }
    pending = NOTICE_NONE;
}
