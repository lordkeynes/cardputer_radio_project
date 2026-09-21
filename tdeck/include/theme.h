#pragma once
#include <Arduino_GFX_Library.h>

// Runtime-switchable terminal themes. Colors are stored in PROGMEM-style
// struct and loaded into globals at startup / when the user picks a theme
// in the Settings app. The theme index persists in /config/theme.txt on SD.

struct Theme {
  const char *name;
  uint16_t fg;       // primary text / lines
  uint16_t bright;   // highlighted text
  uint16_t dim;      // secondary text
  uint16_t selBg;    // selection highlight background
  uint16_t accent;   // attention color
  uint16_t red;      // errors / low battery
  uint16_t cyan;     // secondary accent
};

extern uint16_t TERM_GREEN, TERM_BRIGHT, TERM_DIM, TERM_SEL_BG,
                TERM_ACCENT, TERM_RED, TERM_CYAN;

void themeInit();                 // load saved theme (or default) from SD
void themeSet(int idx);           // apply theme idx immediately
int  themeCount();
const char *themeName(int idx);   // name for the settings list
int  themeIndex();                // current index

// Legacy #defines kept so existing code compiles unchanged; they now alias
// the runtime globals. (Define TERM_GREEN as the global variable.)
