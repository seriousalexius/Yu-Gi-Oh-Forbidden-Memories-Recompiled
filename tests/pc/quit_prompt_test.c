#include "pc/platform/quit_prompt.h"
#include "pc/platform/menu.h"
#include "pc/platform/settings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int confirm = 1, notices, opened;
static void (*notice_chosen)(int button, int *quit);

int Netplay_AskResign(void) { return 0; } /* no online duel here */

int Settings_Get(SettingId id)
{
    assert(id == SET_CONFIRM_QUIT);
    return confirm;
}
void Menu_ShowNotice(const char *title, const char *text, const char *const *buttons, int count, int focus,
                     void (*chosen)(int button, int *quit))
{
    (void)text;
    /* Quit, then Keep playing: focused and last, which Enter, Escape and
     * Circle press. */
    assert(!strcmp(title, "Quit the game?") && !strcmp(buttons[0], "Quit") && chosen);
    assert((count == 2 && !strcmp(buttons[1], "Keep playing") && focus == 1) ||
           (count == 3 && !strcmp(buttons[1], "Menu") && !strcmp(buttons[2], "Keep playing") && focus == 2));
    notices++;
    notice_chosen = chosen;
}

void Menu_Open(void) { opened++; }

int main(void)
{
    int quit = 0;
    QuitPrompt_Request(&quit);
    assert(!quit && notices == 1);
    notice_chosen(1, &quit);
    assert(!quit);
    /* Asked again, it asks again; Quit ends the game. */
    QuitPrompt_Request(&quit);
    QuitPrompt_Request(&quit);
    assert(!quit && notices == 3);
    notice_chosen(0, &quit);
    assert(quit);
    /* Confirm before quitting off: straight out, no notice. */
    confirm = 0;
    quit = 0;
    QuitPrompt_Request(&quit);
    assert(quit && notices == 3);
    /* Back: Menu opens the menus and keeps playing; off, straight out. */
    quit = 0;
    QuitPrompt_Back(&quit);
    assert(quit && notices == 3);
    confirm = 1;
    quit = 0;
    QuitPrompt_Back(&quit);
    assert(!quit && notices == 4);
    notice_chosen(1, &quit);
    assert(!quit && opened == 1);
    QuitPrompt_Back(&quit);
    notice_chosen(2, &quit);
    assert(!quit && opened == 1);
    QuitPrompt_Back(&quit);
    notice_chosen(0, &quit);
    assert(quit);
    puts("quit prompt: asks, keeps playing, quits, skips the question when off, and Back's Menu passed");
    return 0;
}
