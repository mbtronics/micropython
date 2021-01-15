/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2017 Ayke van Laethem, FastLED
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "py/nlr.h"
#include "py/runtime.h"
#include "py/runtime0.h"
#include "py/stream.h"
#include "py/binary.h"
#include "py/mphal.h" // mp_hal_ticks_ms

#if MICROPY_PY_PIXELS

#include "etshal.h"   // WDEV_HWRNG on the ESP8266

//#define MICROPY_PY_PIXELS_SMALL_FUNCTIONS

/**** BEGIN FASTLED CODE ****/

#define K255 255
#define K171 171
#define K170 170
#define K85  85

static int mod_pixels_scale8(int i, int frac) {
    return (i * (1+frac)) >> 8;
}

static uint16_t mod_pixels_scale16(uint16_t i, uint32_t scale) {
    return ((uint32_t)(i) * (1+scale)) / 65536;
}

///  The "video" version of scale8 guarantees that the output will
///  be only be zero if one or both of the inputs are zero.  If both
///  inputs are non-zero, the output is guaranteed to be non-zero.
///  This makes for better 'video'/LED dimming, at the cost of
///  several additional cycles.
static uint8_t mod_pixels_scale8_video(int i, int scale) {
    uint8_t j = ((i * scale) >> 8) + ((i && scale) ? 1 : 0);
    // uint8_t nonzeroscale = (scale != 0) ? 1 : 0;
    // uint8_t j = (i == 0) ? 0 : (((int)i * (int)(scale) ) >> 8) + nonzeroscale;
    return j;
}


static uint32_t mod_pixels_hsv2rgb_rainbow(uint8_t hue, uint8_t sat, uint8_t val) {
    // Yellow has a higher inherent brightness than
    // any other color; 'pure' yellow is perceived to
    // be 93% as bright as white.  In order to make
    // yellow appear the correct relative brightness,
    // it has to be rendered brighter than all other
    // colors.
    // Level Y1 is a moderate boost, the default.
    // Level Y2 is a strong boost.
    const uint8_t Y1 = 1;
    const uint8_t Y2 = 0;

    // G2: Whether to divide all greens by two.
    // Depends GREATLY on your particular LEDs
    const uint8_t G2 = 0;

    uint8_t offset = hue & 0x1F; // 0..31

    // offset8 = offset * 8
    uint8_t offset8 = offset << 3;

    uint8_t third = mod_pixels_scale8( offset8, (256 / 3)); // max = 85

    // 32-bit math takes less code space and is ~1% faster
    uint32_t r, g, b;

    if( ! (hue & 0x80) ) {
        // 0XX
        if( ! (hue & 0x40) ) {
            // 00X
            //section 0-1
            if( ! (hue & 0x20) ) {
                // 000
                //case 0: // R -> O
                r = K255 - third;
                g = third;
                b = 0;
            } else {
                // 001
                //case 1: // O -> Y
                if( Y1 ) {
                    r = K171;
                    g = K85 + third ;
                    b = 0;
                }
                if( Y2 ) {
                    r = K170 + third;
                    //uint8_t twothirds = (third << 1);
                    uint8_t twothirds = mod_pixels_scale8( offset8, ((256 * 2) / 3)); // max=170
                    g = K85 + twothirds;
                    b = 0;
                }
            }
        } else {
            //01X
            // section 2-3
            if( !  (hue & 0x20) ) {
                // 010
                //case 2: // Y -> G
                if( Y1 ) {
                    //uint8_t twothirds = (third << 1);
                    uint8_t twothirds = mod_pixels_scale8( offset8, ((256 * 2) / 3)); // max=170
                    r = K171 - twothirds;
                    g = K170 + third;
                    b = 0;
                }
                if( Y2 ) {
                    r = K255 - offset8;
                    g = K255;
                    b = 0;
                }
            } else {
                // 011
                // case 3: // G -> A
                r = 0;
                g = K255 - third;
                b = third;
            }
        }
    } else {
        // section 4-7
        // 1XX
        if( ! (hue & 0x40) ) {
            // 10X
            if( ! ( hue & 0x20) ) {
                // 100
                //case 4: // A -> B
                r = 0;
                //uint8_t twothirds = (third << 1);
                uint8_t twothirds = mod_pixels_scale8( offset8, ((256 * 2) / 3)); // max=170
                g = K171 - twothirds; //K170?
                b = K85  + twothirds;

            } else {
                // 101
                //case 5: // B -> P
                r = third;
                g = 0;
                b = K255 - third;

            }
        } else {
            if( !  (hue & 0x20)  ) {
                // 110
                //case 6: // P -- K
                r = K85 + third;
                g = 0;
                b = K171 - third;

            } else {
                // 111
                //case 7: // K -> R
                r = K170 + third;
                g = 0;
                b = K85 - third;

            }
        }
    }

    // This is one of the good places to scale the green down,
    // although the client can scale green down as well.
    if( G2 ) g = g >> 1;

    // Scale down colors if we're desaturated at all
    // and add the brightness_floor to r, g, and b.
    if( sat != 255 ) {
        if( sat == 0) {
            r = 255; b = 255; g = 255;
        } else {
            //nscale8x3_video( r, g, b, sat);
            if( r ) r = mod_pixels_scale8( r, sat);
            if( g ) g = mod_pixels_scale8( g, sat);
            if( b ) b = mod_pixels_scale8( b, sat);

            uint8_t desat = 255 - sat;
            desat = mod_pixels_scale8( desat, desat);

            uint8_t brightness_floor = desat;
            r += brightness_floor;
            g += brightness_floor;
            b += brightness_floor;
        }
    }

    // Now scale everything down if we're at value < 255.
    if( val != 255 ) {

        val = mod_pixels_scale8_video( val, val);
        if( val == 0 ) {
            r=0; g=0; b=0;
        } else {
            // nscale8x3_video( r, g, b, val);
            if( r ) r = mod_pixels_scale8( r, val);
            if( g ) g = mod_pixels_scale8( g, val);
            if( b ) b = mod_pixels_scale8( b, val);
        }
    }

    return r << 16 | g << 8 | b;
}

