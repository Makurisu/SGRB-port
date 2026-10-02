package com.makurisu.sgrb;

import android.app.Activity;
import android.os.Bundle;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.Toast;

import java.io.File;

public class MainActivity extends Activity implements SurfaceHolder.Callback, TouchInputHandler.TouchListener {

    static {
        System.loadLibrary("sgre_wrapper");
    }

    public static native void nativeInit(Surface surface, String dataPath);
    public static native void nativeSendTouchEvent(int action, float normalizedX, float normalizedY);
    public static native void nativePause();
    public static native void nativeResume();
    public static native void nativeDestroy();

    private SurfaceView surfaceView;
    private TouchInputHandler touchInputHandler;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // Chế độ toàn màn hình không viền (Full immersive mode)
        getWindow().setFlags(
                WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
                WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS
        );
        hideSystemUI();

        surfaceView = new SurfaceView(this);
        setContentView(surfaceView);

        touchInputHandler = new TouchInputHandler(this);

        surfaceView.getHolder().addCallback(this);
        surfaceView.setOnTouchListener((v, event) ->
                touchInputHandler.handleTouchEvent(event, v.getWidth(), v.getHeight())
        );

        // Kiểm tra thư mục dữ liệu game
        File dataDir = getExternalFilesDir("data");
        if (dataDir != null && !dataDir.exists()) {
            boolean created = dataDir.mkdirs();
            if (created) {
                Toast.makeText(this, "Đã tạo thư mục data cho game.", Toast.LENGTH_SHORT).show();
            }
        }
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
        File dataDir = getExternalFilesDir("data");
        String dataPath = dataDir != null ? dataDir.getAbsolutePath() : "";
        nativeInit(holder.getSurface(), dataPath);
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        // Xử lý khi kích thước màn hình thay đổi (xoay chiều)
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        nativeDestroy();
    }

    @Override
    public void onTouchEventDispatched(int action, float normalizedX, float normalizedY) {
        nativeSendTouchEvent(action, normalizedX, normalizedY);
    }

    @Override
    protected void onPause() {
        super.onPause();
        nativePause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemUI();
        nativeResume();
    }
}
