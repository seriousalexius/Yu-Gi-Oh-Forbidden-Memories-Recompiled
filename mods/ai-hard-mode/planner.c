#include "planner.h"

static int max(int a, int b) { return a > b ? a : b; }
static int min(int a, int b) { return a < b ? a : b; }
static int clamp(int n) { return max(0, min(9999, n)); }
static int monster(HmCard c) { return c.id && c.type < 20; }
static int value(HmCard c) { return max(c.attack, c.defense); }
static int occupied(const HmCard *row)
{
    int n = 0, i;
    for (i = 0; i < 5; i++) n += row[i].id != 0;
    return n;
}

/* The disc's two cycles, for the fixtures, which run without the game. */
static int retail_star_bonus(int a, int b)
{
    int start = a >= 7 ? 7 : 1, size = a >= 7 ? 4 : 6;
    if (a < 1 || a > 10 || b < start || b >= start + size) return 0;
    if ((a - start + 1) % size == b - start) return 500;
    if ((a - start + size - 1) % size == b - start) return -500;
    return 0;
}

int (*Hm_StarMatchup)(int attacker, int defender);

int Hm_StarBonus(int a, int b)
{
    /* In the game, the game's own matchup, which follows a mod's
       "guardian_stars" (stars 11-15 included); no star is no bonus. */
    if (Hm_StarMatchup) return a >= 1 && a <= 15 && b >= 1 && b <= 15 ? Hm_StarMatchup(a, b) : 0;
    return retail_star_bonus(a, b);
}

int Hm_KeepValue(HmCard c)
{
    if (!c.id) return 0;
    if (monster(c)) return value(c);
    if (c.effect == 337 || c.effect == 348 || c.effect == 672) return 3000;
    if (c.type == 23 || c.type == 21) return 1400;
    return 800;
}

/* Hidden cards arrive redacted (id=-1), with no secret identity or stats. */
static int safety(const HmBoard *b, HmCard c, int defense)
{
    int loss = 0, i;
    if (b->enemy_pinned) return 0;
    for (i = 0; i < 5; i++) if (b->enemy[i].id) {
        HmCard e = b->enemy[i];
        int power = e.id < 0 ? 1800 : e.attack + Hm_StarBonus(e.star, c.star);
        int diff = power - (defense ? c.defense : c.attack);
        if (diff > 0) loss = max(loss, (defense ? 0 : diff) + value(c) / 2);
    }
    return loss;
}

int Hm_ChooseStar(const HmBoard *b, HmCard c, int pinned)
{
    int choice, best = -0x3fffffff, answer = 0, i;
    for (choice = 0; choice < 2; choice++) {
        HmCard current = c;
        int score = 0;
        /* A card with one star (no second, or the same twice) has no choice. */
        if (choice && (!c.star2 || c.star2 == c.star)) break;
        current.star = choice ? c.star2 : c.star;
        if (!pinned) for (i = 0; i < 5; i++) if (b->enemy[i].id > 0) {
            HmCard e = b->enemy[i];
            int diff = c.attack + Hm_StarBonus(current.star, e.star) -
                ((e.flags & HM_DEFENSE) ? e.defense : e.attack);
            if (diff > 0) score = max(score, 2000 + diff + value(e) / 2);
        }
        /* fork: the risk after the best hit (the max used to wipe it out:
         * a star that hits a weak card left it under the strong one). */
        score -= min(safety(b, current, 0), safety(b, current, 1));
        if (score > best) { best = score; answer = choice; }
    }
    return answer;
}

typedef struct { int a, t, damage, removed, lost, gain, uncertain; } Attack;
typedef struct {
    const HmBoard *b;
    const HmOptions *o;
    HmDecision best;
    int nodes;
} AttackSearch;

/* fork (expert): the player's strongest face-up monster is worth more than
 * its stats, as an expert goes for the boss; an even trade with it pays
 * only when we keep the best field after it: our best other monster (or the
 * best one in hand, for next turn) beats the best the player keeps, ATK or
 * DEF (a face-down card counts as an average 1800). A bad trade is never taken. */
static int boss(const AttackSearch *s, int attacker, int target, int trade)
{
    HmCard e = s->b->enemy[target];
    int i, best = 0, ours = 0, theirs = 0;
    if (!s->o->expert) return 0;
    for (i = 0; i < 5; i++) if (s->b->enemy[i].id > 0) best = max(best, s->b->enemy[i].attack);
    if (e.attack < best) return 0;
    if (!trade) return 2 * e.attack;
    for (i = 0; i < 5; i++) if (i != attacker && monster(s->b->own[i])) ours = max(ours, s->b->own[i].attack);
    for (i = 0; i < s->b->hand_count; i++) if (s->b->hand[i].id > 0 && monster(s->b->hand[i]))
        ours = max(ours, s->b->hand[i].attack);
    for (i = 0; i < 5; i++) if (i != target && s->b->enemy[i].id) {
        HmCard x = s->b->enemy[i];
        theirs = max(theirs, x.id < 0 ? 1800 : value(x));
    }
    return ours > theirs ? e.attack : -(value(e) + 400);
}

