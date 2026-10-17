package rg;

import java.io.File;

/**
 * Command-line access to the launch-time data patches, for builds that have no Java at run
 * time (the native/Android host): {@code java rg.PatchTool <ResetGeneration dir> <width> <height>}
 * patches the paks and the engine DLL copy in place, keeping the originals as *.orig.
 */
public class PatchTool {
    public static void main(String[] args) {
        if (args.length != 3) {
            System.err.println("usage: rg.PatchTool <game data dir> <width> <height>");
            System.exit(2);
        }
        File dir = new File(args[0]);
        int w = Integer.parseInt(args[1]);
        int h = Integer.parseInt(args[2]);
        PakPatcher.ensureAll(dir, w, h);
        DllPatcher.ensure(dir, w, h);
    }
}
