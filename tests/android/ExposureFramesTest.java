// SPDX-License-Identifier: MPL-2.0
package com.oculus;
import java.nio.ByteBuffer;
import java.util.Arrays;

public final class ExposureFramesTest {
  public static void main(String[] args) {
    ExposureFrames q = new ExposureFrames();
    byte[] a = {1}, b = {2}, c = {3};
    q.add(10, a);
    q.add(11, b);
    q.add(12, c);
    if (q.take(10) != null || q.take(12) != c || q.take(12) != null || q.take(11) != b)
      throw new AssertionError("sensor timestamp pairing");
    ByteBuffer input = ByteBuffer.wrap(new byte[] {99, 1, 9, 2, 9, 9, 3, 9, 4});
    input.position(1);
    byte[] dest = new byte[6];
    ExposureFrames.copyPlane(input, 5, 2, 2, 2, dest, 1);
    if (!Arrays.equals(dest, new byte[] {0, 1, 2, 3, 4, 0}) || input.position() != 1)
      throw new AssertionError("pixel stride");
    ExposureFrames.copyPlane(input, 5, 1, 2, 2, dest, 0);
    if (!Arrays.equals(dest, new byte[] {1, 9, 3, 9, 4, 0}))
      throw new AssertionError("row stride");
    System.out.println("Exposure timestamp pairing and YUV stride tests passed.");
  }
}
