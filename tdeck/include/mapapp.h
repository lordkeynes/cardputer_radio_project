#pragma once

#define MAP_TILE_DIR "/map"
#define TILE_PX 256
#include <Arduino_GFX_Library.h>
#include <TinyGPS++.h>

uint32_t lonToTileX(double lon, int zoom);
uint32_t latToTileY(double lat, int zoom);
double tileXToLon(uint32_t x, int zoom);
double tileYToLat(uint32_t y, int zoom);
void mapAppRender(Arduino_GFX *display, TinyGPSPlus *gps, int centerPixelX, int centerPixelY, int zoom, int screenW, int screenH);
