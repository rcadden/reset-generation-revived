/*
 * Decompiled with CFR 0.152.
 */
package rg;

import java.awt.Graphics2D;
import java.awt.image.BufferedImage;
import java.awt.image.DataBufferInt;
import java.awt.image.WritableRaster;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.IntBuffer;
import java.util.Arrays;

public class SharedBackBuffer {
    private int screenWidth;
    private int screenHeight;
    private int[][] pixelBuffer = new int[2][];
    private BufferedImage[] backBuffers = new BufferedImage[2];
    private Graphics2D[] backbufferGraphics = new Graphics2D[2];
    private IntBuffer nativeIntBuffer;
    private int activeBuffer = 0;

    public SharedBackBuffer(int screenWidth, int screenHeight, boolean playerMode) {
        this.screenWidth = screenWidth;
        this.screenHeight = screenHeight;
        for (int i2 = 0; i2 < 2; ++i2) {
            this.backBuffers[i2] = new BufferedImage(this.screenWidth, this.screenHeight + (playerMode ? 32 : 0) + 1, 1);
            WritableRaster bbRaster = this.backBuffers[i2].getRaster();
            DataBufferInt dbuf = (DataBufferInt)bbRaster.getDataBuffer();
            this.pixelBuffer[i2] = dbuf.getData();
            this.backbufferGraphics[i2] = this.backBuffers[i2].createGraphics();
        }
        ByteBuffer sharedBBByteView = ByteBuffer.allocateDirect(screenWidth * (screenHeight + 1) * 4);
        sharedBBByteView.order(ByteOrder.nativeOrder());
        this.nativeIntBuffer = sharedBBByteView.asIntBuffer();
        int[] zeroArray = new int[screenWidth * (screenHeight + 1)];
        for (int i3 = 0; i3 < zeroArray.length; ++i3) {
            zeroArray[i3] = 0;
        }
        this.nativeIntBuffer.put(zeroArray, 0, zeroArray.length);
        this.nativeIntBuffer.rewind();
        this.clear(0);
    }

    public BufferedImage getSecondaryBuffer() {
        return this.backBuffers[this.activeBuffer ^ 1];
    }

    public void swap() {
        this.activeBuffer ^= 1;
    }

    public int[] getPixelBuffer() {
        return this.pixelBuffer[this.activeBuffer];
    }

    public Graphics2D getBackbufferGraphics() {
        return this.backbufferGraphics[this.activeBuffer];
    }

    public IntBuffer getNativeIntBuffer() {
        return this.nativeIntBuffer;
    }

    public BufferedImage getBackBuffer() {
        return this.backBuffers[this.activeBuffer];
    }

    public void blit() {
        this.nativeIntBuffer.get(this.pixelBuffer[this.activeBuffer], 0, this.screenWidth * (this.screenHeight + 1));
        this.nativeIntBuffer.rewind();
    }

    public void draw(String str, int x2, int y) {
        this.backbufferGraphics[this.activeBuffer].drawString(str, x2, y);
    }

    public void clear(int color) {
        Arrays.fill(this.pixelBuffer[this.activeBuffer], 0, this.screenWidth * (this.screenHeight + 1), color);
    }

    private int[] zeroLine;

    public void clearNative(int color) {
        int n = this.screenWidth * (this.screenHeight + 1);
        if (this.zeroLine == null || this.zeroLine.length != n) {
            this.zeroLine = new int[n];
        }
        if (color != 0 || this.zeroLine[0] != color) {
            Arrays.fill(this.zeroLine, color);
        }
        this.nativeIntBuffer.put(this.zeroLine, 0, n);
        this.nativeIntBuffer.rewind();
    }

    public void deinit() {
        this.backBuffers = null;
        this.backbufferGraphics = null;
    }
}