static int attack_options(AttackSearch *s, unsigned used, unsigned dead, Attack *out)
{
    int i, j, n = 0, targets = 0;
    for (j = 0; j < 5; j++) if (s->b->enemy[j].id && !(dead & (1u << j))) targets++;
    for (i = 0; i < 5; i++) {
        HmCard a = s->b->own[i];
        if (!monster(a) || (a.flags & HM_USED) || (used & (1u << i))) continue;
        if (!targets) {
            out[n++] = (Attack){i, -1, a.attack, 0, 0, a.attack * 4, 0};
            continue;
        }
        for (j = 0; j < 5; j++) {
            HmCard e = s->b->enemy[j];
            Attack move = {i, j, 0, 0, 0, 0, 0};
            int diff;
            if (!e.id || (dead & (1u << j))) continue;
            if (e.id < 0) {
                /* A declared risk policy, never a peek at the hidden card. */
                if (!s->o->blind_risk || a.attack < 3000 - s->o->blind_risk * 20) continue;
                move.uncertain = 1;
                move.gain = 100 + s->o->blind_risk * 3;
            } else {
                diff = a.attack + Hm_StarBonus(a.star, e.star) -
                    ((e.flags & HM_DEFENSE) ? e.defense : e.attack);
                if (diff < 0 || (diff == 0 && ((e.flags & HM_DEFENSE) || !s->o->trades))) continue;
                move.damage = (e.flags & HM_DEFENSE) ? 0 : diff;
                move.removed = value(e) + 400 + boss(s, i, j, diff == 0);
                move.lost = diff == 0 ? value(a) + 400 : 0;
                move.gain = move.damage * 4 + move.removed - move.lost;
            }
            out[n++] = move;
        }
    }
    /* Stable best-first ordering; economical attacker breaks ties. */
    for (i = 1; i < n; i++) {
        Attack v = out[i];
        j = i;
        while (j && (out[j-1].gain < v.gain ||
               (out[j-1].gain == v.gain && s->b->own[out[j-1].a].attack > s->b->own[v.a].attack))) {
            out[j] = out[j-1]; j--;
        }
        out[j] = v;
    }
    return n;
}

static void attack_search(AttackSearch *s, unsigned used, unsigned dead,
                          int damage, int score, int depth, Attack first)
{
    Attack moves[25];
    int n, i;
    if (s->nodes >= s->o->budget) return;
    s->nodes++;
    n = attack_options(s, used, dead, moves);
    /* All root alternatives, up to three promising continuations per node. */
    if (depth) n = min(n, 3);
    for (i = 0; i < n; i++) {
        Attack m = moves[i], root = depth ? first : m;
        int next_damage = damage + m.damage;
        int next_score = score + m.gain;
        int ranked = next_score + (next_damage >= s->b->enemy_lp ? HM_WIN : 0);
        if (ranked > s->best.score || (ranked == s->best.score && ranked > 0 &&
            s->best.selection[9] && s->b->own[root.a].attack <
            s->b->own[s->best.selection[9]-1].attack)) {
            s->best.score = ranked;
            s->best.result = 2;
            s->best.selection[9] = (unsigned char)(root.a + 1);
            s->best.selection[10] = (unsigned char)(root.t < 0 ? 58 : root.t + 56);
        }
        if (!m.uncertain && depth < 4 && next_damage < s->b->enemy_lp)
            attack_search(s, used | (1u << m.a), dead | (m.t < 0 ? 0 : 1u << m.t),
                          next_damage, next_score, depth + 1, root);
    }
}

static HmDecision attacks(const HmBoard *b, const HmOptions *o)
{
    AttackSearch s = {b, o, {{0}, 0, 0, 0}, 0};
    Attack first = {0};
    if (!b->pinned) attack_search(&s, 0, 0, 0, 0, 0, first);
    s.best.nodes = s.nodes;
    return s.best;
}

static int wall(int id)
{
    static const int ids[] = {72, 362, 366, 2, 40, 83, 88, 121, 156, 170,
                              255, 274, 277, 369, 416, 418, 449, 468, 497, 518, 0};
    int i;
    for (i = 0; ids[i]; i++) if (ids[i] == id) return 1;
    return 0;
}
static int stays(int id)
{
    return id == 380 || id == 374 || id == 67 || id == 713 || id == 217 ||
           id == 364 || id == 1 || id == 392;
}

static int terrain_delta(const HmBoard *b, const HmRules *r, int terrain)
{
    int i, score = 0;
    for (i = 0; i < 5; i++) {
        if (monster(b->own[i])) score += r->terrain(b->own[i].type, terrain) - r->terrain(b->own[i].type, b->terrain);
        if (b->enemy[i].id > 0) score -= r->terrain(b->enemy[i].type, terrain) - r->terrain(b->enemy[i].type, b->terrain);
    }
    return score;
}

static int removal_type(int effect)
{
    switch (effect) {
    case 329: return 0; case 653: return 3; case 656: return 2;
    case 660: return 14; case 662: return 9; case 663: return 18; case 664: return 12;
    default: return -1;
    }
}

/* Returns utility and an own-field target (equips only). Unknown effects are
 * left set, never speculatively activated. The live game resolves all effects. */
