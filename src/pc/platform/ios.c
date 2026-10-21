/* The iPhone (MEMORIES_IOS; tools/pc/build_arm64.py --target ios): SDL's
 * UIKit start (SDL_main.h) makes the app and calls this file's main, which
 * sets the game up as android.c does on Android, then runs the game's own
 * (guest/main.c's, Memories_Main there). Host code only (ORDINARY). */
#ifdef MEMORIES_IOS
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int Memories_Main(int argc, char **argv);

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
    return Memories_Main(argc, argv);
}
#endif
