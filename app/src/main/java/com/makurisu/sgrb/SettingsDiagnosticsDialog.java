package com.makurisu.sgrb;

import android.app.Activity;
import android.app.Dialog;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.widget.EditText;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;

/**
 * Hộp thoại tùy chỉnh kiểm tra dữ liệu khởi chạy, cấu hình tỷ lệ khung hình,
 * giới hạn FPS, ngôn ngữ Steam và sao lưu / khôi phục dữ liệu Save.
 */
public final class SettingsDiagnosticsDialog {

    public interface DialogActionCallback {
        boolean hasAllFilesPermission();
        void onRequestAllFilesPermission();
        void onApplySettings(String dataDirPath, boolean maintain16x9, int targetFps, boolean showHudStats, String language);
        void onExportSaves(String dataDirPath);
        void onImportSaves(String dataDirPath);
        void onLaunchOrResumeGame();
        int onQueryCpuTranslationStatus(String dataDirPath);
    }

    private SettingsDiagnosticsDialog() {
    }

    public static Dialog show(
        Activity activity,
        GameConfig config,
        boolean preLaunchBlockingMode,
        DialogActionCallback callback
    ) {
        Dialog dialog = new Dialog(activity);
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
        dialog.setCancelable(!preLaunchBlockingMode);

        ScrollView scrollRoot = new ScrollView(activity);
        scrollRoot.setFillViewport(true);
        scrollRoot.setBackground(QuickOverlayController.makeRoundedBackground("#F50E131B", "#6638BDF8", dp(activity, 12)));

        LinearLayout container = new LinearLayout(activity);
        container.setOrientation(LinearLayout.VERTICAL);
        container.setPadding(dp(activity, 20), dp(activity, 16), dp(activity, 20), dp(activity, 16));
        scrollRoot.addView(container);

        // Tiêu đề kiểu thiết bị phòng thí nghiệm Future Gadget Lab
        TextView titleView = new TextView(activity);
        titleView.setText(preLaunchBlockingMode
            ? "KIỂM TRA DỮ LIỆU KHỞI CHẠY — STEINS;GATE REBOOT"
            : "CÀI ĐẶT HỆ THỐNG & SAO LƯU — STEINS;GATE REBOOT");
        titleView.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
        titleView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f);
        titleView.setTextColor(Color.parseColor("#FF9E2C"));
        container.addView(titleView);

        String currentDir = config.getDataDirectoryPath();
        GameDataValidator.ValidationReport report = GameDataValidator.validateDataDirectory(new File(currentDir));
        boolean storageGranted = callback.hasAllFilesPermission();
        int abiStatus = callback.onQueryCpuTranslationStatus(currentDir);

