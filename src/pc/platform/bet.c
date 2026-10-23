/* fork: the bet before an online duel (bet.h). */
#include "bet.h"
#include "menu.h"
#include "paths.h"
#include "pc/cards/cards.h"
#include "pc/cards/deck_tiers.h"
#include "pc/cards/tables.h"
#include "pc/mods/json.h"
#include "pc/saves/save_menu.h"
#include "pc/saves/save_slots.h"
#include "game/card_constants.h"
#include "game/duel_card.h"
#include "game/save_data.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RETAIL_CARDS 722
#define DECK_CARDS 40
#define CHEST_OFFSET 0x50
#define STARCHIPS_OFFSET 0x5E0
#define CHEST_MAX 250
#define STARCHIPS_MAX 999999
#define DECK_COPIES 3
#define NAME 13
#define PAGE 18 /* cards a page of the collection shows, three to a row */

typedef struct {
    int card, deck, starchips, ready; /* deck: the card is the deck's (none of it in the trunk) */
    char choosing[12];                /* "carta", "starchip" or "" */
} Stake;

typedef struct {
    unsigned short id;
    unsigned char deck;
} Owned;

enum { VIEW_MAIN, VIEW_CARDS, VIEW_CHIPS };

static struct {
    int active, side, slot, test, view, page, focus, owned_count, chest_total;
    unsigned starchips; /* the save's */
    char room[40], mode[8], rival[NAME];
    Stake mine, theirs;
    void (*send)(const char *line);
    void (*back)(void);
} bet;
static Owned owned[RETAIL_CARDS];
static unsigned char state[SAVE_SLOT_STATE_SIZE];
static char settled[600]; /* the settled bet's notice, until shown */

static int check(unsigned char *at) { return SaveData_ValidateIntegrity(at) != 0; }

static int bet_path(char *path, size_t size) { return Paths_User(path, size, "scommessa.txt"); }

/* --- the cards ------------------------------------------------------- */

static int monster(int id)
{
    int cls = Cards_Class(id);
    return cls == CARD_CLASS_MONSTER || cls == CARD_CLASS_EFFECT_MONSTER;
}

static int stat(int id, int defense)
{
    int value = (int)((gDuel_adwCardStats[id - 1] >> (defense ? CARD_STAT_DEFENSE_SHIFT : 0)) & CARD_STAT_VALUE_MASK) *
                CARD_STAT_SCALE;
    return value > Tables_StatCap(defense) ? Tables_StatCap(defense) : value;
}

static char tier(int id) { return id >= 1 && id < (int)sizeof(deck_tiers) - 1 ? deck_tiers[id] : 'C'; }

static const char *tier_name(int id)
{
    switch (tier(id)) {
    case 'L': return "Leggendaria";
    case 'E': return "Epica";
    case 'R': return "Rara";
    default: return "Comune";
    }
}

/* The strongest first: monsters by attack, then defense; the others by tier. */
static int power(int id)
{
    static const char order[] = "CRELV";
    const char *at = strchr(order, tier(id));
    if (monster(id)) return 100000 + stat(id, 0) * 10 + stat(id, 1) / 1000;
    return (at ? (int)(at - order) : 0) * 1000 + id % 1000;
}

static int stronger(const void *a, const void *b)
{
    const Owned *x = a, *y = b;
    int p = power(x->id), q = power(y->id);
    return p != q ? q - p : (int)x->id - (int)y->id;
}

static void card_name(int id, char *out, size_t size)
{
    if (!id || Cards_NameUtf8(id, out, size) <= 0) snprintf(out, size, id ? "carta %d" : "niente", id);
}

/* "Drago Bianco Occhi Blu (3000/2500, Leggendaria)" */
static void card_full(int id, char *out, size_t size)
{
    char name[64];
    card_name(id, name, sizeof(name));
    if (!id) snprintf(out, size, "nessuna carta");
    else if (monster(id)) snprintf(out, size, "%s (%d/%d, %s)", name, stat(id, 0), stat(id, 1), tier_name(id));
    else snprintf(out, size, "%s (%s)", name, tier_name(id));
}

