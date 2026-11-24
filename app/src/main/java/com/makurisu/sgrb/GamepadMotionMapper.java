package com.makurisu.sgrb;

import android.view.InputDevice;
import android.view.MotionEvent;

/**
 * Bộ trích xuất và lọc nhiễu (deadzone) các trục Analog Stick, D-Pad HAT và cò L2/R2
 * từ thiết bị tay cầm chơi game Bluetooth/USB trên Android.
 */
public final class GamepadMotionMapper {

    public static final float ANALOG_DEADZONE = 0.15f;

    public static final class AxisSnapshot {
        public final float leftX;
        public final float leftY;
        public final float rightX;
        public final float rightY;
        public final float leftTrigger;
        public final float rightTrigger;
        public final float hatX;
        public final float hatY;

        public AxisSnapshot(
                float leftX,
                float leftY,
                float rightX,
                float rightY,
                float leftTrigger,
                float rightTrigger,
                float hatX,
                float hatY
        ) {
            this.leftX = applyDeadzone(leftX, ANALOG_DEADZONE);
            this.leftY = applyDeadzone(leftY, ANALOG_DEADZONE);
            this.rightX = applyDeadzone(rightX, ANALOG_DEADZONE);
            this.rightY = applyDeadzone(rightY, ANALOG_DEADZONE);
            this.leftTrigger = clampUnit(leftTrigger);
            this.rightTrigger = clampUnit(rightTrigger);
            this.hatX = applyDeadzone(hatX, ANALOG_DEADZONE);
            this.hatY = applyDeadzone(hatY, ANALOG_DEADZONE);
        }
    }

    private GamepadMotionMapper() {
    }

    public static boolean isGamepadMotionEvent(MotionEvent event) {
        if (event == null) {
            return false;
        }
        int source = event.getSource();
        boolean isJoystickOrGamepad =
                (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
                        || (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD;
        return isJoystickOrGamepad && event.getActionMasked() == MotionEvent.ACTION_MOVE;
    }

    public static AxisSnapshot extractAxes(MotionEvent event) {
        float lx = event.getAxisValue(MotionEvent.AXIS_X);
        float ly = event.getAxisValue(MotionEvent.AXIS_Y);
        float rx = event.getAxisValue(MotionEvent.AXIS_Z);
        float ry = event.getAxisValue(MotionEvent.AXIS_RZ);

        float lt = Math.max(
                event.getAxisValue(MotionEvent.AXIS_LTRIGGER),
                event.getAxisValue(MotionEvent.AXIS_BRAKE)
        );
        float rt = Math.max(
                event.getAxisValue(MotionEvent.AXIS_RTRIGGER),
                event.getAxisValue(MotionEvent.AXIS_GAS)
        );

        float hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X);
        float hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y);

        return new AxisSnapshot(lx, ly, rx, ry, lt, rt, hatX, hatY);
    }

    public static float applyDeadzone(float value, float deadzone) {
        if (Float.isNaN(value) || Float.isInfinite(value)) {
            return 0.0f;
        }
        float clamped = Math.max(-1.0f, Math.min(1.0f, value));
        return Math.abs(clamped) < deadzone ? 0.0f : clamped;
    }

    public static float clampUnit(float value) {
        if (Float.isNaN(value) || Float.isInfinite(value)) {
            return 0.0f;
        }
        return Math.max(0.0f, Math.min(1.0f, value));
    }
}
