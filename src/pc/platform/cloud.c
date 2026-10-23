/* The saves on the online account (cloud.h). Everything here runs on the
 * main thread; the slot writes only set a bit (they may come from anywhere),
 * Cloud_Frame does the rest. One request at a time is in flight ("manda" or
 * "scarica", a slot file each way), so the lobby's buffers never hold more
 * than one. */
#define _GNU_SOURCE
#include "pc/compat/fs.h"
#include "cloud.h"
#include "lobby.h"
#include "menu.h"
#include "paths.h"
#include "platform.h"
#include "pc/saves/deck_menu.h"
#include "pc/saves/save_menu.h"
#include "pc/saves/save_slots.h"
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#elif defined(__ANDROID__)
#include <sys/system_properties.h>
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#include <sys/utsname.h>
#endif

#define HASH 17
#define BASE64 ((SAVE_SLOT_FILE_SIZE + 2) / 3 * 4)
#define TEST_WAIT 120 /* frames the test's answer waits, for a photo of the question */
#define FILES_MAX 32
#define FILE_MAX (160 * 1024) /* worker.js FILE_MAX */
#define FILE_BASE64 ((FILE_MAX + 2) / 3 * 4)
#define FILE_LOOK 300 /* frames between looks at the files */
#define DIARY "mod-data/diario/diario.txt"
#define ARENA "mod-data/arena/arena.txt"

typedef struct {
    int version; /* 0: none */
    long long when;
    char device[48];
    char hash[HASH]; /* the file's, as the device that sent it said */
} Copy;

static struct {
    int synced;       /* the account's version this device sent or took last, 0 for none */
    char hash[HASH];  /* the slot file's hash then */
    Copy server;      /* the account's latest, once listed */
    int later;        /* the player put the conflict off: not again on this connection */
    int take;         /* the player chose the account's: taken even though the local one changed */
} slots[SAVE_SLOT_COUNT];

/* The other files on the account (decks/<code>.txt, online-deck.txt, the
 * diary, the arena's run), sent and taken like the slots. A conflict asks
 * nothing: the diary's lines are joined (it only grows), the rest goes to
 * the newer one. */
typedef struct {
    char name[40];
    int synced;
    char hash[HASH];
    long size;            /* the file's size then: the diary's lines after it are this device's */
    Copy server;
    int later;
    char local[HASH];     /* "" for none */
    long local_size;
    long long local_when; /* its time: with the size, what says to read it again */
} Shared;

static Shared files[FILES_MAX];
static int file_count, files_listed, file_busy = -1, file_look;
static char file_busy_hash[HASH];
static long file_busy_size;
static long long file_busy_when;

/* The slot files' hashes, "" for none: read at the start, at a join and
 * after each write, never every frame. */
static char local[SAVE_SLOT_COUNT][HASH];

static char account[16];
static int started, listed, busy = -1; /* busy: the slot of the request in flight */
static char busy_hash[HASH];
static long long busy_when;
static unsigned dirty; /* slots written since the last frame; atomic */
static int asking = -1, test_wait;
static char news[600]; /* the downloads to tell, once nothing else is up */
static int news_restart;

/* --- small things ---------------------------------------------------- */

static void hash_bytes(const unsigned char *data, long size, char out[HASH])
{
    uint64_t value = 1469598103934665603ull; /* FNV-1a */
    long i;
    for (i = 0; i < size; i++) value = (value ^ data[i]) * 1099511628211ull;
    snprintf(out, HASH, "%016llx", (unsigned long long)value);
}

static void hash_of(const unsigned char *image, char out[HASH]) { hash_bytes(image, SAVE_SLOT_FILE_SIZE, out); }

/* The slot file as it is: 1 with its bytes, hash and time, 0 when there is none. */
static int read_slot(int slot, unsigned char image[SAVE_SLOT_FILE_SIZE], char hash[HASH], long long *when)
{
    char path[1024];
    struct stat info;
    FILE *file;
    size_t got;
    if (SaveSlots_Path(slot, path, sizeof(path)) || !(file = fopen(path, "rb"))) return 0;
    memset(image, 0, SAVE_SLOT_FILE_SIZE);
    got = fread(image, 1, SAVE_SLOT_FILE_SIZE, file);
    fclose(file);
    if (!got) return 0;
    hash_of(image, hash);
    if (when) *when = stat(path, &info) ? (long long)time(NULL) : (long long)info.st_mtime;
    return 1;
}

static void look(int slot)
{
    static unsigned char image[SAVE_SLOT_FILE_SIZE];
    if (!read_slot(slot, image, local[slot], NULL)) local[slot][0] = '\0';
}