/* A button's few letters: the name cut short, then the attack. */
static void card_short(int id, const char *before, char *out, size_t size)
{
    char name[64];
    size_t fit;
    card_name(id, name, sizeof(name));
    fit = Menu_TextFit(name, 14);
    name[fit] = '\0';
    if (id && monster(id)) snprintf(out, size, "%s%s %d", before, name, stat(id, 0));
    else snprintf(out, size, "%s%s", before, id ? name : "nessuna");
}

/* The save the duel loads (lobby.c's go): its cards, one row each. */
static void read_collection(void)
{
    int id, i, count[RETAIL_CARDS + 1] = {0};
    bet.owned_count = bet.chest_total = 0;
    bet.starchips = 0;
    if (SaveSlots_ReadState(bet.slot, state, check)) {
        fprintf(stderr, "memories-pc: bet: save slot %d unreadable: nothing to put up\n", bet.slot + 1);
        return;
    }
    for (i = 0; i < DECK_CARDS; i++) {
        id = state[2 * i] | state[2 * i + 1] << 8;
        if (id >= 1 && id <= RETAIL_CARDS) count[id]++;
    }
    for (id = 1; id <= RETAIL_CARDS; id++) {
        int chest = state[CHEST_OFFSET + id - 1];
        bet.chest_total += chest;
        if (!chest && !count[id]) continue;
        owned[bet.owned_count].id = (unsigned short)id;
        owned[bet.owned_count].deck = !chest;
        bet.owned_count++;
    }
    qsort(owned, (size_t)bet.owned_count, sizeof(owned[0]), stronger);
    memcpy(&bet.starchips, state + STARCHIPS_OFFSET, sizeof(bet.starchips));
    if (bet.starchips > STARCHIPS_MAX) bet.starchips = STARCHIPS_MAX;
}

/* --- to the server --------------------------------------------------- */

static void send_stake(void)
{
    char line[200];
    const char *choosing = bet.view == VIEW_CARDS ? "carta" : bet.view == VIEW_CHIPS ? "starchip" : "";
    snprintf(line, sizeof(line),
             "{\"t\":\"puntata\",\"carta\":%d,\"starchip\":%d,\"pronto\":%s,\"vedo\":[%d,%d],\"sceglie\":\"%s\"}",
             bet.mine.card, bet.mine.starchips, bet.mine.ready ? "true" : "false", bet.theirs.card,
             bet.theirs.starchips, choosing);
    if (bet.send) bet.send(line);
}

/* --- the screens ----------------------------------------------------- */

static void show(void);

static void close_bet(void)
{
    bet.active = 0;
    Menu_CloseNotice();
}

static void back_closed(int button, int *quit)
{
    (void)button, (void)quit;
    if (bet.back) bet.back();
}

void Bet_Cancel(const char *why)
{
    static const char *const ok[] = {"OK"};
    if (!bet.active) return;
    fprintf(stderr, "memories-pc: bet: off (%s)\n", why);
    close_bet();
    Menu_ShowNotice("Scommessa", why, ok, 1, 0, back_closed);
}

static void cancel_chosen(int button, int *quit)
{
    static const char *const no[] = {"OK"};
    (void)quit;
    if (button == 0) {
        if (bet.send) bet.send("{\"t\":\"annulla_scommessa\"}");
        fprintf(stderr, "memories-pc: bet: cancelled here\n");
        close_bet();
        Menu_ShowNotice("Scommessa", "Sfida annullata.", no, 1, 0, back_closed);
    } else {
        show();
    }
}

static void changed(void)
{
    bet.mine.ready = bet.theirs.ready = 0;
}

