package com.sgame.overlay;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.graphics.PixelFormat;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;
import android.view.Gravity;
import android.view.WindowManager;

import java.io.DataInputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public class OverlayService extends Service {
    private static final String TAG = "sgame_overlay";
    // SELinux blocks untrusted_app -> untrusted_app abstract unix sockets,
    // so we use TCP loopback instead.
    private static final String SOCK_HOST = "127.0.0.1";
    private static final int SOCK_PORT = 47291;
    private static final int ACTOR_BYTES = 56;  // v2: 14 int32 fields, packed
    // Notification action: emergency stop that always works, even when the
    // overlay has wedged the touch stream.
    public static final String ACTION_STOP_SELF = "com.sgame.overlay.STOP_SELF";

    private WindowManager wm;
    private OverlayView view;
    private Thread sockThread;
    private volatile boolean running = false;

    @Override
    public IBinder onBind(Intent intent) { return null; }

    @Override
    public void onCreate() {
        super.onCreate();

        // 1) Window first — if this fails there is no point going foreground.
        wm = (WindowManager) getSystemService(WINDOW_SERVICE);
        view = new OverlayView(this);

        int flags = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                // Never intercept input: every touch falls through to the game.
                | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                // Even if some OEM ignores NOT_TOUCHABLE, do not block other windows.
                | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                // Let it grow over the status bar / cutout.
                | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                // Do NOT use LAYOUT_NO_LIMITS: on several OEM WMS builds an
                // unbounded full-screen overlay is treated as an input-owning
                // window and swallows the whole touch stream.
                | WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED;

        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
            WindowManager.LayoutParams.MATCH_PARENT,
            WindowManager.LayoutParams.MATCH_PARENT,
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                ? WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
                : WindowManager.LayoutParams.TYPE_PHONE,
            flags,
            PixelFormat.TRANSLUCENT
        );
        lp.gravity = Gravity.TOP | Gravity.START;
        lp.setTitle("sgame_esp_overlay");
        // Keep the window out of the touchable region entirely.
        lp.alpha = 1.0f;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            lp.layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }

        try {
            wm.addView(view, lp);
            Log.i(TAG, "overlay window added (not-touchable)");
        } catch (Exception e) {
            Log.e(TAG, "addView failed: " + e, e);
        }

        // 2) Foreground last, and never let a failure kill the overlay.
        try {
            startInForeground();
        } catch (Exception e) {
            Log.e(TAG, "startForeground failed (continuing anyway): " + e, e);
        }

        running = true;
        sockThread = new Thread(this::socketLoop, "sgame_esp_sock");
        sockThread.start();
    }

    private void startInForeground() {
        String ch = "sgame_overlay";
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            nm.createNotificationChannel(new NotificationChannel(
                ch, "sgame ESP overlay", NotificationManager.IMPORTANCE_LOW));
        }
        Notification.Builder b = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
            ? new Notification.Builder(this, ch)
            : new Notification.Builder(this);
        Notification.Action stopAction = new Notification.Action.Builder(
                android.R.drawable.ic_menu_close_clear_cancel,
                "停止 Overlay",
                android.app.PendingIntent.getService(
                    this, 1,
                    new Intent(this, OverlayService.class)
                        .setAction(ACTION_STOP_SELF),
                    android.app.PendingIntent.FLAG_IMMUTABLE))
                .build();

        Notification n = b.setContentTitle("sgame ESP overlay")
            .setContentText("等待 sgame 模块连接...")
            .setSmallIcon(android.R.drawable.ic_dialog_info)
            .setOngoing(true)
            .addAction(stopAction)
            .build();

        if (Build.VERSION.SDK_INT >= 34) {
            // manifest declares foregroundServiceType="specialUse"
            startForeground(1, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(1, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
        } else {
            startForeground(1, n);
        }
    }

    private void socketLoop() {
        while (running) {
            Socket sock = new Socket();
            try {
                sock.connect(new InetSocketAddress(SOCK_HOST, SOCK_PORT), 3000);
                Log.i(TAG, "connected " + SOCK_HOST + ":" + SOCK_PORT);
                DataInputStream in = new DataInputStream(sock.getInputStream());

                byte[] hdrBuf = new byte[8];
                while (running) {
                    in.readFully(hdrBuf);
                    if (hdrBuf[0]!='E' || hdrBuf[1]!='S' || hdrBuf[2]!='P' || hdrBuf[3]!='2') {
                        Log.e(TAG, "bad magic: "
                            + (char)hdrBuf[0] + (char)hdrBuf[1] + (char)hdrBuf[2] + (char)hdrBuf[3]);
                        break;
                    }
                    int count = ByteBuffer.wrap(hdrBuf, 4, 4)
                        .order(ByteOrder.LITTLE_ENDIAN).getInt();
                    if (count < 0 || count > 256) { Log.e(TAG, "count="+count); break; }

                    Actor[] actors = new Actor[count];
                    if (count > 0) {
                        byte[] body = new byte[count * ACTOR_BYTES];
                        in.readFully(body);
                        ByteBuffer bb = ByteBuffer.wrap(body).order(ByteOrder.LITTLE_ENDIAN);
                        for (int i = 0; i < count; i++) {
                            Actor a = new Actor();
                            a.key      = bb.getInt();
                            a.type     = bb.getInt();
                            a.configId = bb.getInt();
                            a.camp     = bb.getInt();
                            a.battleOrder = bb.getInt();
                            a.objId    = bb.getInt();
                            a.x = bb.getFloat();
                            a.y = bb.getFloat();
                            a.z = bb.getFloat();
                            a.fwdX = bb.getInt();
                            a.fwdY = bb.getInt();
                            a.fwdZ = bb.getInt();
                            a.hp    = bb.getInt();
                            a.maxHp = bb.getInt();
                            actors[i] = a;
                        }
                    }
                    view.setActors(actors);
                }
            } catch (Exception e) {
                Log.w(TAG, "socket error: " + e.getMessage());
            } finally {
                try { sock.close(); } catch (Exception ignored) {}
            }
            try { Thread.sleep(2000); } catch (InterruptedException ignored) {}
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_STOP_SELF.equals(intent.getAction())) {
            Log.i(TAG, "stop requested from notification");
            stopSelf();
            return START_NOT_STICKY;
        }
        // Never let the system restart us into a half-dead state.
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        running = false;
        // Remove the window FIRST so touch is restored even if the socket
        // thread is wedged inside a blocking read.
        if (view != null && wm != null) {
            try {
                wm.removeViewImmediate(view);
                Log.i(TAG, "overlay window removed");
            } catch (Exception e) {
                Log.w(TAG, "removeView failed: " + e);
            }
        }
        if (sockThread != null) {
            sockThread.interrupt();
            try { sockThread.join(1500); } catch (InterruptedException ignored) {}
        }
        super.onDestroy();
    }

    public static class Actor {
        public int key, type, configId, camp, battleOrder, objId;
        public float x, y, z;
        public int fwdX, fwdY, fwdZ;
        public int hp, maxHp;
    }
}
