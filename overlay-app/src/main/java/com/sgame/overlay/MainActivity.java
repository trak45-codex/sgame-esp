package com.sgame.overlay;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

public class MainActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(50, 100, 50, 50);

        TextView tv = new TextView(this);
        tv.setText("sgame ESP Overlay\n\n1. 授权悬浮窗权限\n2. 启动 Overlay\n3. 启动 sgame 进对战");
        tv.setTextSize(18);
        root.addView(tv);

        Button btnPerm = new Button(this);
        btnPerm.setText("授权悬浮窗权限");
        btnPerm.setOnClickListener(v -> {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M
                && !Settings.canDrawOverlays(this)) {
                Intent i = new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                    Uri.parse("package:" + getPackageName()));
                startActivity(i);
            }
        });
        root.addView(btnPerm);

        Button btnStart = new Button(this);
        btnStart.setText("启动 Overlay");
        btnStart.setOnClickListener(v -> {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M
                && !Settings.canDrawOverlays(this)) {
                tv.setText("先授权悬浮窗权限!");
                return;
            }
            startForegroundService(new Intent(this, OverlayService.class));
            tv.setText("已启动 Overlay\n等待 sgame 连接...");
        });
        root.addView(btnStart);

        Button btnStop = new Button(this);
        btnStop.setText("停止 Overlay");
        btnStop.setOnClickListener(v -> {
            stopService(new Intent(this, OverlayService.class));
            tv.setText("已停止");
        });
        root.addView(btnStop);

        setContentView(root);
    }
}