static uint32_t mod_pixels_color_from_palette(uint32_t *pal, uint16_t index, uint8_t brightness) {
    // Count the number of bits in `len`. Unfortunately, this is a pretty
    // expensive operation (costing 48%) so it's disabled.
    //uint32_t bits = mod_pixels_nbits(len);
    size_t  highindex = index >> 12; //(16 - bits);
    uint8_t lowindex  = index >> 4;  //(8  - bits);

    uint32_t entry = pal[highindex];

    // Using 32-bit math instead of 8-bit math results in a 6.1% performance
    // improvement (and even a small code size reduction)
    uint_fast8_t red1   = (entry >> 16) & 0xff;
    uint_fast8_t green1 = (entry >> 8 ) & 0xff;
    uint_fast8_t blue1  = (entry >> 0 ) & 0xff;

    if (lowindex) { // need to blend

        if( highindex == 15 ) {
            entry = pal[0];
        } else {
            entry = pal[highindex+1];
        }

        uint_fast8_t f2 = lowindex;
        uint_fast8_t f1 = 255 - f2;

        //    rgb1.nscale8(f1);
        uint_fast8_t red2   = (entry >> 16) & 0xff;
        red1   = mod_pixels_scale8( red1,   f1);
        red2   = mod_pixels_scale8( red2,   f2);
        red1   += red2;

        uint_fast8_t green2 = (entry >> 8) & 0xff;
        green1 = mod_pixels_scale8( green1, f1);
        green2 = mod_pixels_scale8( green2, f2);
        green1 += green2;

        uint_fast8_t blue2  = (entry >> 0) & 0xff;
        blue1  = mod_pixels_scale8( blue1,  f1);
        blue2  = mod_pixels_scale8( blue2,  f2);
        blue1  += blue2;
    }

    if( brightness != 255) {
        if( brightness ) {
            brightness++; // adjust for rounding
            if( red1 )   {
                red1 = mod_pixels_scale8( red1, brightness);
            }
            if( green1 ) {
                green1 = mod_pixels_scale8( green1, brightness);
            }
            if( blue1 )  {
                blue1 = mod_pixels_scale8( blue1, brightness);
            }
        } else {
            red1 = 0;
            green1 = 0;
            blue1 = 0;
        }
    }

    return red1 << 16 | green1 << 8 | blue1;
}

/// linear interpolation between two signed 15-bit values,
/// with 8-bit fraction
static int16_t mod_pixels_lerp15by16(int16_t a, int16_t b, uint16_t frac) {
    int16_t result;
    if( b > a) {
        uint16_t delta = b - a;
        uint16_t scaled = mod_pixels_scale16( delta, frac);
        result = a + scaled;
    } else {
        uint16_t delta = a - b;
        uint16_t scaled = mod_pixels_scale16( delta, frac);
        result = a - scaled;
    }
    return result;
}

static int8_t inline mod_pixels_lerp7by8(int8_t a, int8_t b, uint8_t frac) {
    // int8_t delta = b - a;
    // int16_t prod = (uint16_t)delta * (uint16_t)frac;
    // int8_t scaled = prod >> 8;
    // int8_t result = a + scaled;
    // return result;
    int8_t result;
    if( b > a) {
        uint8_t delta = b - a;
        uint8_t scaled = mod_pixels_scale8( delta, frac);
        result = a + scaled;
    } else {
        uint8_t delta = a - b;
        uint8_t scaled = mod_pixels_scale8( delta, frac);
        result = a - scaled;
    }
    return result;
}


/// Calculate an integer average of two signed 15-bit
///       integers (int16_t)
///       If the first argument is even, result is rounded down.
///       If the first argument is odd, result is result up.
static int16_t mod_pixels_avg15(int16_t i, int16_t j) {
    return ((int32_t)((int32_t)(i) + (int32_t)(j)) >> 1) + (i & 0x1);
}

/// Calculate an integer average of two signed 7-bit
///       integers (int8_t)
///       If the first argument is even, result is rounded down.
///       If the first argument is odd, result is result up.
static int8_t mod_pixels_avg7(int8_t i, int8_t j) {
    return ((i + j) >> 1) + (i & 0x1);
}

static int16_t inline mod_pixels_grad16(uint8_t hash, int16_t x, int16_t y) {
  hash = hash & 7;
  int16_t u,v;
  if(hash < 4) { u = x; v = y; } else { u = y; v = x; }
  if(hash&1) { u = -u; }
  if(hash&2) { v = -v; }

  return mod_pixels_avg15(u,v);
}

static int8_t inline mod_pixels_grad8(uint8_t hash, int8_t x, int8_t y) {
  // since the tests below can be done bit-wise on the bottom
  // three bits, there's no need to mask off the higher bits
  //  hash = hash & 7;

  int8_t u,v;
  if( hash & 4) {
      u = y; v = x;
  } else {
      u = x; v = y;
  }

  if(hash&1) { u = -u; }
  if(hash&2) { v = -v; }

  return mod_pixels_avg7(u,v);
}


// TODO store in flash?
static uint8_t const mod_pixels_noise_p[] = { 151,160,137,91,90,15,
   131,13,201,95,96,53,194,233,7,225,140,36,103,30,69,142,8,99,37,240,21,10,23,
   190, 6,148,247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32,57,177,33,
   88,237,149,56,87,174,20,125,136,171,168, 68,175,74,165,71,134,139,48,27,166,
   77,146,158,231,83,111,229,122,60,211,133,230,220,105,92,41,55,46,245,40,244,
   102,143,54, 65,25,63,161, 1,216,80,73,209,76,132,187,208, 89,18,169,200,196,
   135,130,116,188,159,86,164,100,109,198,173,186, 3,64,52,217,226,250,124,123,
   5,202,38,147,118,126,255,82,85,212,207,206,59,227,47,16,58,17,182,189,28,42,
   223,183,170,213,119,248,152, 2,44,154,163, 70,221,153,101,155,167, 43,172,9,
   129,22,39,253, 19,98,108,110,79,113,224,232,178,185, 112,104,218,246,97,228,
   251,34,242,193,238,210,144,12,191,179,162,241, 81,51,145,235,249,14,239,107,
   49,192,214, 31,181,199,106,157,184, 84,204,176,115,121,50,45,127, 4,150,254,
   138,236,205,93,222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180,151
   };

