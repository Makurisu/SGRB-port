package com.makurisu.sgrb;

import org.junit.Test;
import static org.junit.Assert.*;

public class GamepadMotionMapperTest {

    @Test
    public void testDeadzoneFiltersStickDriftAndPreservesActiveTilt() {
        // Nhiễu nhẹ dưới ngưỡng 0.15f phải bị lọc về 0.0f
        assertEquals(0.0f, GamepadMotionMapper.applyDeadzone(0.08f, GamepadMotionMapper.ANALOG_DEADZONE), 0.0001f);
        assertEquals(0.0f, GamepadMotionMapper.applyDeadzone(-0.149f, GamepadMotionMapper.ANALOG_DEADZONE), 0.0001f);

        // Chuyển động hợp lệ ngoài vùng chết được giữ nguyên và giới hạn [-1, 1]
        assertEquals(0.75f, GamepadMotionMapper.applyDeadzone(0.75f, GamepadMotionMapper.ANALOG_DEADZONE), 0.0001f);
        assertEquals(-1.0f, GamepadMotionMapper.applyDeadzone(-1.8f, GamepadMotionMapper.ANALOG_DEADZONE), 0.0001f);
    }

    @Test
    public void testNanAndInfiniteValuesHandledSafely() {
        assertEquals(0.0f, GamepadMotionMapper.applyDeadzone(Float.NaN, 0.15f), 0.0001f);
        assertEquals(0.0f, GamepadMotionMapper.applyDeadzone(Float.POSITIVE_INFINITY, 0.15f), 0.0001f);
        assertEquals(0.0f, GamepadMotionMapper.clampUnit(Float.NaN), 0.0001f);
        assertEquals(0.0f, GamepadMotionMapper.clampUnit(-0.5f), 0.0001f);
        assertEquals(1.0f, GamepadMotionMapper.clampUnit(2.5f), 0.0001f);
    }

    @Test
    public void testAxisSnapshotAppliesFilteringConsistently() {
        GamepadMotionMapper.AxisSnapshot snap = new GamepadMotionMapper.AxisSnapshot(
            0.05f, -0.82f, 1.4f, -0.10f, -0.2f, 0.95f, 1.0f, -1.0f
        );
        assertEquals(0.0f, snap.leftX, 0.0001f);
        assertEquals(-0.82f, snap.leftY, 0.0001f);
        assertEquals(1.0f, snap.rightX, 0.0001f);
        assertEquals(0.0f, snap.rightY, 0.0001f);
        assertEquals(0.0f, snap.leftTrigger, 0.0001f);
        assertEquals(0.95f, snap.rightTrigger, 0.0001f);
        assertEquals(1.0f, snap.hatX, 0.0001f);
        assertEquals(-1.0f, snap.hatY, 0.0001f);
    }

    @Test
    public void testNullMotionEventRejectedSafely() {
        assertFalse(GamepadMotionMapper.isGamepadMotionEvent(null));
    }
}
