#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "planner.h"

static int probes;
static HmCard card(int id)
{
    HmCard c = {id, id, 0, 1000, 800, 1, 2, 0};
    if (id == 100) c.attack = c.defense = 3000;
    if (id == 101) c.attack = c.defense = 3500;
    return c;
}
static int fusion(int a, int b)
{
    probes++;
    if ((a == 10 && b == 11) || (a == 11 && b == 10)) return 100;
    if ((a == 100 && b == 12) || (a == 12 && b == 100)) return 101;
    return 0;
}
static int equip(int a, int b) { return a == 657 && b > 0; }
static int terrain(int type, int field) { return type == 0 && field == 3 ? 500 : 0; }
static int ritual(int id) { return id == 700; }
/* A mod's table: star 11 beats Saturn (3) by 1000 and nothing else counts. */
static int custom_matchup(int a, int d) { return a == 11 && d == 3 ? 1000 : a == 3 && d == 11 ? -1000 : 0; }
static const HmRules rules = {card, fusion, equip, terrain, ritual};
static HmOptions opts(void)
{
    HmOptions o = {1,1,1,1,1,1,1,4,6000,50,150,1,0};
    return o;
}
static HmBoard empty(void)
{
    HmBoard b = {0};
    b.lp = b.max_lp = b.enemy_lp = 8000;
    return b;
}
static HmCard monster(int id, int atk, int def)
{
    HmCard c = card(id); c.attack = atk; c.defense = def; return c;
}
static HmCard magic(int id, int type)
{
    HmCard c = {id,id,type,0,0,0,0,0}; return c;
}

static void tactical_cases(void)
{
    HmBoard b = empty(); HmOptions o = opts(); HmDecision d;
    b.own[0] = monster(2, 800, 2000); b.enemy_lp = 500;
    d = Hm_PlanField(&b,&o,&rules,0);
    assert(d.selection[9] == 1 && d.selection[10] == 58 && d.score >= HM_WIN);
    b.pinned = 1;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[10] == 0);
    b.pinned = 0; b.enemy_lp = 8000; b.enemy[0] = monster(20,1800,1000);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[11] == 1);
    b.own[0].flags |= HM_USED;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.result == 3);

    b = empty(); b.enemy[0] = monster(20,2100,1000); b.enemy[0].star = 3;
    assert(Hm_ChooseStar(&b,monster(10,1800,1000),0) == 1);
    assert(Hm_StarBonus(2,3) == 500 && Hm_StarBonus(3,2) == -500);
    assert(Hm_StarBonus(6,1) == 500 && Hm_StarBonus(10,7) == 500);
    assert(Hm_StarBonus(6,7) == 0 && Hm_StarBonus(0,1) == 0);
    /* In the game the planner scores stars by the game's matchup, which a
       mod's "guardian_stars" changes (stars 11-15 included). */
    Hm_StarMatchup = custom_matchup;
    assert(Hm_StarBonus(11,3) == 1000 && Hm_StarBonus(3,11) == -1000 && Hm_StarBonus(2,3) == 0);
    assert(Hm_StarBonus(0,11) == 0 && Hm_StarBonus(11,16) == 0);
    b = empty(); b.enemy[0] = monster(20,2100,1000); b.enemy[0].star = 3;
    {
        HmCard c = monster(10,1800,1000);
        c.star = 2; c.star2 = 11;
        assert(Hm_ChooseStar(&b,c,0) == 1);
    }
    Hm_StarMatchup = 0;

    b = empty(); b.own[0] = monster(1,3000,2500); b.own[1] = monster(3,1600,1000);
    b.enemy[0] = monster(4,1500,1000); b.enemy_lp = 3000;
    d = Hm_PlanField(&b,&o,&rules,0);
    assert(d.selection[9] == 2 && d.selection[10] == 56 && d.score >= HM_WIN);
    b.own[0] = (HmCard){0}; b.own[1].attack = 1500; b.enemy[0].defense = 3000;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 2 && d.selection[10] == 56);
    o.trades = 0;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[10] == 0);

    b = empty(); o = opts(); b.own[0] = monster(1,3000,2500);
    b.enemy[0] = (HmCard){-1,0,0,0,0,0,0,HM_DOWN}; o.blind_risk = 0;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[10] == 0);
    o.blind_risk = 100;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[10] == 56 && d.score < HM_WIN);
}

