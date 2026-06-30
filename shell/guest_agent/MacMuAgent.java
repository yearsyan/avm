// SPDX-License-Identifier: MIT

package dev.macmu.agent;

import android.os.IBinder;
import android.os.SystemClock;
import android.system.Os;
import android.system.OsConstants;
import android.system.VmSocketAddress;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.MotionEvent;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;

public final class MacMuAgent {
    private static final int INJECT_INPUT_EVENT_MODE_ASYNC = 0;
    private static final int VMADDR_CID_HOST = 2;
    private static final int VSOCK_DATA_NEW_TRANSPORT_PORT = 5002;
    private static final String GOLDFISH_PIPE_DEVICE = "/dev/goldfish_pipe_dprctd";
    private static final long RECONNECT_DELAY_MS = 1000;

    private final Object inputManager;
    private final Method injectInputEvent;
    private final Method setDisplayId;
    private final String socketPath;
    private boolean hoverActive;
    private int hoverDisplayId = -1;
    private float hoverX;
    private float hoverY;
    private boolean touchActive;
    private int touchDisplayId = -1;
    private int touchPointerId;
    private float touchX;
    private float touchY;
    private long touchDownTime;
    private int mouseButtonState;
    private int mouseDisplayId = -1;
    private float mouseX;
    private float mouseY;
    private long mouseDownTime;

    private MacMuAgent(String socketPath) throws Exception {
        this.socketPath = socketPath;

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
        String socketPath = systemProperty("ro.boot.macmu_rpc_socket", "");
        new MacMuAgent(socketPath).run();
    }

    private void run() throws Exception {
        if (socketPath.isEmpty()) {
            System.err.println("MacMu RPC socket path is not configured.");
            while (true) {
                Thread.sleep(RECONNECT_DELAY_MS);
            }
        }

        while (true) {
            try (HostPipe pipe = connectHostPipe(socketPath)) {
                handleHost(pipe.input, pipe.output);
            } catch (Exception e) {
                e.printStackTrace(System.err);
                Thread.sleep(RECONNECT_DELAY_MS);
            }
        }
    }

    private void handleHost(FileInputStream input, FileOutputStream output) throws Exception {
        try (BufferedReader reader =
                        new BufferedReader(
                                new InputStreamReader(
                                        input, StandardCharsets.US_ASCII),
                                4096);
                BufferedWriter writer =
                        new BufferedWriter(
                                new OutputStreamWriter(
                                        output, StandardCharsets.US_ASCII),
                                256)) {
            String line;
            while ((line = reader.readLine()) != null) {
                try {
                    handleLine(line, writer);
                } catch (Exception e) {
                    e.printStackTrace(System.err);
                }
            }
        } finally {
            resetPointerState();
        }
    }

    private static HostPipe connectHostPipe(String socketPath) throws Exception {
        try {
            return connectGoldfishPipe(socketPath);
        } catch (Exception ignored) {
            return connectVsockPipe(socketPath);
        }
    }

    private static HostPipe connectGoldfishPipe(String socketPath) throws Exception {
        FileDescriptor fd = Os.open(GOLDFISH_PIPE_DEVICE, OsConstants.O_RDWR, 0);
        boolean success = false;
        try {
            FileInputStream input = new FileInputStream(fd);
            FileOutputStream output = new FileOutputStream(fd);
            byte[] selector = pipeSelector(socketPath);
            output.write(selector);
            output.flush();
            success = true;
            return new HostPipe(fd, input, output);
        } finally {
            if (!success) {
                try {
                    Os.close(fd);
                } catch (Exception ignored) {
                }
            }
        }
    }

    private static HostPipe connectVsockPipe(String socketPath) throws Exception {
        FileDescriptor fd = Os.socket(OsConstants.AF_VSOCK, OsConstants.SOCK_STREAM, 0);
        boolean success = false;
        try {
            Os.connect(fd, new VmSocketAddress(VSOCK_DATA_NEW_TRANSPORT_PORT, VMADDR_CID_HOST));
            FileInputStream input = new FileInputStream(fd);
            FileOutputStream output = new FileOutputStream(fd);
            byte[] selector = pipeSelector(socketPath);
            output.write(selector);
            output.flush();
            success = true;
            return new HostPipe(fd, input, output);
        } finally {
            if (!success) {
                try {
                    Os.close(fd);
                } catch (Exception ignored) {
                }
            }
        }
    }