static const char DIGITS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void encode(const unsigned char *data, size_t size, char *out)
{
    size_t i, used = 0;
    for (i = 0; i < size; i += 3) {
        uint32_t three = (uint32_t)data[i] << 16 | (i + 1 < size ? (uint32_t)data[i + 1] << 8 : 0) |
                         (i + 2 < size ? data[i + 2] : 0);
        out[used++] = DIGITS[three >> 18 & 63];
        out[used++] = DIGITS[three >> 12 & 63];
        out[used++] = i + 1 < size ? DIGITS[three >> 6 & 63] : '=';
        out[used++] = i + 2 < size ? DIGITS[three & 63] : '=';
    }
    out[used] = '\0';
}

/* The bytes, or -1 for a text that is not base64 of at most `size` bytes. */
static long decode(const char *text, unsigned char *out, size_t size)
{
    size_t used = 0;
    uint32_t bits = 0;
    int count = 0;
    for (; *text && *text != '='; text++) {
        const char *at = strchr(DIGITS, *text);
        if (!at || !*text) return -1;
        bits = bits << 6 | (uint32_t)(at - DIGITS);
        if (++count == 4) {
            if (used + 3 > size) return -1;
            out[used++] = (unsigned char)(bits >> 16), out[used++] = (unsigned char)(bits >> 8);
            out[used++] = (unsigned char)bits;
            bits = 0, count = 0;
        }
    }
    if (count == 1) return -1;
    if (count >= 2) {
        if (used + (size_t)(count - 1) > size) return -1;
        bits <<= 6 * (4 - count);
        out[used++] = (unsigned char)(bits >> 16);
        if (count == 3) out[used++] = (unsigned char)(bits >> 8);
    }
    return (long)used;
}

/* "PC", "Android (model)", "iPhone (model)": the copies' "dispositivo". */
static const char *device(void)
{
    static char name[48];
    char model[96] = "";
    size_t i, used = 0;
    if (name[0]) return name;
#if defined(_WIN32)
    snprintf(name, sizeof(name), "PC");
#elif defined(__ANDROID__)
    __system_property_get("ro.product.model", model);
    snprintf(name, sizeof(name), model[0] ? "Android (%s)" : "Android", model);
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    {
        struct utsname about;
        if (!uname(&about)) snprintf(model, sizeof(model), "%s", about.machine);
    }
    snprintf(name, sizeof(name), model[0] ? "iPhone (%s)" : "iPhone", model);
#else
    snprintf(name, sizeof(name), "PC");
#endif
    (void)model;
    /* Only what JSON takes as it is. */
    for (i = 0; name[i]; i++)
        if ((unsigned char)name[i] >= ' ' && (unsigned char)name[i] < 127 && name[i] != '"' && name[i] != '\\')
            name[used++] = name[i];
    name[used] = '\0';
    return name;
}

/* "23/10 18:40" in the device's time. */
static void when_text(long long when, char *out, size_t size)
{
    time_t at = (time_t)when;
    struct tm *parts = when > 0 ? localtime(&at) : NULL;
    if (parts) strftime(out, size, "%d/%m %H:%M", parts);
    else snprintf(out, size, "?");
}

/* --- saves/cloud.txt ------------------------------------------------- */

static int state_path(char *path, size_t size) { return Paths_User(path, size, "saves/cloud.txt"); }

static void write_state(void)
{
    char path[1024];
    FILE *file;
    int slot;
    if (state_path(path, sizeof(path)) || !(file = fopen(path, "wb"))) {
        fprintf(stderr, "memories-pc: cloud: cannot write saves/cloud.txt\n");
        return;
    }
    fprintf(file, "account=%s\n", account);
    for (slot = 0; slot < SAVE_SLOT_COUNT; slot++)
        if (slots[slot].synced || slots[slot].hash[0])
            fprintf(file, "slot=%d versione=%d hash=%s\n", slot + 1, slots[slot].synced,
                    slots[slot].hash[0] ? slots[slot].hash : "-");
    for (slot = 0; slot < file_count; slot++)
        if (files[slot].synced || files[slot].hash[0])
            fprintf(file, "file=%s versione=%d hash=%s byte=%ld\n", files[slot].name, files[slot].synced,
                    files[slot].hash[0] ? files[slot].hash : "-", files[slot].size);
    fclose(file);
}

static int find_file(const char *name);