static void spell_cases(void)
{
    HmBoard b = empty(); HmOptions o = opts(); HmDecision d;
    b.enemy_lp = 900; b.back[0] = magic(347,20);
    d = Hm_PlanField(&b,&o,&rules,0);
    assert(d.selection[9] == 6 && d.selection[10] == 6 && d.score >= HM_WIN);
    b.enemy_back[0] = magic(687,21);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.result == 3);
    b.enemy_back[0] = (HmCard){-1,0,21,0,0,0,0,HM_DOWN};
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 6);

    b = empty(); b.back[0] = magic(342,20);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.result == 3);
    b.lp = 1000;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 6);
    b = empty(); b.back[0] = magic(672,20);
    b.enemy_back[0] = (HmCard){-1,0,21,0,0,0,0,HM_DOWN};
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 6);

    b = empty(); b.back[0] = magic(336,20); b.own[0] = monster(1,3000,2500);
    b.enemy[0] = monster(4,500,500); b.pinned = 1;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] <= 5);
    b.own[0] = (HmCard){0};
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 6);

    b = empty(); b.back[0] = magic(332,20); b.own[0] = monster(10,1000,1000); b.pinned = 1;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 6);
    b.terrain = 3;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] != 6);

    b = empty(); b.own[0] = monster(10,1000,1000); b.own[1] = monster(11,2000,1000);
    b.enemy[0] = monster(12,2500,2000); b.back[0] = magic(657,23);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 6 && d.selection[10] == 2);
    b = empty(); b.back[0] = magic(999,20);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.result == 3);
}

static void hand_cases(void)
{
    HmBoard b = empty(); HmOptions o = opts(); HmDecision d;
    b.hand_count = 2; b.hand[0] = card(10); b.hand[1] = card(11);
    d = Hm_PlanHand(&b,&o,&rules);
    assert(d.result == 1 && d.selection[0] >= 11 && d.selection[1] >= 11 && !d.selection[2]);
    assert(d.selection[0] != d.selection[1] && d.selection[6] == 1);
    o.fusion = 0; d = Hm_PlanHand(&b,&o,&rules); assert(d.selection[1] == 0);
    o.fusion = 1; b.own[0] = card(10); b.hand[0] = card(11); b.hand_count = 1;
    d = Hm_PlanHand(&b,&o,&rules);
    assert(d.selection[0] == 11 && !d.selection[1] && !d.selection[8]);
    b = empty(); b.hand_count = 3; b.hand[0] = card(10); b.hand[1] = card(11); b.hand[2] = card(12);
    o.materials = 2; d = Hm_PlanHand(&b,&o,&rules); assert(!d.selection[2]);
    o.materials = 3; o.material_cost = 0; d = Hm_PlanHand(&b,&o,&rules); assert(d.selection[2] >= 11);
    b = empty(); b.hand_count = 1; b.hand[0] = magic(347,20); b.enemy_lp = 900;
    d = Hm_PlanHand(&b,&o,&rules); assert(d.selection[0] == 11 && !d.selection[8]);
    /* A replacement Magic with an ai_effect acts like that analogue; without
     * one it is safely set rather than guessed as its obsolete retail card.
     * A trap analogue is still a reactive face-down card, never a spell. */
    b.hand[0] = (HmCard){500,347,20,0,0,0,0,0};
    d = Hm_PlanHand(&b,&o,&rules); assert(d.selection[0] == 11 && !d.selection[8]);
    b.hand[0].effect = 0;
    d = Hm_PlanHand(&b,&o,&rules); assert(d.selection[0] == 11 && d.selection[8]);
    b.hand[0] = (HmCard){501,681,21,0,0,0,0,0};
    d = Hm_PlanHand(&b,&o,&rules);
    assert(d.selection[0] == 11 && d.selection[6] == 6 && d.selection[8]);
    /* Retail trap analogues currently have no utility score. Keep this
     * deliberately cross-type fixture as a future-proof guard: even if a
     * Trap is assigned positive utility, it must still be set, never cast. */
    b.hand[0] = (HmCard){502,347,21,0,0,0,0,0};
    d = Hm_PlanHand(&b,&o,&rules); assert(d.selection[0] == 11 && d.selection[8]);
    b = empty(); b.hand_count = 1; b.hand[0] = magic(657,23);
    b.own[0] = monster(10,1800,1000); b.own[0].star = 2;
    b.enemy[0] = monster(20,2600,1000); b.enemy[0].star = 3;
    o.fusion = 0;
    d = Hm_PlanHand(&b,&o,&rules);
    assert(d.selection[6] == 1 && !d.selection[8] && d.selection[7] == 1);
}

