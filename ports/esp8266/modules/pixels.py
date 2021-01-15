# LED strip driver (WS2812) for MicroPython on ESP8266
# MIT license; Copyright (c) 2017 Ayke van Laethem

import _pixels
import array
from esp import neopixel_write
from micropython import const

RGB    = const(0xf210)
GRB    = const(0xf120)
GRBW   = const(0x0231)
KHZ400 = const(0x10000)
KHZ800 = const(0x00000)

beatsin = _pixels.beatsin

class Pixels:
    '''
    Array of pixels, for use in ledstrips. Every value is backed by an
    array.array buffer. Every pixel is a 32-bit value, in the form 0xrrggbb.
    '''
    def __init__(self, pin, n, config=KHZ800|GRB):
        '''
        Create a new pixel array.
        pin:    machine.Pin()
        n:      number of LEDs
        config: type and color ordering
        '''
        self.config = config
        if isinstance(n, memoryview):
            # internal, slice of another NeoPixel object
            self.buf = n
        else:
            self.buf = array.array('L', bytearray(n*4))
            self.pin = pin
            self.pin.init(pin.OUT)

    def __len__(self):
        return len(self.buf)

    def __getitem__(self, index):
        '''
        Return the given color, in the form 0xrrggbb.
        Should usually not be used directly.
        '''
        if isinstance(index, slice):
            return NeoPixel(None, memoryview(self.buf)[index], None)
        return self.buf[index]

    def __setitem__(self, index, value):
        '''
        Directly set a given color, in the form 0xrrggbb.
        Should usually not be used directly.
        '''
        if isinstance(index, slice):
            _pixels.array_copy(memoryview(self.buf)[index], value.buf)
        else:
            self.buf[index] = value

    def write(self):
        '''
        Update the LED strip.
        '''
        neopixel_write(self.pin, self.buf, self.config)

    def fill_solid(self, color):
        '''
        Set every color to the given value, in the form 0xrrggbb.
        '''
        _pixels.fill_solid(self.buf, color)

    def fill_rainbow(self, hue, inc):
        '''
        Set a hue [0.0, 1.0] to every pixel in the array, with a given
        increment. Can also be used to set the whole array to a given hue, using
        a zero increment.

        https://github.com/FastLED/FastLED/wiki/FastLED-HSV-Colors
        '''
        _pixels.fill_rainbow(self.buf, hue, inc)

    def fill_rainbow_array(self, hues, sats=None, values=None):
        '''
        Set a given hue, saturation and value taken from an Array. A None
        saturation means fully saturated (1.0) and a None value means full
        brightness.

        https://github.com/FastLED/FastLED/wiki/FastLED-HSV-Colors
        '''
        sats = None if sats is None else sats.buf
        values = None if values is None else values.buf
        _pixels.fill_rainbow_array(self.buf, hues.buf, sats, values)

    def fill_palette_array(self, palette, indexes, brightness=1.0):
        '''
        Set a value from the palette to every pixel in the array.

        palette: 16-element array with 32-bit color values.
        indexes: Array with index into the palette.
        '''
        _pixels.fill_palette_array(self.buf, palette, indexes.buf, brightness)

    def scale(self, by=0.5):
        '''
        Increase or reduce brightness by the given amount. Never turns pixels
        off when they were on before the scale.
        '''
        _pixels.scale8_video(self.buf, by)

    def scale_raw(self, by=0.5):
        '''
        Increase or reduce brightness by the given amount. May turn off pixels
        when they fall below visibility.
        '''
        _pixels.scale8_raw(self.buf, by)

    def fade(self, by=0.5):
        '''
        Inverse of scale().
        '''
        _pixels.scale8_video(self.buf, 1.0-by)

    def fade_raw(self, by=0.5):
        '''
        Inverse of scale_raw().
        '''
        _pixels.scale8_raw(self.buf, 1.0-by)

    def reverse(self):
        '''
        Swap the order of the pixels.
        '''
        _pixels.array_reverse(self.buf)


class Array:
    '''
    Array of values in the range [0.0, 1.0]. This can be either a simple
    height/intensity (for e.g. saturation/value) or a circular buffer (for e.g.
    hue).
    The backend is an array.array memory buffer, which can be 8 bits or 16 bits.
    '''
    def __init__(self, n, bits=8):
        self.bits = bits
        if isinstance(n, memoryview): # internal
            self.buf = n
        elif self.bits == 8:
            self.buf = bytearray(n)
        elif self.bits == 16:
            self.buf = array.array('H', bytearray(n*2))
        else:
            raise ValueError('invalid number of bits, must be 8 or 16')

    def __getitem__(self, index):
        if isinstance(index, slice):
            return Array(memoryview(self.buf)[index], self.bits)
        if self.bits == 8:
            return self.buf[index] / 255.0
        else:
            return self.buf[index] / 65535.0

    def __setitem__(self, index, value):
        if isinstance(index, slice):
            _pixels.array_copy(memoryview(self.buf)[index], value.buf)
        else:
            value = max(0.0, min(1.0, value))
            if self.bits == 8:
                value *= 255.0
            else:
                value *= 65535.0
            self.buf[index] = int(value)

    def __len__(self):
        return len(self.buf)

    def __isub__(self, value):
        '''
        Subtrict this amout from every element. Value can be a number or another
        Array.
        '''
        if isinstance(value, Array):
            _pixels.array_sub_array(self.buf, value.buf)
        else:
            _pixels.array_add(self.buf, -value)
        return self

    def __iadd__(self, value):
        '''
        Add this amout to every element. Value can be a number or another Array.
        '''
        if isinstance(value, Array):
            _pixels.array_add_array(self.buf, value.buf)
        else:
            _pixels.array_add(self.buf, value)
        return self

    def itruediv(self, value):
        '''
        Divide this array by the given value.
        '''
        # TODO add support to MicroPython
        # TODO varying types
        # TODO: add support for dividing by another array.
        self.scale(1/value)

    def imul(self, value):
        '''
        Multiply every element by a given amount. Value can be a number or
        another Array.
        '''
        # TODO add support to MicroPython
        if isinstance(value, Array):
            _pixels.array_mul_array(self.buf, value.buf)
        else:
            self.scale(value)

    def scale(self, stop):
        '''
        Scale the array, e.g. to reduce to 25% do scale(0.25).
        '''
        # TODO overflow
        if self.bits == 8:
            _pixels.scale8_raw(self.buf, stop)
        else:
            _pixels.scale16_raw(self.buf, stop)

    def fill(self, value):
        '''
        Set every element to the given value.
        '''
        _pixels.array_fill(self.buf, value)

    def range(self, start=0, step=0.05):
        '''
        Like range() but wraps around.
        '''
        _pixels.array_range(self.buf, start, step)

    def random(self, start=None, stop=None):
        '''
        Fill array with random values between start and stop (inclusive).
        '''
        if start is None:
            start = 1.0
        if stop is None:
            stop = start
            start = 0.0
        _pixels.array_fill_random(self.buf, start, stop)

    def noise(self, xscale, y, yscale):
        '''
        Fill with Simplex noise. The x axis is the array.

        xscale: Distance between points on the x axis (e.g. 0.2).
        y:      2nd axis, e.g. time.
        yscale: What to incremet y by for eacy pixel. Usually 0.
        '''
        _pixels.array_fill_noise(self.buf, xscale, y, yscale)

    def sin(self):
        '''
        Apply sin() to every value in the array (in place).
        '''
        _pixels.array_sin(self.buf)

