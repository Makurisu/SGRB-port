package com.makurisu.sgrb;

import android.app.Activity;
import android.app.Dialog;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.Toast;
import java.io.File;
import java.io.IOException;

public class MainActivity extends Activity implements SurfaceHolder.Callback, TouchInputHandler.TouchListener {

    static {
        System.loadLibrary("sgre_wrapper");
    }

    public static native void nativeInit(Surface surface, String dataPath);
    public static native void nativeSendTouchEvent(int action, float normalizedX, float normalizedY);
    public static native void nativeSendKeyEvent(int keyCode, boolean isDown);
    public static native void nativeSendGamepadAxes(
        float leftX, float leftY, float rightX, float rightY,
        float triggerL, float triggerR, float hatX, float hatY
    );
    public static native void nativeSetAspectRatio16x9(boolean maintain16x9);
    public static native void nativeSetTargetFps(int targetFps);
    public static native void nativeSetLanguageOverride(String langCode);
    public static native float nativeGetCurrentFps();
    public static native float nativeGetCpuMemoryMb();
    public static native int nativeGetCpuTranslationStatus(String dataPath);
    public static native void nativePause();
    public static native void nativeResume();
    public static native void nativeDestroy();

    private SurfaceView surfaceView;
    private TouchInputHandler touchInputHandler;
    private QuickOverlayController overlayController;
    private GameConfig gameConfig;
    private Dialog activeDialog;
    private boolean surfaceReady = false;
    private boolean engineSurfaceAttached = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // Chế độ toàn màn hình tràn qua vùng khuyết (Display Cutout) trên màn hình tỷ lệ dài
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().setFlags(
            WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
            WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS
        );
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(lp);
        }
        hideSystemUI();

        gameConfig = GameConfig.load(this);

        FrameLayout rootFrame = new FrameLayout(this);
        rootFrame.setBackgroundColor(Color.BLACK);

        surfaceView = new SurfaceView(this);
        surfaceView.getHolder().setFormat(PixelFormat.RGBA_8888);
        rootFrame.addView(surfaceView, new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT
        ));

        touchInputHandler = new TouchInputHandler(this);
        touchInputHandler.setMaintainAspectRatio16x9(gameConfig.isMaintainAspectRatio16x9());

        overlayController = new QuickOverlayController(this, new QuickOverlayController.OverlayActionListener() {
            @Override
            public void onSendKeyPulse(int androidKeyCode) {
                nativeSendKeyEvent(androidKeyCode, true);
                nativeSendKeyEvent(androidKeyCode, false);
            }

            @Override
            public void onSetKeyHold(int androidKeyCode, boolean isDown) {
                nativeSendKeyEvent(androidKeyCode, isDown);
            }

            @Override
            public void onHideTextboxRightClick() {
                nativeSendTouchEvent(GameConfig.ACTION_RIGHT_CLICK, 0.5f, 0.5f);
            }

            @Override
            public void onOpenSettingsDialog() {
                openDiagnosticsDialog(false);
            }

            @Override
            public float onQueryCurrentFps() {
                return nativeGetCurrentFps();
            }

            @Override
            public float onQueryCpuMemoryMb() {
                return nativeGetCpuMemoryMb();
            }
        });
        rootFrame.addView(overlayController.buildOverlayView());
        setContentView(rootFrame);

        applyRuntimeConfigToNative();

        surfaceView.getHolder().addCallback(this);
        surfaceView.setOnTouchListener((v, event) ->
            touchInputHandler.handleTouchEvent(event, v.getWidth(), v.getHeight())
        );

        File appDataDir = getExternalFilesDir("data");
        if (appDataDir != null && !appDataDir.exists()) {
            appDataDir.mkdirs();
        }
    }

    private void applyRuntimeConfigToNative() {
        touchInputHandler.setMaintainAspectRatio16x9(gameConfig.isMaintainAspectRatio16x9());
        nativeSetAspectRatio16x9(gameConfig.isMaintainAspectRatio16x9());
        nativeSetTargetFps(gameConfig.getTargetFps());
        nativeSetLanguageOverride(gameConfig.getLanguageCode());
        if (overlayController != null) {
            overlayController.setTelemetryEnabled(gameConfig.isShowOverlayStats());
        }
    }

    private boolean checkStoragePermissionGranted() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            return Environment.isExternalStorageManager();
        }
        return true;
    }

    private void requestAllFilesPermission() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            try {
                Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
                intent.setData(Uri.parse("package:" + getPackageName()));
                startActivity(intent);
            } catch (Exception e) {
                try {
                    Intent fallback = new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION);
                    startActivity(fallback);
                } catch (Exception ignored) {
                    showShortToast("Không thể mở trang cấp quyền bộ nhớ hệ thống");
                }
            }
        }
    }

    private void tryBootOrShowSetup() {
        if (!surfaceReady || surfaceView == null) {
            return;
        }
        Surface surface = surfaceView.getHolder().getSurface();
        if (surface == null || !surface.isValid()) {
            return;
        }

        String dataDir = gameConfig.getDataDirectoryPath();
        boolean storageOk = checkStoragePermissionGranted();
        GameDataValidator.ValidationReport report = GameDataValidator.validateDataDirectory(new File(dataDir));

        if (storageOk && report.isReadyToLaunch()) {
            if (activeDialog != null && activeDialog.isShowing()) {
                activeDialog.dismiss();
            }
            applyRuntimeConfigToNative();
            nativeInit(surface, dataDir);
            engineSurfaceAttached = true;
        } else {
            openDiagnosticsDialog(true);
        }
    }

    private void openDiagnosticsDialog(boolean preLaunchBlockingMode) {
        if (isFinishing()) {
            return;
        }
        if (activeDialog != null && activeDialog.isShowing()) {
            activeDialog.dismiss();
        }
        activeDialog = SettingsDiagnosticsDialog.show(
            this,
            gameConfig,
            preLaunchBlockingMode,
            new SettingsDiagnosticsDialog.DialogActionCallback() {
                @Override
                public boolean hasAllFilesPermission() {
                    return checkStoragePermissionGranted();
                }

                @Override
                public void onRequestAllFilesPermission() {
                    requestAllFilesPermission();
                }

                @Override
                public void onApplySettings(
                    String dataDirPath,
                    boolean maintain16x9,
                    int targetFps,
                    boolean showHudStats,
                    String language
                ) {
                    if (!GameDataValidator.isSafeDirectoryPath(dataDirPath)) {
                        showShortToast("Đường dẫn dữ liệu không hợp lệ");
                        return;
                    }
                    gameConfig.setDataDirectoryPath(dataDirPath);
                    gameConfig.setMaintainAspectRatio16x9(maintain16x9);
                    gameConfig.setTargetFps(targetFps);
                    gameConfig.setShowOverlayStats(showHudStats);
                    gameConfig.setLanguageCode(language);
                    gameConfig.save(MainActivity.this);
                    GameDataValidator.writeConfiguredLanguage(new File(gameConfig.getDataDirectoryPath()), language);
                    applyRuntimeConfigToNative();
                    showShortToast("Đã lưu cấu hình hệ thống");
                }

                @Override
                public void onExportSaves(String dataDirPath) {
                    if (!GameDataValidator.isSafeDirectoryPath(dataDirPath)) {
                        showShortToast("Đường dẫn dữ liệu không hợp lệ");
                        return;
                    }
                    File root = new File(dataDirPath);
                    File zipOut = new File(root, GameConfig.SAVE_BACKUP_FILE_NAME);
                    try {
                        int count = GameDataValidator.exportSavesToZip(root, zipOut);
                        if (count > 0) {
                            showShortToast("Đã sao lưu " + count + " tệp vào " + GameConfig.SAVE_BACKUP_FILE_NAME);
                        } else {
                            showShortToast("Chưa có tệp lưu nào trong thư mục saves");
                        }
                    } catch (IOException | IllegalArgumentException e) {
                        showShortToast("Sao lưu tệp lưu thất bại, vui lòng kiểm tra quyền bộ nhớ");
                    }
                }

                @Override
                public void onImportSaves(String dataDirPath) {
                    if (!GameDataValidator.isSafeDirectoryPath(dataDirPath)) {
                        showShortToast("Đường dẫn dữ liệu không hợp lệ");
                        return;
                    }
                    File root = new File(dataDirPath);
                    File zipIn = new File(root, GameConfig.SAVE_BACKUP_FILE_NAME);
                    if (!zipIn.isFile()) {
                        showShortToast("Không tìm thấy tệp " + GameConfig.SAVE_BACKUP_FILE_NAME);
                        return;
                    }
                    try {
                        int count = GameDataValidator.importSavesFromZip(zipIn, root);
                        showShortToast("Đã khôi phục thành công " + count + " tệp lưu");
                    } catch (SecurityException se) {
                        showShortToast("Từ chối tệp nén do chứa đường dẫn không an toàn");
                    } catch (IOException | IllegalArgumentException e) {
                        showShortToast("Khôi phục tệp lưu thất bại");
                    }
                }

                @Override
                public void onLaunchOrResumeGame() {
                    tryBootOrShowSetup();
                }

                @Override
                public int onQueryCpuTranslationStatus(String dataDirPath) {
                    return nativeGetCpuTranslationStatus(dataDirPath);
                }
            }
        );
    }

    private void showShortToast(String messageVi) {
        Toast.makeText(this, messageVi, Toast.LENGTH_SHORT).show();
    }

    private void hideSystemUI() {
        View decorView = getWindow().getDecorView();
        decorView.setSystemUiVisibility(
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
        );
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        surfaceReady = true;
        tryBootOrShowSetup();
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        hideSystemUI();
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        surfaceReady = false;
        if (engineSurfaceAttached) {
            nativeDestroy();
            engineSurfaceAttached = false;
        }
    }

    @Override
    public void onTouchEventDispatched(int action, float normalizedX, float normalizedY) {
        nativeSendTouchEvent(action, normalizedX, normalizedY);
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (touchInputHandler != null) {
            View decorView = getWindow().getDecorView();
            int w = decorView.getWidth() > 0 ? decorView.getWidth() : GameConfig.TARGET_WIDTH;
            int h = decorView.getHeight() > 0 ? decorView.getHeight() : GameConfig.TARGET_HEIGHT;
            return touchInputHandler.handleTouchEvent(event, w, h);
        }
        return super.onTouchEvent(event);
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            nativeSendKeyEvent(KeyEvent.KEYCODE_BUTTON_B, true);
            return true;
        }
        nativeSendKeyEvent(keyCode, true);
        return true;
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            nativeSendKeyEvent(KeyEvent.KEYCODE_BUTTON_B, false);
            return true;
        }
        nativeSendKeyEvent(keyCode, false);
        return true;
    }

    @Override
    public void onBackPressed() {
        nativeSendKeyEvent(KeyEvent.KEYCODE_BUTTON_B, true);
        nativeSendKeyEvent(KeyEvent.KEYCODE_BUTTON_B, false);
    }

    @Override
    public boolean onGenericMotionEvent(MotionEvent event) {
        if (GamepadMotionMapper.isGamepadMotionEvent(event)) {
            GamepadMotionMapper.AxisSnapshot state = GamepadMotionMapper.extractAxes(event);
            nativeSendGamepadAxes(
                state.leftX, state.leftY,
                state.rightX, state.rightY,
                state.leftTrigger, state.rightTrigger,
                state.hatX, state.hatY
            );
            return true;
        }
        return super.onGenericMotionEvent(event);
    }

    @Override
    protected void onPause() {
        super.onPause();
        if (overlayController != null) {
            overlayController.onPause();
        }
        nativePause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemUI();
        if (overlayController != null) {
            overlayController.onResume();
        }
        nativeResume();
        if (surfaceReady && !engineSurfaceAttached) {
            tryBootOrShowSetup();
        }
    }
}
