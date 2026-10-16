package rg;

import java.awt.Color;
import java.awt.Dimension;
import java.awt.DisplayMode;
import java.awt.GraphicsDevice;
import java.awt.GraphicsEnvironment;
import java.awt.KeyEventDispatcher;
import java.awt.KeyboardFocusManager;
import java.awt.Rectangle;
import java.awt.event.KeyEvent;
import java.awt.event.WindowAdapter;
import java.awt.event.WindowEvent;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Properties;
import javax.swing.JFrame;
import javax.swing.SwingUtilities;

/**
 * Reset Generation Revived: modern standalone launcher.
 *
 * Replaces the 2020 community StandaloneWrapper. The native engine
 * (ResetGeneration-Win32-Web.dll) lays its UI out for whatever screen size it
 * is initialised with, so we pick a low "virtual" resolution that matches the
 * monitor's aspect ratio and integer-scales it up to the window.
 *
 * Settings live in revived.ini next to the jar; command-line flags override them.
 */
public class Revived extends StandaloneClient {
    private static final long serialVersionUID = 1L;

    /** Largest frame the engine has been verified to handle (see docs/ENGINE_NOTES.md). */
    static final int MAX_RENDER_PIXELS = 1920 * 1080;

    /** Smallest engine height chosen automatically; cutscenes are 320 tall. */
    static final int MIN_AUTO_HEIGHT = 320;
    /** The engine's UI assumes at least the original 240-pixel screen edge. */
    static final int MIN_SIZE = 240;

    static Properties config = new Properties();
    static File configFile;

    static int renderWidth;
    static int renderHeight;
    static int windowScale;
    static boolean startFullscreen;
    static boolean startMaximized;

    final LauncherApplet applet;
    private boolean fullscreen = false;
    private Rectangle windowedBounds = null;

    Revived(String title) {
        super(title);
        try {
            java.net.URL icon = Revived.class.getResource("/revived/icon.png");
            if (icon != null) {
                this.setIconImage(javax.imageio.ImageIO.read(icon));
            }
        } catch (Exception ex) {
            // cosmetic only
        }
        this.applet = new LauncherApplet(this);
        this.applet.setBackground(Color.BLACK);
        this.getContentPane().setBackground(Color.BLACK);
        this.applet.init();
        this.applet.start();
        this.applet.setPreferredSize(new Dimension(renderWidth * windowScale, renderHeight * windowScale));
        this.getContentPane().add(this.applet);
        this.setResizable(true);
        this.setMinimumSize(new Dimension(320, 200));
        this.pack();
        this.setLocationRelativeTo(null);
        if (startMaximized) {
            this.setExtendedState(JFrame.MAXIMIZED_BOTH);
        }
        this.setDefaultCloseOperation(JFrame.DO_NOTHING_ON_CLOSE);
        this.addWindowListener(new WindowAdapter() {
            @Override
            public void windowClosing(WindowEvent e) {
                Revived.this.applet.stop();
                System.exit(0);
            }

            @Override
            public void windowActivated(WindowEvent e) {
                Revived.this.applet.requestFocus();
            }
        });
        KeyboardFocusManager.getCurrentKeyboardFocusManager().addKeyEventDispatcher(new KeyEventDispatcher() {
            @Override
            public boolean dispatchKeyEvent(KeyEvent e) {
                return Revived.this.handleHotkey(e);
            }
        });
        this.applet.setFocusable(true);
        this.setVisible(true);
        this.applet.requestFocus();
        if (startFullscreen) {
            this.setFullscreen(true);
        }
    }

    /** Launcher-level hotkeys; returns true if the event was consumed. */
    boolean handleHotkey(KeyEvent e) {
        boolean toggle = e.getKeyCode() == KeyEvent.VK_F11
                || (e.getKeyCode() == KeyEvent.VK_ENTER && e.isAltDown());
        if (!toggle) {
            return false;
        }
        if (e.getID() == KeyEvent.KEY_PRESSED) {
            SwingUtilities.invokeLater(new Runnable() {
                @Override
                public void run() {
                    Revived.this.setFullscreen(!Revived.this.fullscreen);
                }
            });
        }
        return true;
    }

    /** Borderless fullscreen on the window's current monitor. */
    void setFullscreen(boolean on) {
        if (on == this.fullscreen) {
            return;
        }
        GraphicsDevice dev = this.getGraphicsConfiguration().getDevice();
        if (on) {
            this.windowedBounds = this.getBounds();
            this.dispose();
            this.setUndecorated(true);
            this.setBounds(dev.getDefaultConfiguration().getBounds());
            this.setVisible(true);
        } else {
            this.dispose();
            this.setUndecorated(false);
            if (this.windowedBounds != null) {
                this.setBounds(this.windowedBounds);
            }
            this.setVisible(true);
        }
        this.fullscreen = on;
        this.applet.requestFocus();
        config.setProperty("fullscreen", String.valueOf(on));
        saveConfig();
    }

