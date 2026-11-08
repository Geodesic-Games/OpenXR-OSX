// SPDX-License-Identifier: MPL-2.0
package com.oculus;

import java.nio.ByteBuffer;

/** Camera-looper confined. At most two buffers survive out-of-order sensor notifications. */
final class ExposureFrames {
  private final long[] times = {-1, -1};
  private final byte[][] frames = new byte[2][];
  private int next;

  void add(long stamp, byte[] pixels) {
    times[next] = stamp;
    frames[next] = pixels;
    next = 1 - next;
  }

  byte[] take(long stamp) {
    for (int i = 0; i < 2; i++)
      if (times[i] == stamp) {
        byte[] value = frames[i];
        times[i] = -1;
        frames[i] = null;
        return value;
      }
    return null;
  }

  static void copyPlane(ByteBuffer source, int rowStride, int pixelStride, int width, int height,
      byte[] output, int offset) {
    ByteBuffer buf = source.duplicate();
    int base = buf.position();
    for (int y = 0; y < height; y++) {
      if (pixelStride == 1) {
        buf.position(base + y * rowStride);
        buf.get(output, offset + y * width, width);
      } else
        for (int x = 0; x < width; x++)
          output[offset + y * width + x] = buf.get(base + y * rowStride + x * pixelStride);
    }
  }
}