static int spell(const HmBoard *b, const HmOptions *o, const HmRules *r,
                 HmCard c, int *target)
{
    int i, score = 0, id = c.effect, amount, type = removal_type(id);
    *target = 0;
    if (id >= 343 && id <= 347) {
        static const int burn[] = {50, 100, 200, 500, 1000};
        amount = burn[id - 343];
        /* Known reflection is actionable information, hidden traps are not. */
        for (i = 0; i < 5; i++) if (b->enemy_back[i].effect == 687) return 0;
        return amount * 4 + (amount >= b->enemy_lp ? HM_WIN : 0);
    }
    if (id >= 338 && id <= 342) {
        static const int heal[] = {200, 500, 1000, 2000, 5000};
        for (i = 0; i < 5; i++) if (b->enemy_back[i].effect == 688) return 0;
        amount = max(0, min(heal[id - 338], b->max_lp - b->lp));
        return amount * (b->lp < 2500 ? 3 : 1);
    }
    if (id == 672) return occupied(b->enemy_back) * 1600;
    if (id >= 330 && id <= 335) return max(0, terrain_delta(b, r, id - 329) * 2);
    if (id == 348) {
        if (b->enemy_pinned) return 0;
        for (i = 0; i < 5; i++) if (b->enemy[i].id)
            score += b->enemy[i].id < 0 ? 600 : b->enemy[i].attack / 2;
        return score;
    }
    if (id == 350) {
        for (i = 0; i < 5; i++) if (b->enemy[i].id < 0) score += 500;
        return score;
    }
    if (id == 337 || id == 336 || id == 661 || type >= 0) {
        for (i = 0; i < 5; i++) {
            HmCard e = b->enemy[i];
            if (!e.id) continue;
            if (id == 337 || id == 336 ||
                (e.id > 0 && (e.type == type || (id == 661 && e.attack >= 1500))))
                score += e.id < 0 ? 1400 : value(e) + 600;
        }
        if (id == 336) {
            int skipped_self = 0;
            for (i = 0; i < 5; i++) {
                if (b->own[i].id) score -= value(b->own[i]) + 600;
                if (b->enemy_back[i].id) score += 1000;
                if (b->back[i].id == c.id && !skipped_self) skipped_self = 1;
                else score -= Hm_KeepValue(b->back[i]);
            }
        }
        return max(0, score);
    }
    if (id == 349 || id == 669) {
        for (i = 0; i < 5; i++) if (b->enemy[i].id) score += 700;
        return score;
    }
    if (id == 320) { /* Stop Defense: only use when it improves our attack sequence. */
        HmBoard next = *b;
        HmOptions cheap = *o;
        HmDecision before, after;
        cheap.budget = min(128, o->budget);
        for (i = 0; i < 5; i++) next.enemy[i].flags &= ~HM_DEFENSE;
        before = attacks(b, &cheap); after = attacks(&next, &cheap);
        return max(0, after.score - before.score);
    }
    if (c.type == 23) {
        for (i = 0; i < 5; i++) if (monster(b->own[i]) && r->equip(c.id, b->own[i].id)) {
            HmBoard next = *b;
            HmOptions cheap = *o;
            int bonus = id == 657 ? 1000 : 500, gain;
            cheap.budget = min(128, o->budget);
            next.own[i].attack = clamp(next.own[i].attack + bonus);
            next.own[i].defense = clamp(next.own[i].defense + bonus);
            gain = bonus + max(0, attacks(&next, &cheap).score - attacks(b, &cheap).score);
            if (gain > score) { score = gain; *target = i + 1; }
        }
        return score;
    }
    if (c.type == 22 && r->ritual(c.id)) return 2000;
    return 0;
}

static int stalling(const HmBoard *b);
HmDecision Hm_PlanField(const HmBoard *b, const HmOptions *from, const HmRules *r,
                        const HmDecision *retail)
{
    HmDecision best = {{0}, 0, 0, 0}, a;
    HmOptions careful = *from;
    const HmOptions *o = &careful;
    int i, target, score, wait = from->expert && stalling(b);
    /* fork: winning the deck race, no blind attacks and defense on a tie. */
    if (wait) careful.blind_risk = 0;
    if (retail && !o->attacks && !o->defense && !o->spells) return *retail;
    if (retail && retail->result == 2) {
        int slot = retail->selection[9];
        int is_attack = slot <= 5 && retail->selection[10] >= 56;
        if ((slot >= 6 && !o->spells) || (!is_attack && slot <= 5 && !o->defense)) return *retail;
        if (is_attack && !o->attacks) { best = *retail; best.score = 1; }
    }
    if (!o->defense && !retail) for (i = 0; i < 5; i++)
        if (b->own[i].id && !(b->own[i].flags & HM_USED) && wall(b->own[i].effect)) {
            best.result = 2; best.selection[9] = i + 1; best.selection[11] = 1; return best;
        }
    if (o->attacks) best = attacks(b, o);
    if (o->spells) for (i = 0; i < 5; i++) if (b->back[i].id && !(b->back[i].flags & HM_USED)) {
        score = spell(b, o, r, b->back[i], &target);
        if (score > best.score) {
            a = (HmDecision){{0}, 2, score, best.nodes};
            a.selection[9] = i + 6;
            a.selection[10] = target ? target : i + 6;
            best = a;
        }
    }
    if (best.result) return best;
    if (retail && !o->defense) { best.result = 3; return best; }
    for (i = 0; i < 5; i++) if (b->own[i].id && !(b->own[i].flags & HM_USED)) {
        best.result = 2;
        best.selection[9] = i + 1;
        best.selection[11] = o->defense ?
            safety(b, b->own[i], 1) < safety(b, b->own[i], 0) + wait : !stays(b->own[i].effect);
        return best;
    }
    best.result = 3;
    return best;
}

