package com.sgame.overlay;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.view.View;

public class OverlayView extends View {
    private OverlayService.Actor[] actors = new OverlayService.Actor[0];
    private int dumpCounter = 0;
    private final Paint dotPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint bgPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint axisPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint hpBgPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint hpFgPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    // Minimap area in top-right corner
    private static final float MAP_SIZE_DP = 240;
    private static final float MAP_MARGIN_DP = 16;
    // sgame map world coordinate range (verified): roughly -65..+65 Unity units
    private static final float WORLD_HALF = 65f;
    // HP bar geometry (px, scaled by density at draw time)
    private static final float HP_BAR_W_DP = 30;
    private static final float HP_BAR_H_DP = 4;

    public OverlayView(Context ctx) {
        super(ctx);
        // Absolutely never consume touch: let every event reach the app below.
        setFocusable(false);
        setFocusableInTouchMode(false);
        setClickable(false);
        setLongClickable(false);
        setEnabled(false);
        setWillNotDraw(false);
        dotPaint.setStyle(Paint.Style.FILL);
        textPaint.setColor(Color.WHITE);
        textPaint.setTextSize(28);
        bgPaint.setColor(Color.argb(120, 0, 0, 0));
        axisPaint.setColor(Color.argb(80, 255, 255, 255));
        axisPaint.setStrokeWidth(1.5f);
        hpBgPaint.setColor(Color.argb(200, 40, 40, 40));
        hpFgPaint.setColor(Color.argb(255, 80, 255, 80));
    }

    @Override
    public boolean onTouchEvent(android.view.MotionEvent e) {
        return false;   // never consume
    }

    @Override
    public boolean onGenericMotionEvent(android.view.MotionEvent e) {
        return false;   // never consume (hover/stylus/mouse)
    }

    public void setActors(OverlayService.Actor[] a) {
        this.actors = a;
        postInvalidate();
    }

    @Override
    protected void onDraw(Canvas c) {
        super.onDraw(c);
        float density = getResources().getDisplayMetrics().density;
        float mapSize = MAP_SIZE_DP * density;
        float margin = MAP_MARGIN_DP * density;
        float left = getWidth() - mapSize - margin;
        float top = margin + 100;  // below status bar
        float cx = left + mapSize / 2;
        float cy = top + mapSize / 2;

        // Background panel
        c.drawRect(left, top, left + mapSize, top + mapSize, bgPaint);
        // Crosshair
        c.drawLine(left, cy, left + mapSize, cy, axisPaint);
        c.drawLine(cx, top, cx, top + mapSize, axisPaint);

        float scale = (mapSize / 2) / WORLD_HALF;
        float hpBarW = HP_BAR_W_DP * density;
        float hpBarH = HP_BAR_H_DP * density;

        int enemyCount = 0;
        StringBuilder dbg = new StringBuilder();
        for (OverlayService.Actor a : actors) {
            if (a.type != 0) continue;
            boolean enemy = (a.camp == 2);
            if (enemy) enemyCount++;

            float dx = cx + a.x * scale;
            float dy = cy - a.z * scale;
            float clampedX = Math.max(left + 14, Math.min(left + mapSize - 14, dx));
            float clampedY = Math.max(top + 14, Math.min(top + mapSize - 14, dy));
            boolean offMap = (clampedX != dx || clampedY != dy);

            int mainCol = enemy ? Color.argb(255, 255, 70, 70)
                                : Color.argb(255, 70, 160, 255);
            int haloCol = enemy ? Color.argb(offMap ? 50 : 100, 255, 70, 70)
                                : Color.argb(offMap ? 50 : 100, 70, 160, 255);
            dotPaint.setColor(haloCol);
            c.drawCircle(clampedX, clampedY, 22f, dotPaint);
            dotPaint.setColor(mainCol);
            c.drawCircle(clampedX, clampedY, 11f, dotPaint);

            // --- HP bar under the dot (only when hp is known) ---
            if (a.hp >= 0) {
                float bx = clampedX - hpBarW / 2;
                float by = clampedY + 16f;
                c.drawRect(bx, by, bx + hpBarW, by + hpBarH, hpBgPaint);
                float frac = 1.0f;
                if (a.maxHp > 0) {
                    frac = Math.max(0f, Math.min(1f, (float) a.hp / (float) a.maxHp));
                } else if (a.hp == 0) {
                    frac = 0f;
                }
                int col = frac > 0.5f ? Color.argb(255, 80, 255, 80)
                        : frac > 0.25f ? Color.argb(255, 255, 220, 60)
                        : Color.argb(255, 255, 70, 70);
                if (a.hp == 0) col = Color.argb(160, 120, 120, 120);
                hpFgPaint.setColor(col);
                c.drawRect(bx, by, bx + hpBarW * frac, by + hpBarH, hpFgPaint);
            }

            // Labels: battleOrder + hero id + hp number
            textPaint.setColor(Color.WHITE);
            textPaint.setTextSize(20);
            c.drawText("b" + a.battleOrder, clampedX - 14, clampedY - 16, textPaint);
            c.drawText(String.valueOf(a.configId % 10000), clampedX - 20, clampedY + 34, textPaint);
            if (a.hp >= 0) {
                String hpTxt = a.maxHp > 0 ? (a.hp + "/" + a.maxHp) : String.valueOf(a.hp);
                c.drawText(hpTxt, clampedX - 24, clampedY + 54, textPaint);
            }
            textPaint.setTextSize(28);
        }

        c.drawText("enemy=" + enemyCount + "  /" + actors.length,
                   left + 8, top + 30, textPaint);

        // Periodic log dump for debugging.
        dumpCounter++;
        if (dumpCounter >= 20) {
            dumpCounter = 0;
            for (OverlayService.Actor a : actors) {
                if (a.type != 0 || a.camp != 2) continue;
                if (dbg.length() < 800) {
                    dbg.append("b").append(a.battleOrder)
                       .append(" cfg=").append(a.configId)
                       .append(" (").append((int) a.x).append(",").append((int) a.z).append(")")
                       .append(" hp=").append(a.hp).append("/").append(a.maxHp)
                       .append(" | ");
                }
            }
            if (dbg.length() > 0) {
                android.util.Log.i("sgame_overlay", "enemies: " + dbg);
            }
        }
    }
}