static void read_state(void)
{
    char path[1024], line[160], hash[HASH + 8], name[48];
    FILE *file;
    int slot, version, mine = 0;
    long size;
    for (slot = 0; slot < SAVE_SLOT_COUNT; slot++) slots[slot].synced = 0, slots[slot].hash[0] = '\0';
    file_count = 0;
    if (state_path(path, sizeof(path)) || !(file = fopen(path, "rb"))) return;
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = '\0';
        /* What another account left: nothing of this one's. */
        if (!strncmp(line, "account=", 8)) mine = !strcmp(line + 8, account);
        else if (mine && sscanf(line, "slot=%d versione=%d hash=%16s", &slot, &version, hash) == 3 && slot >= 1 &&
                 slot <= SAVE_SLOT_COUNT) {
            slots[slot - 1].synced = version;
            snprintf(slots[slot - 1].hash, HASH, "%s", strcmp(hash, "-") ? hash : "");
        } else if (mine && sscanf(line, "file=%39s versione=%d hash=%16s byte=%ld", name, &version, hash, &size) == 4 &&
                   (slot = find_file(name)) >= 0) {
            files[slot].synced = version;
            snprintf(files[slot].hash, HASH, "%s", strcmp(hash, "-") ? hash : "");
            files[slot].size = size;
        }
    }
    fclose(file);
}

/* --- the requests ---------------------------------------------------- */

static void upload(int slot, int base)
{
    static unsigned char image[SAVE_SLOT_FILE_SIZE];
    static char line[BASE64 + 256], data[BASE64 + 1];
    long long when = 0;
    if (!read_slot(slot, image, busy_hash, &when)) {
        local[slot][0] = '\0'; /* gone meanwhile */
        return;
    }
    encode(image, SAVE_SLOT_FILE_SIZE, data);
    snprintf(line, sizeof(line),
             "{\"t\":\"manda\",\"slot\":%d,\"base\":%d,\"quando\":%lld,\"dispositivo\":\"%s\",\"hash\":\"%s\","
             "\"dati\":\"%s\"}",
             slot, base, when, device(), busy_hash, data);
    Lobby_Send(line);
    busy = slot;
    busy_when = when;
}

static void download(int slot)
{
    char line[64];
    snprintf(line, sizeof(line), "{\"t\":\"scarica\",\"slot\":%d}", slot);
    Lobby_Send(line);
    busy = slot;
}

/* The account's copy, `data`, in place of the slot's file: the local one
 * first to saves/backup/. */
static void take(int slot, int version, long long when, const char *from, const char *data)
{
    static unsigned char image[SAVE_SLOT_FILE_SIZE], old[SAVE_SLOT_FILE_SIZE];
    char hash[HASH], old_hash[HASH], path[1024], name[64], date[32], line[200];
    long long old_when = 0;
    if (decode(data, image, sizeof(image)) != SAVE_SLOT_FILE_SIZE) {
        fprintf(stderr, "memories-pc: cloud: save slot %d from the account is not a save\n", slot + 1);
        slots[slot].later = 1;
        return;
    }
    hash_of(image, hash);
    if (read_slot(slot, old, old_hash, &old_when) && strcmp(old_hash, hash)) {
        time_t at = (time_t)old_when;
        struct tm *parts = localtime(&at);
        char stamp[32] = "0";
        FILE *file;
        if (parts) strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", parts);
        snprintf(name, sizeof(name), "saves/backup/slot%02d-%s.sav", slot + 1, stamp);
        if (Paths_User(path, sizeof(path), name) || !(file = fopen(path, "wb")) ||
            (fwrite(old, 1, SAVE_SLOT_FILE_SIZE, file) != SAVE_SLOT_FILE_SIZE) + fclose(file)) {
            /* No copy, no replacing it. */
            fprintf(stderr, "memories-pc: cloud: cannot keep a copy of save slot %d, not replaced\n", slot + 1);
            slots[slot].later = 1;
            return;
        }
        fprintf(stderr, "memories-pc: cloud: local save slot %d kept in %s\n", slot + 1, name);
    }
    /* Known as the account's before it is written, so its write is not one to send. */
    slots[slot].synced = version;
    snprintf(slots[slot].hash, HASH, "%s", hash);
    slots[slot].take = 0;
    write_state();
    if (SaveSlots_WriteImage(slot, image)) {
        fprintf(stderr, "memories-pc: cloud: cannot write save slot %d: %s\n", slot + 1, SaveSlots_LastError());
        slots[slot].later = 1;
        return;
    }
    __atomic_and_fetch(&dirty, ~(1u << slot), __ATOMIC_ACQ_REL);
    snprintf(local[slot], HASH, "%s", hash);
    fprintf(stderr, "memories-pc: cloud: save slot %d downloaded (version %d)\n", slot + 1, version);
    when_text(when, date, sizeof(date));
    snprintf(line, sizeof(line), "Slot %d: salvataggio aggiornato dall'account (%s, %s).\n", slot + 1, date, from);
    if (strlen(news) + strlen(line) < sizeof(news)) strcat(news, line);
    if (slot == SaveMenu_CurrentSlot()) news_restart = 1;
}

/* --- the other files ------------------------------------------------- */