typedef struct {
    const HmBoard *b;
    const HmOptions *o;
    const HmRules *r;
    HmDecision best;
    int nodes, path[5];
    int reach[5]; /* fork: the strongest ATK any play brings against each enemy, star included */
} HandSearch;

static int destination(const HmBoard *b)
{
    int i, best = 0;
    for (i = 0; i < 5; i++) {
        if (!b->own[i].id) return i;
        if (value(b->own[i]) < value(b->own[best])) best = i;
    }
    return best;
}

static void consider_monster(HandSearch *s, HmCard card, int count, int field)
{
    int dest = field >= 0 ? field : destination(s->b);
    HmCard identity = s->r->card(card.id);
    int choice;
    int i, score, hit = 0, old = s->b->own[dest].id ? value(s->b->own[dest]) : 0;
    /* Field snapshots contain the selected star; output byte 7 is instead
     * an index into the card's original pair. Re-establish that pair. */
    card.star = identity.star; card.star2 = identity.star2;
    choice = s->o->guardian ? Hm_ChooseStar(s->b, card, s->b->pinned) : 0;
    if (choice) card.star = card.star2;
    for (i = 0; i < 5; i++) if (s->b->enemy[i].id > 0)
        s->reach[i] = max(s->reach[i], card.attack + Hm_StarBonus(card.star, s->b->enemy[i].star));
    for (i = 0; i < 5; i++) if (s->b->enemy[i].id > 0 && !s->b->pinned) {
        HmCard e = s->b->enemy[i];
        int diff = card.attack + Hm_StarBonus(card.star, e.star) -
            ((e.flags & HM_DEFENSE) ? e.defense : e.attack);
        if (diff > 0) hit = max(hit, value(e) / 2 + ((e.flags & HM_DEFENSE) ? 0 : diff));
        if (!(e.flags & HM_DEFENSE) && diff >= s->b->enemy_lp) hit = HM_WIN;
    }
    if (!occupied(s->b->enemy) && !s->b->pinned) {
        hit = card.attack;
        if (card.attack >= s->b->enemy_lp) hit += HM_WIN;
    }
    score = value(card) - old + hit - min(safety(s->b, card, 0), safety(s->b, card, 1)) / 2;
    score -= s->o->material_cost * count;
    for (i = 0; i < count; i++) score -= Hm_KeepValue(s->b->hand[s->path[i]]) / 12;
    if (!s->best.result || score > s->best.score) {
        s->best = (HmDecision){{0}, 1, score, 0};
        for (i = 0; i < count; i++) s->best.selection[i] = s->path[i] + 11;
        s->best.selection[6] = dest + 1;
        s->best.selection[7] = choice;
        s->best.selection[8] = field < 0;
    }
}

static HmCard combine(HandSearch *s, HmCard a, HmCard b)
{
    HmCard out = {0};
    int id = s->r->fusion(a.id, b.id);
    if (id) {
        out = s->r->card(id);
        if (s->o->effective) {
            int bonus = s->r->terrain(out.type, s->b->terrain);
            out.attack = clamp(out.attack + bonus); out.defense = clamp(out.defense + bonus);
        }
    } else if (monster(a) && b.type == 23 && s->r->equip(b.id, a.id)) {
        out = a;
        out.attack = clamp(out.attack + (b.effect == 657 ? 1000 : 500));
        out.defense = clamp(out.defense + (b.effect == 657 ? 1000 : 500));
    }
    return out;
}

static void fusion_search(HandSearch *s, HmCard card, unsigned used, int count, int field)
{
    int i;
    if (s->nodes >= s->o->budget || count >= s->o->materials || count >= 5) return;
    for (i = 0; i < s->b->hand_count && s->nodes < s->o->budget; i++) if (!(used & (1u << i))) {
        HmCard next;
        s->nodes++;
        next = combine(s, card, s->b->hand[i]);
        if (!monster(next)) continue;
        s->path[count] = i;
        consider_monster(s, next, count + 1, field);
        fusion_search(s, next, used | (1u << i), count + 1, field);
    }
}

