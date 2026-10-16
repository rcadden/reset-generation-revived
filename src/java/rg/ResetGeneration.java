/*
 * Decompiled with CFR 0.152.
 */
package rg;

import com.redlynx.mt.snapmobile.SMLog;
import com.redlynx.mt.snapmobile.SNAPMobileClient;
import com.redlynx.mt.snapmobile.SNAPMobileSSOProvider;
import java.applet.Applet;
import java.awt.Container;
import java.awt.Font;
import java.awt.Graphics;
import java.awt.Graphics2D;
import java.awt.GraphicsEnvironment;
import java.awt.Image;
import java.awt.event.FocusEvent;
import java.awt.event.FocusListener;
import java.awt.event.InputEvent;
import java.awt.event.KeyEvent;
import java.awt.event.KeyListener;
import java.awt.event.MouseAdapter;
import java.awt.event.MouseEvent;
import java.awt.event.MouseMotionListener;
import java.awt.image.BufferedImage;
import java.io.File;
import java.io.FilenameFilter;
import java.io.IOException;
import java.io.InputStream;
import java.net.MalformedURLException;
import java.util.LinkedList;
import java.util.Properties;
import java.util.Vector;
import rg.AppletSettings;
import rg.PlatformAbstractor;
import rg.SharedBackBuffer;
import rg.SoundSourceWrapper;
import rg.activity.ActivityManager;
import rg.downloadmanager.DownloadManager;
import rg.downloadmanager.LoadVisualizer;
import rg.downloadmanager.ReplayDataDownloadTask;
import rg.natives.NativeGameEngine;
import rg.natives.NativeInputEventListener;
import rg.natives.NativeSoundOutputDevice;
import rg.screens.GameScreen;
import rg.screens.LoginScreen;
import rg.screens.PlayerInterfaceScreen;
import rg.screens.Screen;
import rg.screens.fx.ImageLoader;
import rg.sound.SoundOutput;
import rg.tools.CookieHandler;

