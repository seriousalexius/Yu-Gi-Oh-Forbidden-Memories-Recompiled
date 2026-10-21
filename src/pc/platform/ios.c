/* The iPhone (MEMORIES_IOS; tools/pc/build_arm64.py --target ios): SDL's
 * UIKit start (SDL_main.h) makes the app and calls this file's main, which
 * sets the game up as android.c does on Android, then runs the game's own
 * (guest/main.c's, Memories_Main there). Host code only (ORDINARY). */
#ifdef MEMORIES_IOS
#include "pc/compat/fs.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int Memories_Main(int argc, char **argv);

/* MEMORIES_TEST_MODS (tools/pc/ios/simulator_test.sh): each code mod's
 * framework (package_ios.py) loaded as mods.c loads it, with every name
 * it takes from the game bound at once, before the game, which reaches
 * its mods only with the disc. */
static void test_mods(void)
{
    char folder[1024], path[1300];
    const char *base = SDL_GetBasePath();
    struct dirent *entry;
    DIR *frameworks;
    snprintf(folder, sizeof(folder), "%sFrameworks", base ? base : "");
    frameworks = opendir(folder);
    if (!frameworks) {
        fprintf(stderr, "memories-pc: mod test: no %s\n", folder);
        return;
    }
    while ((entry = readdir(frameworks))) {
        size_t length = strlen(entry->d_name);
        void *handle;
        if (length <= 10 || strcmp(entry->d_name + length - 10, ".framework")) continue;
        snprintf(path, sizeof(path), "%s/%s/%.*s", folder, entry->d_name, (int)(length - 10), entry->d_name);
        handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            fprintf(stderr, "memories-pc: mod test: %s FAILED: %s\n", entry->d_name, dlerror());
            continue;
        }
        fprintf(stderr, "memories-pc: mod test: %s loaded, entry %s, registration %s\n", entry->d_name,
                dlsym(handle, "MemoriesModInit") ? "yes" : "NO",
                dlsym(handle, "MemoriesModRegisterGlobals") ? "yes" : "NO");
        dlclose(handle);
    }
    closedir(frameworks);
}

int main(int argc, char **argv)
{
    char folder[1024], game[1100];
    const char *documents = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS), *home = getenv("HOME");
    size_t length;
    /* The user folder is Documents, which the Files app shows of the game
     * (Info.plist, UIFileSharingEnabled): the player puts the disc in its
     * game folder, which game_files.c looks in. */
    if (documents && *documents) snprintf(folder, sizeof(folder), "%s", documents);
    else snprintf(folder, sizeof(folder), "%s/Documents", home && *home ? home : ".");
    length = strlen(folder);
    while (length > 1 && folder[length - 1] == '/') folder[--length] = 0;
    setenv("MEMORIES_USER_DIR", folder, 0);
    snprintf(game, sizeof(game), "%s/game", folder);
    mkdir(folder, 0755);
    mkdir(game, 0755);
    /* The game is a landscape picture; the whole screen, no update check,
     * no monitor process (an app makes none). */
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    setenv("MEMORIES_FULLSCREEN", "1", 0);
    setenv("MEMORIES_NO_MONITOR", "1", 0);
    setenv("MEMORIES_NO_UPDATE_CHECK", "1", 0);
    fprintf(stderr, "memories-pc: iPhone, user folder %s\n", folder);
    if (getenv("MEMORIES_TEST_MODS")) test_mods();
    return Memories_Main(argc, argv);
}
#endif
