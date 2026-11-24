package com.makurisu.sgrb;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

import static org.junit.Assert.*;

public class GameDataValidatorTest {

    @Rule
    public TemporaryFolder tempFolder = new TemporaryFolder();

    private void createFakeValidGameDirectory(File rootDir) throws IOException {
        File exeFile = new File(rootDir, "sgre_steam.exe");
        try (FileOutputStream fos = new FileOutputStream(exeFile)) {
            byte[] headerAndPadding = new byte[(int) GameDataValidator.MIN_EXE_SIZE_BYTES + 64];
            headerAndPadding[0] = 'M';
            headerAndPadding[1] = 'Z';
            fos.write(headerAndPadding);
        }

        File winData = new File(rootDir, "wind3d11data");
        assertTrue(winData.mkdirs());
        writeNonEmptyFile(new File(winData, "config_body.bin"), "CFG");
        writeNonEmptyFile(new File(winData, "scenario_body.bin"), "SCN");
        writeNonEmptyFile(new File(winData, "motion_body.bin"), "MOT");
    }

    private void writeNonEmptyFile(File file, String content) throws IOException {
        File parent = file.getParentFile();
        if (parent != null && !parent.exists()) {
            assertTrue(parent.mkdirs());
        }
        try (FileOutputStream fos = new FileOutputStream(file)) {
            fos.write(content.getBytes(StandardCharsets.UTF_8));
        }
    }

    @Test
    public void testValidateDataDirectoryHappyPath() throws IOException {
        File root = tempFolder.newFolder("sgrb_valid");
        createFakeValidGameDirectory(root);
        writeNonEmptyFile(new File(root, "wind3d11data/movie/prologue01.webm"), "WEBM");
        writeNonEmptyFile(new File(root, "saves/slot01.sav"), "SAVE1");

        GameDataValidator.ValidationReport report = GameDataValidator.validateDataDirectory(root);
        assertTrue(report.directoryExists);
        assertTrue(report.executableValid);
        assertTrue(report.missingRequiredFiles.isEmpty());
        assertTrue(report.openingMoviePresent);
        assertEquals(1, report.saveFileCount);
        assertTrue(report.isReadyToLaunch());
    }

    @Test
    public void testValidateDataDirectoryRejectsInvalidPeMagic() throws IOException {
        File root = tempFolder.newFolder("sgrb_bad_mz");
        createFakeValidGameDirectory(root);

        // Ghi đè chữ ký sai (không phải MZ)
        File exe = new File(root, "sgre_steam.exe");
        try (FileOutputStream fos = new FileOutputStream(exe)) {
            byte[] bad = new byte[(int) GameDataValidator.MIN_EXE_SIZE_BYTES + 16];
            bad[0] = 'E';
            bad[1] = 'L';
            fos.write(bad);
        }

        GameDataValidator.ValidationReport report = GameDataValidator.validateDataDirectory(root);
        assertFalse(report.executableValid);
        assertFalse(report.isReadyToLaunch());
    }

    @Test
    public void testValidateMissingDirectoryEdgeCase() {
        File missing = new File(tempFolder.getRoot(), "does_not_exist");
        GameDataValidator.ValidationReport report = GameDataValidator.validateDataDirectory(missing);
        assertFalse(report.directoryExists);
        assertFalse(report.isReadyToLaunch());
        assertEquals(4, report.missingRequiredFiles.size());
    }

    @Test
    public void testPathSanitizationBlocksTraversalAndControlChars() {
        assertTrue(GameDataValidator.isSafeDirectoryPath("/storage/emulated/0/sgrb_game_data"));
        assertFalse(GameDataValidator.isSafeDirectoryPath(null));
        assertFalse(GameDataValidator.isSafeDirectoryPath("   "));
        assertFalse(GameDataValidator.isSafeDirectoryPath("/sdcard/../etc/passwd"));
        assertFalse(GameDataValidator.isSafeDirectoryPath("/sdcard/bad\u0000path"));
    }

    @Test
    public void testLanguageConfigReadWriteAndAllowlistFallback() throws IOException {
        File root = tempFolder.newFolder("sgrb_lang");
        assertEquals("english", GameDataValidator.readConfiguredLanguage(root));

        assertTrue(GameDataValidator.writeConfiguredLanguage(root, "JAPANESE"));
        assertEquals("japanese", GameDataValidator.readConfiguredLanguage(root));

        // Giá trị lạ phải tự động chuẩn hóa về english
        assertTrue(GameDataValidator.writeConfiguredLanguage(root, "unknown_script_injection"));
        assertEquals("english", GameDataValidator.readConfiguredLanguage(root));
    }

    @Test
    public void testExportAndImportSavesRoundTrip() throws IOException {
        File srcRoot = tempFolder.newFolder("src_game");
        writeNonEmptyFile(new File(srcRoot, "saves/system.dat"), "SYS_STATE");
        writeNonEmptyFile(new File(srcRoot, "saves/profile1/save00.sav"), "SLOT_00_PAYLOAD");

        File backupZip = new File(tempFolder.getRoot(), "backup.zip");
        int exported = GameDataValidator.exportSavesToZip(srcRoot, backupZip);
        assertEquals(2, exported);
        assertTrue(backupZip.isFile());

        File dstRoot = tempFolder.newFolder("dst_game");
        int imported = GameDataValidator.importSavesFromZip(backupZip, dstRoot);
        assertEquals(2, imported);
        assertTrue(new File(dstRoot, "saves/system.dat").isFile());
        assertTrue(new File(dstRoot, "saves/profile1/save00.sav").isFile());
    }

    @Test(expected = SecurityException.class)
    public void testImportSavesBlocksZipSlipPathTraversalAttack() throws IOException {
        File maliciousZip = new File(tempFolder.getRoot(), "malicious.zip");
        try (ZipOutputStream zos = new ZipOutputStream(new FileOutputStream(maliciousZip))) {
            ZipEntry slipEntry = new ZipEntry("../../pwned_outside_saves.txt");
            zos.putNextEntry(slipEntry);
            zos.write("EXPLOIT".getBytes(StandardCharsets.UTF_8));
            zos.closeEntry();
        }

        File targetRoot = tempFolder.newFolder("victim_game");
        GameDataValidator.importSavesFromZip(maliciousZip, targetRoot);
    }
}