/* fork: the hand cards in skip stay out of every play. */
static HmDecision plan_hand(const HmBoard *b, const HmOptions *o, const HmRules *r,
                            unsigned skip, int *reach)
{
    HandSearch s = {b, o, r, {{0}, 0, 0, 0}, 0, {0}, {0}};
    int i, j, slot, target, score, immediate;
    /* Score every single-card option before spending the bounded fusion budget. */
    for (i = 0; i < b->hand_count; i++) {
        HmCard c = b->hand[i];
        if (!c.id || (skip & (1u << i))) continue;
        if (monster(c)) {
            if (o->effective) {
                int bonus = r->terrain(c.type, b->terrain);
                c.attack = clamp(c.attack + bonus); c.defense = clamp(c.defense + bonus);
            }
            s.path[0] = i;
            consider_monster(&s, c, 1, -1);
        } else {
            slot = -1;
            for (j = 0; j < 5; j++) if (!b->back[j].id) { slot = j + 6; break; }
            if (c.type == 22 && slot < 0) continue;
            target = 0;
            score = o->spells ? spell(b, o, r, c, &target) : 0;
            /* Traps are reactive even when ai_effect gives them a useful
             * tactical analogue: set them for the duel's trigger logic,
             * never try to cast them as a Magic card. */
            immediate = score > 0 && c.type != 21 && c.type != 22;
            if (score <= 0) {
                if (slot < 0) continue;
                score = c.type == 21 ? 500 : 50;
            }
            score -= o->material_cost;
            if (!s.best.result || score > s.best.score) {
                s.best = (HmDecision){{0}, 1, score, 0};
                s.best.selection[0] = i + 11;
                s.best.selection[6] = target ? target : (slot < 0 ? 6 : slot);
                /* Rituals are set first: the field controller checks tributes. */
                s.best.selection[8] = !immediate;
                if (c.type == 23 && immediate && target && o->guardian) {
                    HmCard equipped = b->own[target - 1], identity = r->card(equipped.id);
                    int bonus = c.effect == 657 ? 1000 : 500;
                    equipped.attack = clamp(equipped.attack + bonus);
                    equipped.defense = clamp(equipped.defense + bonus);
                    equipped.star = identity.star; equipped.star2 = identity.star2;
                    s.best.selection[7] = Hm_ChooseStar(b, equipped, b->pinned);
                }
            }
        }
    }
    if (o->fusion) {
        /* Iterative material limits prevent one early seed consuming the whole
         * budget before the other two-card combinations have been considered. */
        HmOptions bounded = *o;
        int depth;
        s.o = &bounded;
        for (depth = 2; depth <= o->materials && s.nodes < o->budget; depth++) {
            bounded.materials = depth;
            for (i = 0; i < b->hand_count && s.nodes < o->budget; i++) {
                HmCard c = b->hand[i];
                if (!monster(c) || (skip & (1u << i))) continue;
                if (o->effective) {
                    int bonus = r->terrain(c.type, b->terrain);
                    c.attack = clamp(c.attack + bonus); c.defense = clamp(c.defense + bonus);
                }
                s.path[0] = i;
                fusion_search(&s, c, skip | 1u << i, 1, -1);
            }
            for (i = 0; i < 5 && s.nodes < o->budget; i++) if (monster(b->own[i]))
                fusion_search(&s, b->own[i], skip, 0, i);
        }
    }
    s.best.nodes = s.nodes;
    if (reach) for (i = 0; i < 5; i++) reach[i] = s.reach[i];
    return s.best;
}


/* fork: the arena's expert hand (option expert). The CPU knows which cards
 * are still in its deck, as a player knows his deck list (never their
 * order), sees only what the player sees, and plays like an expert:
 * - facing a monster no play of this turn beats or destroys, it keeps the
 *   cards that make an answer with a card still in its deck, plays the best
 *   of the others and throws the rest away in front of it, through fusions
 *   that fail as a player's would, so that next turn it draws more cards;
 * - otherwise it throws away only dead monsters, while its deck is long;
 * - it never thins its deck below the player's once that race matters, and
 *   spends one card a turn when the player will run out first. */
int Hm_Discards, Hm_Outs, Hm_Threat = -1, Hm_Need, Hm_Reach[5], Hm_Why;
unsigned Hm_Keep, Hm_Hold, Hm_Junk;
static int power(const HmBoard *b, const HmOptions *o, const HmRules *r, HmCard c, int field)
{
    if (!monster(c)) return 0;
    /* Field snapshots already carry the terrain (effective stats). */
    return field ? c.attack : c.attack + (o->effective ? r->terrain(c.type, b->terrain) : 0);
}
static int bonus(HmCard equip) { return equip.effect == 657 ? 1000 : 500; }
/* fork: what the guardian star adds in a fight with e: a field card has its
 * star, a card still to summon picks the better of its two. */
static int edge(HmCard c, HmCard e, int field)
{
    int best = Hm_StarBonus(c.star, e.star);
    if (!field && c.star2) best = max(best, Hm_StarBonus(c.star2, e.star));
    return best;
}
/* fork: the attack traps spring on an attacker up to their ATK (retail
 * thresholds; Widespread Ruin stops any). */
static int trap_reach(int effect)
{
    switch (effect) {
    case 681: return 500; case 682: return 1000; case 683: return 1500;
    case 684: return 2000; case 685: return 3000; case 686: return 9999;
    default: return -1;
    }
}
static int kills(HmCard c, HmCard e)
{
    int type = removal_type(c.effect);
    return c.id > 0 && !monster(c) && (c.effect == 337 || c.effect == 336 ||
        (type >= 0 && e.type == type) || (c.effect == 661 && e.attack >= 1500) ||
        e.attack <= trap_reach(c.effect));
}
/* fork: what Spellbinding Circle and Shadow Spell take off the player's
 * monsters; the terrain a field spell sets. */
static int penalty(HmCard c)
{
    return c.id <= 0 || monster(c) ? 0 : c.effect == 349 ? 500 : c.effect == 669 ? 1000 : 0;
}
static int field_spell(HmCard c)
{
    return c.id > 0 && !monster(c) && c.effect >= 330 && c.effect <= 335 ? c.effect - 329 : 0;
}
static int useful_spell(HmCard c)
{
    /* Traps wait face down; removal, Swords and the weakening spells stay useful. */
    return c.id > 0 && !monster(c) && (c.type == 21 || c.effect == 348 || c.effect == 672 ||
        c.effect == 337 || c.effect == 336 || c.effect == 661 || removal_type(c.effect) >= 0 ||
        penalty(c));
}
/* Our strongest ATK on the field or in hand, on a terrain. */
static int ours(const HmBoard *b, const HmOptions *o, const HmRules *r, int terrain)
{
    int i, best = 0;
    for (i = 0; i < 5; i++) if (monster(b->own[i]))
        best = max(best, b->own[i].attack + (o->effective ?
            r->terrain(b->own[i].type, terrain) - r->terrain(b->own[i].type, b->terrain) : 0));
    for (i = 0; i < b->hand_count; i++) if (b->hand[i].id > 0 && monster(b->hand[i]))
        best = max(best, b->hand[i].attack + (o->effective ? r->terrain(b->hand[i].type, terrain) : 0));
    return best;
}
/* What e's stat (need) becomes on another terrain. */
static int moved(const HmBoard *b, const HmOptions *o, const HmRules *r, HmCard e, int need, int terrain)
{
    return o->effective ? need - r->terrain(e.type, b->terrain) + r->terrain(e.type, terrain) : need;
}
/* fork: c answers e with what we already hold: it destroys it (removal, an
 * attack trap that stops it), weakens it under our best, or sets a terrain
 * that turns the fight. */
