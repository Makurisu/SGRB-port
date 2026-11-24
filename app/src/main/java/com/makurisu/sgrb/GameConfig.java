package com.makurisu.sgrb;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Environment;
import java.io.File;

/**
 * Quản lý các hằng số giao thức đầu vào và cấu hình lưu trữ bền vững của người dùng.
 */
public class GameConfig {
    public static final int TARGET_WIDTH = 1920;
    public static final int TARGET_HEIGHT = 1080;

    public static final int ACTION_TOUCH_DOWN = 0;
    public static final int ACTION_TOUCH_MOVE = 1;
    public static final int ACTION_TOUCH_UP = 2;
    public static final int ACTION_SWIPE_UP = 3;
    public static final int ACTION_SWIPE_DOWN = 4;
    public static final int ACTION_SWIPE_RIGHT = 5;
    public static final int ACTION_LONG_PRESS = 6;
    public static final int ACTION_RIGHT_CLICK = 7;

    public static final int DEFAULT_FPS = 60;
    public static final int POWER_SAVE_FPS = 30;
    public static final String SAVE_BACKUP_FILE_NAME = "sgrb_saves_backup.zip";

    private static final String PREFS_NAME = "sgrb_runtime_prefs";
    private static final String KEY_DATA_DIR = "data_dir_path";
    private static final String KEY_ASPECT_16_9 = "maintain_aspect_16_9";
    private static final String KEY_TARGET_FPS = "target_fps";
    private static final String KEY_SHOW_HUD_STATS = "show_hud_stats";
    private static final String KEY_LANGUAGE = "steam_language";

    private String dataDirectoryPath;
    private boolean maintainAspectRatio16x9 = true;
    private int targetFps = DEFAULT_FPS;
    private boolean showOverlayStats = false;
    private String languageCode = "english";

    public static GameConfig load(Context context) {
        GameConfig cfg = new GameConfig();
        String defaultPath = resolveDefaultDataDirectory(context);
        SharedPreferences prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
        String savedPath = prefs.getString(KEY_DATA_DIR, defaultPath);
        cfg.dataDirectoryPath = GameDataValidator.isSafeDirectoryPath(savedPath) ? savedPath : defaultPath;
        cfg.maintainAspectRatio16x9 = prefs.getBoolean(KEY_ASPECT_16_9, true);
        int rawFps = prefs.getInt(KEY_TARGET_FPS, DEFAULT_FPS);
        cfg.targetFps = (rawFps <= POWER_SAVE_FPS) ? POWER_SAVE_FPS : DEFAULT_FPS;
        cfg.showOverlayStats = prefs.getBoolean(KEY_SHOW_HUD_STATS, false);

        String fileLang = GameDataValidator.readConfiguredLanguage(new File(cfg.dataDirectoryPath));
        String prefLang = prefs.getString(KEY_LANGUAGE, fileLang);
        cfg.languageCode = GameDataValidator.normalizeLanguageCode(prefLang);
        return cfg;
    }

    public void save(Context context) {
        SharedPreferences prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
        prefs.edit()
            .putString(KEY_DATA_DIR, dataDirectoryPath)
            .putBoolean(KEY_ASPECT_16_9, maintainAspectRatio16x9)
            .putInt(KEY_TARGET_FPS, targetFps)
            .putBoolean(KEY_SHOW_HUD_STATS, showOverlayStats)
            .putString(KEY_LANGUAGE, languageCode)
            .apply();
    }

    public static String resolveDefaultDataDirectory(Context context) {
        File sharedDir = new File(Environment.getExternalStorageDirectory(), "sgrb_game_data");
        if (sharedDir.exists() && new File(sharedDir, "sgre_steam.exe").exists()) {
            return sharedDir.getAbsolutePath();
        }
        File appExtDir = context.getExternalFilesDir("data");
        if (appExtDir != null && new File(appExtDir, "sgre_steam.exe").exists()) {
            return appExtDir.getAbsolutePath();
        }
        return sharedDir.getAbsolutePath();
    }

    public String getDataDirectoryPath() {
        return dataDirectoryPath;
    }

    public void setDataDirectoryPath(String path) {
        if (GameDataValidator.isSafeDirectoryPath(path)) {
            this.dataDirectoryPath = path.trim();
        }
    }

    public boolean isMaintainAspectRatio16x9() {
        return maintainAspectRatio16x9;
    }

    public void setMaintainAspectRatio16x9(boolean maintainAspectRatio16x9) {
        this.maintainAspectRatio16x9 = maintainAspectRatio16x9;
    }

    public int getTargetFps() {
        return targetFps;
    }

    public void setTargetFps(int targetFps) {
        this.targetFps = (targetFps <= POWER_SAVE_FPS) ? POWER_SAVE_FPS : DEFAULT_FPS;
    }

    public boolean isShowOverlayStats() {
        return showOverlayStats;
    }

    public void setShowOverlayStats(boolean showOverlayStats) {
        this.showOverlayStats = showOverlayStats;
    }

    public String getLanguageCode() {
        return languageCode;
    }

    public void setLanguageCode(String languageCode) {
        this.languageCode = GameDataValidator.normalizeLanguageCode(languageCode);
    }
}