static int valid_name(const char *name)
{
    int i;
    if (!strcmp(name, "online-deck.txt") || !strcmp(name, DIARY) || !strcmp(name, ARENA)) return 1;
    if (strncmp(name, "decks/", 6) || strlen(name) != 18 || strcmp(name + 14, ".txt")) return 0;
    for (i = 6; i < 14; i++)
        if (!name[i] || !strchr("0123456789ABCDEF", name[i])) return 0;
    return 1;
}

/* The file's entry, made when it is new; -1 for a name not sent. */
static int find_file(const char *name)
{
    int i;
    if (!valid_name(name)) return -1;
    for (i = 0; i < file_count; i++)
        if (!strcmp(files[i].name, name)) return i;
    if (file_count == FILES_MAX) return -1;
    memset(&files[file_count], 0, sizeof(files[file_count]));
    snprintf(files[file_count].name, sizeof(files[file_count].name), "%s", name);
    files[file_count].local_when = -1;
    return file_count++;
}

/* The file as it is: its size with its bytes and hash, 0 when there is
 * none, -1 when it is too big to send. */
static long read_file(const char *name, unsigned char *data, char hash[HASH], long long *when)
{
    char path[1024];
    struct stat info;
    FILE *file;
    long got;
    if (Paths_User(path, sizeof(path), name) || !(file = fopen(path, "rb"))) return 0;
    got = (long)fread(data, 1, FILE_MAX + 1, file);
    fclose(file);
    if (got > FILE_MAX) return -1;
    hash_bytes(data, got, hash);
    if (when) *when = stat(path, &info) ? (long long)time(NULL) : (long long)info.st_mtime;
    return got;
}

/* The files' hashes again (`all`: even those whose time and size are the same). */
static void look_files(int all)
{
    static unsigned char data[FILE_MAX + 1];
    char path[1024];
    struct stat info;
    DIR *folder;
    struct dirent *item;
    int i;
    find_file("online-deck.txt");
    find_file(DIARY);
    find_file(ARENA);
    if (!Paths_User(path, sizeof(path), "decks") && (folder = opendir(path))) {
        while ((item = readdir(folder)) != NULL) {
            char name[300];
            snprintf(name, sizeof(name), "decks/%s", item->d_name);
            if (valid_name(name)) find_file(name);
        }
        closedir(folder);
    }
    for (i = 0; i < file_count; i++) {
        Shared *f = &files[i];
        long size;
        if (Paths_User(path, sizeof(path), f->name) || stat(path, &info)) {
            f->local[0] = '\0', f->local_size = 0, f->local_when = 0;
            continue;
        }
        if (!all && f->local[0] && (long long)info.st_mtime == f->local_when && (long)info.st_size == f->local_size)
            continue;
        size = read_file(f->name, data, f->local, &f->local_when);
        if (size < 0 && !f->later) {
            fprintf(stderr, "memories-pc: cloud: %s is too big to send\n", f->name);
            f->later = 1;
        }
        if (size <= 0) f->local[0] = '\0';
        f->local_size = size > 0 ? size : 0;
    }
}

static void upload_file(int i, int base)
{
    static unsigned char data[FILE_MAX + 1];
    static char text[FILE_BASE64 + 1], line[FILE_BASE64 + 512];
    long long when = 0;
    long size = read_file(files[i].name, data, file_busy_hash, &when);
    if (size <= 0) {
        files[i].local[0] = '\0'; /* gone meanwhile, or too big now */
        if (size < 0) files[i].later = 1;
        return;
    }
    encode(data, (size_t)size, text);
    snprintf(line, sizeof(line),
             "{\"t\":\"manda_file\",\"nome\":\"%s\",\"base\":%d,\"quando\":%lld,\"dispositivo\":\"%s\",\"hash\":\"%s\","
             "\"dati\":\"%s\"}",
             files[i].name, base, when, device(), file_busy_hash, text);
    Lobby_Send(line);
    file_busy = i;
    file_busy_size = size;
    file_busy_when = when;
}

static void download_file(int i)
{
    char line[96];
    snprintf(line, sizeof(line), "{\"t\":\"scarica_file\",\"nome\":\"%s\"}", files[i].name);
    Lobby_Send(line);
    file_busy = i;
}

