#ifndef MEMORIES_DECOMP_DUEL_REWARDS_H
#define MEMORIES_DECOMP_DUEL_REWARDS_H

#include "../types.h"
#include "card_constants.h"

/* One weighted card-drop row: CARD_COUNT halfword weights followed by the
 * established padding that makes each pool row 1,460 bytes. */
typedef struct {
    u16 weights[CARD_COUNT];
    u8 pad_05A4[DUEL_DROP_TABLE_PADDING_SIZE];
} DuelDropTable;

extern DuelDropTable gDuel_awSaPowCardDrops[];

void Duel_ShowResultPage(s32 page);
void Duel_CalcRankScore(void);
s32 Duel_SelectCardDrop(s32 pool_index);
void Duel_AwardCard(s32 card_id);
#ifdef MEMORIES_PC
/* 1 when a mod's "chest_overflow" leaves no room for another copy
   (Tables_ChestFull); always 0 without one. */
s32 Duel_ChestFull(s32 card_id);
/* 1 when a mod's "password_copies" is set and the player already owns that
   many copies, chest and deck together; always 0 without one. */
s32 Duel_PasswordCopiesFull(s32 card_id);
/* Take away the "xN" beside a starchip prize past the row's eight
   (pc/cards/starchip_prize.h), when the box is there. */
void Duel_HideStarchipCount(void);
#endif

#endif
