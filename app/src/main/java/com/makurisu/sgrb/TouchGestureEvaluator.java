package com.makurisu.sgrb;

public class TouchGestureEvaluator {

    public interface GestureListener {
        void onGestureRecognized(int action, float normalizedX, float normalizedY);
    }

    private final GestureListener listener;
    private float startX;
    private float startY;
    private long touchDownTime;
    private static final int SWIPE_THRESHOLD = 120;
    private static final int LONG_PRESS_THRESHOLD_MS = 450;
    private boolean isHandledGesture = false;

    public TouchGestureEvaluator(GestureListener listener) {
        this.listener = listener;
    }

    public void processDown(float x, float y, int viewWidth, int viewHeight, long timestamp) {
        startX = x;
        startY = y;
        touchDownTime = timestamp;
        isHandledGesture = false;
        if (listener != null && viewWidth > 0 && viewHeight > 0) {
            listener.onGestureRecognized(GameConfig.ACTION_TOUCH_DOWN, x / viewWidth, y / viewHeight);
        }
    }

    public void processMove(float x, float y, int viewWidth, int viewHeight, long timestamp) {
        if (listener == null || viewWidth <= 0 || viewHeight <= 0) return;
        float deltaX = x - startX;
        float deltaY = y - startY;
        long duration = timestamp - touchDownTime;

        if (!isHandledGesture && duration > LONG_PRESS_THRESHOLD_MS && Math.abs(deltaX) < 40 && Math.abs(deltaY) < 40) {
            isHandledGesture = true;
            listener.onGestureRecognized(GameConfig.ACTION_LONG_PRESS, x / viewWidth, y / viewHeight);
            return;
        }

        listener.onGestureRecognized(GameConfig.ACTION_TOUCH_MOVE, x / viewWidth, y / viewHeight);
    }

    public void processUp(float x, float y, int viewWidth, int viewHeight) {
        if (listener == null || viewWidth <= 0 || viewHeight <= 0) return;
        float totalDeltaX = x - startX;
        float totalDeltaY = y - startY;

        if (!isHandledGesture) {
            if (totalDeltaY < -SWIPE_THRESHOLD && Math.abs(totalDeltaY) > Math.abs(totalDeltaX)) {
                listener.onGestureRecognized(GameConfig.ACTION_SWIPE_UP, x / viewWidth, y / viewHeight);
            } else if (totalDeltaY > SWIPE_THRESHOLD && Math.abs(totalDeltaY) > Math.abs(totalDeltaX)) {
                listener.onGestureRecognized(GameConfig.ACTION_SWIPE_DOWN, x / viewWidth, y / viewHeight);
            } else if (totalDeltaX > SWIPE_THRESHOLD && Math.abs(totalDeltaX) > Math.abs(totalDeltaY)) {
                listener.onGestureRecognized(GameConfig.ACTION_SWIPE_RIGHT, x / viewWidth, y / viewHeight);
            } else {
                listener.onGestureRecognized(GameConfig.ACTION_TOUCH_UP, x / viewWidth, y / viewHeight);
            }
        } else {
            listener.onGestureRecognized(GameConfig.ACTION_TOUCH_UP, x / viewWidth, y / viewHeight);
        }
    }
}