/* `size` bytes in place of the file: the local one first to saves/backup/. */
static int write_file(const Shared *f, const unsigned char *data, long size)
{
    static unsigned char old[FILE_MAX + 1];
    char path[1024], partial[1040], folder[1024], name[96], hash[HASH], *slash;
    long long old_when = 0;
    long old_size = read_file(f->name, old, hash, &old_when);
    FILE *file;
    int failed;
    size_t i;
    if (old_size > 0) {
        time_t at = (time_t)old_when;
        struct tm *parts = localtime(&at);
        char stamp[32] = "0", flat[40];
        snprintf(flat, sizeof(flat), "%s", f->name);
        for (i = 0; flat[i]; i++)
            if (flat[i] == '/') flat[i] = '-';
        if (parts) strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", parts);
        snprintf(name, sizeof(name), "saves/backup/%s-%s", stamp, flat);
        if (Paths_User(path, sizeof(path), name) || !(file = fopen(path, "wb")) ||
            (fwrite(old, 1, (size_t)old_size, file) != (size_t)old_size) + fclose(file)) {
            fprintf(stderr, "memories-pc: cloud: cannot keep a copy of %s, not replaced\n", f->name);
            return -1;
        }
    }
    if (Paths_User(path, sizeof(path), f->name)) return -1;
    snprintf(folder, sizeof(folder), "%s", path);
    slash = strrchr(folder, '/');
    if (strrchr(folder, '\\') > slash) slash = strrchr(folder, '\\');
    if (slash) {
        *slash = '\0';
        Paths_MakeDirs(folder);
    }
    snprintf(partial, sizeof(partial), "%s.part", path);
    if (!(file = fopen(partial, "wb"))) {
        fprintf(stderr, "memories-pc: cloud: cannot write %s\n", f->name);
        return -1;
    }
    failed = fwrite(data, 1, (size_t)size, file) != (size_t)size;
    if (fclose(file) != 0) failed = 1;
    if (failed || rename(partial, path) != 0) {
        fprintf(stderr, "memories-pc: cloud: cannot write %s\n", f->name);
        remove(partial);
        return -1;
    }
    return 0;
}

/* The account's copy in place of the file. The diary keeps the lines this
 * device wrote since it last sent or took it, after the account's: they go
 * to the account next. */
static void take_file(int i, const char *text)
{
    static unsigned char data[FILE_MAX + 1], mine[FILE_MAX + 1];
    Shared *f = &files[i];
    char hash[HASH], start[HASH];
    long size = decode(text, data, FILE_MAX), joined, have;
    long long when = 0;
    if (size < 0) {
        fprintf(stderr, "memories-pc: cloud: %s from the account is not a file\n", f->name);
        f->later = 1;
        return;
    }
    hash_bytes(data, size, hash);
    joined = size;
    have = read_file(f->name, mine, start, &when);
    if (!strcmp(f->name, DIARY) && have > 0 && strcmp(start, f->hash) && have >= f->size) {
        /* Only lines added since: what was sent or taken then is this file's start. */
        hash_bytes(mine, f->size, start);
        if ((!f->hash[0] && !f->size) || !strcmp(start, f->hash)) {
            long tail = have - f->size;
            if (size && data[size - 1] != '\n' && size < FILE_MAX) data[joined++] = '\n';
            if (joined + tail <= FILE_MAX) {
                memcpy(data + joined, mine + f->size, (size_t)tail);
                joined += tail;
                fprintf(stderr, "memories-pc: cloud: %ld byte(s) of this device's diary kept after the account's\n",
                        tail);
            } else {
                joined = size;
            }
        }
    }
    if (write_file(f, data, joined)) {
        f->later = 1;
        return;
    }
    f->synced = f->server.version;
    snprintf(f->hash, HASH, "%s", hash);
    f->size = size;
    write_state();
    look_files(1);
    fprintf(stderr, "memories-pc: cloud: %s downloaded (version %d)\n", f->name, f->synced);
    if (!strncmp(f->name, "decks/", 6)) DeckMenu_FileReplaced();
}

/* Like step() for the slots, without questions. */
static void files_step(void)
{
    int i;
    if (!files_listed) return;
    for (i = 0; i < file_count; i++) {
        Shared *f = &files[i];
        int here = f->local[0] != '\0';
        int changed = here && strcmp(f->local, f->hash) != 0;
        if (f->later) continue;
        if (f->server.version < f->synced) {
            f->synced = f->server.version;
            f->hash[0] = '\0';
            f->size = 0;
            changed = here;
        }
        if (f->server.version > f->synced) {
            if (here && !strcmp(f->local, f->server.hash)) {
                f->synced = f->server.version;
                snprintf(f->hash, HASH, "%s", f->local);
                f->size = f->local_size;
                write_state();
                continue;
            }
            /* Changed here too: the diary is joined, the rest goes to the newer. */
            if (changed && strcmp(f->name, DIARY) && f->local_when > f->server.when) {
                fprintf(stderr, "memories-pc: cloud: %s changed here and there, this one is newer\n", f->name);
                upload_file(i, f->server.version);
            } else {
                download_file(i);
            }
            return;
        }
        if (changed) {
            upload_file(i, f->synced);
            return;
        }
    }
}

static int files_waiting(void)
{
    int i, waiting = 0;
    for (i = 0; i < file_count; i++)
        if (files[i].local[0] && strcmp(files[i].local, files[i].hash) && !files[i].later) waiting++;
    return waiting;
}

/* --- the questions --------------------------------------------------- */

