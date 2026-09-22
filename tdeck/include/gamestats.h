#pragma once
// Game indexes (order matches the games hub grid)
#define GS_CHESS     0
#define GS_GO        1
#define GS_SOLITAIRE 2
#define GS_CHECKERS   3
#define GS_SNAKE      4
#define GS_FLAPPY     5
#define GS_TETRIS     6
#define GS_BREAKOUT   7
#define GS_2048       8
#define GS_MINES      9
#define GS_PONG      10
#define GS_REVERSI   11
#define GS_CONNECT4  12
#define GS_BATTLESHIP 13
#define GS_WORDLE    14
#define GS_SUDOKU    15
#define GS_SOKOBAN   16
#define GS_INVADERS  17
#define GS_ASTEROIDS 18
#define GS_DOODLE    19
#define GS_HEARTS    20
#define GS_SPADES    21
#define GS_BACKGAMMON 22
#define GS_N         23

void gsRecordResult(int game, bool win);   // W/L tally (versus games)
void gsRecordScore(int game, int score);  // persistent high score
int  gsGetHi(int game);                    // high score for a game
int  gsGetSnakeHi();                       // legacy helper
int  gsGetFlappyHi();                      // legacy helper
void gsStatsScreen();
