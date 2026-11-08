// SPDX-License-Identifier: MPL-2.0
package com.oculus;

import android.annotation.SuppressLint;
import android.content.Context;
import android.graphics.ImageFormat;
import android.graphics.Rect;
import android.hardware.camera2.*;
import android.media.Image;
import android.media.ImageReader;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.util.Size;
import java.io.*;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Collections;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import org.json.JSONObject;

/** Opt-in single exposures over authenticated local ADB. Never runs on the XR frame thread. */
final class StationCamera implements AutoCloseable {
  static final String SOCKET = "oxrsys_station_camera_v1";
  private static final int W = 1280, H = 960;
  private final CameraManager manager;
  private final HandlerThread cameraThread = new HandlerThread("OXRSys-station-camera");
  private Handler handler;
  private volatile boolean closed;
  private volatile String status = "opening passthrough camera";
  private LocalServerSocket server;
  private LocalSocket client;
  // Camera objects and pending shot are confined to the camera looper.
  private CameraDevice device;
  private CameraCaptureSession session;
  private ImageReader reader;
  private String cameraId;
  private Rect active;
  private boolean disableDistortion;
  private CaptureRequest preview;
  private Shot pending;

  private static final class Shot {
    final CountDownLatch done = new CountDownLatch(1);
    long resultTime = -1, imageTime = -2;
    byte[] pixels;
    // ImageReader and CaptureCallback can be scheduled in either order.
    final ExposureFrames early = new ExposureFrames();
    JSONObject metadata;
    String error;
  }

  StationCamera(Context context) {
    manager = context.getSystemService(CameraManager.class);
  }

  void start() {
    cameraThread.start();
    handler = new Handler(cameraThread.getLooper());
    handler.post(this::openCamera);
    new Thread(this::serve, "OXRSys-station-adb").start();
  }