public class ResetGeneration
extends MouseAdapter
implements KeyListener,
MouseMotionListener,
SNAPMobileSSOProvider {
    private static final int BG_COLOR = 17408;
    public static final int SCREEN_WIDTH_GAME = 240;
    public static final int SCREEN_HEIGHT_GAME = 320;
    public static final int SCREEN_WIDTH_REPLAY = 420;
    public static final int SCREEN_HEIGHT_REPLAY = 378;
    private static final String DEFAULT_SM_HOST = "resetgeneration-gw.arena.n-gage.com";
    private static final String SM_CONNECTORS = "tcp,http:80";
    private static final String SM_LOBBY = "resgen";
    private static final String SM_USERNAME = "";
    private static final String SM_PASSWD = "";
    private static int gameclass = 42713;
    private static String gateway = "resetgeneration-gw.arena.n-gage.com";
    private static final String SM_PRESENCE = "";
    private static String unifiedLoginURL = "not.used.for.now";
    private static String snapLoginURL = "not.used.for.now";
    public static String LANGUAGE = "en";
    public static Properties localizedStrings;
    private int screenWidth;
    private int screenHeight;
    private SNAPMobileClient smClient;
    private String username;
    private String passwd;
    private Applet theApplet;
    private SharedBackBuffer backBuffer;
    BufferedImage waitImg = null;
    private NativeGameEngine gameEngine;
    private NativeInputEventListener eventListener;
    private NativeSoundOutputDevice soundOutputDevice;
    private SoundOutput soundOutput;
    private boolean isRunning;
    public int waitForKeyReleaseCode;
    public LinkedList<InputEvent> keyEventQueue;
    private LoginScreen loginScreen;
    private PlayerInterfaceScreen replayPlayerScreen;
    private GameScreen gameScreen;
    private Screen activeScreen;
    private DownloadManager downloadManager;
    private String homeDir;
    private LoadVisualizer loadVisualizer;
    private AppletSettings appletSettings;
    private SoundSourceWrapper soundOutputWrapper;
    private boolean inPlayerMode;
    private String replayDataFilename;
    private ReplayDataDownloadTask replayDataDownloader;
    public static String commandLineArguments;
    public static String dbgfilename;
    public static String overrideUsername;
    public static String overridePassword;
    public static String overrideGateway;
    public static String overrideGameclass;
    public static int overrideTimeout;
    public static int overrideInterval;
    private BufferedImage[] helpImages;
    private BufferedImage[] focusImages;
    private static final int HELP_ENGLISH = 0;
    private static final int HELP_FRENCH = 1;
    private static final int HELP_SPANISH = 2;
    private static final int HELP_ITALIAN = 3;
    private static final int HELP_GERMAN = 4;
    private static final int HELP_CHINESE_SIMPLIFIED = 5;
    private static final int HELP_CHINESE_TRADITIONAL = 6;
    private static final int HELP_RUSSIAN = 7;
    static boolean DOUBLE_PIXELS;
    /** Engine framebuffer size; set by the Revived launcher. */
    static int SCREEN_W = 240;
    static int SCREEN_H = 320;
    /** Clear the native framebuffer before every engine tick (needed when the
     *  screen is larger than the art the engine was designed for). */
    static boolean CLEAR_EACH_FRAME = false;
    static boolean SMOOTH_SCALING = false;
    private boolean mouseClickedFirst = false;
    private boolean helpIsVisible = false;
    private boolean outOfFocusVisible = false;
    private boolean standalone;
    private Object paintLock = new Object();
    private LinkedList<Screen> screenStack = new LinkedList();
    private int languageTicks;
    private int cachedLanguage;
    private FocusListener focusListener = null;
    private boolean exitInternal = false;

    void reloadLocalizedStrings() {
        localizedStrings = new Properties();
        InputStream stream = ActivityManager.getInstance().getAnyAppletClass().getResourceAsStream("/resources/" + LANGUAGE.replace('-', '_') + "/lang.properties");
        try {
            localizedStrings.load(stream);
            stream.close();
        }
        catch (IOException e1) {
            localizedStrings = new Properties();
            localizedStrings.put("load.failed", "Library loading failed");
            localizedStrings.put("save.failed", "Unable to save to local drive");
            localizedStrings.put("permission.denied", "Permission denied");
            localizedStrings.put("server.failure", "Server failure. Try again later");
            localizedStrings.put("loading.msg", "Loading...");
            localizedStrings.put("downloading.msg", "Downloading...");
            localizedStrings.put("verifying.msg", "Verifying...");
            localizedStrings.put("integrity.check", "Local file integrity check");
            localizedStrings.put("invalid.url", "Invalid replay data URL");
        }
    }

    public ResetGeneration(Applet theApplet, String replayDataFile, boolean standalone) {
        this.theApplet = theApplet;
        this.username = "";
        this.passwd = "";
        this.standalone = standalone;
        if (overrideGameclass != null) {
            gameclass = Integer.valueOf(overrideGameclass);
        }
        if (overrideGateway != null) {
            gateway = overrideGateway;
        }
        this.reloadLocalizedStrings();
        this.inPlayerMode = replayDataFile != null && replayDataFile.trim().length() > 0;
        this.replayDataFilename = replayDataFile;
        this.screenWidth = this.inPlayerMode ? 420 : SCREEN_W;
        this.screenHeight = this.inPlayerMode ? 378 : SCREEN_H;
        this.backBuffer = new SharedBackBuffer(this.screenWidth, this.screenHeight, this.inPlayerMode);
        try {
            PlatformAbstractor.getInstance();
            this.helpImages = new BufferedImage[8];
            this.helpImages[0] = ImageLoader.loadRGBImage("/resources/en/help.png");
            this.helpImages[1] = ImageLoader.loadRGBImage("/resources/fr/help.png");
            this.helpImages[2] = ImageLoader.loadRGBImage("/resources/es/help.png");
            this.helpImages[3] = ImageLoader.loadRGBImage("/resources/it/help.png");
            this.helpImages[4] = ImageLoader.loadRGBImage("/resources/de/help.png");
            this.helpImages[5] = ImageLoader.loadRGBImage("/resources/zh_cn/help.png");
            this.helpImages[6] = ImageLoader.loadRGBImage("/resources/zh/help.png");
            this.helpImages[7] = ImageLoader.loadRGBImage("/resources/ru/help.png");
            this.focusImages = new BufferedImage[8];
            this.focusImages[0] = ImageLoader.loadRGBImage("/resources/en/out_of_focus.png");
            this.focusImages[1] = ImageLoader.loadRGBImage("/resources/fr/out_of_focus.png");
            this.focusImages[2] = ImageLoader.loadRGBImage("/resources/es/out_of_focus.png");
            this.focusImages[3] = ImageLoader.loadRGBImage("/resources/it/out_of_focus.png");
            this.focusImages[4] = ImageLoader.loadRGBImage("/resources/de/out_of_focus.png");
            this.focusImages[5] = ImageLoader.loadRGBImage("/resources/zh_cn/out_of_focus.png");
            this.focusImages[6] = ImageLoader.loadRGBImage("/resources/zh/out_of_focus.png");
            this.focusImages[7] = ImageLoader.loadRGBImage("/resources/ru/out_of_focus.png");
            this.waitImg = replayDataFile == null || replayDataFile.trim().length() == 0 ? ImageLoader.loadRGBImage("/resources/Loading_bg.png") : ImageLoader.loadRGBImage("/resources/loading_bg_player.png");
            this.gameScreen = new GameScreen(theApplet, this);
            this.loginScreen = new LoginScreen(theApplet, this, this.backBuffer, this.username, this.passwd, 240, 320);
            if (this.inPlayerMode) {
                this.replayPlayerScreen = new PlayerInterfaceScreen(theApplet, this);
            }
        }
        catch (Exception exception) {
            // empty catch block
        }
        this.homeDir = PlatformAbstractor.getInstance().getRGHomeDirectory();
        this.loadVisualizer = new LoadVisualizer(this, this.screenWidth, this.screenHeight);
        this.downloadManager = new DownloadManager(this.homeDir, PlatformAbstractor.getInstance().getPlatformDescription());
        if (PlatformAbstractor.getInstance() == null) {
            this.downloadManager.fatalError("" + localizedStrings.get("permission.denied"));
            this.isRunning = true;
            return;
        }
        try {
            this.appletSettings = new AppletSettings(theApplet);
            this.appletSettings.load();
        }
        catch (Exception e3) {
            SMLog.println("Error in loading settings:");
            SMLog.printStackTrace(e3);
        }
        if (this.inPlayerMode) {
            try {
                this.replayDataDownloader = new ReplayDataDownloadTask(this.replayDataFilename, this.downloadManager);
                this.downloadManager.addExternalTask(this.replayDataDownloader);
            }
            catch (MalformedURLException e4) {
                this.downloadManager.fatalError("" + localizedStrings.get("invalid.url"));
                this.isRunning = true;
            }
            this.activeScreen = this.replayPlayerScreen;
        } else {
            this.activeScreen = this.gameScreen;
        }
        this.keyEventQueue = new LinkedList();
        this.isRunning = true;
        this.focusListener = new FocusListener(){

            @Override
            public void focusLost(FocusEvent e2) {
                ResetGeneration.this.outOfFocusVisible = true;
                ResetGeneration.this.loseFocusNotifyKeys();
            }

            @Override
            public void focusGained(FocusEvent e2) {
                ResetGeneration.this.outOfFocusVisible = false;
            }
        };
        theApplet.addFocusListener(this.focusListener);
    }

    public void startDownloadManager() {
        if (!this.standalone) {
            this.downloadManager.start();
        }
    }

    void initAfterNativeLoad() throws Exception {
        LinkedList<String> dllList;
        String homeDirectory = this.homeDir;
        if (this.standalone) {
            SMLog.printf("GETTING LOCAL FILES DLL\n");
            dllList = this.getLocalFiles("dll", true);
        } else {
            SMLog.printf("NOT GETTING LOCAL DLLs\u00bd!!!!!\n");
            dllList = this.downloadManager.getLoadableLibraryList();
        }
        this.gameEngine = NativeGameEngine.getInstance();
        this.gameEngine.loadNativeLibs(dllList);
        this.gameEngine.setHomeDirectory(homeDirectory);
        this.eventListener = new NativeInputEventListener();
        this.soundOutputDevice = new NativeSoundOutputDevice();
    }

    private LinkedList<String> getLocalFiles(final String suffix, boolean absolutePaths) {
        File ld = new File(this.homeDir);
        String abs = new File(ld.getAbsolutePath()).getAbsolutePath();
        String[] f2 = ld.list(new FilenameFilter(){

            @Override
            public boolean accept(File dir, String name) {
                return name.endsWith(suffix);
            }
        });
        LinkedList<String> dllList = new LinkedList<String>();
        for (int i2 = 0; i2 < f2.length; ++i2) {
            if (absolutePaths) {
                dllList.add(abs + PlatformAbstractor.getInstance().getFileSeparator() + f2[i2]);
                continue;
            }
            dllList.add(f2[i2]);
        }
        return dllList;
    }

    public void startSoundOutput() {
        this.soundOutputWrapper = new SoundSourceWrapper(this.soundOutputDevice);
        this.soundOutput = new SoundOutput(this.soundOutputWrapper);
        this.soundOutput.setSoundListener(this.soundOutputWrapper);
        Thread soundOutputThread = new Thread(this.soundOutput);
        soundOutputThread.setPriority(10);
        soundOutputThread.start();
    }

    public void initRG() {
        try {
            this.initAfterNativeLoad();
        }
        catch (Exception e2) {
            this.downloadManager.fatalError("" + localizedStrings.getProperty("load.failed"));
            return;
        }
        this.smClient = new SNAPMobileClient(gateway, SM_CONNECTORS, SM_LOBBY, this.loginScreen, gameclass, "", this.theApplet);
        this.smClient.setClientSSOProvider(this);
        if (this.inPlayerMode) {
            this.gameEngine.setReplayData(this.replayDataDownloader.getReplay());
        }
        this.backBuffer.clear(17408);
        this.gameEngine.init(this.backBuffer.getNativeIntBuffer(), LANGUAGE, this.inPlayerMode ? 1 : 0, this.screenWidth, this.screenHeight, commandLineArguments, overrideUsername, overrideTimeout, overrideInterval);
        this.smClient.initNative();
        SMLog.initNative();
        this.startSoundOutput();
        if (this.standalone) {
            LinkedList<String> pakFiles = this.getLocalFiles("pak", false);
            for (String s2 : pakFiles) {
                NativeGameEngine.getInstance().notifyDataPackageReady(s2);
            }
        }
        this.isRunning = true;
    }

    public void saveSettings() {
    }

    public void uninitRG() {
        if (this.smClient != null) {
            this.smClient.uninit();
            this.smClient = null;
        }
        this.theApplet.removeFocusListener(this.focusListener);
        this.focusListener = null;
        if (this.soundOutput != null) {
            this.soundOutput.stop();
        }
        if (this.gameEngine != null) {
            this.gameEngine.deinit();
            NativeGameEngine.uninitInstance();
        }
        if (!this.standalone) {
            this.downloadManager.stop();
        }
        if (this.appletSettings != null) {
            this.appletSettings.setApplet(null);
        }
    }

    public boolean tickRG() throws Exception {
        String nextNotification;
        this.dispatchEvents();
        if (this.exitInternal) {
            return false;
        }
        if (!this.standalone && (nextNotification = this.downloadManager.getNextPackageNotification()) != null && nextNotification.endsWith(".pak")) {
            this.gameEngine.notifyDataPackageReady(nextNotification);
        }
        if (CLEAR_EACH_FRAME || Boolean.getBoolean("rg.clear")) {
            this.backBuffer.clearNative(0);
        }
        this.isRunning = this.gameEngine.tick();
        this.smClient.tick();
        return this.isRunning;
    }

    public boolean isRunning() {
        return this.isRunning;
    }

    /*
     * WARNING - Removed try catching itself - possible behaviour change.
     */
    public void dispatchEvents() {
        LinkedList<InputEvent> linkedList;
        LinkedList<InputEvent> linkedList2 = linkedList = this.keyEventQueue;
        synchronized (linkedList2) {
            while (!this.keyEventQueue.isEmpty()) {
                InputEvent event = this.keyEventQueue.removeFirst();
                if (this.helpIsVisible || this.outOfFocusVisible) {
                    if (event.getID() != 505) {
                        this.hideHelpOrFocus();
                    }
                    if (event.getID() == 401) {
                        this.waitForKeyReleaseCode = ((KeyEvent)event).getKeyCode();
                    }
                    if (event.getID() != 505) continue;
                }
                if (this.waitForKeyReleaseCode != 0 && event.getID() != 505) {
                    if (event.getID() != 402 || this.waitForKeyReleaseCode != ((KeyEvent)event).getKeyCode()) continue;
                    this.waitForKeyReleaseCode = 0;
                    continue;
                }
                switch (event.getID()) {
                    case 401: {
                        KeyEvent keyEvent = (KeyEvent)event;
                        int keyCode = keyEvent.getKeyCode();
                        char keyChar = keyEvent.getKeyChar();
                        if (this.inPlayerMode) break;
                        this.eventListener.onKeyPressedJava(keyEvent.getWhen(), keyCode, keyChar, keyEvent.getKeyLocation());
                        break;
                    }
                    case 402: {
                        KeyEvent keyEvent = (KeyEvent)event;
                        int keyCode = keyEvent.getKeyCode();
                        char keyChar = keyEvent.getKeyChar();
                        if (this.inPlayerMode) break;
                        this.eventListener.onKeyReleasedJava(keyEvent.getWhen(), keyCode, keyChar, keyEvent.getKeyLocation());
                        break;
                    }
                    case 400: {
                        KeyEvent keyEvent = (KeyEvent)event;
                        char keyChar = keyEvent.getKeyChar();
                        if (this.inPlayerMode) break;
                        this.eventListener.onKeyTypedJava(keyEvent.getWhen(), keyChar);
                        break;
                    }
                    case 501: {
                        break;
                    }
                    case 505: {
                        this.eventListener.releaseAllControls();
                    }
                }
            }
        }
    }

    /*
     * WARNING - Removed try catching itself - possible behaviour change.
     */
    private void loseFocusNotifyKeys() {
        LinkedList<InputEvent> linkedList;
        LinkedList<InputEvent> linkedList2 = linkedList = this.keyEventQueue;
        synchronized (linkedList2) {
            this.keyEventQueue.addLast(new MouseEvent(new Container(), 505, 0L, 0, 0, 0, 0, false));
        }
    }

    private void hideHelpOrFocus() {
        this.helpIsVisible = false;
        this.outOfFocusVisible = false;
        this.mouseClickedFirst = false;
    }

    @Override
    public void keyPressed(KeyEvent e2) {
        this.activeScreen.getEventDelegate().keyPressed(e2);
    }

    @Override
    public void keyReleased(KeyEvent e2) {
        this.activeScreen.getEventDelegate().keyReleased(e2);
    }

    @Override
    public void keyTyped(KeyEvent e2) {
        this.activeScreen.getEventDelegate().keyTyped(e2);
    }

    @Override
    public void mousePressed(MouseEvent e2) {
        if (this.activeScreen == this.gameScreen || this.activeScreen == this.loginScreen) {
            if (this.helpImages == null || this.helpImages[0] == null) {
                this.theApplet.requestFocus();
                this.outOfFocusVisible = false;
                return;
            }
            if (this.outOfFocusVisible) {
                this.helpIsVisible = false;
            } else if (!this.helpIsVisible) {
                if (this.mouseClickedFirst) {
                    this.helpIsVisible = true;
                    this.mouseClickedFirst = false;
                } else {
                    this.mouseClickedFirst = true;
                }
            } else {
                this.helpIsVisible = false;
                this.mouseClickedFirst = false;
            }
            this.outOfFocusVisible = false;
            this.waitForKeyReleaseCode = 0;
        } else {
            this.activeScreen.getEventDelegate().mousePressed(e2);
        }
    }

    @Override
    public void mouseReleased(MouseEvent e2) {
        if (this.activeScreen != null && this.activeScreen.receivesMouseEvents()) {
            this.activeScreen.getEventDelegate().mouseReleased(e2);
        }
    }

    @Override
    public void mouseDragged(MouseEvent e2) {
        if (this.activeScreen != null && this.activeScreen.receivesMouseEvents()) {
            this.activeScreen.getEventDelegate().mouseDragged(e2);
        }
    }

    @Override
    public void mouseMoved(MouseEvent e2) {
        if (this.activeScreen != null && this.activeScreen.receivesMouseEvents()) {
            this.activeScreen.getEventDelegate().mouseMoved(e2);
        }
    }

    /*
     * WARNING - Removed try catching itself - possible behaviour change.
     */
    public void drawBuffer(Graphics g2) {
        Object object;
        Object object2 = object = this.paintLock;
        synchronized (object2) {
            if (this.backBuffer.getBackbufferGraphics() == null || g2 == null) {
                return;
            }
            this.activeScreen.draw(this.backBuffer.getBackbufferGraphics());
            if (this.helpIsVisible) {
                int lang = this.getCachedHelpLanguage();
                this.backBuffer.getBackbufferGraphics().drawImage((Image)this.helpImages[lang], 0, 0, null);
            } else if (this.outOfFocusVisible) {
                int lang = this.getCachedHelpLanguage();
                this.backBuffer.getBackbufferGraphics().drawImage((Image)this.focusImages[lang], 0, 0, null);
            }
            if (this.inPlayerMode) {
                this.replayPlayerScreen.draw(this.backBuffer.getBackbufferGraphics());
            }
            this.updateOverlay();
            this.present(g2, this.backBuffer.getBackBuffer());
            this.backBuffer.swap();
        }
    }

    public String getLanguageFilePath() {
        String lang = LANGUAGE;
        if (this.gameEngine != null && this.isNativeLibraryLoadingCompleted()) {
            String nativeLang = this.gameEngine.getLanguage();
            if (nativeLang == null) {
                lang = LANGUAGE;
            } else {
                if (nativeLang.indexOf("english") != -1) {
                    lang = "en";
                }
                if (nativeLang.indexOf("spanish") != -1) {
                    lang = "es";
                } else if (nativeLang.indexOf("french") != -1) {
                    lang = "fr";
                } else if (nativeLang.indexOf("italian") != -1) {
                    lang = "it";
                } else if (nativeLang.indexOf("german") != -1) {
                    lang = "de";
                } else if (nativeLang.indexOf("chinese-traditional") != -1) {
                    lang = "zh";
                } else if (nativeLang.indexOf("chinese-simplified") != -1) {
                    lang = "zh-cn";
                } else if (nativeLang.indexOf("chinese") != -1) {
                    lang = "zh";
                } else if (nativeLang.indexOf("russian") != -1) {
                    lang = "ru";
                }
                LANGUAGE = lang;
            }
        }
        this.reloadLocalizedStrings();
        if (lang.indexOf("es") != -1) {
            return "es";
        }
        if (lang.indexOf("fr") != -1) {
            return "fr";
        }
        if (lang.indexOf("it") != -1) {
            return "it";
        }
        if (lang.indexOf("de") != -1) {
            return "de";
        }
        if (lang.indexOf("zh-cn") != -1) {
            return "zh_cn";
        }
        if (lang.indexOf("zh") != -1) {
            return "zh";
        }
        if (lang.indexOf("ru") != -1) {
            return "ru";
        }
        return "en";
    }

    private int getCachedHelpLanguage() {
        if (this.languageTicks++ < 20) {
            return this.cachedLanguage;
        }
        this.languageTicks = 0;
        int result = 0;
        if (!this.isNativeLibraryLoadingCompleted()) {
            return result;
        }
        String nativeLang = this.gameEngine.getLanguage();
        if (nativeLang == null) {
            nativeLang = LANGUAGE;
            if (nativeLang.indexOf("es") != -1) {
                result = 2;
            } else if (nativeLang.indexOf("fr") != -1) {
                result = 1;
            } else if (nativeLang.indexOf("it") != -1) {
                result = 3;
            } else if (nativeLang.indexOf("de") != -1) {
                result = 4;
            } else if (nativeLang.indexOf("zh-cn") != -1) {
                result = 5;
            } else if (nativeLang.indexOf("zh") != -1) {
                result = 6;
            } else if (nativeLang.indexOf("ru") != -1) {
                result = 7;
            }
        } else if ((nativeLang = nativeLang.toLowerCase()).indexOf("spanish") != -1) {
            result = 2;
        } else if (nativeLang.indexOf("french") != -1) {
            result = 1;
        } else if (nativeLang.indexOf("italian") != -1) {
            result = 3;
        } else if (nativeLang.indexOf("german") != -1) {
            result = 4;
        } else if (nativeLang.indexOf("chinese-traditional") != -1) {
            result = 6;
        } else if (nativeLang.indexOf("chinese-simplified") != -1) {
            result = 5;
        } else if (nativeLang.indexOf("chinese") != -1) {
            result = 5;
        } else if (nativeLang.indexOf("russian") != -1) {
            result = 7;
        }
        this.cachedLanguage = result;
        return result;
    }

    /*
     * WARNING - Removed try catching itself - possible behaviour change.
     */
    public void update(Graphics g2) {
        Object object;
        Object object2 = object = this.paintLock;
        synchronized (object2) {
            this.present(g2, this.backBuffer.getSecondaryBuffer());
        }
    }

    /**
     * Scales the engine frame to the applet, preserving aspect ratio.
     * Integer scale factors use nearest-neighbour (crisp pixels); other factors
     * use bilinear filtering if SMOOTH_SCALING is on.
     */
    private void present(Graphics g2, BufferedImage frame) {
        if (frame == null) {
            return;
        }
        int srcW = this.screenWidth;
        int srcH = this.screenHeight;
        if (!DOUBLE_PIXELS) {
            g2.drawImage(frame, 0, 0, srcW, srcH, 0, 0, srcW, srcH, null);
            return;
        }
        int aw = this.theApplet.getWidth();
        int ah = this.theApplet.getHeight();
        double scale = Math.min((double) aw / srcW, (double) ah / srcH);
        if (scale >= 1.0 && !SMOOTH_SCALING) {
            scale = Math.floor(scale);
        }
        int dw = (int) Math.round(srcW * scale);
        int dh = (int) Math.round(srcH * scale);
        int dx = (aw - dw) / 2;
        int dy = (ah - dh) / 2;
        g2.setColor(java.awt.Color.BLACK);
        if (dx > 0) {
            g2.fillRect(0, 0, dx, ah);
            g2.fillRect(dx + dw, 0, aw - dx - dw, ah);
        }
        if (dy > 0) {
            g2.fillRect(dx, 0, dw, dy);
            g2.fillRect(dx, dy + dh, dw, ah - dy - dh);
        }
        if (g2 instanceof Graphics2D) {
            boolean integer = scale == Math.floor(scale);
            ((Graphics2D) g2).setRenderingHint(java.awt.RenderingHints.KEY_INTERPOLATION,
                    SMOOTH_SCALING && !integer ? java.awt.RenderingHints.VALUE_INTERPOLATION_BILINEAR
                            : java.awt.RenderingHints.VALUE_INTERPOLATION_NEAREST_NEIGHBOR);
        }
        // Source rect excludes the extra guard row the native buffer carries.
        g2.drawImage(frame, dx, dy, dx + dw, dy + dh, 0, 0, srcW, srcH, null);
    }

    public boolean isNativeLibraryLoadingCompleted() {
        if (this.downloadManager == null) {
            return false;
        }
        return this.downloadManager.isNativeLibraryLoadingCompleted();
    }

    private void updateOverlay() {
        boolean displayWaitScreen = false;
        if (!this.downloadManager.isNativeLibraryLoadingCompleted() && !this.standalone) {
            displayWaitScreen = true;
        } else if (this.downloadManager.isNativeLibraryLoadingCompleted() && this.gameEngine != null) {
            displayWaitScreen = this.gameEngine.isInWaitScreen();
        }
        if (!this.downloadManager.isAllCompleted()) {
            boolean isInWait = true;
            if (this.downloadManager.isNativeLibraryLoadingCompleted()) {
                isInWait = this.gameEngine.isInWaitScreen();
            }
            if (!this.standalone && isInWait) {
                this.drawWaitScreen();
            }
            if (!this.standalone && this.loadVisualizer != null) {
                this.loadVisualizer.update(this.backBuffer.getBackbufferGraphics(), this.downloadManager, isInWait);
            }
        }
    }

    private void drawWaitScreen() {
        Graphics2D g2 = this.backBuffer.getBackbufferGraphics();
        if (g2 != null) {
            g2.drawImage((Image)this.waitImg, 0, 0, null);
        }
    }

    public SharedBackBuffer getBackBuffer() {
        return this.backBuffer;
    }

    public void popScreen() {
        if (this.screenStack.size() == 0) {
            return;
        }
        this.activeScreen = this.screenStack.removeFirst();
    }

    public void pushScreen(LoginScreen screen) {
        this.screenStack.addFirst(this.activeScreen);
        this.activeScreen = screen;
    }

    /*
     * WARNING - Removed try catching itself - possible behaviour change.
     */
    public void queueEvent(KeyEvent e2) {
        LinkedList<InputEvent> linkedList;
        LinkedList<InputEvent> linkedList2 = linkedList = this.keyEventQueue;
        synchronized (linkedList2) {
            this.keyEventQueue.addLast(e2);
        }
    }

    @Override
    public String getSessionCookie() {
        try {
            CookieHandler cookieHandler = new CookieHandler();
            String sessionIDCookie = CookieHandler.get(this.theApplet, "JSESSIONID");
            return sessionIDCookie;
        }
        catch (Exception e2) {
            return null;
        }
    }

    @Override
    public String getUnifiedLoginEndpoint() {
        return unifiedLoginURL;
    }

    @Override
    public String getSNAPLoginEndpoint() {
        return snapLoginURL;
    }

    @Override
    public int getSSOType() {
        return this.isUsingSSO() ? 1 : 0;
    }

    public AppletSettings getSettings() {
        return this.appletSettings;
    }

    public boolean isUsingSSO() {
        return false;
    }

    public int getSelectKeyCode() {
        if (this.gameEngine == null) {
            return 0;
        }
        return this.gameEngine.getSelectButton() & 0xFFFF;
    }

    public int getBackKeyCode() {
        if (this.gameEngine == null) {
            return 0;
        }
        return this.gameEngine.getBackButton() & 0xFFFF;
    }

    public Font getFont() {
        String lang = this.getLanguageFilePath();
        if (lang.indexOf("zh") != -1) {
            Vector<String> chinesefonts = new Vector<String>();
            Font[] allfonts = GraphicsEnvironment.getLocalGraphicsEnvironment().getAllFonts();
            int fontcount = 0;
            String chinesesample = lang.equalsIgnoreCase("zh_cn") ? "\u53f6" : "\u8449";
            for (int j2 = 0; j2 < allfonts.length; ++j2) {
                if (allfonts[j2].canDisplayUpTo(chinesesample) != -1) continue;
                chinesefonts.add(allfonts[j2].getFontName());
                ++fontcount;
            }
            if (fontcount == 0) {
                return new Font("Arial", 0, 12);
            }
            return new Font((String)chinesefonts.get(0), 0, 12);
        }
        return new Font("Arial", 0, 12);
    }

    public void deinitGraphics() {
        this.helpImages = null;
        this.focusImages = null;
        this.waitImg = null;
        this.backBuffer.deinit();
        this.loginScreen.deinit();
        this.gameScreen.deinit();
        this.loadVisualizer.deinit();
        this.loadVisualizer = null;
    }

    public void doExit() {
        this.exitInternal = true;
    }

    static {
        commandLineArguments = "";
        dbgfilename = null;
        overrideGateway = null;
        overrideGameclass = null;
        overrideTimeout = 0;
        overrideInterval = 0;
        DOUBLE_PIXELS = false;
    }
}

