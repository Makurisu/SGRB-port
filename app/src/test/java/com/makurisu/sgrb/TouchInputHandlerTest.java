package com.makurisu.sgrb;

import org.junit.Before;
import org.junit.Test;
import java.util.ArrayList;
import java.util.List;

import static org.junit.Assert.*;

public class TouchInputHandlerTest {

    private static class RecordedEvent {
        final int action;
        final float x;
        final float y;

        RecordedEvent(int action, float x, float y) {
            this.action = action;
            this.x = x;
            this.y = y;
        }
    }

    private List<RecordedEvent> recordedEvents;
    private TouchInputHandler handler;
    private static final int VIEW_WIDTH = 1920;
    private static final int VIEW_HEIGHT = 1080;

    @Before
    public void setUp() {
        recordedEvents = new ArrayList<>();
        handler = new TouchInputHandler((action, normalizedX, normalizedY) ->
                recordedEvents.add(new RecordedEvent(action, normalizedX, normalizedY))
        );
    }

    @Test
    public void testTouchDownAndUpNormalTap() {
        // Tap thông thường tại giữa màn hình (960, 540)
        boolean downHandled = handler.handleTouchEvent(0, 960f, 540f, VIEW_WIDTH, VIEW_HEIGHT, 1000L);
        assertTrue(downHandled);
        assertEquals(1, recordedEvents.size());
        assertEquals(GameConfig.ACTION_TOUCH_DOWN, recordedEvents.get(0).action);
        assertEquals(0.5f, recordedEvents.get(0).x, 0.001f);
        assertEquals(0.5f, recordedEvents.get(0).y, 0.001f);

        // Nhả tay sau 50ms
        boolean upHandled = handler.handleTouchEvent(1, 960f, 540f, VIEW_WIDTH, VIEW_HEIGHT, 1050L);
        assertTrue(upHandled);
        assertEquals(2, recordedEvents.size());
        assertEquals(GameConfig.ACTION_TOUCH_UP, recordedEvents.get(1).action);
        assertEquals(0.5f, recordedEvents.get(1).x, 0.001f);
        assertEquals(0.5f, recordedEvents.get(1).y, 0.001f);
    }

    @Test
    public void testTouchMove() {
        handler.handleTouchEvent(0, 100f, 100f, VIEW_WIDTH, VIEW_HEIGHT, 1000L);
        handler.handleTouchEvent(2, 110f, 110f, VIEW_WIDTH, VIEW_HEIGHT, 1020L);

        assertEquals(2, recordedEvents.size());
        assertEquals(GameConfig.ACTION_TOUCH_MOVE, recordedEvents.get(1).action);
        assertEquals(110f / VIEW_WIDTH, recordedEvents.get(1).x, 0.001f);
    }

    @Test
    public void testTwoFingerRightClick() {
        // Chạm ngón 1
        handler.handleTouchEvent(0, 500f, 500f, VIEW_WIDTH, VIEW_HEIGHT, 1000L);
        // Chạm ngón 2 (ACTION_POINTER_DOWN = 5)
        boolean pointerHandled = handler.handleTouchEvent(5, 600f, 500f, VIEW_WIDTH, VIEW_HEIGHT, 1020L);
        assertTrue(pointerHandled);

        // Phải phát ACTION_TOUCH_UP nhả chuột trái trước, sau đó phát ACTION_RIGHT_CLICK
        assertTrue(recordedEvents.size() >= 3);
        assertEquals(GameConfig.ACTION_TOUCH_DOWN, recordedEvents.get(0).action);
        assertEquals(GameConfig.ACTION_TOUCH_UP, recordedEvents.get(1).action);
        assertEquals(GameConfig.ACTION_RIGHT_CLICK, recordedEvents.get(2).action);

        // Nhả tay
        handler.handleTouchEvent(1, 500f, 500f, VIEW_WIDTH, VIEW_HEIGHT, 1050L);
        assertEquals(GameConfig.ACTION_TOUCH_UP, recordedEvents.get(recordedEvents.size() - 1).action);
    }

