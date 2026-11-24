package com.makurisu.sgrb;

import org.junit.Test;
import static org.junit.Assert.*;

public class ViewportLayoutCalculatorTest {

    @Test
    public void testExact16By9SurfaceHasZeroBars() {
        ViewportLayoutCalculator.ViewportRect rect =
            ViewportLayoutCalculator.computeViewport(1920, 1080, true);
        assertEquals(0, rect.left);
        assertEquals(0, rect.top);
        assertEquals(1920, rect.width);
        assertEquals(1080, rect.height);
    }

    @Test
    public void testUltrawide20By9PhoneUsesPillarbox() {
        // Màn hình 2400x1080 (tỷ lệ 20:9): vùng 16:9 rộng 1920, viền đen trái/phải mỗi bên 240px
        ViewportLayoutCalculator.ViewportRect rect =
            ViewportLayoutCalculator.computeViewport(2400, 1080, true);
        assertEquals(240, rect.left);
        assertEquals(0, rect.top);
        assertEquals(1920, rect.width);
        assertEquals(1080, rect.height);

        // Chạm vào viền đen bên trái (< 240) phải kẹp an toàn về 0.0f
        assertEquals(0.0f, ViewportLayoutCalculator.mapTouchX(100f, rect), 0.0001f);
        // Chạm chính giữa màn hình (x = 1200) phải ánh xạ chuẩn xác tới 0.5f của khung game
        assertEquals(0.5f, ViewportLayoutCalculator.mapTouchX(1200f, rect), 0.0001f);
        // Chạm vào viền đen bên phải (> 2160) phải kẹp an toàn về 1.0f
        assertEquals(1.0f, ViewportLayoutCalculator.mapTouchX(2350f, rect), 0.0001f);
    }

    @Test
    public void testTablet4By3SurfaceUsesLetterbox() {
        // Màn hình 2048x1536 (4:3): vùng 16:9 cao 1152, viền đen trên/dưới mỗi bên 192px
        ViewportLayoutCalculator.ViewportRect rect =
            ViewportLayoutCalculator.computeViewport(2048, 1536, true);
        assertEquals(0, rect.left);
        assertEquals(192, rect.top);
        assertEquals(2048, rect.width);
        assertEquals(1152, rect.height);

        assertEquals(0.0f, ViewportLayoutCalculator.mapTouchY(50f, rect), 0.0001f);
        assertEquals(0.5f, ViewportLayoutCalculator.mapTouchY(768f, rect), 0.0001f);
        assertEquals(1.0f, ViewportLayoutCalculator.mapTouchY(1500f, rect), 0.0001f);
    }

    @Test
    public void testStretchModeUsesFullSurface() {
        ViewportLayoutCalculator.ViewportRect rect =
            ViewportLayoutCalculator.computeViewport(2400, 1080, false);
        assertEquals(0, rect.left);
        assertEquals(0, rect.top);
        assertEquals(2400, rect.width);
        assertEquals(1080, rect.height);
        assertEquals(0.25f, ViewportLayoutCalculator.mapTouchX(600f, rect), 0.0001f);
    }

    @Test
    public void testDegenerateZeroOrNullDimensionsSafeHandling() {
        ViewportLayoutCalculator.ViewportRect rect =
            ViewportLayoutCalculator.computeViewport(0, -10, true);
        assertTrue(rect.width >= 1);
        assertTrue(rect.height >= 1);
        assertEquals(0.0f, ViewportLayoutCalculator.mapTouchX(10f, null), 0.0001f);
        assertEquals(0.0f, ViewportLayoutCalculator.mapTouchY(10f, null), 0.0001f);
    }
}
