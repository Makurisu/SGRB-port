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

    public TouchInputHandler(TouchListener listener) {
        this.listener = listener;
    }

    public boolean handleTouchEvent(MotionEvent event, int viewWidth, int viewHeight) {
        if (listener == null || viewWidth <= 0 || viewHeight <= 0) {
            return false;
        }

        float currentX = event.getX();
        float currentY = event.getY();
        float normalizedX = Math.max(0.0f, Math.min(1.0f, currentX / (float) viewWidth));
        float normalizedY = Math.max(0.0f, Math.min(1.0f, currentY / (float) viewHeight));

        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                startX = currentX;
                startY = currentY;
                touchDownTime = System.currentTimeMillis();
                isHandledGesture = false;
                listener.onTouchEventDispatched(GameConfig.ACTION_TOUCH_DOWN, normalizedX, normalizedY);
                return true;

            case MotionEvent.ACTION_MOVE:
                float deltaX = currentX - startX;
                float deltaY = currentY - startY;
                long duration = System.currentTimeMillis() - touchDownTime;

                if (!isHandledGesture && duration > LONG_PRESS_THRESHOLD_MS && Math.abs(deltaX) < 40 && Math.abs(deltaY) < 40) {
                    isHandledGesture = true;
                    listener.onTouchEventDispatched(GameConfig.ACTION_LONG_PRESS, normalizedX, normalizedY);
                    return true;
                }

                listener.onTouchEventDispatched(GameConfig.ACTION_TOUCH_MOVE, normalizedX, normalizedY);
                return true;

            case MotionEvent.ACTION_UP:
                float totalDeltaX = currentX - startX;
                float totalDeltaY = currentY - startY;

                if (!isHandledGesture) {
                    if (totalDeltaY < -SWIPE_THRESHOLD && Math.abs(totalDeltaY) > Math.abs(totalDeltaX)) {
                        listener.onTouchEventDispatched(GameConfig.ACTION_SWIPE_UP, normalizedX, normalizedY);
                    } else if (totalDeltaY > SWIPE_THRESHOLD && Math.abs(totalDeltaY) > Math.abs(totalDeltaX)) {
                        listener.onTouchEventDispatched(GameConfig.ACTION_SWIPE_DOWN, normalizedX, normalizedY);
                    } else if (totalDeltaX > SWIPE_THRESHOLD && Math.abs(totalDeltaX) > Math.abs(totalDeltaY)) {
                        listener.onTouchEventDispatched(GameConfig.ACTION_SWIPE_RIGHT, normalizedX, normalizedY);
                    } else {
                        listener.onTouchEventDispatched(GameConfig.ACTION_TOUCH_UP, normalizedX, normalizedY);
                    }
                } else {
                    listener.onTouchEventDispatched(GameConfig.ACTION_TOUCH_UP, normalizedX, normalizedY);
                }
                return true;

            default:
                return false;
        }
    }
}
