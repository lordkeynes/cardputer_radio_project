#pragma once

// Game indexes for stats (order matches games hub)
#define GS_CHESS     0
#define GS_GO        1
#define GS_SOLITAIRE 2
#define GS_CHECKERS  3
#define GS_SNAKE     4
#define GS_N         5

void gsRecordResult(int game, bool win);
void gsRecordScore(int game, int score);
int  gsGetSnakeHi();
void gsStatsScreen();