static void cards_chosen(int button, int *quit)
{
    int pages = (bet.owned_count + PAGE - 1) / PAGE, shown = bet.owned_count - bet.page * PAGE, card;
    (void)quit;
    if (!bet.active) return;
    if (shown > PAGE) shown = PAGE;
    if (button >= 0 && button < shown) {
        const Owned *pick = &owned[bet.page * PAGE + button];
        card = pick->id;
        if (card != bet.mine.card) changed();
        bet.mine.card = card;
        bet.mine.deck = pick->deck;
        bet.view = VIEW_MAIN;
        bet.focus = 0;
    } else if (button == shown) {
        bet.page = bet.page > 0 ? bet.page - 1 : (pages ? pages - 1 : 0);
    } else if (button == shown + 1) {
        bet.page = bet.page + 1 < pages ? bet.page + 1 : 0;
    } else if (button == shown + 2) {
        if (bet.mine.card) changed();
        bet.mine.card = bet.mine.deck = 0;
        bet.view = VIEW_MAIN;
        bet.focus = 0;
    } else {
        bet.view = VIEW_MAIN;
        bet.focus = 0;
    }
    send_stake();
    show();
}

static void chips_chosen(int button, int *quit)
{
    static const int steps[] = {-1000, -100, -10, -1, 1, 10, 100, 1000};
    long chips = bet.mine.starchips;
    (void)quit;
    if (!bet.active) return;
    if (button >= 0 && button < 8) chips += steps[button];
    else if (button == 8) chips = 0;
    else if (button == 9) chips = (long)bet.starchips;
    if (chips < 0) chips = 0;
    if (chips > (long)bet.starchips) chips = (long)bet.starchips;
    if (chips != bet.mine.starchips) changed();
    bet.mine.starchips = (int)chips;
    if (button < 0 || button >= 10) {
        bet.view = VIEW_MAIN;
        bet.focus = 2;
    } else {
        bet.focus = button;
    }
    send_stake();
    show();
}

static void main_chosen(int button, int *quit)
{
    static const char *const sure[] = {"Si, annulla", "No"};
    (void)quit;
    if (!bet.active) return;
    bet.focus = button;
    if (button == 0) {
        bet.view = VIEW_CARDS;
        bet.page = 0;
        bet.focus = 0;
        /* the card picked shows on its page */
        if (bet.mine.card) {
            int i;
            for (i = 0; i < bet.owned_count; i++)
                if (owned[i].id == bet.mine.card) bet.page = i / PAGE, bet.focus = i % PAGE;
        }
        send_stake();
    } else if (button == 2) {
        bet.view = VIEW_CHIPS;
        bet.focus = 4;
        send_stake();
    } else if (button == 4) {
        bet.mine.ready = !bet.mine.ready;
        fprintf(stderr, "memories-pc: bet: %s (card %d, %d starchips)\n", bet.mine.ready ? "ready" : "not ready",
                bet.mine.card, bet.mine.starchips);
        send_stake();
    } else {
        Menu_ShowNotice("Scommessa", "Vuoi annullare la sfida?", sure, 2, 1, cancel_chosen);
        return;
    }
    show();
}

static void show_main(void)
{
    static char labels[6][48];
    static const char *names[6];
    static signed char styles[6];
    char text[900], mine[96], theirs[96], status[96];
    int i;
    card_short(bet.mine.card, "Tu: ", labels[0], sizeof(labels[0]));
    snprintf(status, sizeof(status), "%.10s: ", bet.rival);
    card_short(bet.theirs.card, status, labels[1], sizeof(labels[1]));
    snprintf(labels[2], sizeof(labels[2]), "Tu: %d starchip", bet.mine.starchips);
    snprintf(labels[3], sizeof(labels[3]), "%.10s: %d starchip", bet.rival, bet.theirs.starchips);
    snprintf(labels[4], sizeof(labels[4]), bet.mine.ready ? "PRONTO!" : "Pronto");
    snprintf(labels[5], sizeof(labels[5]), "Annulla");
    for (i = 0; i < 6; i++) names[i] = labels[i], styles[i] = MENU_NOTICE_PLAIN;
    styles[1] = styles[3] = MENU_NOTICE_DIM;
    if (bet.mine.ready) styles[4] = MENU_NOTICE_GREEN;
    card_full(bet.mine.card, mine, sizeof(mine));
    card_full(bet.theirs.card, theirs, sizeof(theirs));
    if (!strcmp(bet.theirs.choosing, "carta")) snprintf(status, sizeof(status), "sta scegliendo la carta...");
    else if (!strcmp(bet.theirs.choosing, "starchip")) snprintf(status, sizeof(status), "sta scegliendo gli starchip...");
    else snprintf(status, sizeof(status), bet.theirs.ready ? "e' PRONTO." : "non e' ancora pronto.");
    snprintf(text, sizeof(text),
             "A sinistra la tua puntata, a destra quella di %s: chi vince prende tutte e due%s.\n"
             "Tu: %s + %d starchip.\n%s: %s + %d starchip.\n%s %s\n"
             "Niente da tutti e due = si gioca senza scommessa. Si parte quando siete pronti tutti e due.",
             bet.rival, !strcmp(bet.mode, "arena") ? " (vale per tutta la serie d'arena)" : "", mine,
             bet.mine.starchips, bet.rival, theirs, bet.theirs.starchips, bet.rival, status);
    if (bet.mine.deck)
        snprintf(text + strlen(text), sizeof(text) - strlen(text),
                 "\nLa tua carta e' nel mazzo: se la perdi, al suo posto va la piu' debole del baule.");
    Menu_ShowNoticeGrid("Scommessa", text, names, 6, 2, styles, bet.focus == 1 || bet.focus == 3 ? 0 : bet.focus,
                        main_chosen);
}