#define P(x) mod_pixels_noise_p[x]
#define FADE(x) mod_pixels_scale16(x,x)
#define LERP(a,b,u) mod_pixels_lerp15by16(a,b,u)


static int16_t mod_pixels_inoise16_raw(uint32_t x, uint32_t y) {
  // Find the unit cube containing the point
  uint8_t X = x>>16;
  uint8_t Y = y>>16;

  // Hash cube corner coordinates
  uint8_t A = P(X)+Y;
  uint8_t AA = P(A);
  uint8_t AB = P(A+1);
  uint8_t B = P(X+1)+Y;
  uint8_t BA = P(B);
  uint8_t BB = P(B+1);

  // Get the relative position of the point in the cube
  uint16_t u = x & 0xFFFF;
  uint16_t v = y & 0xFFFF;

  // Get a signed version of the above for the grad function
  int16_t xx = (u >> 1) & 0x7FFF;
  int16_t yy = (v >> 1) & 0x7FFF;
  uint16_t N = 0x8000L;

  u = FADE(u); v = FADE(v);

  int16_t X1 = LERP(mod_pixels_grad16(P(AA), xx, yy), mod_pixels_grad16(P(BA), xx - N, yy), u);
  int16_t X2 = LERP(mod_pixels_grad16(P(AB), xx, yy-N), mod_pixels_grad16(P(BB), xx - N, yy - N), u);

  int16_t ans = LERP(X1,X2,v);

  return ans;
}

static uint16_t mod_pixels_inoise16(uint32_t x, uint32_t y) {
  int32_t ans = mod_pixels_inoise16_raw(x,y);
  ans = ans + 17308L;
  uint32_t pan = ans;
  // pan = (ans * 242L) >> 7.  That's the same as:
  // pan = (ans * 484L) >> 8.  And this way avoids a 7X four-byte shift-loop on AVR.
  // Identical math, except for the highest bit, which we don't care about anyway,
  // since we're returning the 'middle' 16 out of a 32-bit value anyway.
  pan *= 484L;
  return (pan>>8);

  // return (uint32_t)(((int32_t)inoise16_raw(x,y)+(uint32_t)17308)*242)>>7;
  // return scale16by8(inoise16_raw(x,y)+17308,242)<<1;
}

/// Fast 16-bit approximation of sin(x). This approximation never varies more than
/// 0.69% from the floating point value you'd get by doing
///
///     float s = sin(x) * 32767.0;
///
/// @param theta input angle from 0-65535
/// @returns sin of theta, value between -32767 to 32767.
static int16_t mod_pixels_sin16(uint16_t theta) {
    static const uint16_t base[] =
    { 0, 6393, 12539, 18204, 23170, 27245, 30273, 32137 };
    static const uint8_t slope[] =
    { 49, 48, 44, 38, 31, 23, 14, 4 };

    uint16_t offset = (theta & 0x3FFF) >> 3; // 0..2047
    if( theta & 0x4000 ) offset = 2047 - offset;

    uint8_t section = offset / 256; // 0..7
    uint16_t b   = base[section];
    uint8_t  m   = slope[section];

    uint8_t secoffset8 = (uint8_t)(offset) / 2;

    uint16_t mx = m * secoffset8;
    int16_t  y  = mx + b;

    if (theta & 0x8000) {
        y = -y;
    }

    return y;
}

// TODO use a better counter, this one wraps around fast.
#define GET_MILLIS mp_hal_ticks_ms

// beat88 generates a 16-bit 'sawtooth' wave at a given BPM,
///        with BPM specified in Q8.8 fixed-point format; e.g.
///        for this function, 120 BPM MUST BE specified as
///        120*256 = 30720.
///        If you just want to specify "120", use beat16 or beat8.
static inline uint16_t mod_pixels_beat88(uint16_t beats_per_minute_88, uint32_t timebase) {
    // BPM is 'beats per minute', or 'beats per 60000ms'.
    // To avoid using the (slower) division operator, we
    // want to convert 'beats per 60000ms' to 'beats per 65536ms',
    // and then use a simple, fast bit-shift to divide by 65536.
    //
    // The ratio 65536:60000 is 279.620266667:256; we'll call it 280:256.
    // The conversion is accurate to about 0.05%, more or less,
    // e.g. if you ask for "120 BPM", you'll get about "119.93".
    return (((GET_MILLIS()) - timebase) * beats_per_minute_88 * 280) >> 16;
}

/// beatsin88 generates a 16-bit sine wave at a given BPM,
///           that oscillates within a given range.
///           For this function, BPM MUST BE SPECIFIED as
///           a Q8.8 fixed-point value; e.g. 120BPM must be
///           specified as 120*256 = 30720.
static inline uint16_t mod_pixels_beatsin88(uint16_t beats_per_minute_88, uint16_t lowest, uint16_t highest,
                              uint32_t timebase, uint16_t phase_offset) {
    uint16_t beat = mod_pixels_beat88( beats_per_minute_88, timebase);
    uint16_t beatsin = (mod_pixels_sin16( beat + phase_offset) + 32768);
    uint16_t rangewidth = highest - lowest;
    uint16_t scaledbeat = mod_pixels_scale16(beatsin, rangewidth);
    uint16_t result = lowest + scaledbeat;
    return result;
}

/**** END FASTLED CODE ****/


#ifdef MICROPY_PY_PIXELS_SMALL_FUNCTIONS