static void retail_cases(void)
{
    HmBoard b = empty(); HmOptions o = opts(); HmDecision d, retail = {{0},3,0,0};
    b.own[0] = monster(2,800,2000); b.enemy[0] = monster(3,3000,3000);
    o.attacks = o.defense = o.spells = 0;
    d = Hm_PlanField(&b,&o,&rules,&retail); assert(d.result == 3);
    o.attacks = 1;
    d = Hm_PlanField(&b,&o,&rules,&retail); assert(d.result == 3);
    retail.result = 2; retail.selection[9] = 1; retail.selection[11] = 1;
    d = Hm_PlanField(&b,&o,&rules,&retail);
    assert(d.result == 2 && d.selection[9] == 1 && d.selection[11] == 1);
}

static unsigned random_state = 1;
static unsigned rnd(void) { random_state = random_state * 1664525u + 1013904223u; return random_state; }
static void bounded_fixtures(void)
{
    int round, i, j;
    HmOptions o = opts(); o.budget = 128; o.materials = 5;
    for (round = 0; round < 500; round++) {
        HmBoard b = empty(); HmBoard saved; HmDecision d;
        b.hand_count = 1 + rnd() % 20;
        for (i = 0; i < b.hand_count; i++) b.hand[i] = card(10 + rnd() % 3);
        for (i = 0; i < 5; i++) {
            if (rnd() % 2) b.own[i] = monster(10 + i, rnd() % 4000, rnd() % 4000);
            if (rnd() % 2) b.enemy[i] = monster(20 + i, rnd() % 4000, rnd() % 4000);
            b.enemy[i].flags = rnd() % 2 ? HM_DEFENSE : 0;
        }
        saved = b; probes = 0; d = Hm_PlanHand(&b,&o,&rules);
        assert(d.result == 1 && probes <= o.budget && d.nodes <= o.budget);
        assert(!d.selection[5] && d.selection[6] >= 1 && d.selection[6] <= 5);
        for (i = 0; i < 5 && d.selection[i]; i++) {
            assert(d.selection[i] >= 11 && d.selection[i] < 11 + b.hand_count);
            for (j = 0; j < i; j++) assert(d.selection[i] != d.selection[j]);
        }
        d = Hm_PlanField(&b,&o,&rules,0);
        assert(d.nodes <= o.budget && (d.result == 2 || d.result == 3));
        if (d.result == 2) {
            assert(d.selection[9] >= 1 && d.selection[9] <= 5);
            assert(b.own[d.selection[9]-1].id);
            if (d.selection[10]) {
                int t = d.selection[10] - 56;
                assert(t >= 0 && t < 5);
                assert(b.enemy[t].id || d.selection[10] == 58);
            }
        }
        assert(!memcmp(&b,&saved,sizeof(b)));
    }
}


/* fork: the expert hand. Monsters 1-99 have ATK id*100; 10 + 20 fuse into
 * 40; 657 is an equip (+1000), 337 destroys every enemy, 400 is a trap. */