static void ask(int slot);

static void conflict_chosen(int button, int *quit)
{
    int slot = asking;
    (void)quit;
    asking = -1;
    if (slot < 0) return;
    if (button == 0) {
        fprintf(stderr, "memories-pc: cloud: kept local (slot %d)\n", slot + 1);
        /* On top of the account's, which stays there as an older copy. */
        upload(slot, slots[slot].server.version);
    } else if (button == 1) {
        fprintf(stderr, "memories-pc: cloud: kept server (slot %d)\n", slot + 1);
        slots[slot].take = 1;
    } else {
        fprintf(stderr, "memories-pc: cloud: conflict slot %d put off\n", slot + 1);
        slots[slot].later = 1;
    }
}

static void ask(int slot)
{
    static char mine[64], theirs[80];
    const char *buttons[3];
    char text[512], date[32], local_date[32], hash[HASH];
    static unsigned char image[SAVE_SLOT_FILE_SIZE];
    long long when = 0;
    read_slot(slot, image, hash, &when);
    when_text(when, local_date, sizeof(local_date));
    when_text(slots[slot].server.when, date, sizeof(date));
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    snprintf(mine, sizeof(mine), "Questo telefono - %s", local_date);
#else
    snprintf(mine, sizeof(mine), "Questo PC - %s", local_date);
#endif
    snprintf(theirs, sizeof(theirs), "Account (%s) - %s", slots[slot].server.device, date);
    snprintf(text, sizeof(text),
             "Slot %d: il salvataggio e' cambiato sia qui sia sull'account. Quale tieni?\n\n"
             "Quello che non scegli resta come copia di sicurezza.",
             slot + 1);
    buttons[0] = mine, buttons[1] = theirs, buttons[2] = "Decidi dopo";
    asking = slot;
    Menu_ShowNotice("Salvataggi sull'account", text, buttons, 3, 2, conflict_chosen);
}

static void news_closed(int button, int *quit)
{
    (void)quit;
    if (news_restart && button == 0) {
        fprintf(stderr, "memories-pc: cloud: restarting for the new save\n");
        fflush(stderr);
        Platform_RestartGame();
    }
    news_restart = 0;
}

static void tell_news(void)
{
    static const char *const ok[] = {"OK"};
    static const char *const restart[] = {"Riavvia ora", "Dopo"};
    char text[800];
    if (news_restart)
        snprintf(text, sizeof(text),
                 "%s\nIl gioco in corso usa il salvataggio di prima: riavvia per giocare quello nuovo (se salvi "
                 "prima, quello dell'account resta come copia).",
                 news);
    else snprintf(text, sizeof(text), "%s", news);
    news[0] = '\0';
    Menu_ShowNotice("Salvataggi sull'account", text, news_restart ? restart : ok, news_restart ? 2 : 1, 0,
                    news_closed);
}

/* --- the work -------------------------------------------------------- */

/* The next thing to do: a newer save to take, a conflict to ask, a save to
 * send. */
static void step(void)
{
    int slot;
    if (!listed || busy >= 0 || file_busy >= 0 || asking >= 0 || SaveMenu_Active()) return;
    for (slot = 0; slot < SAVE_SLOT_COUNT; slot++) {
        const char *hash = local[slot];
        int here = hash[0] != '\0';
        int changed = here && strcmp(hash, slots[slot].hash) != 0;
        if (slots[slot].later) continue;
        if (slots[slot].server.version < slots[slot].synced) {
            /* The account has less than this device took (its copies gone): all again. */
            slots[slot].synced = slots[slot].server.version;
            slots[slot].hash[0] = '\0';
            changed = here;
        }
        if (slots[slot].server.version > slots[slot].synced) {
            if (here && !strcmp(hash, slots[slot].server.hash)) {
                /* The same save here and there (e.g. sent from here before cloud.txt was lost). */
                slots[slot].synced = slots[slot].server.version;
                snprintf(slots[slot].hash, HASH, "%s", hash);
                write_state();
                continue;
            }
            if (!changed || slots[slot].take) {
                download(slot);
                return;
            }
            if (Menu_NoticeShown()) continue; /* asked once the window up now is gone */
            fprintf(stderr, "memories-pc: cloud: conflict slot %d\n", slot + 1);
            ask(slot);
            test_wait = TEST_WAIT;
            return;
        }
        if (changed) {
            upload(slot, slots[slot].synced);
            return;
        }
    }
    files_step();
}

static void written(int slot, const unsigned char *image)
{
    (void)image;
    if (slot >= 0 && slot < SAVE_SLOT_COUNT) __atomic_or_fetch(&dirty, 1u << slot, __ATOMIC_ACQ_REL);
}

