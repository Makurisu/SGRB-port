package com.makurisu.sgrb;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

/**
 * Kiểm tra tính toàn vẹn của thư mục dữ liệu game, cấu hình ngôn ngữ
 * và sao lưu / khôi phục tệp lưu an toàn (chống tấn công Zip-Slip).
 */
public final class GameDataValidator {

    public static final long MIN_EXE_SIZE_BYTES = 1024L * 1024L;
    public static final int MAX_SAVE_ZIP_ENTRIES = 512;
    public static final long MAX_SAVE_UNCOMPRESSED_BYTES = 128L * 1024L * 1024L;

    private static final List<String> REQUIRED_RELATIVE_FILES = Collections.unmodifiableList(Arrays.asList(
        "sgre_steam.exe",
        "wind3d11data/config_body.bin",
        "wind3d11data/scenario_body.bin",
        "wind3d11data/motion_body.bin"
    ));

    private static final Set<String> ALLOWED_LANGUAGES = Collections.unmodifiableSet(new HashSet<>(Arrays.asList(
        "english",
        "japanese",
        "schinese",
        "tchinese"
    )));

    public static final class ValidationReport {
        public final boolean directoryExists;
        public final boolean executableValid;
        public final List<String> missingRequiredFiles;
        public final boolean openingMoviePresent;
        public final int saveFileCount;

        public ValidationReport(
            boolean directoryExists,
            boolean executableValid,
            List<String> missingRequiredFiles,
            boolean openingMoviePresent,
            int saveFileCount
        ) {
            this.directoryExists = directoryExists;
            this.executableValid = executableValid;
            this.missingRequiredFiles = Collections.unmodifiableList(new ArrayList<>(missingRequiredFiles));
            this.openingMoviePresent = openingMoviePresent;
            this.saveFileCount = saveFileCount;
        }

        public boolean isReadyToLaunch() {
            return directoryExists && executableValid && missingRequiredFiles.isEmpty();
        }
    }

    private GameDataValidator() {
    }

    public static boolean isSafeDirectoryPath(String rawPath) {
        if (rawPath == null) {
            return false;
        }
        String trimmed = rawPath.trim();
        if (trimmed.isEmpty() || trimmed.length() > 512) {
            return false;
        }
        for (int i = 0; i < trimmed.length(); i++) {
            char ch = trimmed.charAt(i);
            if (ch < 0x20 || ch == '\u007F') {
                return false;
            }
        }
        return !trimmed.contains("..");
    }

    public static ValidationReport validateDataDirectory(File rootDir) {
        List<String> missing = new ArrayList<>();
        if (rootDir == null || !rootDir.exists() || !rootDir.isDirectory()) {
            missing.addAll(REQUIRED_RELATIVE_FILES);
            return new ValidationReport(false, false, missing, false, 0);
        }

        boolean exeValid = false;
        for (String relPath : REQUIRED_RELATIVE_FILES) {
            File target = resolveCaseInsensitiveChild(rootDir, relPath);
            if (target == null || !target.isFile() || target.length() == 0) {
                missing.add(relPath);
            } else if ("sgre_steam.exe".equalsIgnoreCase(relPath)) {
                exeValid = verifyMzHeader(target);
                if (!exeValid) {
                    missing.add("sgre_steam.exe (sai chữ ký PE MZ)");
                }
            }
        }

        File movieFile = resolveCaseInsensitiveChild(rootDir, "wind3d11data/movie/prologue01.webm");
        boolean hasMovie = movieFile != null && movieFile.isFile() && movieFile.length() > 0;

        int saveCount = countFilesRecursively(new File(rootDir, "saves"), 0);
        return new ValidationReport(true, exeValid, missing, hasMovie, saveCount);
    }

    public static String normalizeLanguageCode(String rawLang) {
        if (rawLang == null) {
            return "english";
        }
        String clean = rawLang.trim().toLowerCase(Locale.ROOT);
        return ALLOWED_LANGUAGES.contains(clean) ? clean : "english";
    }

