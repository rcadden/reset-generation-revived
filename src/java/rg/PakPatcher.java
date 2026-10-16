package rg;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.Deflater;
import java.util.zip.Inflater;

/**
 * Adapts the game's data packs to the engine resolution chosen at launch.
 * Each pak is rebuilt from a pristine "*.pak.orig" copy; a "*.pak.revived"
 * stamp records what it was patched for so this only runs when that changes.
 *
 * base.pak: screen-transition masks (data/base/gfx/fadeWipe*.gfx) are resized.
 * The wipe effect composites the new screen through a mask image and loops over
 * the full screen while indexing the mask with its own stride. The shipped masks
 * are 240x320, so any taller screen reads past the mask (crash) and any wider
 * screen shows striped artefacts. Resizing the masks fixes both.
 *
 * intro.pak / story.pak: cutscene scripts place sprites in absolute 240x320
 * coordinates; they are offset so the scene sits in the centre of the screen.
 *
 * Pak layout (little endian): u32 headerSize, u32 count, then count entries of
 * {u32 nameHash, u32 storedSize, u32 rawSize, u8 zlib, u32 offset}. The last
 * entry lists every other entry's path, in order.
 */
final class PakPatcher {
    private static final String MASK_PREFIX = "data\\base\\gfx\\fadewipe";
    private static final int GFX_HEADER = 40;

    private PakPatcher() {
    }

    static final class Entry {
        int hash;
        boolean zlib;
        byte[] raw;
    }

    /** Patch format version; bump when transforms change so old patched paks are rebuilt. */
    private static final String VERSION = "4";

    /** Ensures every patched pak matches the engine resolution. Safe to call every launch. */
    static void ensureAll(File gameDir, int width, int height) {
        final int w = width;
        final int h = height;
        // Cutscenes are authored in a 240x320 portrait space; centre them.
        final int dx = Math.max(0, (width - 240) / 2);
        final int dy = Math.max(0, (height - 320) / 2);
        patchPak(gameDir, "base", w + "x" + h, new Transform() {
            public byte[] apply(String name, byte[] raw) {
                return name.startsWith(MASK_PREFIX) ? resizeMask(raw, w, h) : null;
            }
        });
        restoreUnmanaged(gameDir);
        for (int i = 0; i < SCENE_PAKS.length; ++i) {
            final java.util.regex.Pattern files = java.util.regex.Pattern.compile(SCENE_FILES[i]);
            patchPak(gameDir, SCENE_PAKS[i], dx + "," + dy, new Transform() {
                public byte[] apply(String name, byte[] raw) {
                    return files.matcher(name).matches() ? offsetScene(raw, dx, dy) : null;
                }
            });
        }
    }

    /**
     * Scene scripts that use absolute 240x320 coordinates and are drawn straight to
     * the screen, per pak (regex on the lower-case path). Not menus.pak: its
     * win/lose screens and fight.pak's cut-ins render into a fixed 240x320 surface,
     * so shifting their scripts moves content out of view (verified 2026-10-16).
     */
    static final String[] SCENE_PAKS = {"intro", "story"};
    static final String[] SCENE_FILES = {
        ".*\\.xml",
        ".*\\.xml",
    };

    /** Puts back the original of any pak an earlier version patched but this one does not. */
    static void restoreUnmanaged(File gameDir) {
        java.util.Set<String> managed = new java.util.HashSet<String>(java.util.Arrays.asList(SCENE_PAKS));
        managed.add("base");
        File[] stamps = gameDir.listFiles();
        if (stamps == null) {
            return;
        }
        for (File stamp : stamps) {
            String n = stamp.getName();
            if (!n.endsWith(".pak.revived")) {
                continue;
            }
            String base = n.substring(0, n.length() - ".pak.revived".length());
            File orig = new File(gameDir, base + ".pak.orig");
            if (managed.contains(base) || !orig.exists()) {
                continue;
            }
            try {
                copy(orig, new File(gameDir, base + ".pak"));
                stamp.delete();
                System.out.println("Revived: restored original " + base + ".pak");
            } catch (IOException ex) {
                System.out.println("Revived: could not restore " + base + ".pak (" + ex + ")");
            }
        }
    }

    interface Transform {
        /** Returns replacement bytes, or null to keep the entry. Names are lower-case. */
        byte[] apply(String name, byte[] raw);
    }

