#pragma once
#include <Arduino_GFX_Library.h>

// Shared event codes (must match AppEvent values in main.cpp exactly)
#define PDA_EV_UP 1
#define PDA_EV_DOWN 2
#define PDA_EV_LEFT 3
#define PDA_EV_RIGHT 4
#define PDA_EV_SELECT 5
#define PDA_EV_LONGSELECT 6
#define PDA_EV_NEWLINE 7
#define PDA_EV_BACK 8
#define PDA_EV_SPACE 9
#define PDA_EV_CHAR 10
#define PDA_EV_DELETE 11

struct InputEventP {
  uint8_t ev;
  char ch;
  unsigned long ts;
};

#define SCREEN_W 320
#define SCREEN_H 240
#define SCREEN_SLEEP_MS 45000UL

// Implemented in main.cpp (wraps the shared input queue)
bool pdaGetInput(InputEventP &e, uint32_t waitMs);

// Battery
int batteryPercent();
int batteryMillivolts();

// Screen sleep
void pdaNoteActivity();
bool pdaScreenTick();
bool pdaScreenAwake();
void pdaWake();

// Time
void pdaApplyGpsTime(int y, int m, int d, int hh, int mm, int ss);
bool pdaTimeSynced();

// Apps
void clockApp();
void calendarApp();
void todoApp();

// Shared to-do task storage (used by todoApp and calendarApp)
// File format per line: "[ ] task text" or "[x] task text", optionally
// followed by " @YYYY-MM-DD" for a due date.
#define TODO_MAX_TASKS 32
#define TODO_MAX_LEN 60
struct TodoTask {
  char text[TODO_MAX_LEN];
  bool done;
  int dueYear, dueMonth, dueDay;  // 0 = no date
};
// Returns task count; fills the shared task array.
int todoLoadTasks(TodoTask *tasks, int maxN);
bool todoSaveTasks(TodoTask *tasks, int n);
int todoCountForDate(int y, int m, int d);