    public static String readConfiguredLanguage(File rootDir) {
        if (rootDir == null || !rootDir.isDirectory()) {
            return "english";
        }
        File langFile = new File(rootDir, "language.txt");
        if (!langFile.isFile() || langFile.length() == 0 || langFile.length() > 256) {
            return "english";
        }
        byte[] buf = new byte[(int) langFile.length()];
        try (FileInputStream fis = new FileInputStream(langFile)) {
            int read = fis.read(buf);
            if (read <= 0) {
                return "english";
            }
            return normalizeLanguageCode(new String(buf, 0, read, StandardCharsets.UTF_8));
        } catch (IOException e) {
            return "english";
        }
    }

    public static boolean writeConfiguredLanguage(File rootDir, String langCode) {
        if (rootDir == null || (!rootDir.exists() && !rootDir.mkdirs())) {
            return false;
        }
        String normalized = normalizeLanguageCode(langCode);
        File langFile = new File(rootDir, "language.txt");
        try (FileOutputStream fos = new FileOutputStream(langFile, false)) {
            fos.write((normalized + "\n").getBytes(StandardCharsets.UTF_8));
            fos.flush();
            return true;
        } catch (IOException e) {
            return false;
        }
    }

    public static int exportSavesToZip(File rootDir, File destinationZip) throws IOException {
        if (rootDir == null || destinationZip == null) {
            throw new IllegalArgumentException("Đường dẫn sao lưu không hợp lệ");
        }
        File savesDir = new File(rootDir, "saves");
        if (!savesDir.exists() || !savesDir.isDirectory()) {
            return 0;
        }
        File parent = destinationZip.getParentFile();
        if (parent != null && !parent.exists() && !parent.mkdirs()) {
            throw new IOException("Không thể tạo thư mục chứa bản sao lưu");
        }

        List<File> files = new ArrayList<>();
        collectRegularFiles(savesDir, files);
        if (files.isEmpty()) {
            return 0;
        }

        String baseCanonical = savesDir.getCanonicalPath();
        byte[] buffer = new byte[16 * 1024];
        int exported = 0;

        try (ZipOutputStream zos = new ZipOutputStream(new BufferedOutputStream(new FileOutputStream(destinationZip)))) {
            for (File file : files) {
                String fileCanonical = file.getCanonicalPath();
                if (!fileCanonical.startsWith(baseCanonical + File.separator)) {
                    continue;
                }
                String relName = fileCanonical.substring(baseCanonical.length() + 1).replace('\\', '/');
                ZipEntry entry = new ZipEntry(relName);
                entry.setTime(file.lastModified());
                zos.putNextEntry(entry);
                try (BufferedInputStream bis = new BufferedInputStream(new FileInputStream(file))) {
                    int n;
                    while ((n = bis.read(buffer)) > 0) {
                        zos.write(buffer, 0, n);
                    }
                }
                zos.closeEntry();
                exported++;
            }
        }
        return exported;
    }