    static void patchPak(File gameDir, String base, String key, Transform t) {
        File pak = new File(gameDir, base + ".pak");
        File orig = new File(gameDir, base + ".pak.orig");
        File stamp = new File(gameDir, base + ".pak.revived");
        String want = VERSION + ":" + key;
        try {
            if (stamp.exists() && want.equals(new String(readAll(stamp), "UTF-8").trim()) && pak.exists()) {
                return;
            }
            if (!orig.exists()) {
                if (!pak.exists()) {
                    return;
                }
                copy(pak, orig);
            }
            List<Entry> entries = read(readAll(orig));
            List<String> names = nameList(entries.get(entries.size() - 1).raw);
            int patched = 0;
            for (int i = 0; i < entries.size() - 1 && names != null; ++i) {
                byte[] out = t.apply(names.get(i).toLowerCase(), entries.get(i).raw);
                if (out != null) {
                    entries.get(i).raw = out;
                    ++patched;
                }
            }
            File tmp = new File(gameDir, base + ".pak.tmp");
            OutputStream out = new FileOutputStream(tmp);
            try {
                out.write(write(entries));
            } finally {
                out.close();
            }
            if (pak.exists() && !pak.delete()) {
                throw new IOException("cannot replace " + pak);
            }
            if (!tmp.renameTo(pak)) {
                throw new IOException("cannot rename " + tmp);
            }
            OutputStream s = new FileOutputStream(stamp);
            try {
                s.write(want.getBytes("UTF-8"));
            } finally {
                s.close();
            }
            System.out.println("Revived: patched " + patched + " entries in " + base + ".pak (" + key + ")");
        } catch (Exception ex) {
            System.out.println("Revived: could not patch " + base + ".pak (" + ex + ")");
        }
    }

    private static final java.util.regex.Pattern PHYSICS_RESET =
            java.util.regex.Pattern.compile("<physics\\b[^>]*type=\"RESET\"[^>]*>");
    private static final java.util.regex.Pattern PARTICLE =
            java.util.regex.Pattern.compile("<effect\\b[^>]*name=\"PARTICLE\"[^>]*>");

    /** Shifts absolute positions in a scene script by (dx, dy). */
    static byte[] offsetScene(byte[] raw, int dx, int dy) {
        if (dx == 0 && dy == 0) {
            return null;
        }
        String xml;
        try {
            xml = new String(raw, "ISO-8859-1");
        } catch (java.io.UnsupportedEncodingException ex) {
            return null;
        }
        String out = shift(xml, PHYSICS_RESET, "positionX", "positionY", dx, dy);
        out = shift(out, PARTICLE, "posX", "posY", dx, dy);
        if (out.equals(xml)) {
            return null;
        }
        try {
            return out.getBytes("ISO-8859-1");
        } catch (java.io.UnsupportedEncodingException ex) {
            return null;
        }
    }

    private static String shift(String xml, java.util.regex.Pattern tag, String ax, String ay, int dx, int dy) {
        java.util.regex.Matcher m = tag.matcher(xml);
        StringBuffer sb = new StringBuffer();
        while (m.find()) {
            String t = addTo(addTo(m.group(), ax, dx), ay, dy);
            m.appendReplacement(sb, java.util.regex.Matcher.quoteReplacement(t));
        }
        m.appendTail(sb);
        return sb.toString();
    }

    private static String addTo(String tag, String attr, int d) {
        java.util.regex.Matcher m = java.util.regex.Pattern.compile("\\b" + attr + "=\"(-?[0-9.]+)\"").matcher(tag);
        if (!m.find()) {
            return tag;
        }
        String v = m.group(1);
        String nv;
        if (v.indexOf('.') >= 0) {
            nv = String.valueOf(Double.parseDouble(v) + d);
        } else {
            nv = String.valueOf(Integer.parseInt(v) + d);
        }
        return tag.substring(0, m.start(1)) + nv + tag.substring(m.end(1));
    }