STATIC mp_obj_t mod_pixels_hsv2rgb_rainbow_(mp_obj_t hue, mp_obj_t sat, mp_obj_t val) {
    uint8_t h = mp_obj_get_float(hue) * 255.0f + 0.5f;
    uint8_t s = mp_obj_get_float(sat) * 255.0f + 0.5f;
    uint8_t v = mp_obj_get_float(val) * 255.0f + 0.5f;
    return mp_obj_new_int(mod_pixels_hsv2rgb_rainbow(h, s, v));
}
STATIC MP_DEFINE_CONST_FUN_OBJ_3(mod_pixels_hsv2rgb_rainbow_obj, mod_pixels_hsv2rgb_rainbow_);


STATIC mp_obj_t mod_pixels_color_from_palette_(mp_obj_t palette, mp_obj_t index, mp_obj_t brightness) {
    mp_buffer_info_t paletteinfo;
    mp_get_buffer_raise(palette, &paletteinfo, MP_BUFFER_READ);
    if (paletteinfo.len != 64 || mp_binary_get_size('<', paletteinfo.typecode, NULL) != 4) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad palette"));
    }
    uint32_t *pal = (uint32_t*)paletteinfo.buf;
    uint32_t idx = mp_obj_get_int(index);
    float bright = mp_obj_get_float(brightness);
    if (bright < 0.0f || bright > 1.0f) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad brightness");
    }
    return mp_obj_new_int(mod_pixels_color_from_palette(pal, idx, bright * 255.0f + 0.5f));
}
STATIC MP_DEFINE_CONST_FUN_OBJ_3(mod_pixels_color_from_palette_obj, mod_pixels_color_from_palette_);


STATIC mp_obj_t mod_pixels_noise_(size_t n_args, const mp_obj_t *args) {
    uint32_t x = mp_obj_get_float(args[0]) * 65535.0f + 0.5f;
    uint32_t y = mp_obj_get_float(args[1]) * 65535.0f + 0.5f;
    uint16_t result = mod_pixels_inoise16(x, y);
    return mp_obj_new_float(result / 65535.0f);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_pixels_noise_obj, 2, 2, mod_pixels_noise_);

#endif // MICROPY_PY_PIXELS_SMALL_FUNCTIONS


STATIC mp_obj_t mod_pixels_beatsin_(size_t n_args, const mp_obj_t *args) {
    uint16_t bpm     = mp_obj_get_float(args[0]) * 255.0f + 0.5f;
    uint16_t lowest  = mp_obj_get_float(args[1]) * 65535.0f + 0.5f;
    uint16_t highest = mp_obj_get_float(args[2]) * 65535.0f + 0.5f;
    uint16_t result = mod_pixels_beatsin88(bpm, lowest, highest, 0, 0);
    return mp_obj_new_float(result / 65535.0f);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_pixels_beatsin_obj, 3, 3, mod_pixels_beatsin_);