static void show_cards(void)
{
    static char labels[PAGE + 4][40];
    static const char *names[PAGE + 4];
    static signed char styles[PAGE + 4];
    char text[300];
    int pages = (bet.owned_count + PAGE - 1) / PAGE, shown, i;
    if (bet.page >= pages) bet.page = 0;
    shown = bet.owned_count - bet.page * PAGE;
    if (shown > PAGE) shown = PAGE;
    if (shown < 0) shown = 0;
    for (i = 0; i < shown; i++) {
        const Owned *card = &owned[bet.page * PAGE + i];
        card_short(card->id, card->deck ? "*" : "", labels[i], sizeof(labels[i]));
        names[i] = labels[i];
        /* a card of the deck only when the trunk has one to take its place */
        styles[i] = card->id == bet.mine.card ? MENU_NOTICE_GREEN
                  : card->deck && !bet.chest_total ? MENU_NOTICE_DIM : MENU_NOTICE_PLAIN;
    }
    names[shown] = "< Pagina";
    names[shown + 1] = "Pagina >";
    names[shown + 2] = "Nessuna carta";
    names[shown + 3] = "Indietro";
    for (i = shown; i < shown + 4; i++) styles[i] = MENU_NOTICE_PLAIN;
    if (!bet.owned_count)
        snprintf(text, sizeof(text), "Nel salvataggio (slot %d) non ci sono carte.", bet.slot + 1);
    else
        snprintf(text, sizeof(text),
                 "Le carte del salvataggio (slot %d), dalla piu' forte. * = nel mazzo. Pagina %d di %d.",
                 bet.slot + 1, bet.page + 1, pages ? pages : 1);
    Menu_ShowNoticeGrid("Scommessa - la tua carta", text, names, shown + 4, 3, styles,
                        bet.focus < shown && styles[bet.focus] != MENU_NOTICE_DIM ? bet.focus : shown + 1,
                        cards_chosen);
}

static void show_chips(void)
{
    static const char *const names[] = {"-1000", "-100", "-10", "-1", "+1", "+10", "+100", "+1000",
                                        "Zero", "Tutti", "OK"};
    char text[160];
    snprintf(text, sizeof(text), "Punti %d starchip (ne hai %u).", bet.mine.starchips, bet.starchips);
    Menu_ShowNoticeGrid("Scommessa - starchip", text, names, 11, 4, NULL, bet.focus, chips_chosen);
}

static void show(void)
{
    if (!bet.active) return;
    if (bet.view == VIEW_CARDS) show_cards();
    else if (bet.view == VIEW_CHIPS) show_chips();
    else show_main();
}

/* --- the lobby's ----------------------------------------------------- */

