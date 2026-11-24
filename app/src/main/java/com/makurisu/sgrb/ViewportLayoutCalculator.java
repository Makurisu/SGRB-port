package com.makurisu.sgrb;

/**
 * Bộ tính toán khung hình hiển thị 16:9 (Letterbox / Pillarbox) hoặc Kéo giãn toàn màn hình,
 * đồng thời ánh xạ chính xác tọa độ cảm ứng sang không gian chuẩn hóa [0.0, 1.0] của game.
 */
public final class ViewportLayoutCalculator {

    private static final int ASPECT_NUMERATOR = 16;
    private static final int ASPECT_DENOMINATOR = 9;

    public static final class ViewportRect {
        public final int left;
        public final int top;
        public final int width;
        public final int height;

        public ViewportRect(int left, int top, int width, int height) {
            this.left = left;
            this.top = top;
            this.width = width;
            this.height = height;
        }
    }

    private ViewportLayoutCalculator() {
    }

    public static ViewportRect computeViewport(int surfaceWidth, int surfaceHeight, boolean maintainAspectRatio16x9) {
        int safeWidth = Math.max(1, surfaceWidth);
        int safeHeight = Math.max(1, surfaceHeight);

        if (!maintainAspectRatio16x9) {
            return new ViewportRect(0, 0, safeWidth, safeHeight);
        }

        long lhs = (long) safeWidth * ASPECT_DENOMINATOR;
        long rhs = (long) safeHeight * ASPECT_NUMERATOR;

        if (lhs > rhs) {
            // Màn hình rộng hơn 16:9 (ví dụ 20:9, 21:9) -> Pillarbox hai bên trái/phải
            int activeWidth = Math.max(1, Math.round((safeHeight * (float) ASPECT_NUMERATOR) / ASPECT_DENOMINATOR));
            int left = (safeWidth - activeWidth) / 2;
            return new ViewportRect(left, 0, activeWidth, safeHeight);
        } else if (lhs < rhs) {
            // Màn hình cao hơn 16:9 (ví dụ tablet 4:3, 16:10) -> Letterbox trên/dưới
            int activeHeight = Math.max(1, Math.round((safeWidth * (float) ASPECT_DENOMINATOR) / ASPECT_NUMERATOR));
            int top = (safeHeight - activeHeight) / 2;
            return new ViewportRect(0, top, safeWidth, activeHeight);
        }

        return new ViewportRect(0, 0, safeWidth, safeHeight);
    }

    public static float mapTouchX(float rawX, ViewportRect rect) {
        if (rect == null || rect.width <= 0) {
            return 0.0f;
        }
        float normalized = (rawX - rect.left) / (float) rect.width;
        return Math.max(0.0f, Math.min(1.0f, normalized));
    }

    public static float mapTouchY(float rawY, ViewportRect rect) {
        if (rect == null || rect.height <= 0) {
            return 0.0f;
        }
        float normalized = (rawY - rect.top) / (float) rect.height;
        return Math.max(0.0f, Math.min(1.0f, normalized));
    }
}
