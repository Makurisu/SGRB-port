package com.makurisu.sgrb;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Handler;
import android.os.Looper;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.util.Locale;

/**
 * Thanh điều khiển nhanh nổi (Quick Overlay HUD) thu gọn ở góc trên bên phải.
 * Cung cấp phím tắt Điện thoại (Phone Trigger), Tự động, Tua nhanh, Lưu nhanh, Menu, Ẩn thoại
 * và đồng hồ đo FPS / RAM thời gian thực mà không che khuất khung thoại Visual Novel.
 */
public final class QuickOverlayController {

    public interface OverlayActionListener {
        void onSendKeyPulse(int androidKeyCode);
        void onSetKeyHold(int androidKeyCode, boolean isDown);
        void onHideTextboxRightClick();
        void onOpenSettingsDialog();
        float onQueryCurrentFps();
        float onQueryCpuMemoryMb();
    }

    private static final long IDLE_FADE_DELAY_MS = 3200L;
    private static final long TELEMETRY_INTERVAL_MS = 1000L;
    private static final float IDLE_ALPHA = 0.28f;
    private static final float ACTIVE_ALPHA = 0.96f;

    private final Activity activity;
    private final OverlayActionListener listener;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    private FrameLayout rootContainer;
    private LinearLayout hudRow;
    private LinearLayout actionStrip;
    private TextView telemetryView;
    private TextView skipBtnView;

    private boolean isExpanded = false;
    private boolean isSkipHolding = false;
    private boolean telemetryVisible = false;

    private final Runnable fadeRunnable = new Runnable() {
        @Override
        public void run() {
            if (!isExpanded && hudRow != null) {
                hudRow.animate().alpha(IDLE_ALPHA).setDuration(250L).start();
            }
        }
    };

    private final Runnable telemetryRunnable = new Runnable() {
        @Override
        public void run() {
            if (telemetryVisible && telemetryView != null) {
                float fps = listener.onQueryCurrentFps();
                float memMb = listener.onQueryCpuMemoryMb();
                telemetryView.setText(String.format(Locale.US, "%.1f FPS | %.0f MB", fps, memMb));
                mainHandler.postDelayed(this, TELEMETRY_INTERVAL_MS);
            }
        }
    };

    public QuickOverlayController(Activity activity, OverlayActionListener listener) {
        this.activity = activity;
        this.listener = listener;
    }

    public View buildOverlayView() {
        rootContainer = new FrameLayout(activity);
        rootContainer.setLayoutParams(new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT
        ));

