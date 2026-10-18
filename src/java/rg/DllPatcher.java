package rg;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;

/**
 * Widens the engine's tile-view viewport and centres the in-game fight cut-in popup on
 * wide screens by patching ResetGeneration-Win32-Web.dll.
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
 * The tile views (block-placement ghost and highlight layers, FUN_100867c0 at RVA 0x867c0) draw only
 * the viewport columns [start, grid+0x18+1); the value is sized for a 240 px screen. The playfield is
 * centred on wide screens so its columns start at viewport column 4 and the last 2-3 columns were
 * never drawn (the ghost vanished there). 0x86822 "mov edx,[ecx+0x18]; inc edx" becomes
 * "push 0x1f; pop edx; nop" (a 31-column viewport; the engine trims it to the map width).
 *
 * The end-of-match win/lose scenes (scene draw FUN_100dafa0, RVA 0xdafa0) draw their layers straight
 * onto the screen with 240-wide coordinates. While that function runs a global xoff is set to
 * (width-240)/2 and the engine's sprite/text draw (FUN_10052de0, RVA 0x52de0: this, surface, &pos,
 * &rect, frame) adds it to pos.x for the duration of the call (callers reuse pos), except for the
 * text-box widgets (return address in RVA 0xa2000-0xa2fff) that already centre themselves. The
 * wrappers live in a second code cave (RVA 0x176580) that also holds xoff and the saved return
 * address; .text is made writable for that, and the caves find their data position-independently
 * (call/pop) so the DLL still works if it is relocated.
 *
 * The original is kept as *.dll.orig; every patch site is verified first and
 * nothing is changed if the bytes are not the expected ones.
 */
final class DllPatcher {
    static final String DLL = "ResetGeneration-Win32-Web.dll";
    private static final String VERSION = "3";

    private static final int FRAME_W = 420;
    private static final int FRAME_H = 378;

