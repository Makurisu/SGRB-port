package com.makurisu.sgrb;

import android.view.MotionEvent;

public class TouchInputHandler {

    public interface TouchListener {
        void onTouchEventDispatched(int action, float normalizedX, float normalizedY);
    }

    private final TouchListener listener;
    private float startX;
    private float startY;
    private long touchDownTime;
    private static final int SWIPE_THRESHOLD = 120;
    private static final int LONG_PRESS_THRESHOLD_MS = 450;
    private boolean isHandledGesture = false;
    private boolean isMultiTouch = false;

    public TouchInputHandler(TouchListener listener) {
        this.listener = listener;
    }

    public boolean handleTouchEvent(MotionEvent event, int viewWidth, int viewHeight) {
        if (event == null) {
            return false;
        }
        return handleTouchEvent(event.getActionMasked(), event.getX(), event.getY(), viewWidth, viewHeight, System.currentTimeMillis());
    }

    public boolean handleTouchEvent(int actionMasked, float currentX, float currentY, int viewWidth, int viewHeight, long currentTimeMs) {
        if (listener == null || viewWidth <= 0 || viewHeight <= 0) {
            return false;
        }

        float normalizedX = Math.max(0.0f, Math.min(1.0f, currentX / (float) viewWidth));
        float normalizedY = Math.max(0.0f, Math.min(1.0f, currentY / (float) viewHeight));

        switch (actionMasked) {
            case 0: // MotionEvent.ACTION_DOWN
                startX = currentX;
                startY = currentY;
                touchDownTime = currentTimeMs;
                isHandledGesture = false;
                isMultiTouch = false;
                listener.onTouchEventDispatched(GameConfig.ACTION_TOUCH_DOWN, normalizedX, normalizedY);
                return true;

            case 5: // MotionEvent.ACTION_POINTER_DOWN
                // Nhận diện chạm 2 ngón tay -> Chuột phải (Back / Cancel / Hide dialog)
                if (!isMultiTouch) {
                    isMultiTouch = true;
                    isHandledGesture = true;
                    // Nhả chuột trái ngay lập tức để tránh click nhầm nút đang hover
                    listener.onTouchEventDispatched(GameConfig.ACTION_TOUCH_UP, normalizedX, normalizedY);
                    // Kích hoạt sự kiện Chuột phải
                    listener.onTouchEventDispatched(GameConfig.ACTION_RIGHT_CLICK, normalizedX, normalizedY);
                }
                return true;

            case 2: // MotionEvent.ACTION_MOVE
                if (isMultiTouch) {
                    return true;
                }
                float deltaX = currentX - startX;
                float deltaY = currentY - startY;
                long duration = currentTimeMs - touchDownTime;

                if (!isHandledGesture && duration > LONG_PRESS_THRESHOLD_MS && Math.abs(deltaX) < 40 && Math.abs(deltaY) < 40) {
                    isHandledGesture = true;
                    listener.onTouchEventDispatched(GameConfig.ACTION_LONG_PRESS, normalizedX, normalizedY);
                    return true;
                }

                listener.onTouchEventDispatched(GameConfig.ACTION_TOUCH_MOVE, normalizedX, normalizedY);
                return true;

            case 1: // MotionEvent.ACTION_UP
                if (isMultiTouch) {
                    isMultiTouch = false;
                    listener.onTouchEventDispatched(GameConfig.ACTION_TOUCH_UP, normalizedX, normalizedY);
                    return true;
                }
                float totalDeltaX = currentX - startX;
                float totalDeltaY = currentY - startY;

                if (!isHandledGesture) {
                    if (totalDeltaY < -SWIPE_THRESHOLD && Math.abs(totalDeltaY) > Math.abs(totalDeltaX)) {
                        listener.onTouchEventDispatched(GameConfig.ACTION_SWIPE_UP, normalizedX, normalizedY);
                        listener.onTouchEventDispatched(GameConfig.ACTION_TOUCH_UP, normalizedX, normalizedY);
                    } else if (totalDeltaY > SWIPE_THRESHOLD && Math.abs(totalDeltaY) > Math.abs(totalDeltaX)) {
                        listener.onTouchEventDispatched(GameConfig.ACTION_SWIPE_DOWN, normalizedX, normalizedY);
                        listener.onTouchEventDispatched(GameConfig.ACTION_TOUCH_UP, normalizedX, normalizedY);
                    } else if (totalDeltaX > SWIPE_THRESHOLD && Math.abs(totalDeltaX) > Math.abs(totalDeltaY)) {
                        listener.onTouchEventDispatched(GameConfig.ACTION_SWIPE_RIGHT, normalizedX, normalizedY);
                        listener.onTouchEventDispatched(GameConfig.ACTION_TOUCH_UP, normalizedX, normalizedY);
                    } else {
                        listener.onTouchEventDispatched(GameConfig.ACTION_TOUCH_UP, normalizedX, normalizedY);
                    }
                } else {
                    listener.onTouchEventDispatched(GameConfig.ACTION_TOUCH_UP, normalizedX, normalizedY);
                }
                return true;

            case 3: // MotionEvent.ACTION_CANCEL
                isMultiTouch = false;
                isHandledGesture = false;
                listener.onTouchEventDispatched(GameConfig.ACTION_TOUCH_UP, normalizedX, normalizedY);
                return true;

            default:
                return false;
        }
    }
}