    private static byte[] pipeSelector(String socketPath) {
        return ("pipe:unix:" + socketPath + "\0").getBytes(StandardCharsets.US_ASCII);
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
        if (fields.length == 1 && "e".equals(fields[0])) {
            injectHoverExit();
            return;
        }
        if (fields.length == 6 && "s".equals(fields[0])) {
            int displayId = Integer.parseInt(fields[1]);
            float x = Float.parseFloat(fields[2]);
            float y = Float.parseFloat(fields[3]);
            float hscroll = Integer.parseInt(fields[4]) / 1000.0f;
            float vscroll = Integer.parseInt(fields[5]) / 1000.0f;
            injectScroll(displayId, x, y, hscroll, vscroll);
            return;
        }
        if (fields.length == 6 && "t".equals(fields[0])) {
            int displayId = Integer.parseInt(fields[1]);
            int pointerId = Integer.parseInt(fields[2]);
            String phase = fields[3];
            float x = Float.parseFloat(fields[4]);
            float y = Float.parseFloat(fields[5]);
            injectTouch(displayId, pointerId, phase, x, y);
            return;
        }
        if (fields.length == 5 && "m".equals(fields[0])) {
            int displayId = Integer.parseInt(fields[1]);
            float x = Float.parseFloat(fields[2]);
            float y = Float.parseFloat(fields[3]);
            int buttons = Integer.parseInt(fields[4]);
            injectMouseMove(displayId, x, y, buttons);
            return;
        }
        if (fields.length == 5 && "b".equals(fields[0])) {
            int displayId = Integer.parseInt(fields[1]);
            float x = Float.parseFloat(fields[2]);
            float y = Float.parseFloat(fields[3]);
            int buttons = Integer.parseInt(fields[4]);
            injectMouseButton(displayId, x, y, buttons);
        }
    }

    private synchronized void injectHover(int displayId, float x, float y) throws Exception {
        if (hoverActive && displayId != hoverDisplayId) {
            injectHoverExitLocked();
        }
        if (!hoverActive) {
            injectMotion(displayId, MotionEvent.ACTION_HOVER_ENTER, x, y, 0);
            hoverActive = true;
        }
        hoverDisplayId = displayId;
        hoverX = x;
        hoverY = y;
        injectMotion(displayId, MotionEvent.ACTION_HOVER_MOVE, x, y, 0);
    }

    private synchronized void injectHoverExit() throws Exception {
        injectHoverExitLocked();
    }

    private void injectHoverExitLocked() throws Exception {
        if (!hoverActive) {
            return;
        }
        injectMotion(hoverDisplayId, MotionEvent.ACTION_HOVER_EXIT, hoverX, hoverY, 0);
        hoverActive = false;
        hoverDisplayId = -1;
    }

    private synchronized void injectTouch(
            int displayId, int pointerId, String phase, float x, float y) throws Exception {
        if (phase.isEmpty()) {
            return;
        }

        char command = phase.charAt(0);
        long now = SystemClock.uptimeMillis();
        if (command == 'b') {
            injectHoverExitLocked();
            if (touchActive) {
                injectTouchMotion(
                        touchDisplayId,
                        touchPointerId,
                        touchDownTime,
                        MotionEvent.ACTION_CANCEL,
                        touchX,
                        touchY);
            }
            touchActive = true;
            touchDisplayId = displayId;
            touchPointerId = pointerId;
            touchX = x;
            touchY = y;
            touchDownTime = now;
            injectTouchMotion(
                    displayId,
                    pointerId,
                    touchDownTime,
                    MotionEvent.ACTION_DOWN,
                    x,
                    y);
            return;
        }

        if (!touchActive) {
            return;
        }

        touchDisplayId = displayId;
        touchPointerId = pointerId;
        touchX = x;
        touchY = y;
        if (command == 'm') {
            injectTouchMotion(
                    displayId,
                    pointerId,
                    touchDownTime,
                    MotionEvent.ACTION_MOVE,
                    x,
                    y);
        } else if (command == 'e') {
            injectTouchMotion(
                    displayId,
                    pointerId,
                    touchDownTime,
                    MotionEvent.ACTION_UP,
                    x,
                    y);
            touchActive = false;
            touchDisplayId = -1;
        } else if (command == 'c') {
            injectTouchMotion(
                    displayId,
                    pointerId,
                    touchDownTime,
                    MotionEvent.ACTION_CANCEL,
                    x,
                    y);
            touchActive = false;
            touchDisplayId = -1;
        }
    }