static HmCard xcard(int id)
{
    HmCard c = {id, id, 0, id * 100, id * 50, 1, 2, 0};
    if (id == 657) { c.type = 23; c.attack = c.defense = 0; }
    if (id == 337) { c.type = 20; c.attack = c.defense = 0; }
    if (id == 400) { c.type = 21; c.attack = c.defense = 0; }
    if (id == 349 || id == 669 || (id >= 330 && id <= 335)) { c.type = 20; c.attack = c.defense = 0; }
    if (id >= 681 && id <= 686) { c.type = 21; c.attack = c.defense = 0; }
    if (id == 38) c.star = c.star2 = 2; /* Jupiter only: 500 less against a Mars */
    return c;
}
static int xfusion(int a, int b)
{
    if ((a == 12 || a == 13) && b == 14) return 2; /* weak fusions with the 1400 */
    if (a == 14 && (b == 12 || b == 13)) return 2;
    return (a == 10 && b == 20) || (a == 20 && b == 10) ? 40 : 0;
}
static int xequip(int a, int b) { return a == 657 && b > 0 && b < 100; }
static const HmRules xrules = {xcard, xfusion, xequip, terrain, ritual};
static HmBoard xboard(const int *hand, const int *deck, int decks, int enemy_deck)
{
    HmBoard b = empty();
    int i;
    for (i = 0; i < 5; i++) b.hand[i] = xcard(hand[i]);
    b.hand_count = 5;
    for (i = 0; i < decks; i++) b.deck[i] = deck[i % 10];
    b.deck_count = decks; b.enemy_deck_count = enemy_deck;
    b.enemy[0] = xcard(35); /* a 3500 in attack position */
    return b;
}
static int holds(HmDecision d, int slot)
{
    int i;
    for (i = 0; i < 5 && d.selection[i]; i++) if (d.selection[i] == slot + 11) return 1;
    return 0;
}
static int marks(HmDecision d) { int n = 0; while (n < 5 && d.selection[n]) n++; return n; }
/* Marked in front of the card played: thrown in a failed fusion. */
static int thrown(HmDecision d, int slot) { return holds(d, slot) && d.selection[marks(d) - 1] != slot + 11; }
static void expert_cases(void)
{
    HmOptions o = opts();
    HmBoard b;
    HmDecision d;
    static const int weak[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 11};
    static const int partner[10] = {1, 2, 3, 20, 5, 6, 7, 8, 9, 11};
    static const int strong[10] = {1, 2, 3, 30, 5, 6, 7, 8, 9, 11};
    o.expert = 1;

    /* Nothing beats the 3500: keep 10 (10 + 20 in the deck), play one, throw three. */
    { static const int h[5] = {5, 6, 7, 8, 10}; b = xboard(h, partner, 30, 30); }
    d = Hm_PlanHand(&b, &o, &xrules);
    assert(d.result == 1 && marks(d) == 4 && !holds(d, 4) && Hm_Discards == 3 && Hm_Outs >= 1);
    /* Nothing in the deck answers it: no reason to dig. */
    { static const int h[5] = {5, 6, 7, 8, 10}; b = xboard(h, weak, 30, 30); }
    d = Hm_PlanHand(&b, &o, &xrules);
    assert(marks(d) == 1 && Hm_Discards == 0 && Hm_Outs == 0);
    /* An answer in hand (337): no digging. */
    { static const int h[5] = {5, 6, 7, 337, 10}; b = xboard(h, partner, 30, 30); }
    d = Hm_PlanHand(&b, &o, &xrules);
    assert(Hm_Discards == 0);
    /* 10 + 20 both in hand: the fusion beats it; the three dead monsters
     * (no fusion with anything left) go in front of it, the deck is long. */
    { static const int h[5] = {5, 6, 7, 20, 10}; b = xboard(h, weak, 30, 30); }
    d = Hm_PlanHand(&b, &o, &xrules);
    assert(holds(d, 3) && holds(d, 4) && Hm_Discards == 3 && Hm_Outs == 0);
    /* Not when the deck is short. */
    { static const int h[5] = {5, 6, 7, 20, 10}; b = xboard(h, weak, 16, 30); }
    d = Hm_PlanHand(&b, &o, &xrules);
    assert(marks(d) == 2 && Hm_Discards == 0);
    /* Megamorph makes a 3000 of the deck a 4000: it stays in hand. */
    { static const int h[5] = {5, 6, 7, 8, 657}; b = xboard(h, strong, 30, 30); }
    d = Hm_PlanHand(&b, &o, &xrules);
    assert(!holds(d, 4) && Hm_Discards >= 1);
    /* A strong monster (2500) that does not hold the field is the card it
     * plays; the other three go. */
    { static const int h[5] = {25, 6, 7, 8, 10}; b = xboard(h, partner, 30, 30); }
    d = Hm_PlanHand(&b, &o, &xrules);
    assert(holds(d, 0) && !holds(d, 4) && Hm_Discards == 3);
    /* Two of them: the stronger is played, the other thrown. */
    { static const int h[5] = {25, 28, 7, 8, 10}; b = xboard(h, partner, 30, 30); }
    d = Hm_PlanHand(&b, &o, &xrules);
    assert(holds(d, 1) && !holds(d, 4) && Hm_Discards == 3);
    /* A wall (DEF 3600 against the 3500) is never thrown: here it is the
     * card played, the 2800 goes. */
    { static const int h[5] = {25, 28, 7, 8, 10}; b = xboard(h, partner, 30, 30); }
    b.hand[0].defense = 3600;
    d = Hm_PlanHand(&b, &o, &xrules);
    assert(!thrown(d, 0) && thrown(d, 1) && !holds(d, 4) && Hm_Discards == 3);
    /* So does the one that beats another face-up monster (2600) of the
     * player nothing else handles, beside the wall; not once a monster of
     * ours on the field beats it. */
    { static const int h[5] = {25, 27, 7, 8, 10}; b = xboard(h, partner, 30, 30); }
    b.hand[0].defense = 3600; b.enemy[1] = xcard(26);
    d = Hm_PlanHand(&b, &o, &xrules);
    assert(!thrown(d, 0) && !thrown(d, 1) && !holds(d, 4) && Hm_Discards == 2);
    b.own[0] = xcard(30);
    d = Hm_PlanHand(&b, &o, &xrules);
    assert(!thrown(d, 0) && !holds(d, 4) && Hm_Discards == 3);
    /* fork: other answers in the deck. Shadow Spell (-1000) makes the 3500 a
     * 2500 our 2800 beats; Spellbinding Circle (-500) brings it to 3000, not
     * enough for a 2800. */
    { static const int h[5] = {28, 6, 7, 8, 9}; b = xboard(h, weak, 30, 30); }
    d = Hm_PlanHand(&b, &o, &xrules);
    assert(Hm_Outs == 0 && Hm_Discards == 0);
    { static const int h[5] = {28, 6, 7, 8, 9}; static const int sd[10] = {1, 2, 3, 669, 5, 6, 7, 8, 9, 11};
      b = xboard(h, sd, 30, 30); }
    d = Hm_PlanHand(&b, &o, &xrules);
    assert(Hm_Outs >= 1 && Hm_Discards >= 1);
    { static const int h[5] = {28, 6, 7, 8, 9}; static const int sc[10] = {1, 2, 3, 349, 5, 6, 7, 8, 9, 11};
      b = xboard(h, sc, 30, 30); }
    d = Hm_PlanHand(&b, &o, &xrules);
    assert(Hm_Outs == 0);
    /* Widespread Ruin stops any attacker; Acid Trap Hole only up to 3000. */
    { static const int h[5] = {28, 6, 7, 8, 9}; static const int wr[10] = {1, 2, 3, 686, 5, 6, 7, 8, 9, 11};
      b = xboard(h, wr, 30, 30); }
    d = Hm_PlanHand(&b, &o, &xrules);
    assert(Hm_Outs >= 1);
    { static const int h[5] = {28, 6, 7, 8, 9}; static const int ah[10] = {1, 2, 3, 685, 5, 6, 7, 8, 9, 11};
      b = xboard(h, ah, 30, 30); }
    d = Hm_PlanHand(&b, &o, &xrules);
    assert(Hm_Outs == 0);
    /* Shadow Spell in hand with the 2800: answered, nothing to dig for, and
     * the spell is never thrown. */
    { static const int h[5] = {28, 6, 7, 669, 9}; b = xboard(h, partner, 30, 30); }
    d = Hm_PlanHand(&b, &o, &xrules);
    assert(Hm_Outs == 0 && !thrown(d, 3));
    /* Shadow Spell in hand, a 2600 in the deck: the spell is never thrown
     * (its penalty stays on the monster, so it may well go first). */
    { static const int h[5] = {5, 6, 7, 669, 9}; static const int m[10] = {1, 2, 3, 26, 5, 6, 7, 8, 9, 11};
      b = xboard(h, m, 30, 30); }
    d = Hm_PlanHand(&b, &o, &xrules);
    assert(!thrown(d, 3));
    /* A field spell (terrain 3: type 0 +500 here) lifts our 3200 over the 3500. */
    { static const int h[5] = {32, 6, 7, 8, 9}; static const int f[10] = {1, 2, 3, 332, 5, 6, 7, 8, 9, 11};
      b = xboard(h, f, 30, 30); }
    b.enemy[0].type = 1;
    d = Hm_PlanHand(&b, &o, &xrules);
    assert(Hm_Outs >= 1);
    /* A trap stays too. */
    { static const int h[5] = {5, 6, 400, 8, 10}; b = xboard(h, partner, 30, 30); }
    d = Hm_PlanHand(&b, &o, &xrules);
    assert(!holds(d, 2) && !holds(d, 4) && Hm_Discards == 2);
    /* Fewer cards than the player and few left: it does not run out first. */
    { static const int h[5] = {5, 6, 7, 8, 10}; b = xboard(h, partner, 10, 12); }
    d = Hm_PlanHand(&b, &o, &xrules);
    assert(Hm_Discards == 0 && marks(d) == 1);
    /* Few left but more than the player: it may throw while it stays ahead. */
    { static const int h[5] = {5, 6, 7, 8, 10}; b = xboard(h, partner, 10, 6); }
    d = Hm_PlanHand(&b, &o, &xrules);
    assert(Hm_Discards >= 1 && 10 - marks(d) >= 6);
    /* Only a tie with the 3500 (the game trades both): it is no answer, a
     * 3600 waits in the deck, so it digs. */
    { static const int h[5] = {35, 6, 7, 8, 9}; static const int t[10] = {1, 2, 3, 36, 5, 6, 7, 8, 9, 11};
      b = xboard(h, t, 30, 30); }
    d = Hm_PlanHand(&b, &o, &xrules);
    assert(Hm_Threat == 0 && Hm_Outs >= 1 && Hm_Discards >= 1);
    /* Nor is a 3800 on a star that loses 500 to the 3500's Mars; on the
     * field, the same. */
    { static const int h[5] = {38, 6, 7, 8, 9}; static const int t[10] = {1, 2, 3, 36, 5, 6, 7, 8, 9, 11};
      b = xboard(h, t, 30, 30); }
    d = Hm_PlanHand(&b, &o, &xrules);
    assert(Hm_Threat == 0 && Hm_Discards >= 1);
    { static const int h[5] = {5, 6, 7, 8, 9}; static const int t[10] = {1, 2, 3, 36, 5, 6, 7, 8, 9, 11};
      b = xboard(h, t, 30, 30); }
    b.own[0] = xcard(38);
    d = Hm_PlanHand(&b, &o, &xrules);
    assert(Hm_Threat == 0 && Hm_Discards >= 1);
    /* A 3800 on a neutral star beats it: nothing to dig for. */
    b.own[0].star = 1;
    d = Hm_PlanHand(&b, &o, &xrules);
    assert(Hm_Threat < 0);
    /* Two 10s wait for the same 20: one stays, the other is played and the
     * three others go. */
    { static const int h[5] = {10, 6, 7, 8, 10}; b = xboard(h, partner, 30, 30); }
    d = Hm_PlanHand(&b, &o, &xrules);
    assert(Hm_Keep == 1 && !holds(d, 0) && holds(d, 4) && Hm_Discards == 3);
    /* 12 and 13 fuse with the 1400 into a 200: in front of it they would
     * fuse, so the 12 blocks instead and both others go. */
    { static const int h[5] = {12, 13, 14, 10, 400}; b = xboard(h, partner, 30, 30); }
    d = Hm_PlanHand(&b, &o, &xrules);
    assert(Hm_Discards == 2 && marks(d) == 3 && thrown(d, 1) && thrown(d, 2) && !holds(d, 3) && !holds(d, 4));
    /* Nothing in the deck answers the 3500, a 2500 there beats the 2000
     * beside it: it digs for that one. */
    { static const int h[5] = {5, 6, 7, 8, 9}; static const int a[10] = {1, 2, 3, 25, 5, 6, 7, 8, 9, 11};
      b = xboard(h, a, 30, 30); }
    b.enemy[1] = xcard(20);
    d = Hm_PlanHand(&b, &o, &xrules);
    assert(Hm_Threat == 1 && Hm_Outs >= 1 && Hm_Discards >= 1);
    /* 10 and 20 both still in the deck fuse over the 3500: worth digging. */
    { static const int h[5] = {5, 6, 7, 8, 9}; static const int p[10] = {1, 2, 3, 10, 5, 6, 7, 8, 9, 20};
      b = xboard(h, p, 30, 30); }
    d = Hm_PlanHand(&b, &o, &xrules);
    assert(Hm_Threat == 0 && Hm_Outs >= 2 && Hm_Discards >= 1);
    /* Expert off: the original planner, one card. */
    o.expert = 0;
    { static const int h[5] = {5, 6, 7, 8, 10}; b = xboard(h, partner, 30, 30); }
    d = Hm_PlanHand(&b, &o, &xrules);
    assert(marks(d) == 1);
}

