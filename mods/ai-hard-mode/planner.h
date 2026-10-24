#ifndef AI_HARD_MODE_PLANNER_H
#define AI_HARD_MODE_PLANNER_H

/* Independent of guest pointers: fixtures exercise the exact shipped planner. */
#define HM_ROW 5
#define HM_HAND 20
#define HM_USED 0x4000
#define HM_DOWN 0x1000
#define HM_DEFENSE 0x0800
#define HM_WIN 1000000

typedef struct {
    int id, effect, type, attack, defense, star, star2, flags;
} HmCard;
typedef struct {
    HmCard own[5], enemy[5], back[5], enemy_back[5], hand[HM_HAND];
    int hand_count, lp, max_lp, enemy_lp, pinned, enemy_pinned, terrain;
    /* fork: the cards still in this side's deck, as its deck list tells a
     * player (never their order), and how many each side has left. */
    int deck[40], deck_count, enemy_deck_count;
} HmBoard;
typedef struct {
    int hand, fusion, guardian, attacks, defense, spells, effective;
    int materials, budget, blind_risk, material_cost, trades;
    int expert; /* fork: discard to dig, mind the deck race */
} HmOptions;
typedef struct {
    HmCard (*card)(int id);
    int (*fusion)(int a, int b);
    int (*equip)(int equipment, int monster);
    int (*terrain)(int type, int terrain);
    int (*ritual)(int card);
} HmRules;
typedef struct {
    /* Byte-for-byte contract with the game's twelve-byte pending selection. */
    unsigned char selection[12];
    int result, score, nodes;
} HmDecision;
/* fork: the last expert hand, for the log: cards thrown away, and how many
 * cards left in the deck answer the monster it could not beat. */
extern int Hm_Discards, Hm_Outs;
/* fork: why it dug or not (HM_WHY_*), the threat (enemy slot, -1 none), the
 * stat to beat, our best ATK this turn against each enemy (star included),
 * and the hand masks kept as answers, held for the field and thrown. */
enum { HM_WHY_OFF, HM_WHY_SHORT, HM_WHY_NO_THREAT, HM_WHY_NO_OUTS, HM_WHY_NOT_FROM_HAND,
       HM_WHY_NO_ROOM, HM_WHY_DIGS };
extern int Hm_Threat, Hm_Need, Hm_Reach[5], Hm_Why;
extern unsigned Hm_Keep, Hm_Hold, Hm_Junk;

/* The matchup the planner scores guardian stars by: the game's when the
 * adapter sets it (Duel_CalcGuardianStarMatchup), else the disc's cycles. */
extern int (*Hm_StarMatchup)(int attacker, int defender);
int Hm_StarBonus(int a, int b);
int Hm_KeepValue(HmCard card);
int Hm_ChooseStar(const HmBoard *, HmCard, int pinned);
HmDecision Hm_PlanHand(const HmBoard *, const HmOptions *, const HmRules *);
HmDecision Hm_PlanField(const HmBoard *, const HmOptions *, const HmRules *,
                        const HmDecision *retail);
#endif