    static String opt(String key, String def) {
        String v = System.getProperty("rg." + key);
        if (v == null) {
            v = config.getProperty(key);
        }
        return v == null ? def : v.trim();
    }

    static int optInt(String key, int def) {
        try {
            return Integer.parseInt(opt(key, String.valueOf(def)));
        } catch (NumberFormatException ex) {
            return def;
        }
    }

    static void loadConfig() {
        try {
            File jarDir = new File(PlatformAbstractor.getInstance().getRGHomeDirectory()).getParentFile();
            configFile = new File(jarDir, "revived.ini");
            if (configFile.exists()) {
                InputStream in = new FileInputStream(configFile);
                try {
                    config.load(in);
                } finally {
                    in.close();
                }
            }
        } catch (Exception ex) {
            System.out.println("revived.ini not loaded: " + ex);
        }
    }

    static void saveConfig() {
        if (configFile == null) {
            return;
        }
        try {
            OutputStream out = new FileOutputStream(configFile);
            try {
                config.store(out, "Reset Generation Revived settings (see README)");
            } finally {
                out.close();
            }
        } catch (Exception ex) {
            System.out.println("revived.ini not saved: " + ex);
        }
    }

    /**
     * Works out the engine's virtual resolution.
     * render_height=auto picks the largest integer scale that keeps at least
     * 320 rows (the height the cutscenes were drawn for), so pixel art fills the
     * monitor exactly: 640x360 on any 16:9 display (x2 at 720p, x3 at 1080p,
     * x4 at 1440p, x6 at 4K).
     */
    static void chooseResolution() {
        DisplayMode dm = GraphicsEnvironment.getLocalGraphicsEnvironment().getDefaultScreenDevice().getDisplayMode();
        int sw = dm.getWidth();
        int sh = dm.getHeight();
        String rh = opt("render_height", "auto");
        String rw = opt("render_width", "auto");
        int scale;
        if (rh.equalsIgnoreCase("auto")) {
            scale = Math.max(1, sh / MIN_AUTO_HEIGHT);
            renderHeight = sh / scale;
        } else {
            renderHeight = Integer.parseInt(rh);
            scale = Math.max(1, sh / renderHeight);
        }
        if (rw.equalsIgnoreCase("auto")) {
            renderWidth = (int) Math.round((double) renderHeight * sw / sh);
        } else {
            renderWidth = Integer.parseInt(rw);
        }
        renderWidth = Math.max(MIN_SIZE, renderWidth);
        renderHeight = Math.max(MIN_SIZE, renderHeight);
        while ((long) renderWidth * renderHeight > MAX_RENDER_PIXELS) {
            renderWidth = renderWidth * 9 / 10;
            renderHeight = renderHeight * 9 / 10;
        }
        windowScale = optInt("window_scale", 0);
        if (windowScale <= 0) {
            // Largest integer scale that fits comfortably inside the desktop.
            windowScale = Math.max(1, Math.min((sw - 80) / renderWidth, (sh - 120) / renderHeight));
            // Only 1x fits (small laptop screens): open maximized and scale fractionally instead.
            startMaximized = windowScale == 1 && renderWidth * 10 < sw * 7;
        }
        startFullscreen = Boolean.parseBoolean(opt("fullscreen", "false"));
        ResetGeneration.SMOOTH_SCALING = opt("filter", "nearest").equalsIgnoreCase("smooth");
        System.out.println("Revived: screen " + sw + "x" + sh + ", engine " + renderWidth + "x" + renderHeight
                + ", window x" + windowScale + (startFullscreen ? ", fullscreen" : ""));
    }

    public static void main(String[] args) {
        System.setProperty("sun.java2d.uiScale", "1");
        System.setProperty("sun.awt.noerasebackground", "true");
        loadConfig();
        for (int i = 0; i < args.length; ++i) {
            String a = args[i];
            if (a.startsWith("--") && a.contains("=")) {
                String[] kv = a.substring(2).split("=", 2);
                System.setProperty("rg." + kv[0], kv[1]);
            } else if (a.equals("--fullscreen")) {
                System.setProperty("rg.fullscreen", "true");
            } else if (a.equals("--windowed")) {
                System.setProperty("rg.fullscreen", "false");
            }
        }
        chooseResolution();
        if (configFile != null) {
            PakPatcher.ensureAll(new File(configFile.getParentFile(), "ResetGeneration"), renderWidth, renderHeight);
        }
        ResetGeneration.SCREEN_W = renderWidth;
        ResetGeneration.SCREEN_H = renderHeight;
        ResetGeneration.DOUBLE_PIXELS = true;
        ResetGeneration.CLEAR_EACH_FRAME = true;
        new Revived("Reset Generation");
    }
}