void Cloud_Start(const char *name)
{
    int slot;
    if (started && !strcmp(account, name)) return;
    snprintf(account, sizeof(account), "%s", name);
    read_state();
    for (slot = 0; slot < SAVE_SLOT_COUNT; slot++) slots[slot].server.version = 0, look(slot);
    SaveSlots_SetWriteListener(written);
    started = 1;
}

void Cloud_Joined(void)
{
    int slot;
    if (!started) return;
    listed = 0;
    busy = -1;
    for (slot = 0; slot < SAVE_SLOT_COUNT; slot++) slots[slot].later = 0, look(slot);
    Lobby_Send("{\"t\":\"salvataggi\"}");
    files_listed = 0;
    file_busy = -1;
    for (slot = 0; slot < file_count; slot++) files[slot].later = 0;
    Lobby_Send("{\"t\":\"file\"}");
}

void Cloud_Lost(void)
{
    listed = 0;
    busy = -1;
    files_listed = 0;
    file_busy = -1;
}

void Cloud_Stop(void)
{
    Cloud_Lost();
    started = 0;
    account[0] = ' ';
}

static void read_copy(const JsonValue *from, Copy *copy)
{
    copy->version = (int)Json_Number(Json_Member(from, "versione"), 0);
    copy->when = Json_Number(Json_Member(from, "quando"), 0);
    snprintf(copy->device, sizeof(copy->device), "%s", Json_String(Json_Member(from, "dispositivo"), "?"));
    snprintf(copy->hash, sizeof(copy->hash), "%s", Json_String(Json_Member(from, "hash"), ""));
}

int Cloud_Handle(const char *type, const JsonValue *root)
{
    int slot = (int)Json_Number(Json_Member(root, "slot"), -1);
    int good = slot >= 0 && slot < SAVE_SLOT_COUNT;
    if (!started) return 0;
    if (!strcmp(type, "salvataggi")) {
        const JsonValue *list = Json_Member(root, "slot");
        int i, count = Json_Count(list);
        for (i = 0; i < SAVE_SLOT_COUNT; i++) slots[i].server.version = 0;
        for (i = 0; i < count; i++) {
            const JsonValue *item = Json_At(list, i);
            int at = (int)Json_Number(Json_Member(item, "slot"), -1);
            if (at >= 0 && at < SAVE_SLOT_COUNT) read_copy(item, &slots[at].server);
        }
        listed = 1;
        fprintf(stderr, "memories-pc: cloud: %d saves on the account\n", count);
    } else if (!strcmp(type, "nuovo")) {
        /* fork: another game open on this account sent this slot: step() takes it (or asks) */
        Copy copy;
        read_copy(root, &copy);
        if (good && copy.version > slots[slot].server.version) {
            slots[slot].server = copy;
            slots[slot].later = 0;
            fprintf(stderr, "memories-pc: cloud: save slot %d is new on the account (version %d, %s)\n", slot + 1,
                    copy.version, copy.device);
        }
    } else if (!strcmp(type, "nuovo_file")) {
        /* fork: the same for the other files: files_step() takes it */
        int at = find_file(Json_String(Json_Member(root, "nome"), ""));
        Copy copy;
        read_copy(root, &copy);
        if (at >= 0 && copy.version > files[at].server.version) {
            files[at].server = copy;
            files[at].later = 0;
            fprintf(stderr, "memories-pc: cloud: %s is new on the account (version %d, %s)\n", files[at].name,
                    copy.version, copy.device);
        }
    } else if (!strcmp(type, "salvato")) {
        if (good && slot == busy) {
            slots[slot].synced = (int)Json_Number(Json_Member(root, "versione"), 0);
            snprintf(slots[slot].hash, HASH, "%s", busy_hash);
            slots[slot].server.version = slots[slot].synced;
            slots[slot].server.when = busy_when;
            snprintf(slots[slot].server.device, sizeof(slots[slot].server.device), "%s", device());
            snprintf(slots[slot].server.hash, HASH, "%s", busy_hash);
            write_state();
            fprintf(stderr, "memories-pc: cloud: save slot %d sent (version %d)\n", slot + 1, slots[slot].synced);
        }
        busy = -1;
    } else if (!strcmp(type, "conflitto")) {
        /* Another device sent one meanwhile: step() asks. */
        if (good) read_copy(root, &slots[slot].server);
        busy = -1;
    } else if (!strcmp(type, "salvataggio")) {
        if (good && slot == busy) {
            Copy copy;
            read_copy(root, &copy);
            slots[slot].server = copy;
            take(slot, copy.version, copy.when, copy.device, Json_String(Json_Member(root, "dati"), ""));
        }
        busy = -1;
    } else if (!strcmp(type, "file")) {
        const JsonValue *list = Json_Member(root, "file");
        int i, at, count = Json_Count(list);
        for (i = 0; i < file_count; i++) files[i].server.version = 0;
        for (i = 0; i < count; i++) {
            const JsonValue *item = Json_At(list, i);
            if ((at = find_file(Json_String(Json_Member(item, "nome"), ""))) >= 0) read_copy(item, &files[at].server);
        }
        look_files(1);
        files_listed = 1;
        fprintf(stderr, "memories-pc: cloud: %d other files on the account\n", count);
    } else if (!strcmp(type, "file_salvato") || !strcmp(type, "conflitto_file") || !strcmp(type, "file_dati") ||
               (!strcmp(type, "errore") && Json_Member(root, "nome"))) {
        int i = file_busy;
        file_busy = -1;
        if (i < 0 || strcmp(files[i].name, Json_String(Json_Member(root, "nome"), ""))) return 1;
        if (!strcmp(type, "file_salvato")) {
            Shared *f = &files[i];
            f->synced = (int)Json_Number(Json_Member(root, "versione"), 0);
            snprintf(f->hash, HASH, "%s", file_busy_hash);
            f->size = file_busy_size;
            f->server.version = f->synced;
            f->server.when = file_busy_when;
            snprintf(f->server.device, sizeof(f->server.device), "%s", device());
            snprintf(f->server.hash, HASH, "%s", file_busy_hash);
            write_state();
            fprintf(stderr, "memories-pc: cloud: %s sent (version %d)\n", f->name, f->synced);
        } else if (!strcmp(type, "conflitto_file")) {
            read_copy(root, &files[i].server); /* files_step() decides */
        } else if (!strcmp(type, "file_dati")) {
            read_copy(root, &files[i].server);
            take_file(i, Json_String(Json_Member(root, "dati"), ""));
        } else {
            fprintf(stderr, "memories-pc: cloud: the server says no: %s (%s)\n",
                    Json_String(Json_Member(root, "motivo"), ""), files[i].name);
            files[i].later = 1;
        }
    } else if (!strcmp(type, "errore") && Json_Member(root, "slot")) {
        fprintf(stderr, "memories-pc: cloud: the server says no: %s (slot %d)\n",
                Json_String(Json_Member(root, "motivo"), ""), slot + 1);
        if (good) slots[slot].later = 1;
        busy = -1;
    } else {
        return 0;
    }
    return 1;
}