        // Bảng trạng thái chẩn đoán
        LinearLayout statusBox = new LinearLayout(activity);
        statusBox.setOrientation(LinearLayout.VERTICAL);
        statusBox.setPadding(dp(activity, 12), dp(activity, 10), dp(activity, 12), dp(activity, 10));
        statusBox.setBackground(QuickOverlayController.makeRoundedBackground("#151C28", "#3364748B", dp(activity, 8)));
        LinearLayout.LayoutParams statusLp = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        );
        statusLp.setMargins(0, dp(activity, 10), 0, dp(activity, 12));
        container.addView(statusBox, statusLp);

        statusBox.addView(buildStatusLine(
            activity,
            storageGranted,
            storageGranted
                ? "Quyền truy cập bộ nhớ: Đã cấp quyền đọc/ghi dữ liệu game"
                : "Quyền truy cập bộ nhớ: Chưa cấp quyền quản lý tệp tin"
        ));

        String dataMsg = report.isReadyToLaunch()
            ? "Dữ liệu gốc PC: Hợp lệ (" + report.saveFileCount + " tệp lưu, video mở đầu: "
                + (report.openingMoviePresent ? "có" : "dự phòng") + ")"
            : "Thiếu tệp bắt buộc: " + String.join(", ", report.missingRequiredFiles);
        statusBox.addView(buildStatusLine(activity, report.isReadyToLaunch(), dataMsg));

        boolean abiReady = (abiStatus == 0 || abiStatus == 1);
        String abiText;
        if (abiStatus == 0) {
            abiText = "Kiến trúc thực thi: x86_64 trực tiếp / Hệ thống dịch nhị phân sẵn sàng";
        } else if (abiStatus == 1) {
            abiText = "Kiến trúc thực thi: ARM64 Box64 DynaRec đã nạp thành công";
        } else {
            abiText = "Kiến trúc ARM64 thuần: Chưa có libbox64.so (khuyến nghị cài bản x86_64Only nếu máy hỗ trợ ndk_translation hoặc chép libbox64.so vào thư mục game)";
        }
        statusBox.addView(buildStatusLine(activity, abiReady, abiText));

        if (!storageGranted) {
            TextView permBtn = buildActionButton(activity, "Cấp quyền truy cập tệp tin hệ thống", true);
            permBtn.setOnClickListener(v -> callback.onRequestAllFilesPermission());
            LinearLayout.LayoutParams permLp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            );
            permLp.setMargins(0, 0, 0, dp(activity, 10));
            container.addView(permBtn, permLp);
        }

        // Đường dẫn thư mục dữ liệu
        container.addView(buildSectionLabel(activity, "Đường dẫn thư mục dữ liệu game"));
        EditText pathInput = new EditText(activity);
        pathInput.setText(currentDir);
        pathInput.setSingleLine(true);
        pathInput.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f);
        pathInput.setTypeface(Typeface.MONOSPACE);
        pathInput.setTextColor(Color.parseColor("#F1F5F9"));
        pathInput.setHintTextColor(Color.parseColor("#64748B"));
        pathInput.setPadding(dp(activity, 10), dp(activity, 8), dp(activity, 10), dp(activity, 8));
        pathInput.setBackground(QuickOverlayController.makeRoundedBackground("#161E2B", "#55475569", dp(activity, 6)));
        container.addView(pathInput);

        // State tạm trong hộp thoại
        final boolean[] maintain16x9State = { config.isMaintainAspectRatio16x9() };
        final int[] targetFpsState = { config.getTargetFps() };
        final boolean[] showStatsState = { config.isShowOverlayStats() };
        final String[] langState = { config.getLanguageCode() };

        // Hàng tùy chọn hiển thị & hiệu năng
        container.addView(buildSectionLabel(activity, "Hiển thị, nhịp khung hình & ngôn ngữ"));
        LinearLayout optionsRow = new LinearLayout(activity);
        optionsRow.setOrientation(LinearLayout.HORIZONTAL);
        optionsRow.setGravity(Gravity.CENTER_VERTICAL);

        TextView aspectToggle = buildToggleChip(
            activity,
            maintain16x9State[0] ? "Khung: Chuẩn 16:9" : "Khung: Đầy màn hình",
            maintain16x9State[0]
        );
        aspectToggle.setOnClickListener(v -> {
            maintain16x9State[0] = !maintain16x9State[0];
            aspectToggle.setText(maintain16x9State[0] ? "Khung: Chuẩn 16:9" : "Khung: Đầy màn hình");
            styleToggleChip(activity, aspectToggle, maintain16x9State[0]);
        });
        optionsRow.addView(aspectToggle);

        TextView fpsToggle = buildToggleChip(
            activity,
            "Giới hạn: " + targetFpsState[0] + " FPS",
            targetFpsState[0] == 60
        );
        fpsToggle.setOnClickListener(v -> {
            targetFpsState[0] = (targetFpsState[0] == 60) ? 30 : 60;
            fpsToggle.setText("Giới hạn: " + targetFpsState[0] + " FPS");
            styleToggleChip(activity, fpsToggle, targetFpsState[0] == 60);
        });
        optionsRow.addView(fpsToggle);

        TextView statsToggle = buildToggleChip(
            activity,
            showStatsState[0] ? "FPS/RAM: Bật" : "FPS/RAM: Tắt",
            showStatsState[0]
        );
        statsToggle.setOnClickListener(v -> {
            showStatsState[0] = !showStatsState[0];
            statsToggle.setText(showStatsState[0] ? "FPS/RAM: Bật" : "FPS/RAM: Tắt");
            styleToggleChip(activity, statsToggle, showStatsState[0]);
        });
        optionsRow.addView(statsToggle);

        final String[] langCycle = { "english", "japanese", "schinese", "tchinese" };
        TextView langToggle = buildToggleChip(activity, "Ngôn ngữ: " + formatLangLabel(langState[0]), true);
        langToggle.setOnClickListener(v -> {
            int nextIdx = 0;
            for (int i = 0; i < langCycle.length; i++) {
                if (langCycle[i].equals(langState[0])) {
                    nextIdx = (i + 1) % langCycle.length;
                    break;
                }
            }
            langState[0] = langCycle[nextIdx];
            langToggle.setText("Ngôn ngữ: " + formatLangLabel(langState[0]));
        });
        optionsRow.addView(langToggle);
        container.addView(optionsRow);

        // Sao lưu & Khôi phục Save
        container.addView(buildSectionLabel(activity, "Quản lý dữ liệu lưu (saves_backup.zip trong thư mục game)"));
        LinearLayout backupRow = new LinearLayout(activity);
        backupRow.setOrientation(LinearLayout.HORIZONTAL);

        TextView exportBtn = buildActionButton(activity, "Sao lưu Save (.zip)", false);
        exportBtn.setOnClickListener(v -> callback.onExportSaves(pathInput.getText().toString().trim()));
        backupRow.addView(exportBtn);

        TextView importBtn = buildActionButton(activity, "Khôi phục Save (.zip)", false);
        LinearLayout.LayoutParams importLp = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        );
        importLp.setMargins(dp(activity, 8), 0, 0, 0);
        importBtn.setLayoutParams(importLp);
        importBtn.setOnClickListener(v -> callback.onImportSaves(pathInput.getText().toString().trim()));
        backupRow.addView(importBtn);
        container.addView(backupRow);

        // Nút hành động dưới cùng
        LinearLayout footerRow = new LinearLayout(activity);
        footerRow.setOrientation(LinearLayout.HORIZONTAL);
        footerRow.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
        LinearLayout.LayoutParams footerLp = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        );
        footerLp.setMargins(0, dp(activity, 16), 0, 0);

        if (!preLaunchBlockingMode) {
            TextView closeBtn = buildActionButton(activity, "Đóng", false);
            closeBtn.setOnClickListener(v -> dialog.dismiss());
            footerRow.addView(closeBtn);
        }

        String primaryLabel = preLaunchBlockingMode ? "Kiểm tra lại & Vào game" : "Lưu cài đặt";
        TextView applyBtn = buildActionButton(activity, primaryLabel, true);
        LinearLayout.LayoutParams applyLp = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        );
        applyLp.setMargins(dp(activity, 10), 0, 0, 0);
        applyBtn.setLayoutParams(applyLp);
        applyBtn.setOnClickListener(v -> {
            String enteredDir = pathInput.getText().toString().trim();
            callback.onApplySettings(
                enteredDir,
                maintain16x9State[0],
                targetFpsState[0],
                showStatsState[0],
                langState[0]
            );
            if (preLaunchBlockingMode) {
                GameDataValidator.ValidationReport recheck =
                    GameDataValidator.validateDataDirectory(new File(enteredDir));
                if (callback.hasAllFilesPermission() && recheck.isReadyToLaunch()) {
                    dialog.dismiss();
                    callback.onLaunchOrResumeGame();
                } else {
                    dialog.dismiss();
                    show(activity, config, true, callback);
                }
            } else {
                dialog.dismiss();
            }
        });
        footerRow.addView(applyBtn);
        container.addView(footerRow);

        dialog.setContentView(scrollRoot);
        Window win = dialog.getWindow();
        if (win != null) {
            win.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            int width = Math.min(
                dp(activity, 640),
                (int) (activity.getResources().getDisplayMetrics().widthPixels * 0.90f)
            );
            win.setLayout(width, ViewGroup.LayoutParams.WRAP_CONTENT);
        }
        dialog.show();
        return dialog;
    }

    private static View buildStatusLine(Activity activity, boolean ok, String text) {
        LinearLayout row = new LinearLayout(activity);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, dp(activity, 3), 0, dp(activity, 3));

        ImageView icon = new ImageView(activity);
        icon.setImageResource(ok ? R.drawable.ic_status_ok : R.drawable.ic_status_warn);
        LinearLayout.LayoutParams iconLp = new LinearLayout.LayoutParams(dp(activity, 16), dp(activity, 16));
        iconLp.setMargins(0, 0, dp(activity, 8), 0);
        row.addView(icon, iconLp);

        TextView tv = new TextView(activity);
        tv.setText(text);
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        tv.setTextColor(ok ? Color.parseColor("#CBD5E1") : Color.parseColor("#FCD34D"));
        row.addView(tv);
        return row;
    }

    private static TextView buildSectionLabel(Activity activity, String title) {
        TextView tv = new TextView(activity);
        tv.setText(title);
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f);
        tv.setTypeface(Typeface.SANS_SERIF, Typeface.BOLD);
        tv.setTextColor(Color.parseColor("#94A3B8"));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        );
        lp.setMargins(0, dp(activity, 10), 0, dp(activity, 5));
        tv.setLayoutParams(lp);
        return tv;
    }

    private static TextView buildToggleChip(Activity activity, String text, boolean active) {
        TextView tv = new TextView(activity);
        tv.setText(text);
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f);
        tv.setTypeface(Typeface.SANS_SERIF, Typeface.BOLD);
        tv.setPadding(dp(activity, 10), dp(activity, 7), dp(activity, 10), dp(activity, 7));
        styleToggleChip(activity, tv, active);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        );
        lp.setMargins(0, 0, dp(activity, 6), 0);
        tv.setLayoutParams(lp);
        return tv;
    }

    private static void styleToggleChip(Activity activity, TextView tv, boolean active) {
        tv.setTextColor(active ? Color.parseColor("#38BDF8") : Color.parseColor("#CBD5E1"));
        tv.setBackground(QuickOverlayController.makeRoundedBackground(
            active ? "#172536" : "#1A2230",
            active ? "#8838BDF8" : "#4464748B",
            dp(activity, 6)
        ));
    }

    private static TextView buildActionButton(Activity activity, String text, boolean primary) {
        TextView tv = new TextView(activity);
        tv.setText(text);
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        tv.setTypeface(Typeface.SANS_SERIF, Typeface.BOLD);
        tv.setTextColor(primary ? Color.parseColor("#0B0F15") : Color.parseColor("#E2E8F0"));
        tv.setPadding(dp(activity, 14), dp(activity, 8), dp(activity, 14), dp(activity, 8));
        tv.setBackground(QuickOverlayController.makeRoundedBackground(
            primary ? "#FF9E2C" : "#1E293B",
            primary ? "#FFD166" : "#5564748B",
            dp(activity, 7)
        ));
        return tv;
    }

    private static String formatLangLabel(String code) {
        switch (GameDataValidator.normalizeLanguageCode(code)) {
            case "japanese":
                return "JP";
            case "schinese":
                return "CHS";
            case "tchinese":
                return "CHT";
            default:
                return "EN";
        }
    }

    private static int dp(Activity activity, int value) {
        return Math.round(TypedValue.applyDimension(
            TypedValue.COMPLEX_UNIT_DIP,
            value,
            activity.getResources().getDisplayMetrics()
        ));
    }
}