  @SuppressLint("MissingPermission")
  private void openCamera() {
    try {
      CameraCharacteristics.Key<Integer> source =
          new CameraCharacteristics.Key<>("com.meta.extra_metadata.camera_source", Integer.class);
      CameraCharacteristics.Key<Integer> position =
          new CameraCharacteristics.Key<>("com.meta.extra_metadata.position", Integer.class);
      for (String id : manager.getCameraIdList()) {
        CameraCharacteristics c = manager.getCameraCharacteristics(id);
        if (!Integer.valueOf(0).equals(c.get(source))
            || !Integer.valueOf(0).equals(c.get(position)))
          continue;
        Size[] sizes = c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
                           .getOutputSizes(ImageFormat.YUV_420_888);
        if (sizes == null || !Arrays.asList(sizes).contains(new Size(W, H)))
          continue;
        cameraId = id;
        active = c.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE);
        int[] modes = c.get(CameraCharacteristics.DISTORTION_CORRECTION_AVAILABLE_MODES);
        if (modes != null)
          for (int mode : modes)
            if (mode == CaptureRequest.DISTORTION_CORRECTION_MODE_OFF)
              disableDistortion = true;
        reader = ImageReader.newInstance(W, H, ImageFormat.YUV_420_888, 3);
        reader.setOnImageAvailableListener(this::readImage, handler);
        manager.openCamera(id, new CameraDevice.StateCallback() {
          public void onOpened(CameraDevice d) {
            if (closed) {
              d.close();
              return;
            }
            device = d;
            try {
              CaptureRequest.Builder b = d.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
              b.addTarget(reader.getSurface());
              java.util.List<CaptureRequest.Key<?>> keys = c.getAvailableCaptureRequestKeys();
              int[] focusModes = c.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES);
              if (keys.contains(CaptureRequest.CONTROL_AF_MODE) && focusModes != null
                  && Arrays.stream(focusModes)
                      .anyMatch(mode -> mode == CaptureRequest.CONTROL_AF_MODE_OFF)) {
                b.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_OFF);
                if (keys.contains(CaptureRequest.LENS_FOCUS_DISTANCE))
                  b.set(CaptureRequest.LENS_FOCUS_DISTANCE, 0f);
              }
              if (active != null && keys.contains(CaptureRequest.SCALER_CROP_REGION))
                b.set(CaptureRequest.SCALER_CROP_REGION, active);
              if (disableDistortion && keys.contains(CaptureRequest.DISTORTION_CORRECTION_MODE))
                b.set(CaptureRequest.DISTORTION_CORRECTION_MODE,
                    CaptureRequest.DISTORTION_CORRECTION_MODE_OFF);
              preview = b.build();
              d.createCaptureSession(Collections.singletonList(reader.getSurface()),
                  new CameraCaptureSession.StateCallback() {
                    public void onConfigured(CameraCaptureSession s) {
                      if (closed) {
                        s.close();
                        return;
                      }
                      session = s;
                      try {
                        s.setRepeatingRequest(preview, null, handler);
                        status = "ready";
                      } catch (Exception e) {
                        failCamera(e.toString());
                      }
                    }
                    public void onConfigureFailed(CameraCaptureSession s) {
                      failCamera("YUV capture session failed");
                    }
                  },
                  handler);
            } catch (Exception e) {
              failCamera(e.toString());
            }
          }
          public void onDisconnected(CameraDevice d) {
            d.close();
            failCamera("camera disconnected");
          }
          public void onError(CameraDevice d, int error) {
            d.close();
            failCamera("camera error " + error);
          }
        }, handler);
        return;
      }
      failCamera("no supported left passthrough camera at 1280x960");
    } catch (Exception e) {
      failCamera(e.toString());
    }
  }

  private void failCamera(String reason) {
    status = reason;
    session = null;
    if (pending != null)
      finish(pending, reason);
    Log.w("OXRSys-StationCamera", reason);
  }

  private void finish(Shot shot, String error) {
    if (pending == shot)
      pending = null;
    shot.error = error;
    shot.done.countDown();
  }

  private void capture(Shot shot) {
    if (closed || session == null || !"ready".equals(status)) {
      finish(shot, status);
      return;
    }
    pending = shot;
    try {
      session.capture(preview, new CameraCaptureSession.CaptureCallback() {
        public void onCaptureStarted(
            CameraCaptureSession s, CaptureRequest r, long timestamp, long frame) {
          if (pending == shot) {
            shot.resultTime = timestamp;
            byte[] previous = shot.early.take(timestamp);
            if (previous != null) {
              shot.imageTime = timestamp;
              shot.pixels = previous;
            }
          }
        }
        public void onCaptureCompleted(
            CameraCaptureSession s, CaptureRequest r, TotalCaptureResult res) {
          if (pending != shot)
            return;
          try {
            Long t = res.get(CaptureResult.SENSOR_TIMESTAMP);
            if (t == null) {
              finish(shot, "no exposure timestamp");
              return;
            }
            shot.resultTime = t;
            byte[] previous = shot.early.take(t);
            if (previous != null) {
              shot.imageTime = t;
              shot.pixels = previous;
            }
            Rect crop = res.get(CaptureResult.SCALER_CROP_REGION);
            JSONObject m = new JSONObject();
            m.put("version", 1);
            m.put("id", cameraId);
            m.put("width", W);
            m.put("height", H);
            m.put("timestamp_ns", t);
            m.put("crop", crop == null ? "unknown" : crop.flattenToString());
            m.put("distortion", String.valueOf(res.get(CaptureResult.DISTORTION_CORRECTION_MODE)));
            m.put("focus", String.valueOf(res.get(CaptureResult.LENS_FOCUS_DISTANCE)));
            m.put("build", android.os.Build.FINGERPRINT);
            shot.metadata = m;
            if (shot.imageTime == t)
              finish(shot, null);
          } catch (Exception e) {
            finish(shot, e.toString());
          }
        }
        public void onCaptureFailed(CameraCaptureSession s, CaptureRequest r, CaptureFailure f) {
          if (pending == shot)
            finish(shot, "exposure failed");
        }
      }, handler);
    } catch (Exception e) {
      finish(shot, e.toString());
    }
  }

  private void readImage(ImageReader input) {
    // acquireNextImage preserves the single capture among the AE preview frames.
    try (Image im = input.acquireNextImage()) {
      Shot shot = pending;
      if (im == null || shot == null
          || (shot.resultTime >= 0 && im.getTimestamp() != shot.resultTime))
        return;
      Rect crop = im.getCropRect();
      if (crop.left != 0 || crop.top != 0 || crop.width() != W || crop.height() != H) {
        finish(shot, "unexpected buffer crop");
        return;
      }
      byte[] out = new byte[W * H * 3 / 2];
      Image.Plane[] planes = im.getPlanes();
      for (int idx = 0; idx < 3; idx++) {
        ByteBuffer buf = planes[idx].getBuffer();
        int h = idx == 0 ? H : H / 2, w = idx == 0 ? W : W / 2,
            offset = idx == 0 ? 0 : W * H + (idx - 1) * W * H / 4;
        ExposureFrames.copyPlane(
            buf, planes[idx].getRowStride(), planes[idx].getPixelStride(), w, h, out, offset);
      }
      if (shot.resultTime < 0) {
        shot.early.add(im.getTimestamp(), out);
        return;
      }
      shot.pixels = out;
      shot.imageTime = im.getTimestamp();
      if (shot.metadata != null)
        finish(shot, null);
    } catch (Exception e) {
      if (pending != null)
        finish(pending, e.toString());
    }
  }

  private void serve() {
    try (LocalServerSocket listener = new LocalServerSocket(SOCKET)) {
      synchronized (this) {
        if (closed)
          return;
        server = listener;
      }
      while (!closed) try (LocalSocket peer = listener.accept()) {
          synchronized (this) {
            if (closed)
              return;
            client = peer;
          }
          int uid = peer.getPeerCredentials().getUid();
          if (uid != 2000 && uid != 0)
            continue; // adbd shell or debug-root only
          peer.setSoTimeout(2500);
          if (peer.getInputStream().read() != 1)
            continue;
          Shot shot = new Shot();
          handler.post(() -> capture(shot));
          boolean ready = shot.done.await(1500, TimeUnit.MILLISECONDS);
          if (!ready)
            handler.post(() -> {
              if (pending == shot)
                finish(shot, "timed out");
            });
          JSONObject meta = ready && shot.error == null && shot.metadata != null
              ? shot.metadata
              : new JSONObject().put("error", ready ? shot.error : "exposure timeout");
          byte[] header = meta.toString().getBytes(StandardCharsets.UTF_8);
          DataOutputStream stream = new DataOutputStream(peer.getOutputStream());
          stream.writeInt(header.length);
          stream.write(header);
          if (!meta.has("error") && shot.pixels != null)
            stream.write(shot.pixels);
          stream.flush();
        } catch (Exception e) {
          if (!closed)
            Log.w("OXRSys-StationCamera", "snapshot unavailable: " + e.getClass().getSimpleName());
        }
    } catch (IOException e) {
      if (!closed)
        Log.w("OXRSys-StationCamera", e.toString());
    }
  }

  public synchronized void close() {
    closed = true;
    try {
      if (server != null)
        server.close();
    } catch (IOException ignored) {
    }
    try {
      if (client != null)
        client.close();
    } catch (IOException ignored) {
    }
    handler.post(() -> {
      if (pending != null)
        finish(pending, "paused");
      if (session != null)
        session.close();
      if (device != null)
        device.close();
      if (reader != null)
        reader.close();
      cameraThread.quitSafely();
    });
  }
}