    public static int importSavesFromZip(File sourceZip, File rootDir) throws IOException {
        if (sourceZip == null || !sourceZip.isFile() || rootDir == null) {
            throw new IllegalArgumentException("Tệp sao lưu đầu vào không tồn tại");
        }
        File savesDir = new File(rootDir, "saves");
        if (!savesDir.exists() && !savesDir.mkdirs()) {
            throw new IOException("Không thể khởi tạo thư mục saves");
        }

        String destCanonicalPrefix = savesDir.getCanonicalPath() + File.separator;
        byte[] buffer = new byte[16 * 1024];
        int imported = 0;
        int entriesSeen = 0;
        long totalBytesWritten = 0L;

        try (ZipInputStream zis = new ZipInputStream(new BufferedInputStream(new FileInputStream(sourceZip)))) {
            ZipEntry entry;
            while ((entry = zis.getNextEntry()) != null) {
                entriesSeen++;
                if (entriesSeen > MAX_SAVE_ZIP_ENTRIES) {
                    throw new SecurityException("Tệp nén vượt quá giới hạn số lượng mục cho phép");
                }

                String rawName = entry.getName();
                if (rawName == null || rawName.isEmpty()) {
                    zis.closeEntry();
                    continue;
                }

                // Kiểm tra chống tấn công leo thang thư mục Zip-Slip
                File outTarget = new File(savesDir, rawName);
                String outCanonical = outTarget.getCanonicalPath();
                if (!outCanonical.startsWith(destCanonicalPrefix)) {
                    throw new SecurityException("Phát hiện đường dẫn không an toàn trong tệp nén: " + rawName);
                }

                if (entry.isDirectory()) {
                    if (!outTarget.exists() && !outTarget.mkdirs()) {
                        throw new IOException("Không thể tạo thư mục con khi khôi phục save");
                    }
                    zis.closeEntry();
                    continue;
                }

                File parent = outTarget.getParentFile();
                if (parent != null && !parent.exists() && !parent.mkdirs()) {
                    throw new IOException("Không thể tạo thư mục cha cho tệp save");
                }

                try (BufferedOutputStream bos = new BufferedOutputStream(new FileOutputStream(outTarget))) {
                    int n;
                    while ((n = zis.read(buffer)) > 0) {
                        totalBytesWritten += n;
                        if (totalBytesWritten > MAX_SAVE_UNCOMPRESSED_BYTES) {
                            throw new SecurityException("Dung lượng giải nén vượt quá ngưỡng an toàn");
                        }
                        bos.write(buffer, 0, n);
                    }
                }
                imported++;
                zis.closeEntry();
            }
        }
        return imported;
    }

    private static boolean verifyMzHeader(File exeFile) {
        if (exeFile.length() < MIN_EXE_SIZE_BYTES) {
            return false;
        }
        try (FileInputStream fis = new FileInputStream(exeFile)) {
            int b0 = fis.read();
            int b1 = fis.read();
            return b0 == 'M' && b1 == 'Z';
        } catch (IOException e) {
            return false;
        }
    }

    private static File resolveCaseInsensitiveChild(File baseDir, String relativeSlashPath) {
        File direct = new File(baseDir, relativeSlashPath);
        if (direct.exists()) {
            return direct;
        }
        String[] parts = relativeSlashPath.split("/");
        File cursor = baseDir;
        for (String part : parts) {
            if (part.isEmpty()) {
                continue;
            }
            File exactChild = new File(cursor, part);
            if (exactChild.exists()) {
                cursor = exactChild;
                continue;
            }
            File[] children = cursor.listFiles();
            if (children == null) {
                return null;
            }
            File matched = null;
            for (File candidate : children) {
                if (candidate.getName().equalsIgnoreCase(part)) {
                    matched = candidate;
                    break;
                }
            }
            if (matched == null) {
                return null;
            }
            cursor = matched;
        }
        return cursor;
    }

    private static int countFilesRecursively(File dir, int depth) {
        if (dir == null || !dir.isDirectory() || depth > 5) {
            return 0;
        }
        File[] list = dir.listFiles();
        if (list == null) {
            return 0;
        }
        int total = 0;
        for (File f : list) {
            if (f.isFile()) {
                total++;
            } else if (f.isDirectory()) {
                total += countFilesRecursively(f, depth + 1);
            }
        }
        return total;
    }

    private static void collectRegularFiles(File dir, List<File> outFiles) {
        File[] list = dir.listFiles();
        if (list == null) {
            return;
        }
        for (File f : list) {
            if (f.isFile()) {
                outFiles.add(f);
            } else if (f.isDirectory()) {
                collectRegularFiles(f, outFiles);
            }
        }
    }
}