    private static final int SURFACE_X_IMM = 0x28ac9;
    private static final int SURFACE_Y_IMM = 0x28ad1;
    private static final int FRAME_SITE = 0x28af3;
    private static final int FRAME_RESUME = 0x28afb;
    private static final int VIEW_SITE = 0x86822;
    private static final byte[] VIEW_SITE_ORIG = {(byte) 0x8b, 0x51, 0x18, 0x42};
    private static final byte[] VIEW_SITE_NEW = {0x6a, 0x1f, 0x5a, (byte) 0x90};
    private static final int CAVE = 0x176550;
    private static final int CAVE_LEN = 21;
    private static final int CAVE2 = 0x176580;
    private static final int CAVE2_END = 0x177000;
    private static final int SCENE_ENTRY = 0xdafa0;
    private static final byte[] SCENE_ORIG = {(byte) 0x83, (byte) 0xec, 0x3c, 0x53, 0x55};
    private static final int DRAW_ENTRY = 0x52de0;
    private static final byte[] DRAW_ORIG = {(byte) 0x83, (byte) 0xec, 0x08, 0x56, (byte) 0x8b, 0x74, 0x24, 0x10};
    private static final int TEXT_CHARS_FIELD = 0x1fc;
    private static final int TEXT_CHARS_ORIG = 0x60000020;
    private static final int TEXT_WRITE = 0x80000000;
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
        boolean wide = width > 240;
        int sx = wide ? (width - 240) / 2 : 0;
        String want = VERSION + ":" + dx + "," + dy + (wide ? ",w" + sx : "");
        try {
            if (stamp.exists() && want.equals(new String(PakPatcher.readAll(stamp), "UTF-8").trim()) && dll.exists()) {
                return;
            }
            if (orig.exists() && orig.length() == 0 && !stamp.exists()) {
                orig.delete();   // truncated backup from an interrupted copy; dll is still pristine
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
            if (wide) {
                System.arraycopy(VIEW_SITE_NEW, 0, b, VIEW_SITE, VIEW_SITE_NEW.length);
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
            if (sx != 0) {
                patchScenes(b, sx);
                putInt(b, TEXT_VSIZE_FIELD, TEXT_RAW_SIZE);
                putInt(b, TEXT_CHARS_FIELD, TEXT_CHARS_ORIG | TEXT_WRITE);
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
            System.out.println("Revived: fight popup offset " + dx + "," + dy + (wide ? ", wide tile views, scene offset " + sx : ""));
        } catch (Exception ex) {
            System.out.println("Revived: could not patch " + DLL + " (" + ex + ")");
        }
    }

    private static boolean matchesOriginal(byte[] b) {
        if (b.length < TEXT_RAW_SIZE + 0x1000 || getInt(b, SURFACE_X_IMM) != 0x5a || getInt(b, SURFACE_Y_IMM) != 0x1d
                || getInt(b, TEXT_VSIZE_FIELD) != TEXT_VSIZE_ORIG) {
            return false;
        }
        for (int i = 0; i < VIEW_SITE_ORIG.length; ++i) {
            if (b[VIEW_SITE + i] != VIEW_SITE_ORIG[i]) {
                return false;
            }
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
        if (getInt(b, TEXT_CHARS_FIELD) != TEXT_CHARS_ORIG) {
            return false;
        }
        for (int i = 0; i < SCENE_ORIG.length; ++i) {
            if (b[SCENE_ENTRY + i] != SCENE_ORIG[i]) {
                return false;
            }
        }
        for (int i = 0; i < DRAW_ORIG.length; ++i) {
            if (b[DRAW_ENTRY + i] != DRAW_ORIG[i]) {
                return false;
            }
        }
        for (int i = CAVE2; i < CAVE2_END; ++i) {
            if (b[i] != 0) {
                return false;
            }
        }
        return true;
    }

    /** RVA of the next byte emitted into the second cave. */
    private static int pos;

    private static void emit(byte[] b, int... v) {
        for (int x : v) {
            b[pos++] = (byte) x;
        }
    }

    private static void emit32(byte[] b, int v) {
        putInt(b, pos, v);
        pos += 4;
    }

    /** "call $+5; pop eax": afterwards eax holds the returned RVA (plus the image base). */
    private static int emitAnchor(byte[] b) {
        emit(b, 0xe8, 0, 0, 0, 0, 0x58);
        return pos - 1;
    }

    private static void emitJmp(byte[] b, int target) {
        emit(b, 0xe9);
        emit32(b, target - (pos + 4));
    }

    private static void patchScenes(byte[] b, int sx) {
        int xoff = CAVE2;          // dword: current layout shift, 0 outside the scene draw
        int saved = CAVE2 + 4;     // dword: the scene draw's real return address
        pos = CAVE2 + 0x10;

        // scene entry: arm xoff, divert the return through the clear stub, run the displaced head
        int scene = pos;
        int a = emitAnchor(b);
        emit(b, 0x83, 0xb8); emit32(b, xoff - a); emit(b, 0);             // cmp dword [eax+xoff], 0
        int jne = pos; emit(b, 0x75, 0);                                  // jne skip (nested: already armed)
        emit(b, 0x52, 0x8b, 0x54, 0x24, 0x04);                            // push edx; mov edx,[esp+4]
        emit(b, 0x89, 0x90); emit32(b, saved - a);                        // mov [eax+saved], edx
        emit(b, 0x8d, 0x90); int stubFix = pos; emit32(b, 0);             // lea edx,[eax+stub-a]
        emit(b, 0x89, 0x54, 0x24, 0x04, 0x5a);                            // mov [esp+4],edx; pop edx
        emit(b, 0xc7, 0x80); emit32(b, xoff - a); emit32(b, sx);          // mov dword [eax+xoff], sx
        b[jne + 1] = (byte) (pos - (jne + 2));
        emit(b, 0x83, 0xec, 0x3c, 0x53, 0x55);                            // sub esp,3c; push ebx; push ebp
        emitJmp(b, SCENE_ENTRY + 5);

        // return stub: xoff = 0, then back to the saved return address with eax intact
        int stub = pos;
        putInt(b, stubFix, stub - a);
        emit(b, 0x50);                                                    // push eax
        int a2 = emitAnchor(b);
        emit(b, 0xc7, 0x80); emit32(b, xoff - a2); emit32(b, 0);          // mov dword [eax+xoff], 0
        emit(b, 0x8b, 0x80); emit32(b, saved - a2);                       // mov eax,[eax+saved]
        emit(b, 0x87, 0x04, 0x24, 0xc3);                                  // xchg [esp],eax; ret

        // draw hook: shift pos.x by xoff around the real draw (ecx = this is untouched)
        int draw = pos;
        int d = emitAnchor(b);
        emit(b, 0x83, 0xb8); emit32(b, xoff - d); emit(b, 0);             // cmp dword [eax+xoff], 0
        int jeFast = pos; emit(b, 0x74, 0);                               // je body
        emit(b, 0x52, 0x8b, 0x54, 0x24, 0x04, 0x29, 0xc2);                // push edx; mov edx,[esp+4]; sub edx,eax
        emit(b, 0x81, 0xc2); emit32(b, d - 0xa2000);                      // add edx,d-0xa2000 (= retRva-0xa2000)
        emit(b, 0x81, 0xfa); emit32(b, 0x1000);                           // cmp edx,0x1000
        int jbSkip = pos; emit(b, 0x72, 0);                               // jb skip (text-box widget)
        emit(b, 0x8b, 0x54, 0x24, 0x0c, 0x85, 0xd2);                      // mov edx,[esp+0xc] (pos); test edx,edx
        int jzSkip = pos; emit(b, 0x74, 0);                               // jz skip
        emit(b, 0x8b, 0x80); emit32(b, xoff - d);                         // mov eax,[eax+xoff]
        emit(b, 0x01, 0x02, 0x5a, 0x50);                                  // add [edx],eax; pop edx; push eax
        for (int i = 0; i < 4; ++i) {
            emit(b, 0xff, 0x74, 0x24, 0x14);                              // push dword [esp+0x14]: frame,rect,pos,surf
        }
        int call = pos; emit(b, 0xe8, 0, 0, 0, 0);                        // call body
        emit(b, 0x5a, 0x8b, 0x4c, 0x24, 0x08, 0x29, 0x11);                // pop edx; mov ecx,[esp+8]; sub [ecx],edx
        emit(b, 0xc2, 0x10, 0x00);                                        // ret 0x10
        int skip = pos;
        b[jbSkip + 1] = (byte) (skip - (jbSkip + 2));
        b[jzSkip + 1] = (byte) (skip - (jzSkip + 2));
        emit(b, 0x5a);                                                    // pop edx
        int body = pos;
        b[jeFast + 1] = (byte) (body - (jeFast + 2));
        putInt(b, call + 1, body - (call + 5));
        emit(b, 0x83, 0xec, 0x08, 0x56, 0x8b, 0x74, 0x24, 0x10);          // displaced draw head
        emitJmp(b, DRAW_ENTRY + 8);
        if (pos > CAVE2_END) {
            throw new IllegalStateException("cave overflow");
        }

        b[SCENE_ENTRY] = (byte) 0xe9;
        putInt(b, SCENE_ENTRY + 1, scene - (SCENE_ENTRY + 5));
        b[DRAW_ENTRY] = (byte) 0xe9;
        putInt(b, DRAW_ENTRY + 1, draw - (DRAW_ENTRY + 5));
        b[DRAW_ENTRY + 5] = (byte) 0x90;
        b[DRAW_ENTRY + 6] = (byte) 0x90;
        b[DRAW_ENTRY + 7] = (byte) 0x90;
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