/* fork: Meteor B. Dragon (Mars/Sun) against a face-up Gate Guardian (Moon)
 * and a weaker Jupiter monster: Sun (4000 against it), then the boss. */
static HmCard mcard(int id)
{
    HmCard c = {id, 0, 0, 1500, 1200, 2, 3, 0};
    if (id == 713) { c.attack = 3500; c.defense = 2000; c.star = 1; c.star2 = 8; }
    if (id == 374) { c.attack = 3750; c.defense = 3400; c.star = 9; c.star2 = 1; }
    return c;
}
static int none(int a, int b) { (void)a; (void)b; return 0; }
static void boss_cases(void)
{
    static const HmRules mrules = {mcard, none, none, terrain, ritual};
    HmOptions o = opts();
    HmBoard b = empty();
    HmDecision d;
    o.expert = 1;
    b.enemy[0] = mcard(374); b.enemy[0].star = 9;
    b.enemy[1] = mcard(50); b.enemy[1].star = 2;
    b.hand[0] = mcard(713); b.hand_count = 1;
    d = Hm_PlanHand(&b, &o, &mrules);
    assert(d.result == 1 && d.selection[0] == 11 && d.selection[7] == 1);
    b.hand_count = 0; b.own[0] = mcard(713); b.own[0].star = 8;
    d = Hm_PlanField(&b, &o, &mrules, 0);
    assert(d.result == 2 && d.selection[9] == 1 && d.selection[10] == 56);
    /* An even trade with the boss: only with another monster of ours behind. */
    b = empty(); b.enemy[0] = xcard(37); b.own[0] = xcard(37);
    d = Hm_PlanField(&b, &o, &xrules, 0);
    assert(!(d.result == 2 && d.selection[9] == 1 && d.selection[10] == 56));
    b.own[1] = xcard(12);
    d = Hm_PlanField(&b, &o, &xrules, 0);
    assert(d.result == 2 && d.selection[9] == 1 && d.selection[10] == 56);
    /* Not when the player keeps the best field: a 3800 wall our 3700 cannot
     * break, or a face-down card (an average 1800), against our 1200. */
    b.enemy[1] = xcard(28); b.enemy[1].defense = 3800; b.enemy[1].flags |= HM_DEFENSE;
    d = Hm_PlanField(&b, &o, &xrules, 0);
    assert(!(d.result == 2 && d.selection[9] == 1 && d.selection[10] == 56));
    b.enemy[1].id = -1;
    d = Hm_PlanField(&b, &o, &xrules, 0);
    assert(!(d.result == 2 && d.selection[9] == 1 && d.selection[10] == 56));
    /* A 3900 in hand for next turn makes it pay again. */
    b.enemy[1].id = 28; b.hand[0] = xcard(39); b.hand_count = 1;
    d = Hm_PlanField(&b, &o, &xrules, 0);
    assert(d.result == 2 && d.selection[9] == 1 && d.selection[10] == 56);
}

int main(void)
{
    tactical_cases(); spell_cases(); hand_cases(); retail_cases(); bounded_fixtures(); expert_cases(); boss_cases();
    puts("AI hard mode: tactical, spell, fusion and 500 bounded fixtures passed");
    return 0;
}