        // Nhãn đo hiệu năng góc trên bên trái
        telemetryView = new TextView(activity);
        telemetryView.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
        telemetryView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f);
        telemetryView.setTextColor(Color.parseColor("#FF9E2C"));
        telemetryView.setPadding(dp(8), dp(4), dp(8), dp(4));
        telemetryView.setBackground(makeRoundedBackground("#CC0D1016", "#44FF9E2C", dp(6)));
        telemetryView.setVisibility(View.GONE);

        FrameLayout.LayoutParams teleLp = new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT,
            Gravity.TOP | Gravity.START
        );
        teleLp.setMargins(dp(14), dp(12), dp(14), dp(12));
        rootContainer.addView(telemetryView, teleLp);

        // Cụm phím nổi góc trên bên phải
        hudRow = new LinearLayout(activity);
        hudRow.setOrientation(LinearLayout.HORIZONTAL);
        hudRow.setGravity(Gravity.CENTER_VERTICAL);
        hudRow.setAlpha(ACTIVE_ALPHA);

        actionStrip = new LinearLayout(activity);
        actionStrip.setOrientation(LinearLayout.HORIZONTAL);
        actionStrip.setGravity(Gravity.CENTER_VERTICAL);
        actionStrip.setPadding(dp(6), dp(4), dp(6), dp(4));
        actionStrip.setBackground(makeRoundedBackground("#EB11161F", "#6638BDF8", dp(8)));
        actionStrip.setVisibility(View.GONE);

        // 1. Điện thoại (Phone Trigger - VK_DELETE / X)
        View phoneBtn = buildIconChip(R.drawable.ic_phone_trigger, "Đ.Thoại", v -> {
            releaseSkipIfActive();
            listener.onSendKeyPulse(KeyEvent.KEYCODE_BUTTON_X);
            scheduleIdleFade();
        });
        actionStrip.addView(phoneBtn);

        // 2. Tự động đọc (Auto - 'Q' / L1)
        actionStrip.addView(buildTextChip("Tự động", false, v -> {
            releaseSkipIfActive();
            listener.onSendKeyPulse(KeyEvent.KEYCODE_BUTTON_L1);
            scheduleIdleFade();
        }));

        // 3. Tua nhanh (Skip Hold - VK_CONTROL)
        skipBtnView = buildTextChip("Tua", false, v -> {
            isSkipHolding = !isSkipHolding;
            listener.onSetKeyHold(KeyEvent.KEYCODE_CTRL_LEFT, isSkipHolding);
            updateSkipChipAppearance();
            scheduleIdleFade();
        });
        actionStrip.addView(skipBtnView);

        // 4. Lưu nhanh (Quick Save - 'Z' / L2)
        actionStrip.addView(buildTextChip("Lưu nhanh", false, v -> {
            releaseSkipIfActive();
            listener.onSendKeyPulse(KeyEvent.KEYCODE_BUTTON_L2);
            scheduleIdleFade();
        }));

        // 5. Menu hệ thống / Tải game ('1' / START)
        actionStrip.addView(buildTextChip("Menu", false, v -> {
            releaseSkipIfActive();
            listener.onSendKeyPulse(KeyEvent.KEYCODE_BUTTON_START);
            scheduleIdleFade();
        }));

        // 6. Ẩn khung thoại (Chuột phải)
        actionStrip.addView(buildTextChip("Ẩn thoại", false, v -> {
            releaseSkipIfActive();
            listener.onHideTextboxRightClick();
            scheduleIdleFade();
        }));

        // 7. Cài đặt & Chẩn đoán
        ImageView gearBtn = new ImageView(activity);
        gearBtn.setImageResource(R.drawable.ic_settings_gear);
        gearBtn.setPadding(dp(8), dp(6), dp(8), dp(6));
        gearBtn.setBackground(makeRoundedBackground("#263040", "#5594A3B8", dp(6)));
        LinearLayout.LayoutParams gearLp = new LinearLayout.LayoutParams(dp(34), dp(30));
        gearLp.setMargins(dp(4), 0, 0, 0);
        gearBtn.setLayoutParams(gearLp);
        gearBtn.setOnClickListener(v -> {
            releaseSkipIfActive();
            setExpanded(false);
            listener.onOpenSettingsDialog();
        });
        actionStrip.addView(gearBtn);

        // Nút mở/đóng thanh phím tắt (hỗ trợ kéo dọc để không vướng UI trong game)
        ImageView toggleBtn = new ImageView(activity);
        toggleBtn.setImageResource(R.drawable.ic_hud_menu);
        toggleBtn.setPadding(dp(9), dp(9), dp(9), dp(9));
        toggleBtn.setBackground(makeRoundedBackground("#E611161F", "#88FF9E2C", dp(10)));
        LinearLayout.LayoutParams toggleLp = new LinearLayout.LayoutParams(dp(38), dp(38));
        toggleLp.setMargins(dp(6), 0, 0, 0);
        toggleBtn.setLayoutParams(toggleLp);
        attachDragAndClickHandler(toggleBtn);

        hudRow.addView(actionStrip);
        hudRow.addView(toggleBtn);

        FrameLayout.LayoutParams rowLp = new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT,
            Gravity.TOP | Gravity.END
        );
        rowLp.setMargins(dp(12), dp(12), dp(14), dp(12));
        rootContainer.addView(hudRow, rowLp);

        scheduleIdleFade();
        return rootContainer;
    }

    public void setTelemetryEnabled(boolean enabled) {
        this.telemetryVisible = enabled;
        mainHandler.removeCallbacks(telemetryRunnable);
        if (telemetryView != null) {
            telemetryView.setVisibility(enabled ? View.VISIBLE : View.GONE);
        }
        if (enabled) {
            mainHandler.post(telemetryRunnable);
        }
    }

    public void onPause() {
        releaseSkipIfActive();
        mainHandler.removeCallbacks(telemetryRunnable);
        mainHandler.removeCallbacks(fadeRunnable);
    }

    public void onResume() {
        if (telemetryVisible) {
            mainHandler.removeCallbacks(telemetryRunnable);
            mainHandler.post(telemetryRunnable);
        }
        scheduleIdleFade();
    }

    private void releaseSkipIfActive() {
        if (isSkipHolding) {
            isSkipHolding = false;
            listener.onSetKeyHold(KeyEvent.KEYCODE_CTRL_LEFT, false);
            updateSkipChipAppearance();
        }
    }

    private void updateSkipChipAppearance() {
        if (skipBtnView == null) {
            return;
        }
        if (isSkipHolding) {
            skipBtnView.setTextColor(Color.parseColor("#0B0F15"));
            skipBtnView.setBackground(makeRoundedBackground("#FF9E2C", "#FFD166", dp(6)));
        } else {
            skipBtnView.setTextColor(Color.parseColor("#E2E8F0"));
            skipBtnView.setBackground(makeRoundedBackground("#1E2633", "#4464748B", dp(6)));
        }
    }

    private void setExpanded(boolean expanded) {
        this.isExpanded = expanded;
        if (actionStrip != null) {
            actionStrip.setVisibility(expanded ? View.VISIBLE : View.GONE);
        }
        if (hudRow != null) {
            hudRow.animate().cancel();
            hudRow.setAlpha(ACTIVE_ALPHA);
        }
        scheduleIdleFade();
    }

    private void scheduleIdleFade() {
        mainHandler.removeCallbacks(fadeRunnable);
        if (!isExpanded) {
            mainHandler.postDelayed(fadeRunnable, IDLE_FADE_DELAY_MS);
        }
    }

    private void attachDragAndClickHandler(View toggleBtn) {
        toggleBtn.setOnTouchListener(new View.OnTouchListener() {
            private float downRawY;
            private int initialTopMargin;
            private boolean dragged;

            @Override
            public boolean onTouch(View v, MotionEvent event) {
                FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) hudRow.getLayoutParams();
                switch (event.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        downRawY = event.getRawY();
                        initialTopMargin = lp.topMargin;
                        dragged = false;
                        hudRow.animate().cancel();
                        hudRow.setAlpha(ACTIVE_ALPHA);
                        return true;
                    case MotionEvent.ACTION_MOVE:
                        float dy = event.getRawY() - downRawY;
                        if (Math.abs(dy) > dp(8)) {
                            dragged = true;
                        }
                        if (dragged && rootContainer != null) {
                            int maxTop = Math.max(dp(12), rootContainer.getHeight() - hudRow.getHeight() - dp(12));
                            lp.topMargin = Math.max(dp(8), Math.min(maxTop, initialTopMargin + Math.round(dy)));
                            hudRow.setLayoutParams(lp);
                        }
                        return true;
                    case MotionEvent.ACTION_UP:
                        if (!dragged) {
                            setExpanded(!isExpanded);
                        } else {
                            scheduleIdleFade();
                        }
                        return true;
                    case MotionEvent.ACTION_CANCEL:
                        scheduleIdleFade();
                        return true;
                    default:
                        return false;
                }
            }
        });
    }

    private TextView buildTextChip(String label, boolean highlight, View.OnClickListener onClick) {
        TextView tv = new TextView(activity);
        tv.setText(label);
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f);
        tv.setTypeface(Typeface.SANS_SERIF, Typeface.BOLD);
        tv.setTextColor(highlight ? Color.parseColor("#0B0F15") : Color.parseColor("#E2E8F0"));
        tv.setPadding(dp(9), dp(6), dp(9), dp(6));
        tv.setBackground(makeRoundedBackground(
            highlight ? "#FF9E2C" : "#1E2633",
            highlight ? "#FFD166" : "#4464748B",
            dp(6)
        ));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        );
        lp.setMargins(dp(3), 0, dp(3), 0);
        tv.setLayoutParams(lp);
        tv.setOnClickListener(onClick);
        return tv;
    }

    private View buildIconChip(int drawableRes, String label, View.OnClickListener onClick) {
        LinearLayout box = new LinearLayout(activity);
        box.setOrientation(LinearLayout.HORIZONTAL);
        box.setGravity(Gravity.CENTER_VERTICAL);
        box.setPadding(dp(8), dp(5), dp(9), dp(5));
        box.setBackground(makeRoundedBackground("#1E2633", "#66FF9E2C", dp(6)));

        ImageView icon = new ImageView(activity);
        icon.setImageResource(drawableRes);
        LinearLayout.LayoutParams iconLp = new LinearLayout.LayoutParams(dp(15), dp(15));
        iconLp.setMargins(0, 0, dp(4), 0);
        box.addView(icon, iconLp);

        TextView tv = new TextView(activity);
        tv.setText(label);
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f);
        tv.setTypeface(Typeface.SANS_SERIF, Typeface.BOLD);
        tv.setTextColor(Color.parseColor("#FF9E2C"));
        box.addView(tv);

        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        );
        lp.setMargins(dp(2), 0, dp(3), 0);
        box.setLayoutParams(lp);
        box.setOnClickListener(onClick);
        return box;
    }

    static GradientDrawable makeRoundedBackground(String fillHex, String strokeHex, int radiusPx) {
        GradientDrawable gd = new GradientDrawable();
        gd.setShape(GradientDrawable.RECTANGLE);
        gd.setCornerRadius(radiusPx);
        gd.setColor(Color.parseColor(fillHex));
        gd.setStroke(2, Color.parseColor(strokeHex));
        return gd;
    }

    private int dp(int value) {
        return Math.round(TypedValue.applyDimension(
            TypedValue.COMPLEX_UNIT_DIP,
            value,
            activity.getResources().getDisplayMetrics()
        ));
    }
}
