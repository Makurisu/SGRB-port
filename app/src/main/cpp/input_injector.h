#pragma once
#include <cstdint>

enum class GameTouchAction : int32_t {
    TouchDown = 0,
    TouchMove = 1,
    TouchUp = 2,
    SwipeUp = 3,
    SwipeDown = 4,
    SwipeRight = 5,
    LongPress = 6
};

class InputInjector {
public:
    static void injectTouchEvent(GameTouchAction action, float normalizedX, float normalizedY);
    static void injectKeyEvent(int32_t keyCode, bool isDown);
    static void sendWin32Mouse(uint32_t msg, int32_t x, int32_t y);
    static void sendWin32Key(uint32_t virtualKey, bool isDown);
};