void Bet_Open(const char *room, int side, const char *mode, const char *rival, int test,
              void (*send)(const char *line), void (*back)(void))
{
    int slot = SaveMenu_CurrentSlot();
    memset(&bet, 0, sizeof(bet));
    bet.active = 1;
    bet.side = side;
    bet.slot = slot < 0 ? 0 : slot;
    bet.send = send;
    bet.back = back;
    snprintf(bet.room, sizeof(bet.room), "%s", room);
    snprintf(bet.mode, sizeof(bet.mode), "%s", mode);
    snprintf(bet.rival, sizeof(bet.rival), "%s", rival);
    read_collection();
    fprintf(stderr, "memories-pc: bet: open against %s (%s), save slot %d: %d cards, %u starchips\n", rival, mode,
            bet.slot + 1, bet.owned_count, bet.starchips);
    bet.test = test || getenv("MEMORIES_BET_TEST");
    if (bet.test) {
        const char *given = getenv("MEMORIES_BET_TEST");
        int card = 0, chips = 0, i;
        if (given) sscanf(given, "%d:%d", &card, &chips);
        for (i = 0; i < bet.owned_count; i++)
            if (owned[i].id == card) bet.mine.card = card, bet.mine.deck = owned[i].deck;
        bet.mine.starchips = chips < 0 ? 0 : chips > (int)bet.starchips ? (int)bet.starchips : chips;
        bet.mine.ready = 1;
    }
    send_stake();
    show();
}

int Bet_Active(void) { return bet.active; }

int Bet_With(const char *name) { return bet.active && !strcmp(bet.rival, name); }

int Bet_Handle(const char *type, const JsonValue *root)
{
    const JsonValue *mine;
    int card, before_card = bet.theirs.card, before_chips = bet.theirs.starchips;
    if (strcmp(type, "puntata") != 0) return 0;
    if (!bet.active) return 1;
    card = (int)Json_Number(Json_Member(root, "carta"), 0);
    bet.theirs.card = card >= 1 && card <= RETAIL_CARDS ? card : 0;
    bet.theirs.starchips = (int)Json_Number(Json_Member(root, "starchip"), 0);
    if (bet.theirs.starchips < 0 || bet.theirs.starchips > STARCHIPS_MAX) bet.theirs.starchips = 0;
    bet.theirs.ready = Json_Bool(Json_Member(root, "pronto"), 0);
    snprintf(bet.theirs.choosing, sizeof(bet.theirs.choosing), "%s", Json_String(Json_Member(root, "sceglie"), ""));
    /* the server's word on this one's "Pronto": off when the other's stake changed */
    mine = Json_Member(root, "tuo_pronto");
    if (mine) bet.mine.ready = Json_Bool(mine, 0);
    if (bet.theirs.card != before_card || bet.theirs.starchips != before_chips) bet.mine.ready = 0;
    if (bet.test && !bet.mine.ready) {
        bet.mine.ready = 1;
        send_stake();
    }
    if (Menu_NoticeShown() && bet.view == VIEW_MAIN) show();
    return 1;
}

void Bet_Forget(void)
{
    char path[1024];
    if (!bet_path(path, sizeof(path))) remove(path);
}

void Bet_Go(const JsonValue *root)
{
    char path[1024];
    FILE *file;
    int my_card, my_chips, their_card, their_chips;
    if (!bet.active) return;
    bet.active = 0;
    /* the server's stakes, which both games were shown */
    my_card = (int)Json_Number(Json_Member(root, "mia_carta"), 0);
    my_chips = (int)Json_Number(Json_Member(root, "miei_starchip"), 0);
    their_card = (int)Json_Number(Json_Member(root, "sua_carta"), 0);
    their_chips = (int)Json_Number(Json_Member(root, "suoi_starchip"), 0);
    if (my_card < 0 || my_card > RETAIL_CARDS) my_card = 0;
    if (their_card < 0 || their_card > RETAIL_CARDS) their_card = 0;
    if (my_chips < 0 || my_chips > STARCHIPS_MAX) my_chips = 0;
    if (their_chips < 0 || their_chips > STARCHIPS_MAX) their_chips = 0;
    Bet_Forget();
    if (!my_card && !my_chips && !their_card && !their_chips) {
        fprintf(stderr, "memories-pc: bet: nothing put up, a duel with no bet\n");
        return;
    }
    if (bet_path(path, sizeof(path)) || !(file = fopen(path, "wb"))) {
        fprintf(stderr, "memories-pc: bet: cannot write scommessa.txt\n");
        return;
    }
    fprintf(file, "contro=%s\nslot=%d\nmodo=%s\nmia=%d,%d,%d\nsua=%d,%d\n", bet.rival, bet.slot, bet.mode, my_card,
            my_card == bet.mine.card ? bet.mine.deck : 0, my_chips, their_card, their_chips);
    fclose(file);
    fprintf(stderr, "memories-pc: bet: on: card %d + %d starchips against card %d + %d starchips (save slot %d)\n",
            my_card, my_chips, their_card, their_chips, bet.slot + 1);
}

