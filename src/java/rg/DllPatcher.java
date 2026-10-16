package rg;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;

/**
 * Centres the in-game fight cut-in popup on wide screens by patching two draw
 * positions in ResetGeneration-Win32-Web.dll.
 *
 * When the screen is wider than 240 px the engine draws attack/defence cut-ins
 * inside a decorative frame (data/gameplay/ui/animframe.gfx, 420x378) at the
 * fixed position (0,0), and the 240x320 fight surface at (90,29) inside it
 * (RVA 0x28a70 and 0x28ae0). On a 640-wide screen that leaves the popup hugging
 * the left edge while the playfield is centred.
 *
 * Patches (RVAs equal file offsets in this DLL's .text section):
 *  - 0x28ac9 / 0x28ad1: imm32 x/y of the fight surface  -> 90+dx, 29+dy
 *  - 0x28af3: "mov [esp+8],ebx; mov [esp+0xc],ebx" (frame x/y = 0) -> jmp to a
 *    code cave that stores dx/dy and jumps back
 *  - the cave lives in the unused tail of .text (RVA 0x176550); .text's
 *    VirtualSize is raised to its raw size so the loader maps it.
 * The original is kept as *.dll.orig; every patch site is verified first and
 * nothing is changed if the bytes are not the expected ones.
 */
final class DllPatcher {
    static final String DLL = "ResetGeneration-Win32-Web.dll";
    private static final String VERSION = "1";

    private static final int FRAME_W = 420;
    private static final int FRAME_H = 378;

    private static final int SURFACE_X_IMM = 0x28ac9;
    private static final int SURFACE_Y_IMM = 0x28ad1;
    private static final int FRAME_SITE = 0x28af3;
    private static final int FRAME_RESUME = 0x28afb;
    private static final int CAVE = 0x176550;
    private static final int CAVE_LEN = 21;
    private static final int TEXT_VSIZE_FIELD = 0x1e0;
    private static final int TEXT_VSIZE_ORIG = 0x175544;
    private static final int TEXT_RAW_SIZE = 0x176000;
    private static final byte[] FRAME_SITE_ORIG = {
        (byte) 0x89, 0x5c, 0x24, 0x08, (byte) 0x89, 0x5c, 0x24, 0x0c};

    private DllPatcher() {
    }

    static void ensure(File gameDir, int width, int height) {
        int dx = Math.max(0, (width - FRAME_W) / 2);
        int dy = Math.max(0, (height - FRAME_H) / 2);
        File dll = new File(gameDir, DLL);
        File orig = new File(gameDir, DLL + ".orig");
        File stamp = new File(gameDir, DLL + ".revived");
        String want = VERSION + ":" + dx + "," + dy;
        try {
            if (stamp.exists() && want.equals(new String(PakPatcher.readAll(stamp), "UTF-8").trim()) && dll.exists()) {
                return;
            }
            if (!orig.exists()) {
                if (!dll.exists()) {
                    return;
                }
                PakPatcher.copy(dll, orig);
            }
            byte[] b = PakPatcher.readAll(orig);
            if (!matchesOriginal(b)) {
                System.out.println("Revived: unexpected engine DLL build; fight popup left uncentred");
                return;
            }
            if (dx != 0 || dy != 0) {
                putInt(b, SURFACE_X_IMM, 0x5a + dx);
                putInt(b, SURFACE_Y_IMM, 0x1d + dy);
                b[FRAME_SITE] = (byte) 0xe9;
                putInt(b, FRAME_SITE + 1, CAVE - (FRAME_SITE + 5));
                b[FRAME_SITE + 5] = (byte) 0x90;
                b[FRAME_SITE + 6] = (byte) 0x90;
                b[FRAME_SITE + 7] = (byte) 0x90;
                int p = CAVE;
                b[p++] = (byte) 0xc7; b[p++] = 0x44; b[p++] = 0x24; b[p++] = 0x08;   // mov dword [esp+8], dx
                putInt(b, p, dx); p += 4;
                b[p++] = (byte) 0xc7; b[p++] = 0x44; b[p++] = 0x24; b[p++] = 0x0c;   // mov dword [esp+0xc], dy
                putInt(b, p, dy); p += 4;
                b[p] = (byte) 0xe9;                                                  // jmp back
                putInt(b, p + 1, FRAME_RESUME - (p + 5));
                putInt(b, TEXT_VSIZE_FIELD, TEXT_RAW_SIZE);
            }
            File tmp = new File(gameDir, DLL + ".tmp");
            OutputStream out = new FileOutputStream(tmp);
            try {
                out.write(b);
            } finally {
                out.close();
            }
            if (dll.exists() && !dll.delete()) {
                tmp.delete();
                throw new IOException("cannot replace " + dll + " (is the game already running?)");
            }
            if (!tmp.renameTo(dll)) {
                throw new IOException("cannot rename " + tmp);
            }
            OutputStream s = new FileOutputStream(stamp);
            try {
                s.write(want.getBytes("UTF-8"));
            } finally {
                s.close();
            }
            System.out.println("Revived: fight popup offset " + dx + "," + dy);
        } catch (Exception ex) {
            System.out.println("Revived: could not patch " + DLL + " (" + ex + ")");
        }
    }

    private static boolean matchesOriginal(byte[] b) {
        if (b.length < TEXT_RAW_SIZE + 0x1000 || getInt(b, SURFACE_X_IMM) != 0x5a || getInt(b, SURFACE_Y_IMM) != 0x1d
                || getInt(b, TEXT_VSIZE_FIELD) != TEXT_VSIZE_ORIG) {
            return false;
        }
        for (int i = 0; i < FRAME_SITE_ORIG.length; ++i) {
            if (b[FRAME_SITE + i] != FRAME_SITE_ORIG[i]) {
                return false;
            }
        }
        for (int i = 0; i < CAVE_LEN; ++i) {
            if (b[CAVE + i] != 0) {
                return false;
            }
        }
        return true;
    }

    private static int getInt(byte[] b, int at) {
        return (b[at] & 0xff) | (b[at + 1] & 0xff) << 8 | (b[at + 2] & 0xff) << 16 | (b[at + 3] & 0xff) << 24;
    }

    private static void putInt(byte[] b, int at, int v) {
        b[at] = (byte) v;
        b[at + 1] = (byte) (v >> 8);
        b[at + 2] = (byte) (v >> 16);
        b[at + 3] = (byte) (v >> 24);
    }
}