static int answers(const HmBoard *b, const HmOptions *o, const HmRules *r, HmCard c, HmCard e, int need)
{
    int t = field_spell(c);
    if (kills(c, e)) return 1;
    if (penalty(c)) return ours(b, o, r, b->terrain) > need - penalty(c);
    if (t && t != b->terrain) return ours(b, o, r, t) > moved(b, o, r, e, need, t);
    return 0;
}

static int bits(unsigned long long m) { int n = 0; for (; m; m &= m - 1) n++; return n; }
/* How many cards left in the deck answer e (beat need in battle, or destroy
 * it), alone or with a card in hand or on the field. fork: the hand cards
 * that take part go in keep only for their share of those answers: the one
 * that makes the most of them, then each that adds a third of them (two
 * cards waiting for the same partner are one too many: the other goes, for
 * a fresh draw). */
static int outs(const HmBoard *b, const HmOptions *o, const HmRules *r,
                HmCard e, int need, unsigned *keep)
{
    unsigned long long alone = 0, with[5] = {0}, covered;
    int i, j, f, n;
    for (i = 0; i < b->deck_count; i++) {
        HmCard d = r->card(b->deck[i]);
        int out;
        if (!d.id) continue;
        out = answers(b, o, r, d, e, need) || power(b, o, r, d, 0) + edge(d, e, 0) > need;
        for (j = 0; j < b->hand_count + 5; j++) {
            int field = j >= b->hand_count, hit = 0;
            HmCard h = field ? b->own[j - b->hand_count] : b->hand[j];
            if (h.id <= 0) continue;
            if (monster(h) && monster(d) && (f = r->fusion(h.id, d.id)) != 0)
                hit = power(b, o, r, r->card(f), 0) + edge(r->card(f), e, 0) > need;
            else if (monster(h) && d.type == 23 && r->equip(d.id, h.id))
                hit = power(b, o, r, h, field) + bonus(d) + edge(h, e, field) > need;
            else if (!field && h.type == 23 && monster(d) && r->equip(h.id, d.id))
                hit = power(b, o, r, d, 0) + bonus(h) + edge(d, e, 0) > need;
            else if (!field && monster(d) && penalty(h))
                hit = power(b, o, r, d, 0) + edge(d, e, 0) > need - penalty(h);
            else if (!field && monster(d) && field_spell(h) && field_spell(h) != b->terrain)
                hit = d.attack + (o->effective ? r->terrain(d.type, field_spell(h)) : 0) >
                    moved(b, o, r, e, need, field_spell(h));
            if (hit && field) out = 1;
            else if (hit) with[j] |= 1ull << i;
        }
        if (out) alone |= 1ull << i;
    }
    for (covered = alone, j = 0; j < b->hand_count; j++) covered |= with[j];
    n = bits(covered);
    for (covered = alone;;) {
        int pick = -1, gain = 0;
        for (j = 0; j < b->hand_count; j++)
            if (!(*keep & (1u << j)) && bits(with[j] & ~covered) > gain) { gain = bits(with[j] & ~covered); pick = j; }
        if (pick < 0 || gain * 3 < n) break;
        *keep |= 1u << pick; covered |= with[pick];
    }
    return n;
}

/* The best a hand monster can still become, with any card it may meet. */
static int potential(const HmBoard *b, const HmOptions *o, const HmRules *r, int slot)
{
    HmCard h = b->hand[slot];
    int best = max(power(b, o, r, h, 0), h.defense), i, f;
    for (i = 0; i < b->hand_count + 5 + b->deck_count; i++) {
        HmCard x = i < b->hand_count ? b->hand[i] : i < b->hand_count + 5 ?
            b->own[i - b->hand_count] : r->card(b->deck[i - b->hand_count - 5]);
        if (i == slot || x.id <= 0) continue;
        if (monster(x) && (f = r->fusion(h.id, x.id)) != 0)
            best = max(best, power(b, o, r, r->card(f), 0));
        else if (x.type == 23 && r->equip(x.id, h.id))
            best = max(best, power(b, o, r, h, 0) + bonus(x));
    }
    return best;
}

/* As the game places a hand's marked cards: a fusion, else either card
 * equips the other, else the earlier card goes to the graveyard. */
