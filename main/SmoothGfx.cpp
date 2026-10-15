//
// Anti-aliased arc and rounded rectangle drawing, ported from TFT_eSPI
// (https://github.com/Bodmer/TFT_eSPI, smooth graphics by Bodmer, FreeBSD license).
//
#include <math.h>
#include "SmoothGfx.h"

namespace
{

constexpr float PixelAlphaGain  = 255.0f;
constexpr float LoAlphaThreshold = 1.0f / 32.0f;
constexpr float HiAlphaThreshold = 1.0f - LoAlphaThreshold;
constexpr float deg2rad          = 3.14159265359f / 180.0f;

// Blend two RGB565 colors, alpha 0 is bg and 255 is fg
uint16_t fastBlend(uint8_t alpha, uint16_t fgc, uint16_t bgc)
{
  // Split out and blend 5-bit red and blue channels
  uint32_t rxb = bgc & 0xF81F;
  rxb += ((fgc & 0xF81F) - rxb) * (alpha >> 2) >> 6;
  // Split out and blend 6-bit green channel
  uint32_t xgx = bgc & 0x07E0;
  xgx += ((fgc & 0x07E0) - xgx) * alpha >> 8;
  // Recombine channels
  return (rxb & 0xF81F) | (xgx & 0x07E0);
}

// Compute the fixed point square root of an integer and return the 8 MS bits
// of the fractional part
inline uint8_t sqrtFraction(uint32_t num)
{
  if(num > 0x40000000) return 0;

  uint32_t bsh = 0x00004000;
  uint32_t fpr = 0;
  uint32_t osh = 0;

  // Auto adjust from U8:8 up to U15:16
  while(num > bsh) { bsh <<= 2; osh++; }

  do
  {
    uint32_t bod = bsh + fpr;
    if(num >= bod)
    {
      num -= bod;
      fpr = bsh + bod;
    }
    num <<= 1;
  } while(bsh >>= 1);

  return fpr >> osh;
}

// Distance of px,py to the closest part of the a to b wedge
inline float wedgeLineDistance(float xpax, float ypay, float bax, float bay, float dr)
{
  float h = fmaxf(fminf((xpax * bax + ypay * bay) / (bax * bax + bay * bay), 1.0f), 0.0f);
  float dx = xpax - bax * h, dy = ypay - bay * h;
  return sqrtf(dx * dx + dy * dy) + h * dr;
}

// Draw an anti-aliased line with different width radiused ends
void drawWedgeLine(lgfx::LGFXBase &gfx, float ax, float ay, float bx, float by,
                   float ar, float br, uint16_t fg, uint16_t bg)
{
  if((ar < 0.0f) || (br < 0.0f)) return;
  if((fabsf(ax - bx) < 0.01f) && (fabsf(ay - by) < 0.01f)) bx += 0.01f;  // Avoid divide by zero

  // Find line bounding box and clip it to the drawing area
  int32_t x0 = (int32_t)floorf(fminf(ax - ar, bx - br));
  int32_t x1 = (int32_t) ceilf(fmaxf(ax + ar, bx + br));
  int32_t y0 = (int32_t)floorf(fminf(ay - ar, by - br));
  int32_t y1 = (int32_t) ceilf(fmaxf(ay + ar, by + br));
  if(x0 < 0) x0 = 0;
  if(y0 < 0) y0 = 0;
  if(x1 >= gfx.width())  x1 = gfx.width() - 1;
  if(y1 >= gfx.height()) y1 = gfx.height() - 1;
  if(x0 > x1 || y0 > y1) return;

  // Establish x start and y start
  int32_t ys = ay;
  if((ax - ar) > (bx - br)) ys = by;

  float rdt = ar - br; // Radius delta
  float alpha = 1.0f;
  ar += 0.5f;

  float xpax, ypay, bax = bx - ax, bay = by - ay;

  int32_t xs = x0;
  // Scan bounding box from ys down, calculate pixel intensity from distance to line
  for(int32_t yp = ys; yp <= y1; yp++)
  {
    bool endX = false; // Flag to skip pixels
    ypay = yp - ay;
    for(int32_t xp = xs; xp <= x1; xp++)
    {
      if(endX && alpha <= LoAlphaThreshold) break;  // Skip right side
      xpax = xp - ax;
      alpha = ar - wedgeLineDistance(xpax, ypay, bax, bay, rdt);
      if(alpha <= LoAlphaThreshold) continue;
      // Track edge to minimise calculations
      if(!endX) { endX = true; xs = xp; }
      if(alpha > HiAlphaThreshold)
        gfx.drawPixel(xp, yp, fg);
      else
        gfx.drawPixel(xp, yp, fastBlend((uint8_t)(alpha * PixelAlphaGain), fg, bg));
    }
  }

  // Reset x start to left side of box
  xs = x0;
  // Scan bounding box from ys-1 up, calculate pixel intensity from distance to line
  for(int32_t yp = ys - 1; yp >= y0; yp--)
  {
    bool endX = false; // Flag to skip pixels
    ypay = yp - ay;
    for(int32_t xp = xs; xp <= x1; xp++)
    {
      if(endX && alpha <= LoAlphaThreshold) break;  // Skip right side of drawn line
      xpax = xp - ax;
      alpha = ar - wedgeLineDistance(xpax, ypay, bax, bay, rdt);
      if(alpha <= LoAlphaThreshold) continue;
      // Track line boundary
      if(!endX) { endX = true; xs = xp; }
      if(alpha > HiAlphaThreshold)
        gfx.drawPixel(xp, yp, fg);
      else
        gfx.drawPixel(xp, yp, fastBlend((uint8_t)(alpha * PixelAlphaGain), fg, bg));
    }
  }
}

// Draw an arc clockwise from 6 o'clock, the ends of the arc are not anti-aliased
void drawArc(lgfx::LGFXBase &gfx, int32_t x, int32_t y, int32_t r, int32_t ir,
             uint32_t startAngle, uint32_t endAngle, uint16_t fg, uint16_t bg)
{
  if(endAngle   > 360) endAngle   = 360;
  if(startAngle > 360) startAngle = 360;
  if(startAngle == endAngle) return;
  if(r < ir) { int32_t t = r; r = ir; ir = t; }  // Required that r > ir
  if(r <= 0 || ir < 0) return;                   // Invalid r, ir can be zero (circle sector)

  if(endAngle < startAngle)
  {
    // Arc sweeps through 6 o'clock so draw in two parts
    if(startAngle < 360) drawArc(gfx, x, y, r, ir, startAngle, 360, fg, bg);
    if(endAngle == 0) return;
    startAngle = 0;
  }

  int32_t xs = 0;        // x start position for quadrant scan
  uint8_t alpha = 0;     // alpha value for blending pixels

  uint32_t r2 = r * r;   // Outer arc radius^2
  r++;                   // Outer AA zone radius
  uint32_t r1 = r * r;   // Outer AA radius^2
  int16_t w  = r - ir;   // Width of arc (r - ir + 1)
  uint32_t r3 = ir * ir; // Inner arc radius^2
  ir--;                  // Inner AA zone radius
  uint32_t r4 = ir * ir; // Inner AA radius^2

  //     1 | 2
  //    ---¦---    Arc quadrant index
  //     0 | 3
  // Fixed point U16.16 slope table for arc start/end in each quadrant
  uint32_t startSlope[4] = {0, 0, 0xFFFFFFFF, 0};
  uint32_t   endSlope[4] = {0, 0xFFFFFFFF, 0, 0};

  // Ensure maximum U16.16 slope of arc ends is ~ 0x8000 0000
  constexpr float minDivisor = 1.0f / 0x8000;

  // Fill in start slope table and empty quadrants
  float fabscos = fabsf(cosf(startAngle * deg2rad));
  float fabssin = fabsf(sinf(startAngle * deg2rad));

  // U16.16 slope of arc start
  uint32_t slope = (fabscos / (fabssin + minDivisor)) * (float)(1UL << 16);

  // Update slope table, add slope for arc start
  if(startAngle <= 90)
  {
    startSlope[0] = slope;
  }
  else if(startAngle <= 180)
  {
    startSlope[1] = slope;
  }
  else if(startAngle <= 270)
  {
    startSlope[1] = 0xFFFFFFFF;
    startSlope[2] = slope;
  }
  else
  {
    startSlope[1] = 0xFFFFFFFF;
    startSlope[2] = 0;
    startSlope[3] = slope;
  }

  // Fill in end slope table and empty quadrants
  fabscos = fabsf(cosf(endAngle * deg2rad));
  fabssin = fabsf(sinf(endAngle * deg2rad));

  // U16.16 slope of arc end
  slope = (uint32_t)((fabscos / (fabssin + minDivisor)) * (float)(1UL << 16));

  // Work out which quadrants will need to be drawn and add slope for arc end
  if(endAngle <= 90)
  {
    endSlope[0] = slope;
    endSlope[1] = 0;
    startSlope[2] = 0;
  }
  else if(endAngle <= 180)
  {
    endSlope[1] = slope;
    startSlope[2] = 0;
  }
  else if(endAngle <= 270)
  {
    endSlope[2] = slope;
  }
  else
  {
    endSlope[3] = slope;
  }

  // Scan quadrant
  for(int32_t cy = r - 1; cy > 0; cy--)
  {
    uint32_t len[4] = { 0,  0,  0,  0}; // Pixel run length
    int32_t  xst[4] = {-1, -1, -1, -1}; // Pixel run x start
    uint32_t dy2 = (r - cy) * (r - cy);

    // Find and track arc zone start point
    while((uint32_t)((r - xs) * (r - xs)) + dy2 >= r1) xs++;

    for(int32_t cx = xs; cx < r; cx++)
    {
      // Calculate radius^2
      uint32_t hyp = (r - cx) * (r - cx) + dy2;

      // If in outer zone calculate alpha
      if(hyp > r2)
      {
        alpha = ~sqrtFraction(hyp); // Outer AA zone
      }
      // If within arc fill zone, get line start and lengths for each quadrant
      else if(hyp >= r3)
      {
        // Calculate U16.16 slope
        slope = ((r - cy) << 16) / (r - cx);
        if(slope <= startSlope[0] && slope >= endSlope[0]) // slope hi -> lo
        {
          xst[0] = cx; // Bottom left line end
          len[0]++;
        }
        if(slope >= startSlope[1] && slope <= endSlope[1]) // slope lo -> hi
        {
          xst[1] = cx; // Top left line end
          len[1]++;
        }
        if(slope <= startSlope[2] && slope >= endSlope[2]) // slope hi -> lo
        {
          xst[2] = cx; // Bottom right line start
          len[2]++;
        }
        if(slope <= endSlope[3] && slope >= startSlope[3]) // slope lo -> hi
        {
          xst[3] = cx; // Top right line start
          len[3]++;
        }
        continue; // Next x
      }
      else
      {
        if(hyp <= r4) break;  // Skip inner pixels
        alpha = sqrtFraction(hyp); // Inner AA zone
      }

      if(alpha < 16) continue;  // Skip low alpha pixels

      uint16_t pcol = fastBlend(alpha, fg, bg);
      // Check if an AA pixels need to be drawn
      slope = ((r - cy) << 16) / (r - cx);
      if(slope <= startSlope[0] && slope >= endSlope[0]) // BL
        gfx.drawPixel(x + cx - r, y - cy + r, pcol);
      if(slope >= startSlope[1] && slope <= endSlope[1]) // TL
        gfx.drawPixel(x + cx - r, y + cy - r, pcol);
      if(slope <= startSlope[2] && slope >= endSlope[2]) // TR
        gfx.drawPixel(x - cx + r, y + cy - r, pcol);
      if(slope <= endSlope[3] && slope >= startSlope[3]) // BR
        gfx.drawPixel(x - cx + r, y - cy + r, pcol);
    }
    // Add line in inner zone
    if(len[0]) gfx.drawFastHLine(x + xst[0] - len[0] + 1 - r, y - cy + r, len[0], fg); // BL
    if(len[1]) gfx.drawFastHLine(x + xst[1] - len[1] + 1 - r, y + cy - r, len[1], fg); // TL
    if(len[2]) gfx.drawFastHLine(x - xst[2] + r, y + cy - r, len[2], fg); // TR
    if(len[3]) gfx.drawFastHLine(x - xst[3] + r, y - cy + r, len[3], fg); // BR
  }

  // Fill in centre lines
  if(startAngle ==   0 || endAngle == 360) gfx.drawFastVLine(x, y + r - w, w, fg); // Bottom
  if(startAngle <=  90 && endAngle >=  90) gfx.drawFastHLine(x - r + 1, y, w, fg); // Left
  if(startAngle <= 180 && endAngle >= 180) gfx.drawFastVLine(x, y - r + 1, w, fg); // Top
  if(startAngle <= 270 && endAngle >= 270) gfx.drawFastHLine(x + r - w, y, w, fg); // Right
}

} // namespace