/* --- after the duel -------------------------------------------------- */

typedef struct {
    char rival[NAME], mode[8];
    int slot, my_card, my_deck, my_chips, their_card, their_chips;
} Wager;

static int read_wager(Wager *wager)
{
    char path[1024], line[128];
    FILE *file;
    memset(wager, 0, sizeof(*wager));
    if (bet_path(path, sizeof(path)) || !(file = fopen(path, "rb"))) return -1;
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!strncmp(line, "contro=", 7)) snprintf(wager->rival, sizeof(wager->rival), "%s", line + 7);
        else if (!strncmp(line, "modo=", 5)) snprintf(wager->mode, sizeof(wager->mode), "%s", line + 5);
        else if (!strncmp(line, "slot=", 5)) wager->slot = atoi(line + 5);
        else if (!strncmp(line, "mia=", 4))
            sscanf(line + 4, "%d,%d,%d", &wager->my_card, &wager->my_deck, &wager->my_chips);
        else if (!strncmp(line, "sua=", 4)) sscanf(line + 4, "%d,%d", &wager->their_card, &wager->their_chips);
    }
    fclose(file);
    if (wager->slot < 0 || wager->slot >= SAVE_SLOT_COUNT) wager->slot = 0;
    if (wager->my_card < 0 || wager->my_card > RETAIL_CARDS) wager->my_card = 0;
    if (wager->their_card < 0 || wager->their_card > RETAIL_CARDS) wager->their_card = 0;
    return wager->rival[0] ? 0 : -1;
}

static void stake_text(int card, int chips, char *out, size_t size)
{
    char name[96];
    card_full(card, name, sizeof(name));
    if (card && chips) snprintf(out, size, "%s e %d starchip", name, chips);
    else if (card) snprintf(out, size, "%s", name);
    else snprintf(out, size, "%d starchip", chips);
}

void Bet_Describe(int won, char *out, size_t size)
{
    Wager wager;
    char stake[160];
    out[0] = '\0';
    if (read_wager(&wager)) return;
    if (won && (wager.their_card || wager.their_chips)) {
        stake_text(wager.their_card, wager.their_chips, stake, sizeof(stake));
        snprintf(out, size, "\n\nScommessa vinta: prendi %s.", stake);
    } else if (!won && (wager.my_card || wager.my_chips)) {
        stake_text(wager.my_card, wager.my_chips, stake, sizeof(stake));
        snprintf(out, size, "\n\nScommessa persa: dai %s.", stake);
    } else {
        snprintf(out, size, won ? "\n\nScommessa vinta (non puntava niente)." : "\n\nScommessa persa (non hai puntato niente).");
    }
}

/* The trunk's weakest card with a copy to spare for the deck, -1 when none. */
static int weakest(const unsigned short *deck)
{
    int id, best = -1, i;
    for (id = 1; id <= RETAIL_CARDS; id++) {
        int copies = 0;
        if (!state[CHEST_OFFSET + id - 1]) continue;
        for (i = 0; i < DECK_CARDS; i++) copies += deck[i] == id;
        if (copies >= DECK_COPIES) continue;
        if (best < 0 || power(id) < power(best)) best = id;
    }
    return best;
}