static HmCard chain(const HmBoard *b, const HmRules *r, const unsigned char *slots, int n)
{
    HmCard cur = b->hand[slots[0] - 11], next;
    int i, f;
    for (i = 1; i < n; i++) {
        next = b->hand[slots[i] - 11];
        if ((f = r->fusion(cur.id, next.id)) != 0) cur = r->card(f);
        else if (r->equip(next.id, cur.id)) cur.attack += bonus(next);
        else if (r->equip(cur.id, next.id)) { next.attack += bonus(cur); cur = next; }
        else cur = next;
    }
    return cur;
}
static int marked(const HmDecision *d)
{
    int n = 0;
    while (n < 5 && d->selection[n]) n++;
    return n;
}
static unsigned spent(const HmDecision *d)
{
    unsigned m = 0;
    int i;
    for (i = 0; i < marked(d); i++) m |= 1u << (d->selection[i] - 11);
    return m;
}
/* A monster summoned from the hand alone: discards can go in front of it. */
static int from_hand(const HmBoard *b, const HmDecision *d)
{
    int first = d->selection[0] - 11;
    return d->result == 1 && d->selection[8] && d->selection[6] >= 1 && d->selection[6] <= 5 &&
        first >= 0 && first < b->hand_count && monster(b->hand[first]);
}
static int affordable(const HmBoard *b, int cards)
{
    int left = b->deck_count - cards;
    /* The player draws first: as many cards left keeps the race won. */
    return left >= 12 || (left >= 2 && left >= b->enemy_deck_count);
}
static int stalling(const HmBoard *b)
{
    return b->deck_count && b->enemy_deck_count <= 12 && b->deck_count > b->enemy_deck_count;
}

/* Puts the n discards in front of base in an order that changes nothing of
 * the play: no accidental fusion or equip on the way. */
static int prepend(const HmBoard *b, const HmRules *r, HmDecision *base, const int *junk, int n)
{
    unsigned char full[5];
    int m = marked(base), order[4], i, j, code, total = 1;
    HmCard want = chain(b, r, base->selection, m), got;
    if (n + m > 5) return 0;
    for (i = 0; i < n; i++) total *= n;
    for (code = 0; code < total; code++) {
        unsigned seen = 0;
        int c = code;
        for (i = 0; i < n; i++, c /= n) { order[i] = c % n; seen |= 1u << order[i]; }
        if (seen != (1u << n) - 1) continue;
        for (i = 0; i < n; i++) full[i] = (unsigned char)(junk[order[i]] + 11);
        for (j = 0; j < m; j++) full[n + j] = base->selection[j];
        got = chain(b, r, full, n + m);
        if (got.id == want.id && got.attack == want.attack) {
            for (i = 0; i < n + m; i++) base->selection[i] = full[i];
            return 1;
        }
    }
    return 0;
}

/* fork: puts in front of base the most of the junk cards it can, the least
 * valuable first when the deck allows fewer; any of them, not only the
 * first ones in that order (one may fuse with the card played). */
static int throw_most(const HmBoard *b, const HmRules *r, HmDecision *base, unsigned junk, int threat)
{
    HmDecision t, found = *base;
    int list[5], pick[4], count = 0, n, mask, i, j, k, rank, best;
    for (i = 0; i < b->hand_count && count < 5; i++) if ((junk & (1u << i)) && b->hand[i].id) {
        for (j = count; j > 0 && Hm_KeepValue(b->hand[list[j-1]]) > Hm_KeepValue(b->hand[i]); j--)
            list[j] = list[j-1];
        list[j] = i; count++;
    }
    for (n = min(count, 4); n > 0; n--) {
        int cards = marked(base) + n;
        if (!affordable(b, cards) || (threat < 0 && b->deck_count - cards < 15)) continue;
        for (best = -1, mask = 1; mask < 1 << count; mask++) {
            for (i = k = rank = 0; i < count; i++)
                if (mask & (1 << i)) { if (k < 4) pick[k] = list[i]; k++; rank += i; }
            if (k != n || (best >= 0 && rank >= best)) continue;
            t = *base;
            if (prepend(b, r, &t, pick, n)) { found = t; best = rank; }
        }
        if (best >= 0) { *base = found; return n; }
    }
    return 0;
}

/* fork: a hand monster that does not answer the threat stays (it may be
 * played, it is not thrown) only when it holds the field: a wall whose DEF
 * stands the threat's ATK, or the one that beats another face-up monster of
 * the player nothing else of ours handles. */
static unsigned field_keepers(const HmBoard *b, const HmOptions *o, const HmRules *r,
                              int threat, unsigned keep)
{
    unsigned hold = 0;
    int i, j, k, wall = -1;
    for (i = 0; i < b->hand_count; i++) {
        HmCard h = b->hand[i];
        if ((keep & (1u << i)) || h.id <= 0 || !monster(h)) continue;
        if (h.defense >= b->enemy[threat].attack && (wall < 0 || h.defense > b->hand[wall].defense)) wall = i;
    }
    if (wall >= 0) hold |= 1u << wall;
    for (j = 0; j < 5; j++) {
        HmCard e = b->enemy[j];
        int stat = (e.flags & HM_DEFENSE) ? e.defense : e.attack, handled = 0, best = -1;
        if (j == threat || e.id <= 0) continue;
        for (k = 0; k < 5; k++) handled |= monster(b->own[k]) && b->own[k].attack > stat;
        for (k = 0; k < b->hand_count; k++) handled |= answers(b, o, r, b->hand[k], e, stat);
        for (k = 0; k < 5; k++) handled |= answers(b, o, r, b->back[k], e, stat);
        if (handled) continue;
        for (i = 0; i < b->hand_count; i++)
            if (!(keep & (1u << i)) && power(b, o, r, b->hand[i], 0) > stat &&
                (best < 0 || power(b, o, r, b->hand[i], 0) > power(b, o, r, b->hand[best], 0))) best = i;
        if (best >= 0) hold |= 1u << best;
    }
    return hold;
}