void drawSmoothArc(lgfx::LGFXBase &gfx, int32_t x, int32_t y, int32_t r, int32_t ir,
                   uint32_t startAngle, uint32_t endAngle, uint16_t fg, uint16_t bg)
{
  if(endAngle != startAngle && (startAngle != 0 || endAngle != 360))
  {
    float sx = -sinf(startAngle * deg2rad);
    float sy = +cosf(startAngle * deg2rad);
    float ex = -sinf(  endAngle * deg2rad);
    float ey = +cosf(  endAngle * deg2rad);

    // Square ends
    drawWedgeLine(gfx, sx * ir + x, sy * ir + y, sx * r + x, sy * r + y, 0.3f, 0.3f, fg, bg);
    drawWedgeLine(gfx, ex * ir + x, ey * ir + y, ex * r + x, ey * r + y, 0.3f, 0.3f, fg, bg);

    // Draw arc
    drawArc(gfx, x, y, r, ir, startAngle, endAngle, fg, bg);
  }
  else
  {
    // Draw full 360
    drawArc(gfx, x, y, r, ir, 0, 360, fg, bg);
  }
}

void drawSmoothRoundRect(lgfx::LGFXBase &gfx, int32_t x, int32_t y, int32_t r, int32_t ir,
                         int32_t w, int32_t h, uint16_t fg, uint16_t bg)
{
  if(r < ir) { int32_t t = r; r = ir; ir = t; } // Required that r > ir
  if(r <= 0 || ir < 0) return;                  // Invalid

  w -= 2 * r;
  h -= 2 * r;

  if(w < 0) w = 0;
  if(h < 0) h = 0;

  x += r;
  y += r;

  uint16_t t = r - ir + 1;
  int32_t xs = 0;
  int32_t cx = 0;

  int32_t r2 = r * r;   // Outer arc radius^2
  r++;
  int32_t r1 = r * r;   // Outer AA zone radius^2

  int32_t r3 = ir * ir; // Inner arc radius^2
  ir--;
  int32_t r4 = ir * ir; // Inner AA zone radius^2

  uint8_t alpha = 0;

  // Scan top left quadrant
  for(int32_t cy = r - 1; cy > 0; cy--)
  {
    int32_t len = 0;  // Pixel run length
    int32_t lxst = 0; // Left side run x start
    int32_t rxst = 0; // Right side run x start
    int32_t dy2 = (r - cy) * (r - cy);

    // Find and track arc zone start point
    while((r - xs) * (r - xs) + dy2 >= r1) xs++;

    for(cx = xs; cx < r; cx++)
    {
      // Calculate radius^2
      int32_t hyp = (r - cx) * (r - cx) + dy2;

      // If in outer zone calculate alpha
      if(hyp > r2)
      {
        alpha = ~sqrtFraction(hyp); // Outer AA zone
      }
      // If within arc fill zone, get line lengths for each quadrant
      else if(hyp >= r3)
      {
        rxst = cx; // Right side start
        len++;     // Line segment length
        continue;  // Next x
      }
      else
      {
        if(hyp <= r4) break;  // Skip inner pixels
        alpha = sqrtFraction(hyp); // Inner AA zone
      }

      if(alpha < 16) continue;  // Skip low alpha pixels

      uint16_t pcol = fastBlend(alpha, fg, bg);
      gfx.drawPixel(x + cx - r, y - cy + r + h, pcol);     // BL
      gfx.drawPixel(x + cx - r, y + cy - r, pcol);         // TL
      gfx.drawPixel(x - cx + r + w, y + cy - r, pcol);     // TR
      gfx.drawPixel(x - cx + r + w, y - cy + r + h, pcol); // BR
    }
    // Fill arc inner zone in each quadrant
    lxst = rxst - len + 1; // Calculate line segment start for left side
    gfx.drawFastHLine(x + lxst - r, y - cy + r + h, len, fg);     // BL
    gfx.drawFastHLine(x + lxst - r, y + cy - r, len, fg);         // TL
    gfx.drawFastHLine(x - rxst + r + w, y + cy - r, len, fg);     // TR
    gfx.drawFastHLine(x - rxst + r + w, y - cy + r + h, len, fg); // BR
  }

  // Draw sides
  gfx.fillRect(x, y + r - t + h, w + 1, t, fg); // Bottom
  gfx.fillRect(x - r + 1, y, t, h + 1, fg);     // Left
  gfx.fillRect(x, y - r + 1, w + 1, t, fg);     // Top
  gfx.fillRect(x + r - t + w, y, t, h + 1, fg); // Right
}
