package com.makurisu.sgrb;

import java.util.ArrayList;
import java.util.List;

public class TouchGestureEvaluatorTest {

    public static void main(String[] args) {
        testSingleTap();
        testSwipeUp();
        testSwipeDown();
        testLongPress();
        System.out.println("ALL_UNIT_TESTS_PASSED");
    }

    private static void testSingleTap() {
        List<Integer> actions = new ArrayList<>();
        TouchGestureEvaluator evaluator = new TouchGestureEvaluator((action, x, y) -> actions.add(action));

        evaluator.processDown(500, 500, 1920, 1080, 1000);
        evaluator.processUp(502, 501, 1920, 1080);

        assert actions.size() == 2;
        assert actions.get(0) == GameConfig.ACTION_TOUCH_DOWN;
        assert actions.get(1) == GameConfig.ACTION_TOUCH_UP;
    }

    private static void testSwipeUp() {
        List<Integer> actions = new ArrayList<>();
        TouchGestureEvaluator evaluator = new TouchGestureEvaluator((action, x, y) -> actions.add(action));

        evaluator.processDown(500, 800, 1920, 1080, 1000);
        evaluator.processUp(500, 500, 1920, 1080); // Vuốt lên 300px > 120px

        assert actions.contains(GameConfig.ACTION_SWIPE_UP);
    }

    private static void testSwipeDown() {
        List<Integer> actions = new ArrayList<>();
        TouchGestureEvaluator evaluator = new TouchGestureEvaluator((action, x, y) -> actions.add(action));

        evaluator.processDown(500, 200, 1920, 1080, 1000);
        evaluator.processUp(500, 500, 1920, 1080); // Vuốt xuống 300px > 120px

        assert actions.contains(GameConfig.ACTION_SWIPE_DOWN);
    }

    private static void testLongPress() {
        List<Integer> actions = new ArrayList<>();
        TouchGestureEvaluator evaluator = new TouchGestureEvaluator((action, x, y) -> actions.add(action));

        evaluator.processDown(500, 500, 1920, 1080, 1000);
        evaluator.processMove(502, 502, 1920, 1080, 1600); // 600ms > 450ms

        assert actions.contains(GameConfig.ACTION_LONG_PRESS);
    }
}
