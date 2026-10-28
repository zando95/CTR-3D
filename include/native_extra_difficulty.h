#ifndef NATIVE_EXTRA_DIFFICULTY_H
#define NATIVE_EXTRA_DIFFICULTY_H

/* SUPER HARD and ULTRA HARD, two Arcade difficulties above HARD (3DS only).
 *
 * They are rows below HARD in the DIFFICULTY box, hidden until earned: SUPER HARD once all four Arcade
 * cups are won on HARD, ULTRA HARD once all four are won on SUPER HARD. That is retail's own cup ladder
 * (MEDIUM and HARD cups stay locked until the four cups below are won) carried two tiers on, so a tier is
 * unlocked exactly when the four cup-win bits of the tier below are set; there is no flag of its own. The
 * box is the one single races, cups and CTR CHALLENGE all pass through, so all three offer the new rows.
 * The fourth SUPER HARD cup also unlocks Penta Penguin (UNLOCK_PENTA), retail's one cheat-only racer;
 * that is a real unlock bit, set at that win only.
 *
 * gGT->arcadeDifficulty carries retail's 0x50 step on: EASY 0x50, MEDIUM 0xA0, HARD 0xF0,
 * NATIVE_XDIFF_SUPER_HARD 0x140 (also the SUPER HARD cheat's value), NATIVE_XDIFF_ULTRA_HARD 0x190.
 * BOTS_Adv_AdjustDifficulty blends each track's AI settings past HARD linearly.
 *
 * Cup wins on the new tiers are card-wide gameProgress.unlocks bits laid out like retail's: four win bits
 * per tier from GAME_UNLOCK_BIT_EXTRA_CUP_WIN_FIRST, and their saved copies from
 * GAME_UNLOCK_BIT_EXTRA_CUP_SAVED_FIRST, which decide whether the podium asks to save. The card merge
 * (GAMEPROG_SyncGameAndCard) ORs whole words, so they persist in the retail save format.
 *
 * Once SUPER HARD is unlocked the box draws its rows in the small font under a big title (five big rows
 * run off the bottom of the 216-line screen), and cup select draws five stars per cup, closer together.
 * Before that, both are retail's.
 *
 * On PC every hook is a passthrough and nothing sets the new values. Defined in
 * platform/native_extra_difficulty.c. */

#define NATIVE_XDIFF_SUPER_HARD 0x140
#define NATIVE_XDIFF_ULTRA_HARD 0x190

/* MM_MenuProc_Main, after MM_ToggleRows_Difficulty: points the box at retail's rows or the extended set,
 * and sets its font to match. */
void NativeXDiff_ServeDifficultyBox(struct RectMenu *menu);

/* MM_MenuProc_Difficulty: takes a SUPER HARD or ULTRA HARD row; 0 for every retail row. */
int NativeXDiff_HandleDifficultyRow(struct RectMenu *menu, s16 row);

/* UI_CupStandings, for an Arcade cup the player won: credits a SUPER HARD or ULTRA HARD win and returns 1;
 * 0 leaves the win to retail. */
int NativeXDiff_CreditCupWin(int arcadeDifficulty, int cupID);

/* UI_CupStandings: retail has just completed the four cups of `difficulty` (0 EASY, 1 MEDIUM, 2 HARD). */
void NativeXDiff_OnTierComplete(int difficulty);

/* CS_Camera: the podium's save prompt, extended with the unlock lines when this cup unlocked a tier. */
s16 NativeXDiff_CupProgressString(s16 stringIndex);

/* GAMEPROG_SaveCupProgress: marks the new tiers' cup wins as saved. */
void NativeXDiff_SaveCupProgress(u32 *unlocks);

/* MM_CupSelect: once SUPER HARD is unlocked, draws all five of a cup's stars and returns 1; 0 leaves
 * retail's three. starX is the column retail draws the stars in, startY the cup's content top. */
int NativeXDiff_DrawCupStars(int cupIndex, int starX, int startY);

/* LOAD_LangFile: rebuilds the row labels from the loaded language. */
void NativeXDiff_OnLanguage(int langFileIndex);

#endif
