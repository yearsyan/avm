// SPDX-License-Identifier: MIT

package dev.macmu.input;

import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.os.IBinder;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.MotionEvent;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;

public final class MacMuInputServer {
    private static final int INJECT_INPUT_EVENT_MODE_ASYNC = 0;

    private final Object inputManager;
    private final Method injectInputEvent;
    private final Method setDisplayId;
    private boolean hoverActive;

    private MacMuInputServer() throws Exception {
        Class<?> serviceManagerClass = Class.forName("android.os.ServiceManager");
        Method getService = serviceManagerClass.getDeclaredMethod("getService", String.class);
        IBinder inputBinder = (IBinder) getService.invoke(null, "input");
        if (inputBinder == null) {
            throw new IllegalStateException("input service is unavailable");
        }

        Class<?> stubClass = Class.forName("android.hardware.input.IInputManager$Stub");
        Method asInterface = stubClass.getDeclaredMethod("asInterface", IBinder.class);
        inputManager = asInterface.invoke(null, inputBinder);
        injectInputEvent =
                inputManager.getClass().getMethod("injectInputEvent", InputEvent.class, int.class);
        setDisplayId = InputEvent.class.getDeclaredMethod("setDisplayId", int.class);
        setDisplayId.setAccessible(true);
    }

    public static void main(String[] args) throws Exception {
        new MacMuInputServer().run();
    }

    private void run() throws Exception {
        try (LocalServerSocket serverSocket = new LocalServerSocket("macmu_input")) {
            while (true) {
                final LocalSocket socket = serverSocket.accept();
                Thread thread =
                        new Thread(
                                new Runnable() {
                                    @Override
                                    public void run() {
                                        try {
                                            handleClient(socket);
                                        } catch (Exception e) {
                                            e.printStackTrace(System.err);
                                        }
                                    }
                                },
                                "MacMuInputClient");
                thread.start();
            }
        }
    }

    private void handleClient(LocalSocket socket) throws Exception {
        try (LocalSocket closeableSocket = socket;
                BufferedReader reader =
                        new BufferedReader(
                                new InputStreamReader(
                                        closeableSocket.getInputStream(),
                                        StandardCharsets.US_ASCII),
                                4096);
                BufferedWriter writer =
                        new BufferedWriter(
                                new OutputStreamWriter(
                                        closeableSocket.getOutputStream(),
                                        StandardCharsets.US_ASCII),
                                256)) {
            String line;
            while ((line = reader.readLine()) != null) {
                try {
                    handleLine(line, writer);
                } catch (Exception e) {
                    e.printStackTrace(System.err);
                }
            }
        }
    }

    private void handleLine(String line, BufferedWriter writer) throws Exception {
        if (line.isEmpty()) {
            return;
        }
        String[] fields = line.split(" ");
        if (fields.length == 1 && "v".equals(fields[0])) {
            writer.write("ok\n");
            writer.flush();
            return;
        }
        if (fields.length == 4 && "h".equals(fields[0])) {
            int displayId = Integer.parseInt(fields[1]);
            float x = Float.parseFloat(fields[2]);
            float y = Float.parseFloat(fields[3]);
            injectHover(displayId, x, y);
            return;
        }
        if (fields.length == 6 && "s".equals(fields[0])) {
            int displayId = Integer.parseInt(fields[1]);
            float x = Float.parseFloat(fields[2]);
            float y = Float.parseFloat(fields[3]);
            float hscroll = Integer.parseInt(fields[4]) / 1000.0f;
            float vscroll = Integer.parseInt(fields[5]) / 1000.0f;
            injectScroll(displayId, x, y, hscroll, vscroll);
        }
    }

    private synchronized void injectHover(int displayId, float x, float y) throws Exception {
        if (!hoverActive) {
            injectMotion(displayId, MotionEvent.ACTION_HOVER_ENTER, x, y, 0);
            hoverActive = true;
        }
        injectMotion(displayId, MotionEvent.ACTION_HOVER_MOVE, x, y, 0);
    }

    private synchronized void injectMotion(
            int displayId, int action, float x, float y, int buttonState) throws Exception {
        long now = SystemClock.uptimeMillis();
        MotionEvent.PointerProperties[] properties = {new MotionEvent.PointerProperties()};
        properties[0].id = 0;
        properties[0].toolType = MotionEvent.TOOL_TYPE_MOUSE;

        MotionEvent.PointerCoords[] coords = {new MotionEvent.PointerCoords()};
        coords[0].x = x;
        coords[0].y = y;
        coords[0].pressure = buttonState == 0 ? 0.0f : 1.0f;
        coords[0].size = 1.0f;

        MotionEvent event =
                MotionEvent.obtain(
                        now,
                        now,
                        action,
                        1,
                        properties,
                        coords,
                        0,
                        buttonState,
                        1.0f,
                        1.0f,
                        0,
                        0,
                        InputDevice.SOURCE_MOUSE,
                        0);
        setDisplayId.invoke(event, displayId);
        try {
            injectInputEvent.invoke(inputManager, event, INJECT_INPUT_EVENT_MODE_ASYNC);
        } finally {
            event.recycle();
        }
    }

    private synchronized void injectScroll(
            int displayId, float x, float y, float hscroll, float vscroll) throws Exception {
        long now = SystemClock.uptimeMillis();
        MotionEvent.PointerProperties[] properties = {new MotionEvent.PointerProperties()};
        properties[0].id = 0;
        properties[0].toolType = MotionEvent.TOOL_TYPE_MOUSE;

        MotionEvent.PointerCoords[] coords = {new MotionEvent.PointerCoords()};
        coords[0].x = x;
        coords[0].y = y;
        coords[0].pressure = 0.0f;
        coords[0].size = 1.0f;
        coords[0].setAxisValue(MotionEvent.AXIS_HSCROLL, hscroll);
        coords[0].setAxisValue(MotionEvent.AXIS_VSCROLL, vscroll);

        MotionEvent event =
                MotionEvent.obtain(
                        now,
                        now,
                        MotionEvent.ACTION_SCROLL,
                        1,
                        properties,
                        coords,
                        0,
                        0,
                        1.0f,
                        1.0f,
                        0,
                        0,
                        InputDevice.SOURCE_MOUSE,
                        0);
        setDisplayId.invoke(event, displayId);
        try {
            injectInputEvent.invoke(inputManager, event, INJECT_INPUT_EVENT_MODE_ASYNC);
        } finally {
            event.recycle();
        }
    }
}
