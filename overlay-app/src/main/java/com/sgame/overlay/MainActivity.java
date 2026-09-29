package com.sgame.overlay;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

public class MainActivity extends Activity {
    private TextView tv;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(50, 100, 50, 50);

        tv = new TextView(this);
        tv.setText("sgame ESP Overlay\n\n1. 授权悬浮窗权限\n2. 启动 Overlay\n3. 启动 sgame 进对战\n\n需要 Zygisk 模块已装且生效。");
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
            } else {
                tv.setText("已有悬浮窗权限");
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
            tv.setText("已启动 Overlay\n等待 sgame 模块连接 127.0.0.1:47291 ...");
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
