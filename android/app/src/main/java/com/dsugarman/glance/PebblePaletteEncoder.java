package com.dsugarman.glance;

import android.graphics.Bitmap;

/**
 * Resamples an arbitrary bitmap into a Pebble GBitmapFormat8Bit byte array.
 *
 * Pebble's 8-bit format encodes each pixel as a single byte:
 *   bit 7-6 = alpha (0-3, where 3 = fully opaque)
 *   bit 5-4 = red   (0-3)
 *   bit 3-2 = green (0-3)
 *   bit 1-0 = blue  (0-3)
 *
 * Total: 64 visible RGB colors × 4 alpha levels. We always emit alpha=3.
 */
public final class PebblePaletteEncoder {

    public static final int WIDTH = 100;
    public static final int HEIGHT = 114;

    private PebblePaletteEncoder() {}

    public static byte[] encode(Bitmap src) {
        Bitmap scaled = Bitmap.createScaledBitmap(src, WIDTH, HEIGHT, true);
        byte[] out = new byte[WIDTH * HEIGHT];
        int[] pixels = new int[WIDTH * HEIGHT];
        scaled.getPixels(pixels, 0, WIDTH, 0, 0, WIDTH, HEIGHT);
        for (int i = 0; i < pixels.length; i++) {
            int px = pixels[i];
            int r = (px >> 16) & 0xFF;
            int g = (px >> 8)  & 0xFF;
            int b = (px)       & 0xFF;
            // Quantize 8-bit channels to 2-bit (round to nearest of 0,85,170,255).
            int rq = (r * 3 + 127) / 255;
            int gq = (g * 3 + 127) / 255;
            int bq = (b * 3 + 127) / 255;
            out[i] = (byte) (0xC0 | (rq << 4) | (gq << 2) | bq);
        }
        if (scaled != src) scaled.recycle();
        return out;
    }
}