STATIC mp_obj_t mod_pixels_fill_solid_(mp_obj_t buf, mp_obj_t color) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buf, &bufinfo, MP_BUFFER_WRITE);
    uint32_t *pixels = (uint32_t*)bufinfo.buf;
    uint32_t c = mp_obj_get_int(color);
    for (int i=0; i<bufinfo.len/4; i++) {
        pixels[i] = c;
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(mod_pixels_fill_solid_obj, mod_pixels_fill_solid_);


STATIC mp_obj_t mod_pixels_fill_rainbow_(mp_obj_t buf, mp_obj_t huestart, mp_obj_t hueinc) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buf, &bufinfo, MP_BUFFER_WRITE);
    uint32_t *pixels = (uint32_t*)bufinfo.buf;
    float hue_f = mp_obj_get_float(huestart);
    if (hue_f < 0.0f || hue_f > 1.0f) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad huestart"));
    }
    uint8_t hue  = hue_f * 255.0f + 0.5f;
    int8_t inc = mp_obj_get_float(hueinc) * 255.0f + 0.5f;
    for (int i=0; i<bufinfo.len/4; i++) {
        pixels[i] = mod_pixels_hsv2rgb_rainbow(hue, 255, 255);
        hue += inc;
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_3(mod_pixels_fill_rainbow_obj, mod_pixels_fill_rainbow_);


STATIC mp_obj_t mod_pixels_fill_rainbow_array_(const size_t n_args, const mp_obj_t *args) {
    mp_buffer_info_t pixelinfo, huesinfo;
    mp_get_buffer_raise(args[0], &pixelinfo, MP_BUFFER_WRITE);
    mp_get_buffer_raise(args[1], &huesinfo, MP_BUFFER_READ);
    uint32_t *pixels = (uint32_t*)pixelinfo.buf;

    uint8_t *sats = NULL, *vals = NULL;
    uint8_t sats_inc=1, vals_inc=1;
    if (args[2] != mp_const_none) {
        mp_buffer_info_t satsinfo;
        mp_get_buffer_raise(args[2], &satsinfo, MP_BUFFER_READ);
        sats = (uint8_t*)satsinfo.buf;
        if (satsinfo.typecode == BYTEARRAY_TYPECODE || satsinfo.typecode == 'B') {
            sats_inc = 1;
        } else if (satsinfo.typecode == 'H') {
            sats_inc = 2;
            sats++; // works on little endian systems
        } else {
            mp_raise_ValueError(MP_ERROR_TEXT("bad sats"));
        }
    }
    if (args[3] != mp_const_none) {
        mp_buffer_info_t valsinfo;
        mp_get_buffer_raise(args[3], &valsinfo, MP_BUFFER_READ);
        vals = (uint8_t*)valsinfo.buf;
        if (valsinfo.typecode == BYTEARRAY_TYPECODE || valsinfo.typecode == 'B') {
            vals_inc = 1;
        } else if (valsinfo.typecode == 'H') {
            vals_inc = 2;
            vals++; // works on little endian systems
        } else {
            mp_raise_ValueError(MP_ERROR_TEXT("bad vals"));
        }
    }
    uint8_t sat=255, val=255;

    size_t len = pixelinfo.len/4;
    if (huesinfo.typecode == BYTEARRAY_TYPECODE || huesinfo.typecode == 'B') {
        if (huesinfo.len < len)
            len = huesinfo.len;
        uint8_t *a = (uint8_t*)huesinfo.buf;
        for (int i=0; i<len; i++) {
            if (sats) {
                sat = *sats;
                sats += sats_inc;
            }
            if (vals) {
                val = *vals;
                vals += vals_inc;
            }
            pixels[i] = mod_pixels_hsv2rgb_rainbow(*a++, sat, val);
        }
    } else if (huesinfo.typecode == 'H') {
        if (huesinfo.len/2 < len)
            len = huesinfo.len/2;
        uint16_t *a = (uint16_t*)huesinfo.buf;
        for (int i=0; i<len; i++) {
            if (sats) {
                sat = *sats;
                sats += sats_inc;
            }
            if (vals) {
                val = *vals;
                vals += vals_inc;
            }
            // TODO: 16-bit hue for greater color depth (is that visible?)
            pixels[i] = mod_pixels_hsv2rgb_rainbow(*a++ >> 8, sat, val);
        }
    } else {
        mp_raise_ValueError(MP_ERROR_TEXT("bad array"));
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_pixels_fill_rainbow_array_obj, 4, 4, mod_pixels_fill_rainbow_array_);


STATIC mp_obj_t mod_pixels_fill_palette_array_(const size_t n_args, const mp_obj_t *args) {
    mp_buffer_info_t pixelinfo, paletteinfo, arrayinfo;
    mp_get_buffer_raise(args[0], &pixelinfo, MP_BUFFER_WRITE);
    mp_get_buffer_raise(args[1], &paletteinfo, MP_BUFFER_READ);
    mp_get_buffer_raise(args[2], &arrayinfo, MP_BUFFER_READ);
    if (paletteinfo.len != 64 || mp_binary_get_size('<', paletteinfo.typecode, NULL) != 4) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad palette"));
    }
    int32_t brightness = 255;
    if (n_args >= 4) {
        float b = mp_obj_get_float(args[3]);
        if (b < 0.0f || b > 1.0f) {
            mp_raise_ValueError(MP_ERROR_TEXT("bad brightness"));
        }
        brightness = b * 255.0f + 0.5f;
    }
    uint32_t *pixels = (uint32_t*)pixelinfo.buf;
    uint32_t *palette = (uint32_t*)paletteinfo.buf;
    size_t len = pixelinfo.len/4;
    if (arrayinfo.typecode == BYTEARRAY_TYPECODE || arrayinfo.typecode == 'B') {
        if (arrayinfo.len < len)
            len = arrayinfo.len;
        uint8_t *array = (uint8_t*)arrayinfo.buf;
        for (int i=0; i<len; i++) {
            pixels[i] = mod_pixels_color_from_palette(palette, *array++ << 8, brightness);
        }
    } else if (arrayinfo.typecode == 'H') {
        if (arrayinfo.len/2 < len)
            len = arrayinfo.len/2;
        uint16_t *array = (uint16_t*)arrayinfo.buf;
        for (int i=0; i<len; i++) {
            pixels[i] = mod_pixels_color_from_palette(palette, *array++, brightness);
        }
    } else {
        mp_raise_ValueError(MP_ERROR_TEXT("bad array"));
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_pixels_fill_palette_array_obj, 3, 4, mod_pixels_fill_palette_array_);


STATIC mp_obj_t mod_pixels_scale8_video_(mp_obj_t array, mp_obj_t value) {
    mp_buffer_info_t arrayinfo;
    mp_get_buffer_raise(array, &arrayinfo, MP_BUFFER_WRITE);
    uint8_t *a = (uint8_t*)arrayinfo.buf;
    uint32_t val = mp_obj_get_float(value) * 255.0f + 0.5f;
    for (int i=0; i<arrayinfo.len; i++) {
        a[i] = mod_pixels_scale8_video(a[i], val);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(mod_pixels_scale8_video_obj, mod_pixels_scale8_video_);


STATIC mp_obj_t mod_pixels_scale8_raw_(mp_obj_t array, mp_obj_t value) {
    mp_buffer_info_t arrayinfo;
    mp_get_buffer_raise(array, &arrayinfo, MP_BUFFER_WRITE);
    uint8_t *a = (uint8_t*)arrayinfo.buf;
    uint32_t val = mp_obj_get_float(value) * 255.0f + 0.5f;
    for (int i=0; i<arrayinfo.len; i++) {
        a[i] = mod_pixels_scale8(a[i], val);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(mod_pixels_scale8_raw_obj, mod_pixels_scale8_raw_);


STATIC mp_obj_t mod_pixels_scale16_raw_(mp_obj_t array, mp_obj_t value) {
    mp_buffer_info_t arrayinfo;
    mp_get_buffer_raise(array, &arrayinfo, MP_BUFFER_WRITE);
    uint16_t *a = (uint16_t*)arrayinfo.buf;
    uint32_t val = mp_obj_get_float(value) * 65535.0f + 0.5f;
    for (int i=0; i<arrayinfo.len/2; i++) {
        a[i] = mod_pixels_scale16(a[i], val);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(mod_pixels_scale16_raw_obj, mod_pixels_scale16_raw_);


STATIC mp_obj_t mod_pixels_array_fill_(mp_obj_t array, mp_obj_t value) {
    mp_buffer_info_t arrayinfo;
    mp_get_buffer_raise(array, &arrayinfo, MP_BUFFER_WRITE);
    int32_t val = mp_obj_get_float(value) * 65535.0f;
    if (val > 0xffff || val < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("value out of range"));
    }
    if (arrayinfo.typecode == BYTEARRAY_TYPECODE || arrayinfo.typecode == 'B') {
        val >>= 8;
        uint8_t *a = (uint8_t*)arrayinfo.buf;
        for (int i=0; i<arrayinfo.len; i++) {
            a[i] = val;
        }
    } else if (arrayinfo.typecode == 'H') {
        uint16_t *a = (uint16_t*)arrayinfo.buf;
        for (int i=0; i<arrayinfo.len/2; i++) {
            a[i] = val;
        }
    } else {
        mp_raise_ValueError(MP_ERROR_TEXT("bad buffer type"));
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(mod_pixels_array_fill_obj, mod_pixels_array_fill_);


STATIC mp_obj_t mod_pixels_array_range_(mp_obj_t array, mp_obj_t start, mp_obj_t step) {
    mp_buffer_info_t arrayinfo;
    mp_get_buffer_raise(array, &arrayinfo, MP_BUFFER_WRITE);
    float n_f = mp_obj_get_float(start);
    if (n_f > 1.0f || n_f < 0.0f) {
        mp_raise_ValueError(MP_ERROR_TEXT("start out of range"));
    }
    float inc_f = mp_obj_get_float(step);
    if (arrayinfo.typecode == BYTEARRAY_TYPECODE || arrayinfo.typecode == 'B') {
        uint32_t n = n_f * 255.0f + 0.5f;
        uint32_t inc = inc_f * 255.0f + 0.5f;
        uint8_t *a = (uint8_t*)arrayinfo.buf;
        for (int i=0; i<arrayinfo.len; i++) {
            a[i] = n;
            n += inc;
        }
    } else if (arrayinfo.typecode == 'H') {
        uint32_t n = n_f * 65535.0f + 0.5f;
        uint32_t inc = inc_f * 65535.0f + 0.5f;
        uint16_t *a = (uint16_t*)arrayinfo.buf;
        for (int i=0; i<arrayinfo.len/2; i++) {
            a[i] = n;
            n += inc;
        }
    } else {
        mp_raise_ValueError(MP_ERROR_TEXT("bad buffer type"));
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_3(mod_pixels_array_range_obj, mod_pixels_array_range_);


inline uint32_t mod_pixel_random() {
    // TODO specific for the ESP8266
    return *WDEV_HWRNG;
}

STATIC mp_obj_t mod_pixels_array_fill_random_(const size_t n_args, const mp_obj_t *args) {
    mp_buffer_info_t arrayinfo;
    mp_get_buffer_raise(args[0], &arrayinfo, MP_BUFFER_WRITE);
    uint32_t start = mp_obj_get_float(args[1]) * 65535.0f;
    uint32_t stop = mp_obj_get_float(args[2]) * 65535.0f;
    if (start >= stop || start > 0xffff || start < 0 || stop > 0xffff || stop < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad range"));
    }
    uint32_t width = stop - start;
    // TODO implement step
    if (arrayinfo.typecode == BYTEARRAY_TYPECODE || arrayinfo.typecode == 'B') {
        start >>= 8;
        stop >>= 8;
        width >>= 8;
        uint8_t *array = (uint8_t*)arrayinfo.buf;
        for (size_t i=0; i<arrayinfo.len; i++) {
            uint32_t n = mod_pixel_random();
            (void)n;
            array[i] = start + n % width;
        }
    } else if (arrayinfo.typecode == 'H') {
        uint16_t *array = (uint16_t*)arrayinfo.buf;
        for (size_t i=0; i<arrayinfo.len/2; i++) {
            uint32_t n = mod_pixel_random();
            array[i] = start + n % width;
        }
    } else {
        mp_raise_ValueError(MP_ERROR_TEXT("bad buffer type"));
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_pixels_array_fill_random_obj, 3, 3, mod_pixels_array_fill_random_);


STATIC mp_obj_t mod_pixels_array_fill_noise_(size_t n_args, const mp_obj_t *args) {
    mp_buffer_info_t arrayinfo;
    mp_get_buffer_raise(args[0], &arrayinfo, MP_BUFFER_WRITE);
    uint32_t x = 0;
    uint32_t xscale = mp_obj_get_float(args[1]) * 65535.0f;
    uint32_t y = mp_obj_get_float(args[2]) * 65535.0f;
    uint32_t yscale = mp_obj_get_float(args[3]) * 65535.0f;
    if (arrayinfo.typecode == BYTEARRAY_TYPECODE || arrayinfo.typecode == 'B') {
        uint8_t *array = (uint8_t*)arrayinfo.buf;
        for (int i=0; i<arrayinfo.len; i++) {
            // Using inoise8 would be a bit faster (~15%)
            array[i] = mod_pixels_inoise16(x, y) >> 8;
            x += xscale;
            y += yscale;
        }
    } else if (arrayinfo.typecode == 'H') {
        uint16_t *array = (uint16_t*)arrayinfo.buf;
        for (int i=0; i<arrayinfo.len/2; i++) {
            array[i] = mod_pixels_inoise16(x, y);
            x += xscale;
            y += yscale;
        }
    } else {
        mp_raise_ValueError(MP_ERROR_TEXT("bad buffer type"));
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_pixels_array_fill_noise_obj, 4, 4, mod_pixels_array_fill_noise_);


STATIC bool mod_pixels_array_prepare_op(mp_buffer_info_t *arrayinfo, mp_buffer_info_t *valuesinfo) {
    bool awide, vwide;
    if (arrayinfo->typecode == BYTEARRAY_TYPECODE || arrayinfo->typecode == 'B') {
        awide = false;
    } else if (arrayinfo->typecode == 'H') {
        awide = true;
    } else {
        mp_raise_ValueError(MP_ERROR_TEXT("bad buffer type"));
    }
    if (valuesinfo->typecode == BYTEARRAY_TYPECODE || valuesinfo->typecode == 'B') {
        vwide = false;
    } else if (valuesinfo->typecode == 'H') {
        vwide = true;
    } else {
        mp_raise_ValueError(MP_ERROR_TEXT("bad buffer type"));
    }
    if (awide != vwide) {
        mp_raise_ValueError(MP_ERROR_TEXT("incompatible buffers"));
    }
    return awide;
}


STATIC mp_obj_t mod_pixels_array_add_array_(mp_obj_t array, mp_obj_t values) {
    mp_buffer_info_t arrayinfo, valuesinfo;
    mp_get_buffer_raise(array, &arrayinfo, MP_BUFFER_WRITE);
    mp_get_buffer_raise(values, &valuesinfo, MP_BUFFER_READ);
    bool wide = mod_pixels_array_prepare_op(&arrayinfo, &valuesinfo);
    size_t len = arrayinfo.len;
    if (valuesinfo.len < len) {
        len = valuesinfo.len;
    }
    if (wide) { // 16 bits
        uint16_t *a = (uint16_t*)arrayinfo.buf;
        uint16_t *v = (uint16_t*)valuesinfo.buf;
        for (size_t i=0; i<len/2; i++) {
            uint32_t t = (uint32_t)a[i] + (uint32_t)v[i];
            if (t > 0xffff) t = 0xffff;
            a[i] = t;
        }
    } else { // 8 bits
        uint8_t *a = (uint8_t*)arrayinfo.buf;
        uint8_t *v = (uint8_t*)valuesinfo.buf;
        for (size_t i=0; i<len; i++) {
            uint16_t t = (uint16_t)a[i] + (uint16_t)v[i];
            if (t > 0xff) t = 0xff;
            a[i] = t;
        }
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(mod_pixels_array_add_array_obj, mod_pixels_array_add_array_);


STATIC mp_obj_t mod_pixels_array_add_(mp_obj_t array, mp_obj_t value) {
    mp_buffer_info_t arrayinfo;
    mp_get_buffer_raise(array, &arrayinfo, MP_BUFFER_WRITE);
    float val_f = mp_obj_get_float(value);
    if (arrayinfo.typecode == BYTEARRAY_TYPECODE || arrayinfo.typecode == 'B') {
        uint8_t *a = (uint8_t*)arrayinfo.buf;
        int_fast16_t val = val_f * 255.0 + 0.5;
        for (size_t i=0; i<arrayinfo.len; i++) {
            int_fast16_t t = (int_fast16_t)a[i] + val;
            if (t > 0xff) t = 0xff;
            if (t < 0) t = 0;
            a[i] = t;
        }
    } else if (arrayinfo.typecode == 'H') {
        uint16_t *a = (uint16_t*)arrayinfo.buf;
        int_fast32_t val = val_f * 65535.0 + 0.5;
        for (size_t i=0; i<arrayinfo.len/2; i++) {
            int_fast32_t t = (int_fast32_t)a[i] + val;
            if (t > 0xffff) t = 0xffff;
            if (t < 0) t = 0;
            a[i] = t;
        }
    } else {
        mp_raise_ValueError(MP_ERROR_TEXT("bad buffer type"));
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(mod_pixels_array_add_obj, mod_pixels_array_add_);


STATIC mp_obj_t mod_pixels_array_sub_array_(mp_obj_t array, mp_obj_t values) {
    mp_buffer_info_t arrayinfo, valuesinfo;
    mp_get_buffer_raise(array, &arrayinfo, MP_BUFFER_WRITE);
    mp_get_buffer_raise(values, &valuesinfo, MP_BUFFER_READ);
    bool wide = mod_pixels_array_prepare_op(&arrayinfo, &valuesinfo);
    size_t len = arrayinfo.len;
    if (valuesinfo.len < len) {
        len = valuesinfo.len;
    }
    if (wide) { // 16 bits
        uint16_t *a = (uint16_t*)arrayinfo.buf;
        uint16_t *v = (uint16_t*)valuesinfo.buf;
        for (size_t i=0; i<len/2; i++) {
            int t = a[i] - v[i];
            if (t < 0) t = 0;
            a[i] = t;
        }
    } else { // 8 bits
        uint8_t *a = (uint8_t*)arrayinfo.buf;
        uint8_t *v = (uint8_t*)valuesinfo.buf;
        for (size_t i=0; i<len; i++) {
            int t = a[i] - v[i];
            if (t < 0) t = 0;
            a[i] = t;
        }
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(mod_pixels_array_sub_array_obj, mod_pixels_array_sub_array_);


STATIC mp_obj_t mod_pixels_array_mul_array_(mp_obj_t array, mp_obj_t values) {
    mp_buffer_info_t arrayinfo, valuesinfo;
    mp_get_buffer_raise(array, &arrayinfo, MP_BUFFER_WRITE);
    mp_get_buffer_raise(values, &valuesinfo, MP_BUFFER_READ);
    bool wide = mod_pixels_array_prepare_op(&arrayinfo, &valuesinfo);
    size_t len = arrayinfo.len;
    if (valuesinfo.len < len) {
        len = valuesinfo.len;
    }
    if (wide) { // 16 bits
        uint16_t *a = (uint16_t*)arrayinfo.buf;
        uint16_t *v = (uint16_t*)valuesinfo.buf;
        for (size_t i=0; i<len/2; i++) {
            a[i] = (uint_fast32_t)a[i] * (uint_fast32_t)v[i] / 65536;
        }
    } else { // 8 bits
        uint8_t *a = (uint8_t*)arrayinfo.buf;
        uint8_t *v = (uint8_t*)valuesinfo.buf;
        for (size_t i=0; i<len; i++) {
            a[i] = (uint_fast16_t)a[i] * (uint_fast16_t)v[i] / 256;
        }
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(mod_pixels_array_mul_array_obj, mod_pixels_array_mul_array_);


STATIC mp_obj_t mod_pixels_array_sin_(mp_obj_t array) {
    mp_buffer_info_t arrayinfo;
    mp_get_buffer_raise(array, &arrayinfo, MP_BUFFER_WRITE);
    if (arrayinfo.typecode == BYTEARRAY_TYPECODE || arrayinfo.typecode == 'B') {
        uint8_t *a = (uint8_t*)arrayinfo.buf;
        for (size_t i=0; i<arrayinfo.len; i++) {
            a[i] = (mod_pixels_sin16((uint16_t)a[i] << 8) + 32767) >> 8;
        }
    } else if (arrayinfo.typecode == 'H') {
        uint16_t *a = (uint16_t*)arrayinfo.buf;
        for (size_t i=0; i<arrayinfo.len/2; i++) {
            a[i] = mod_pixels_sin16(a[i]) + 32767;
        }
    } else {
        mp_raise_ValueError(MP_ERROR_TEXT("bad buffer type"));
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mod_pixels_array_sin_obj, mod_pixels_array_sin_);


STATIC mp_obj_t mod_pixels_array_reverse_(mp_obj_t array) {
    mp_buffer_info_t arrayinfo;
    mp_get_buffer_raise(array, &arrayinfo, MP_BUFFER_WRITE);
    if (arrayinfo.typecode == 'L') {
        uint32_t *a = (uint32_t*)arrayinfo.buf;
        size_t len = arrayinfo.len/4;
        for (size_t i=0; i<len/2; i++) {
            uint32_t j = len-i-1;
            uint32_t tmp = a[i];
            a[i] = a[j];
            a[j] = tmp;
        }
    } else {
        // unimplemented
        mp_raise_ValueError(MP_ERROR_TEXT("bad buffer type"));
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mod_pixels_array_reverse_obj, mod_pixels_array_reverse_);


STATIC mp_obj_t mod_pixels_array_copy_(mp_obj_t array, mp_obj_t values) {
    mp_buffer_info_t arrayinfo, valuesinfo;
    mp_get_buffer_raise(array, &arrayinfo, MP_BUFFER_WRITE);
    mp_get_buffer_raise(values, &valuesinfo, MP_BUFFER_READ);
    mod_pixels_array_prepare_op(&arrayinfo, &valuesinfo);
    size_t len = arrayinfo.len;
    if (valuesinfo.len < len) {
        len = valuesinfo.len;
    }
    uint8_t *a = (uint8_t*)arrayinfo.buf;
    uint8_t *v = (uint8_t*)valuesinfo.buf;
    if (a < v) {
        for (size_t i=0; i<len; i++) {
            a[i] = v[i];
        }
    } else {
        for (size_t i=len-1; ; i--) {
            a[i] = v[i];
            if (i == 0) break;
        }
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(mod_pixels_array_copy_obj, mod_pixels_array_copy_);


STATIC const mp_rom_map_elem_t mp_module_pixels_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR__pixels) },
#ifdef MICROPY_PY_PIXELS_SMALL_FUNCTIONS
    { MP_ROM_QSTR(MP_QSTR_hsv2rgb_rainbow), MP_ROM_PTR(&mod_pixels_hsv2rgb_rainbow_obj) },
    { MP_ROM_QSTR(MP_QSTR_color_from_palette), MP_ROM_PTR(&mod_pixels_color_from_palette_obj) },
    { MP_ROM_QSTR(MP_QSTR_noise), MP_ROM_PTR(&mod_pixels_noise_obj) },
#endif
    { MP_ROM_QSTR(MP_QSTR_beatsin), MP_ROM_PTR(&mod_pixels_beatsin_obj) },
    { MP_ROM_QSTR(MP_QSTR_fill_solid), MP_ROM_PTR(&mod_pixels_fill_solid_obj) },
    { MP_ROM_QSTR(MP_QSTR_fill_rainbow), MP_ROM_PTR(&mod_pixels_fill_rainbow_obj) },
    { MP_ROM_QSTR(MP_QSTR_fill_rainbow_array), MP_ROM_PTR(&mod_pixels_fill_rainbow_array_obj) },
    { MP_ROM_QSTR(MP_QSTR_fill_palette_array), MP_ROM_PTR(&mod_pixels_fill_palette_array_obj) },
    { MP_ROM_QSTR(MP_QSTR_scale8_video), MP_ROM_PTR(&mod_pixels_scale8_video_obj) },
    { MP_ROM_QSTR(MP_QSTR_scale8_raw), MP_ROM_PTR(&mod_pixels_scale8_raw_obj) },
    { MP_ROM_QSTR(MP_QSTR_scale16_raw), MP_ROM_PTR(&mod_pixels_scale16_raw_obj) },
    { MP_ROM_QSTR(MP_QSTR_array_fill), MP_ROM_PTR(&mod_pixels_array_fill_obj) },
    { MP_ROM_QSTR(MP_QSTR_array_range), MP_ROM_PTR(&mod_pixels_array_range_obj) },
    { MP_ROM_QSTR(MP_QSTR_array_fill_random), MP_ROM_PTR(&mod_pixels_array_fill_random_obj) },
    { MP_ROM_QSTR(MP_QSTR_array_fill_noise), MP_ROM_PTR(&mod_pixels_array_fill_noise_obj) },
    { MP_ROM_QSTR(MP_QSTR_array_add_array), MP_ROM_PTR(&mod_pixels_array_add_array_obj) },
    { MP_ROM_QSTR(MP_QSTR_array_add), MP_ROM_PTR(&mod_pixels_array_add_obj) },
    { MP_ROM_QSTR(MP_QSTR_array_sub_array), MP_ROM_PTR(&mod_pixels_array_sub_array_obj) },
    { MP_ROM_QSTR(MP_QSTR_array_mul_array), MP_ROM_PTR(&mod_pixels_array_mul_array_obj) },
    { MP_ROM_QSTR(MP_QSTR_array_sin), MP_ROM_PTR(&mod_pixels_array_sin_obj) },
    { MP_ROM_QSTR(MP_QSTR_array_reverse), MP_ROM_PTR(&mod_pixels_array_reverse_obj) },
    { MP_ROM_QSTR(MP_QSTR_array_copy), MP_ROM_PTR(&mod_pixels_array_copy_obj) },
};

STATIC MP_DEFINE_CONST_DICT(mp_module_pixels_globals, mp_module_pixels_globals_table);

const mp_obj_module_t mp_module_pixels = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t*)&mp_module_pixels_globals,
};

#endif // MICROPY_PY_PIXELS