    private synchronized void injectMouseMove(
            int displayId, float x, float y, int buttonState) throws Exception {
        if (buttonState == 0) {
            injectHover(displayId, x, y);
            return;
        }
        mouseDisplayId = displayId;
        mouseX = x;
        mouseY = y;
        mouseButtonState = buttonState;
        injectMotion(displayId, MotionEvent.ACTION_MOVE, x, y, buttonState, mouseDownTime);
    }

    private synchronized void injectMouseButton(
            int displayId, float x, float y, int buttonState) throws Exception {
        int previousButtonState = mouseButtonState;
        mouseDisplayId = displayId;
        mouseX = x;
        mouseY = y;
        mouseButtonState = buttonState;

        if (previousButtonState == 0 && buttonState != 0) {
            injectHoverExitLocked();
            mouseDownTime = SystemClock.uptimeMillis();
            injectMotion(displayId, MotionEvent.ACTION_DOWN, x, y, buttonState, mouseDownTime);
        } else if (previousButtonState != 0 && buttonState == 0) {
            injectMotion(displayId, MotionEvent.ACTION_UP, x, y, 0, mouseDownTime);
            mouseDownTime = 0;
        } else if (previousButtonState != buttonState) {
            injectMotion(displayId, MotionEvent.ACTION_MOVE, x, y, buttonState, mouseDownTime);
        }
    }

    private synchronized void resetPointerState() throws Exception {
        if (touchActive) {
            injectTouchMotion(
                    touchDisplayId,
                    touchPointerId,
                    touchDownTime,
                    MotionEvent.ACTION_CANCEL,
                    touchX,
                    touchY);
            touchActive = false;
            touchDisplayId = -1;
        }
        if (mouseButtonState != 0) {
            injectMotion(mouseDisplayId, MotionEvent.ACTION_UP, mouseX, mouseY, 0, mouseDownTime);
            mouseButtonState = 0;
            mouseDisplayId = -1;
            mouseDownTime = 0;
        }
        injectHoverExitLocked();
    }

    private void injectTouchMotion(
            int displayId, int pointerId, long downTime, int action, float x, float y)
            throws Exception {
        long now = SystemClock.uptimeMillis();
        MotionEvent.PointerProperties[] properties = {new MotionEvent.PointerProperties()};
        properties[0].id = pointerId;
        properties[0].toolType = MotionEvent.TOOL_TYPE_FINGER;

        MotionEvent.PointerCoords[] coords = {new MotionEvent.PointerCoords()};
        coords[0].x = x;
        coords[0].y = y;
        coords[0].pressure =
                (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL)
                        ? 0.0f
                        : 1.0f;
        coords[0].size = 1.0f;

        MotionEvent event =
                MotionEvent.obtain(
                        downTime,
                        now,
                        action,
                        1,
                        properties,
                        coords,
                        0,
                        0,
                        1.0f,
                        1.0f,
                        0,
                        0,
                        InputDevice.SOURCE_TOUCHSCREEN,
                        0);
        setDisplayId.invoke(event, displayId);
        try {
            injectInputEvent.invoke(inputManager, event, INJECT_INPUT_EVENT_MODE_ASYNC);
        } finally {
            event.recycle();
        }
    }

    private synchronized void injectMotion(
            int displayId, int action, float x, float y, int buttonState) throws Exception {
        injectMotion(displayId, action, x, y, buttonState, SystemClock.uptimeMillis());
    }

    private void injectMotion(
            int displayId, int action, float x, float y, int buttonState, long downTime)
            throws Exception {
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
                        downTime,
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

    private static String systemProperty(String name, String fallback) {
        try {
            Class<?> systemPropertiesClass = Class.forName("android.os.SystemProperties");
            Method get =
                    systemPropertiesClass.getDeclaredMethod(
                            "get", String.class, String.class);
            return (String) get.invoke(null, name, fallback);
        } catch (Exception e) {
            return fallback;
        }
    }

    private static final class HostPipe implements AutoCloseable {
        final FileDescriptor fd;
        final FileInputStream input;
        final FileOutputStream output;

        HostPipe(FileDescriptor fd, FileInputStream input, FileOutputStream output) {
            this.fd = fd;
            this.input = input;
            this.output = output;
        }

        @Override
        public void close() {
            try {
                input.close();
            } catch (Exception ignored) {
            }
            try {
                output.close();
            } catch (Exception ignored) {
            }
            try {
                Os.close(fd);
            } catch (Exception ignored) {
            }
        }
    }
}
