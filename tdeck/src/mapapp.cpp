/**
 * Offline map app: renders pre-downloaded OSM raster tiles from the SD card
 * around the current GPS position, with pan/zoom.
 *
 * SD card layout (created by tools/download_tiles.py):
 *   /map/z<zoom>/<x>/<y>.bin   - raw RGB565 tile, 256x256 px (128KB each)
 *
 * Controls:
 *   trackball up/down/left/right - pan
 *   click - zoom in at center    long-click - back to menu
 *   keyboard +/- - zoom in/out
 */
#include <Arduino.h>
#include <SD.h>
#include <Arduino_GFX_Library.h>
#include <TinyGPS++.h>
#include "mapapp.h"
#include "utilities.h"

#define TILE_LINE_BYTES (TILE_PX * 2)

// Simple linear tile buffer in PSRAM; one line at a time to save RAM.
static uint16_t tileLine[TILE_PX];

// Decode "slippy map" tile numbers from lat/lon (Web Mercator)
uint32_t lonToTileX(double lon, int zoom) {
  return (uint32_t)((lon + 180.0) / 360.0 * (1 << zoom));
}
uint32_t latToTileY(double lat, int zoom) {
  double latR = lat * M_PI / 180.0;
  return (uint32_t)((1.0 - log(tan(latR) + 1.0 / cos(latR)) / M_PI) / 2.0 * (1 << zoom));
}
double tileXToLon(uint32_t x, int zoom) {
  return x / (double)(1 << zoom) * 360.0 - 180.0;
}
double tileYToLat(uint32_t y, int zoom) {
  double n = M_PI - 2.0 * M_PI * y / (double)(1 << zoom);
  return 180.0 / M_PI * atan(0.5 * (exp(n) - exp(-n)));
}

// Draw a 256x256 RGB565 tile from SD at screen position (sx, sy), clipped to the screen.
static void drawTile(Arduino_GFX *display, uint32_t tx, uint32_t ty, int zoom, int sx, int sy, int screenW, int screenH) {
  if (tx >= (uint32_t)(1 << zoom) || ty >= (uint32_t)(1 << zoom)) return;
  String path = String(MAP_TILE_DIR) + "/z" + String(zoom) + "/" + String(tx) + "/" + String(ty) + ".bin";
  if (!SD.exists(path))
    path = String(MAP_TILE_DIR) + "/" + String(zoom) + "/" + String(tx) + "/" + String(ty) + ".bin";
  if (!SD.exists(path)) {
    int w = TILE_PX, h = TILE_PX;
    if (sx + w > screenW) w = screenW - sx;
    if (sy + h > screenH) h = screenH - sy;
    if (sx < 0) { w += sx; sx = 0; }
    if (sy < 0) { h += sy; sy = 0; }
    if (w > 0 && h > 0) display->fillRect(sx, sy, w, h, RGB565(20, 20, 30));
    return;
  }
  File f = SD.open(path, FILE_READ);
  if (!f) {
    // Missing tile: draw a dark placeholder
    int w = TILE_PX, h = TILE_PX;
    if (sx + w > screenW) w = screenW - sx;
    if (sy + h > screenH) h = screenH - sy;
    if (sx < 0) { w += sx; sx = 0; }
    if (sy < 0) { h += sy; sy = 0; }
    if (w > 0 && h > 0) display->fillRect(sx, sy, w, h, RGB565(20, 20, 30));
    return;
  }
  for (int row = 0; row < TILE_PX; row++) {
    int dy = sy + row;
    if (dy < 0) { f.seek(row * TILE_LINE_BYTES); continue; }
    if (dy >= screenH) break;
    if (f.read((uint8_t *)tileLine, TILE_LINE_BYTES) != TILE_LINE_BYTES) break;
    int dxStart = (sx < 0) ? -sx : 0;
    int dxEnd = (sx + TILE_PX > screenW) ? (screenW - sx) : TILE_PX;
    display->draw16bitRGBBitmap(sx + dxStart, dy, tileLine + dxStart, dxEnd - dxStart, 1);
  }
  f.close();
}

void mapAppRender(Arduino_GFX *display, TinyGPSPlus *gps, int centerPixelX, int centerPixelY, int zoom, int screenW, int screenH) {
  // Center pixel in global tile-pixel space:
  // globalX = (1<<zoom)*256 scaled position
  int tilesAcross = 1 << zoom;
  (void)tilesAcross;
  int tileX = centerPixelX / TILE_PX;
  int tileY = centerPixelY / TILE_PX;
  int originX = tileX * TILE_PX;
  int originY = tileY * TILE_PX;
  int sx0 = originX - centerPixelX + screenW / 2;
  int sy0 = originY - centerPixelY + screenH / 2;
  // Draw the visible 3x3 (max) neighborhood of tiles
  for (int ty = tileY - 1; ty <= tileY + 2; ty++) {
    for (int tx = tileX - 1; tx <= tileX + 2; tx++) {
      int sx = sx0 + (tx - tileX) * TILE_PX;
      int sy = sy0 + (ty - tileY) * TILE_PX;
      if (sx >= screenW || sy >= screenH || sx + TILE_PX <= 0 || sy + TILE_PX <= 0) continue;
      drawTile(display, (uint32_t)tx, (uint32_t)ty, zoom, sx, sy, screenW, screenH);
    }
  }
}
