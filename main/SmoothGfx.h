#ifndef SMOOTH_GFX_H
#define SMOOTH_GFX_H

#include <stdint.h>
#include <LovyanGFX.hpp>

//
// Anti-aliased arc and rounded rectangle outline.
// Ported from the TFT_eSPI smooth graphics functions, which LovyanGFX does not have.
//
// Colors are RGB565, the anti-aliased edges are blended with bg.
//

// Draw an arc clockwise from the 6 o'clock position, angles are in the range 0-360.
// Centered at x,y, r is the outer radius and ir is the inner radius, inclusive,
// so the arc thickness is r - ir + 1.
void drawSmoothArc(lgfx::LGFXBase &gfx, int32_t x, int32_t y, int32_t r, int32_t ir,
                   uint32_t startAngle, uint32_t endAngle, uint16_t fg, uint16_t bg);

// Draw a rounded rectangle outline. x,y is the top left corner of the bounding box,
// r is the outer corner radius and ir is the inner corner radius, w and h are the
// width and height of the bounding box.
void drawSmoothRoundRect(lgfx::LGFXBase &gfx, int32_t x, int32_t y, int32_t r, int32_t ir,
                         int32_t w, int32_t h, uint16_t fg, uint16_t bg);

#endif // SMOOTH_GFX_H