static HmDecision expert(const HmBoard *b, const HmOptions *o, const HmRules *r,
                         HmDecision best, int *reach)
{
    HmDecision base = best, spare, plain;
    unsigned keep = 0, hold = 0, junk = 0, all = (1u << b->hand_count) - 1, loose;
    int i, j, need = 0, threat = -1, n = 0, outs_left = 0;
    Hm_Why = HM_WHY_SHORT;
    if (!b->deck_count || b->hand_count > 5) return best;
    for (i = 0; i < 5; i++) if (b->enemy[i].id > 0)
        for (j = 0; j < 5; j++) if (monster(b->own[j]))
            reach[i] = max(reach[i], b->own[j].attack + edge(b->own[j], b->enemy[i], 1));
    for (i = 0; i < 5; i++) Hm_Reach[i] = reach[i];
    /* A face-up monster no play of this turn beats in battle (its guardian
     * star counts, a tie is no answer: the game trades both), and nothing
     * destroys; one in defense is harmless while our best tops its ATK. */
    for (i = 0; i < 5; i++) {
        HmCard e = b->enemy[i];
        int stat = (e.flags & HM_DEFENSE) ? e.defense : e.attack, answered = 0;
        if (e.id <= 0 || stat < reach[i] || ((e.flags & HM_DEFENSE) && e.attack <= reach[i])) continue;
        for (j = 0; j < b->hand_count; j++) answered |= answers(b, o, r, b->hand[j], e, stat);
        for (j = 0; j < 5; j++) answered |= answers(b, o, r, b->back[j], e, stat);
        if (!answered && (threat < 0 || e.attack > b->enemy[threat].attack)) { threat = i; need = stat; }
    }
    Hm_Threat = threat; Hm_Need = need;
    if (threat >= 0) {
        outs_left = Hm_Outs = outs(b, o, r, b->enemy[threat], need, &keep);
        Hm_Keep = keep; Hm_Why = HM_WHY_NO_OUTS;
        if (!outs_left) return best; /* nothing to dig for: hold the line */
        for (i = 0; i < b->hand_count; i++) if (useful_spell(b->hand[i])) keep |= 1u << i;
        hold = field_keepers(b, o, r, threat, keep);
        spare = plan_hand(b, o, r, keep, 0);
        Hm_Keep = keep; Hm_Hold = hold;
        Hm_Why = HM_WHY_NOT_FROM_HAND;
        if (from_hand(b, &best) && !(spent(&best) & keep)) base = best;
        else if (!from_hand(b, &best) && (spent(&best) & keep)) return best; /* Swords, a trap... */
        else if (from_hand(b, &spare) && spare.score >= best.score - 1500) base = spare;
        else if (from_hand(b, &best)) base = best;
        else return best;
        junk = all & ~keep & ~hold & ~spent(&base);
    } else {
        Hm_Why = HM_WHY_NO_THREAT;
        if (stalling(b) || !from_hand(b, &best)) return best;
        for (i = 0; i < b->hand_count; i++)
            if (!(spent(&best) & (1u << i)) && monster(b->hand[i]) && potential(b, o, r, i) < 1300)
                junk |= 1u << i;
    }
    plain = base;
    n = throw_most(b, r, &base, junk, threat);
    /* fork: a fusion or an equip with the card played can keep the junk in
     * hand; facing the threat, another card of no use blocks in its place
     * when more cards go that way (never instead of a wall). */
    loose = (junk | spent(&plain)) & ~keep & ~hold;
    if (threat >= 0 && n < bits(loose) - 1 && !(spent(&plain) & hold))
        for (i = 0; i < b->hand_count; i++) if ((loose & (1u << i)) && monster(b->hand[i])) {
            HmDecision alt = plan_hand(b, o, r, all & ~(1u << i), 0);
            if (!from_hand(b, &alt) || alt.score < plain.score - 1500) continue;
            if ((j = throw_most(b, r, &alt, loose & ~(1u << i), threat)) > n) { base = alt; n = j; }
        }
    Hm_Discards = n; Hm_Outs = outs_left; Hm_Junk = junk;
    Hm_Why = n ? HM_WHY_DIGS : threat >= 0 ? HM_WHY_NO_ROOM : HM_WHY_NO_THREAT;
    return base;
}

HmDecision Hm_PlanHand(const HmBoard *b, const HmOptions *o, const HmRules *r)
{
    HmOptions slow = *o;
    int reach[5] = {0}, i;
    Hm_Discards = Hm_Outs = Hm_Need = 0; Hm_Threat = -1;
    for (i = 0; i < 5; i++) Hm_Reach[i] = 0;
    Hm_Keep = Hm_Hold = Hm_Junk = 0; Hm_Why = HM_WHY_OFF;
    if (!o->expert) return plan_hand(b, o, r, 0, 0);
    /* Ahead in the deck race, or short of cards: one card a turn where it can. */
    if (stalling(b) || (b->deck_count && b->deck_count <= 8)) slow.material_cost += 400;
    return expert(b, &slow, r, plan_hand(b, &slow, r, 0, reach), reach);
}