    @Test
    public void testSwipeUpForBacklog() {
        handler.handleTouchEvent(0, 960f, 800f, VIEW_WIDTH, VIEW_HEIGHT, 1000L);
        // Vuốt lên trên vượt ngưỡng -120px (từ 800 lên 600)
        handler.handleTouchEvent(1, 960f, 600f, VIEW_WIDTH, VIEW_HEIGHT, 1100L);

        assertEquals(3, recordedEvents.size());
        assertEquals(GameConfig.ACTION_SWIPE_UP, recordedEvents.get(1).action);
        assertEquals(GameConfig.ACTION_TOUCH_UP, recordedEvents.get(2).action);
    }

    @Test
    public void testSwipeDownForMenu() {
        handler.handleTouchEvent(0, 960f, 200f, VIEW_WIDTH, VIEW_HEIGHT, 1000L);
        // Vuốt xuống dưới vượt ngưỡng 120px (từ 200 xuống 400)
        handler.handleTouchEvent(1, 960f, 400f, VIEW_WIDTH, VIEW_HEIGHT, 1100L);

        assertEquals(3, recordedEvents.size());
        assertEquals(GameConfig.ACTION_SWIPE_DOWN, recordedEvents.get(1).action);
        assertEquals(GameConfig.ACTION_TOUCH_UP, recordedEvents.get(2).action);
    }

    @Test
    public void testSwipeRightToHideDialogue() {
        handler.handleTouchEvent(0, 200f, 500f, VIEW_WIDTH, VIEW_HEIGHT, 1000L);
        // Vuốt phải vượt ngưỡng 120px (từ 200 sang 400)
        handler.handleTouchEvent(1, 400f, 500f, VIEW_WIDTH, VIEW_HEIGHT, 1100L);

        assertEquals(3, recordedEvents.size());
        assertEquals(GameConfig.ACTION_SWIPE_RIGHT, recordedEvents.get(1).action);
        assertEquals(GameConfig.ACTION_TOUCH_UP, recordedEvents.get(2).action);
    }

    @Test
    public void testLongPressForFastForward() {
        handler.handleTouchEvent(0, 500f, 500f, VIEW_WIDTH, VIEW_HEIGHT, 1000L);
        // Giữ nguyên vị trí sau 500ms (> 450ms ngưỡng long press)
        handler.handleTouchEvent(2, 505f, 505f, VIEW_WIDTH, VIEW_HEIGHT, 1550L);

        assertEquals(2, recordedEvents.size());
        assertEquals(GameConfig.ACTION_LONG_PRESS, recordedEvents.get(1).action);
    }

    @Test
    public void testActionCancelSafelyReleases() {
        handler.handleTouchEvent(0, 500f, 500f, VIEW_WIDTH, VIEW_HEIGHT, 1000L);
        // Hệ thống hủy touch (ACTION_CANCEL = 3)
        boolean cancelHandled = handler.handleTouchEvent(3, 500f, 500f, VIEW_WIDTH, VIEW_HEIGHT, 1050L);
        assertTrue(cancelHandled);

        assertEquals(2, recordedEvents.size());
        assertEquals(GameConfig.ACTION_TOUCH_UP, recordedEvents.get(1).action);
    }

    @Test
    public void testOutOfBoundsCoordinateClamping() {
        // Tọa độ âm và tọa độ vượt ngoài viewWidth / viewHeight
        handler.handleTouchEvent(0, -100f, -50f, VIEW_WIDTH, VIEW_HEIGHT, 1000L);
        assertEquals(0.0f, recordedEvents.get(0).x, 0.0001f);
        assertEquals(0.0f, recordedEvents.get(0).y, 0.0001f);

        handler.handleTouchEvent(1, 3000f, 2000f, VIEW_WIDTH, VIEW_HEIGHT, 1050L);
        assertEquals(1.0f, recordedEvents.get(1).x, 0.0001f);
        assertEquals(1.0f, recordedEvents.get(1).y, 0.0001f);
    }

    @Test
    public void testInvalidViewDimensions() {
        // View width/height <= 0
        boolean handled = handler.handleTouchEvent(0, 100f, 100f, 0, 0, 1000L);
        assertFalse(handled);
        assertTrue(recordedEvents.isEmpty());
    }
}