void Cloud_Frame(void)
{
    unsigned written_now = __atomic_exchange_n(&dirty, 0, __ATOMIC_ACQ_REL);
    int slot;
    if (!started) return;
    for (slot = 0; slot < SAVE_SLOT_COUNT; slot++)
        if (written_now & 1u << slot) {
            slots[slot].later = 0;
            look(slot);
            if (!listed) fprintf(stderr, "memories-pc: cloud: save slot %d queued\n", slot + 1);
        }
    if (asking >= 0) {
        const char *choice = getenv("MEMORIES_CLOUD_TEST_CHOICE");
        if (choice && test_wait > 0 && --test_wait == 0) {
            Menu_CloseNotice();
            conflict_chosen(!strcmp(choice, "local") ? 0 : 1, NULL);
        } else if (!Menu_NoticeShown()) {
            ask(asking); /* another notice took its place: again once it is gone */
        }
        return;
    }
    if (files_listed && file_busy < 0 && ++file_look >= FILE_LOOK) {
        file_look = 0;
        look_files(0);
    }
    step();
    if (news[0] && busy < 0 && asking < 0 && !Menu_NoticeShown()) tell_news();
}

void Cloud_Status(char *text, size_t size)
{
    static unsigned char image[SAVE_SLOT_FILE_SIZE];
    size_t used = 0;
    int slot, waiting = 0, any = 0;
    text[0] = '\0';
    for (slot = 0; slot < SAVE_SLOT_COUNT; slot++) {
        char hash[HASH], date[32];
        if (read_slot(slot, image, hash, NULL) && strcmp(hash, slots[slot].hash)) waiting++;
        if (slot == 0 && listed) waiting += files_waiting();
        if (!slots[slot].server.version || used >= size) continue;
        any = 1;
        when_text(slots[slot].server.when, date, sizeof(date));
        used += (size_t)snprintf(text + used, size - used, "Slot %d: versione %d, %s, %s\n", slot + 1,
                                 slots[slot].server.version, date, slots[slot].server.device);
    }
    if (used >= size) used = size - 1;
    if (!listed)
        snprintf(text + used, size - used, "%s\n%s", any ? "" : "Non so ancora cosa c'e' sull'account.",
                 "Non collegato: mando tutto appena c'e' internet.");
    else if (!any && !waiting)
        snprintf(text + used, size - used, "Nessun salvataggio sull'account.");
    else if (!waiting)
        snprintf(text + used, size - used, "\nTutto salvato sull'account.");
    else
        snprintf(text + used, size - used, "\n%d da mandare.", waiting);
}