void Bet_Settle(int (*result)(const char *rival))
{
    Wager wager;
    unsigned short deck[DECK_CARDS];
    unsigned chips;
    char stake[200], name[96], put[96];
    int won, i;
    settled[0] = '\0';
    if (read_wager(&wager)) return;
    Bet_Forget();
    won = result(wager.rival);
    fprintf(stderr, "memories-pc: bet: against %s: %s\n", wager.rival, won < 0 ? "no duel" : won ? "won" : "lost");
    if (won < 0) {
        snprintf(settled, sizeof(settled), "La scommessa con %s e' annullata: il duello non e' cominciato.",
                 wager.rival);
        return;
    }
    if (won ? !wager.their_card && !wager.their_chips : !wager.my_card && !wager.my_chips) return;
    if (SaveSlots_ReadState(wager.slot, state, check)) {
        snprintf(settled, sizeof(settled), "Non riesco a leggere il salvataggio (slot %d): la scommessa con %s non "
                 "e' stata applicata.", wager.slot + 1, wager.rival);
        return;
    }
    for (i = 0; i < DECK_CARDS; i++) deck[i] = (unsigned short)(state[2 * i] | state[2 * i + 1] << 8);
    memcpy(&chips, state + STARCHIPS_OFFSET, sizeof(chips));
    put[0] = '\0';
    if (won) {
        if (wager.their_card && state[CHEST_OFFSET + wager.their_card - 1] < CHEST_MAX)
            state[CHEST_OFFSET + wager.their_card - 1]++;
        chips = chips + (unsigned)wager.their_chips > STARCHIPS_MAX ? STARCHIPS_MAX : chips + (unsigned)wager.their_chips;
        stake_text(wager.their_card, wager.their_chips, stake, sizeof(stake));
    } else {
        int card = wager.my_card, at = -1;
        if (card) {
            for (i = 0; i < DECK_CARDS; i++)
                if (deck[i] == card) at = i;
            if ((wager.my_deck || !state[CHEST_OFFSET + card - 1]) && at >= 0) {
                /* out of the deck: the weakest of the trunk takes its place */
                int instead;
                deck[at] = 0;
                instead = weakest(deck);
                if (instead > 0) {
                    state[CHEST_OFFSET + instead - 1]--;
                    deck[at] = (unsigned short)instead;
                    state[2 * at] = (unsigned char)(instead & 0xFF);
                    state[2 * at + 1] = (unsigned char)(instead >> 8);
                    card_name(instead, name, sizeof(name));
                    snprintf(put, sizeof(put), "\nEra nel mazzo: al suo posto c'e' %s (dal baule).", name);
                } else {
                    deck[at] = (unsigned short)card;
                    fprintf(stderr, "memories-pc: bet: the trunk is empty: card %d stays in the deck\n", card);
                }
            } else if (state[CHEST_OFFSET + card - 1]) {
                state[CHEST_OFFSET + card - 1]--;
            }
        }
        chips = chips > (unsigned)wager.my_chips ? chips - (unsigned)wager.my_chips : 0;
        stake_text(wager.my_card, wager.my_chips, stake, sizeof(stake));
    }
    memcpy(state + STARCHIPS_OFFSET, &chips, sizeof(chips));
    SaveData_WritePrimarySecondaryIntegrity(state);
    SaveData_WriteTertiaryIntegrity(state);
    if (!check(state) || SaveSlots_WriteState(wager.slot, state)) {
        snprintf(settled, sizeof(settled), "Non riesco a scrivere il salvataggio (slot %d): la scommessa con %s non "
                 "e' stata applicata.", wager.slot + 1, wager.rival);
        fprintf(stderr, "memories-pc: bet: the save was not written\n");
        return;
    }
    fprintf(stderr, "memories-pc: bet: save slot %d: %s card %d, starchips now %u\n", wager.slot + 1,
            won ? "got" : "gave", won ? wager.their_card : wager.my_card, chips);
    snprintf(settled, sizeof(settled), "%s contro %s: %s %s.%s\n\nE' gia' nel salvataggio (slot %d).",
             won ? "Scommessa vinta" : "Scommessa persa", wager.rival, won ? "hai preso" : "hai dato", stake, put,
             wager.slot + 1);
}

static void settled_closed(int button, int *quit) { (void)button, (void)quit; }

void Bet_Frame(void)
{
    static const char *const ok[] = {"OK"};
    if (!settled[0] || Menu_NoticeShown()) return;
    Menu_ShowNotice("Scommessa", settled, ok, 1, 0, settled_closed);
    settled[0] = '\0';
}