    /** Nearest-neighbour horizontal stretch, vertical tiling: keeps the halftone look. */
    static byte[] resizeMask(byte[] gfx, int w, int h) {
        ByteBuffer in = ByteBuffer.wrap(gfx).order(ByteOrder.LITTLE_ENDIAN);
        int format = in.getInt(4);
        int sw = in.getInt(8);
        int sh = in.getInt(12);
        if (format != 0 || sw <= 0 || sh <= 0) {
            return gfx;
        }
        ByteBuffer out = ByteBuffer.allocate(GFX_HEADER + w * h * 4).order(ByteOrder.LITTLE_ENDIAN);
        out.put(gfx, 0, GFX_HEADER);
        out.putInt(8, w);
        out.putInt(12, h);
        out.putInt(16, 1);
        out.putInt(36, w * h * 4);
        for (int y = 0; y < h; ++y) {
            int sy = y % sh;
            for (int x = 0; x < w; ++x) {
                int sx = (int) ((long) x * sw / w);
                out.putInt(GFX_HEADER + (y * w + x) * 4, in.getInt(GFX_HEADER + (sy * sw + sx) * 4));
            }
        }
        return out.array();
    }

    static List<Entry> read(byte[] b) throws IOException {
        ByteBuffer bb = ByteBuffer.wrap(b).order(ByteOrder.LITTLE_ENDIAN);
        int count = bb.getInt(4);
        List<Entry> list = new ArrayList<Entry>();
        for (int i = 0; i < count; ++i) {
            int p = 8 + i * 17;
            Entry e = new Entry();
            e.hash = bb.getInt(p);
            int stored = bb.getInt(p + 4);
            int rawSize = bb.getInt(p + 8);
            e.zlib = b[p + 12] != 0;
            int off = bb.getInt(p + 13);
            byte[] data = new byte[stored];
            System.arraycopy(b, off, data, 0, stored);
            if (e.zlib) {
                Inflater inf = new Inflater();
                inf.setInput(data);
                byte[] raw = new byte[rawSize];
                try {
                    int n = inf.inflate(raw);
                    if (n != rawSize) {
                        throw new IOException("short inflate");
                    }
                } catch (java.util.zip.DataFormatException ex) {
                    throw new IOException(ex);
                } finally {
                    inf.end();
                }
                e.raw = raw;
            } else {
                e.raw = data;
            }
            list.add(e);
        }
        return list;
    }

    static byte[] write(List<Entry> entries) throws IOException {
        int count = entries.size();
        int header = 8 + count * 17;
        List<byte[]> stored = new ArrayList<byte[]>();
        for (Entry e : entries) {
            stored.add(e.zlib ? deflate(e.raw) : e.raw);
        }
        ByteBuffer hb = ByteBuffer.allocate(header).order(ByteOrder.LITTLE_ENDIAN);
        hb.putInt(header);
        hb.putInt(count);
        int off = header;
        for (int i = 0; i < count; ++i) {
            Entry e = entries.get(i);
            hb.putInt(e.hash);
            hb.putInt(stored.get(i).length);
            hb.putInt(e.raw.length);
            hb.put((byte) (e.zlib ? 1 : 0));
            hb.putInt(off);
            off += stored.get(i).length;
        }
        ByteArrayOutputStream out = new ByteArrayOutputStream(off);
        out.write(hb.array());
        for (byte[] s : stored) {
            out.write(s);
        }
        return out.toByteArray();
    }

    static List<String> nameList(byte[] d) {
        try {
            ByteBuffer bb = ByteBuffer.wrap(d).order(ByteOrder.LITTLE_ENDIAN);
            int n = bb.getInt();
            List<String> names = new ArrayList<String>();
            for (int i = 0; i < n; ++i) {
                int len = bb.getShort() & 0xFFFF;
                byte[] s = new byte[len];
                bb.get(s);
                names.add(new String(s, "ISO-8859-1"));
            }
            return names;
        } catch (Exception ex) {
            return null;
        }
    }

    static byte[] deflate(byte[] raw) {
        Deflater d = new Deflater(Deflater.BEST_COMPRESSION);
        d.setInput(raw);
        d.finish();
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[65536];
        while (!d.finished()) {
            out.write(buf, 0, d.deflate(buf));
        }
        d.end();
        return out.toByteArray();
    }

    static byte[] readAll(File f) throws IOException {
        InputStream in = new FileInputStream(f);
        try {
            ByteArrayOutputStream out = new ByteArrayOutputStream((int) f.length());
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
            return out.toByteArray();
        } finally {
            in.close();
        }
    }

    static void copy(File from, File to) throws IOException {
        OutputStream out = new FileOutputStream(to);
        try {
            out.write(readAll(from));
        } finally {
            out.close();
        }
    }
}
