// SPDX-License-Identifier: MIT

package dev.macmu.agent;

import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Point;
import android.graphics.drawable.Drawable;
import android.hardware.display.DisplayManager;
import android.os.IBinder;
import android.os.Looper;
import android.os.SystemClock;
import android.system.Os;
import android.system.OsConstants;
import android.system.VmSocketAddress;
import android.util.Base64;
import android.view.Display;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.MotionEvent;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.ByteArrayOutputStream;
import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import org.json.JSONArray;
import org.json.JSONObject;

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

    // Lazily-initialized system Context + PackageManager used to resolve app
    // labels and launcher icons. This process is a bare app_process main with
    // no Application, so reach ActivityThread.systemMain()/getSystemContext()
    // via reflection the same way the input manager is reached. Stays null if
    // the platform blocks the hidden-API call; listLauncherApps degrades to
    // pkg/activity-only entries.
    private Context systemContext;
    private PackageManager pm;
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
        String ctrlSocketPath = systemProperty("ro.boot.macmu_ctrl_socket", "");
        MacMuAgent agent = new MacMuAgent(socketPath);
        if (!ctrlSocketPath.isEmpty()) {
            Thread ctrlThread = new Thread(() -> agent.runControl(ctrlSocketPath),
                    "macmu-ctrl");
            ctrlThread.setDaemon(true);
            ctrlThread.start();
        }
        agent.run();
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

    // ------------------------------------------------------------------
    // Control RPC connection: request/response line protocol used by the host
    // shell for app management ("<id> apps", "<id> launch <component> <display>",
    // "<id> close <component> <display>"). Kept on a separate host socket so
    // bulky responses never block input.
    // ------------------------------------------------------------------

    private void runControl(String ctrlSocketPath) {
        while (true) {
            try (HostPipe pipe = connectHostPipe(ctrlSocketPath)) {
                // The emulator's MultiDisplayService is normally started via an
                // adb broadcast the MacMu host does not have; send the same
                // broadcast from inside the guest instead. Idempotent.
                startMultiDisplayService();
                handleControl(pipe.input, pipe.output);
            } catch (Exception e) {
                e.printStackTrace(System.err);
                try {
                    Thread.sleep(RECONNECT_DELAY_MS);
                } catch (InterruptedException ignored) {
                }
            }
        }
    }

    private void handleControl(FileInputStream input, FileOutputStream output) throws Exception {
        try (BufferedReader reader =
                        new BufferedReader(
                                new InputStreamReader(input, StandardCharsets.US_ASCII), 65536);
                BufferedWriter writer =
                        new BufferedWriter(
                                new OutputStreamWriter(output, StandardCharsets.US_ASCII),
                                65536)) {
            String line;
            while ((line = reader.readLine()) != null) {
                try {
                    handleControlLine(line, writer);
                } catch (Exception e) {
                    e.printStackTrace(System.err);
                }
            }
        }
    }

    private void handleControlLine(String line, BufferedWriter writer) throws Exception {
        if (line.isEmpty()) {
            return;
        }
        if ("v".equals(line)) {
            writer.write("ok\n");
            writer.flush();
            return;
        }
        int space = line.indexOf(' ');
        if (space <= 0) {
            return;
        }
        String id = line.substring(0, space);
        String[] fields = line.substring(space + 1).split(" ");
        try {
            if (fields.length == 1 && "apps".equals(fields[0])) {
                writer.write(id + " ok " + listLauncherApps() + "\n");
                writer.flush();
                return;
            }
            if (fields.length == 2 && "display-state".equals(fields[0])) {
                int displayId = Integer.parseInt(fields[1]);
                writer.write(id + " ok " + displayState(displayId) + "\n");
                writer.flush();
                return;
            }
            if (fields.length == 3 && "launch".equals(fields[0])) {
                String component = fields[1];
                int displayId = Integer.parseInt(fields[2]);
                String error = launchComponent(component, displayId);
                if (error == null) {
                    writer.write(id + " ok\n");
                } else {
                    writer.write(id + " err " + error.replace('\n', ' ') + "\n");
                }
                writer.flush();
                return;
            }
            if (fields.length == 3 && "close".equals(fields[0])) {
                String component = fields[1];
                int displayId = Integer.parseInt(fields[2]);
                String error = closeComponent(component, displayId);
                if (error == null) {
                    writer.write(id + " ok\n");
                } else {
                    writer.write(id + " err " + error.replace('\n', ' ') + "\n");
                }
                writer.flush();
                return;
            }
            writer.write(id + " err unsupported command\n");
            writer.flush();
        } catch (Exception e) {
            writer.write(id + " err " + String.valueOf(e.getMessage()).replace('\n', ' ')
                    + "\n");
            writer.flush();
        }
    }

    // ------------------------------------------------------------------
    // Display id translation. The host control plane addresses displays by the
    // emulator MultiDisplay index (1..5, also the frame-channel slot). Android
    // assigns its own logical display id to the backing VirtualDisplay
    // (uniqueId "virtual:com.android.emulator.multidisplay:<1234561+index>").
    // Everything that talks to Android framework APIs (input injection,
    // start-activity --display) must use the logical id.
    // ------------------------------------------------------------------

    private static final int MULTI_DISPLAY_UNIQUE_ID_BASE = 1234561;
    private static final Pattern VIRTUAL_DISPLAY_PATTERN = Pattern.compile(
            "displayId=(\\d+), uniqueId='virtual:com\\.android\\.emulator\\.multidisplay:(\\d+)'");

    private final Map<Integer, Integer> displayIdCache = new HashMap<>();

    private synchronized int resolveAndroidDisplayId(int emulatorDisplayId) {
        if (emulatorDisplayId <= 0) {
            return 0;
        }
        Integer cached = displayIdCache.get(emulatorDisplayId);
        if (cached != null) {
            return cached;
        }
        refreshDisplayIdCache();
        cached = displayIdCache.get(emulatorDisplayId);
        return cached != null ? cached : -1;
    }

    private synchronized void invalidateDisplayId(int emulatorDisplayId) {
        displayIdCache.remove(emulatorDisplayId);
    }

    // Polls the DisplayManager dump until the emulator display id resolves to
    // an Android logical display id, or |timeoutMs| elapses. The guest may
    // register the VirtualDisplay slightly after the host confirms DISPLAY_ADD
    // (the control-plane ACK means "host accepted", not "guest live").
    private int waitForAndroidDisplayId(int emulatorDisplayId, long timeoutMs) {
        if (emulatorDisplayId <= 0) {
            return 0;
        }
        // Emulator display ids are reused after remove/re-add, but Android
        // assigns the new VirtualDisplay a fresh logical id. A cached mapping
        // from the previous display at this id would point at a destroyed
        // display, so drop it and re-read dumpsys.
        invalidateDisplayId(emulatorDisplayId);
        final long deadline = SystemClock.elapsedRealtime() + timeoutMs;
        while (true) {
            int resolved = resolveAndroidDisplayId(emulatorDisplayId);
            if (resolved >= 0) {
                return resolved;
            }
            if (SystemClock.elapsedRealtime() >= deadline) {
                return -1;
            }
            try {
                Thread.sleep(200);
            } catch (InterruptedException ie) {
                Thread.currentThread().interrupt();
                return -1;
            }
        }
    }

    private void refreshDisplayIdCache() {
        displayIdCache.clear();
        try {
            List<String> lines = execForLines(
                    new String[] {"/system/bin/dumpsys", "display"});
            for (String line : lines) {
                Matcher matcher = VIRTUAL_DISPLAY_PATTERN.matcher(line);
                while (matcher.find()) {
                    int androidId = Integer.parseInt(matcher.group(1));
                    int uniqueSuffix = Integer.parseInt(matcher.group(2));
                    displayIdCache.put(uniqueSuffix - MULTI_DISPLAY_UNIQUE_ID_BASE, androidId);
                }
            }
        } catch (Exception e) {
            e.printStackTrace(System.err);
        }
    }

    // Returns a JSON array of launcher entries. The baseline fields
    // {"pkg", "activity"} come from `cmd package query-activities`; when the
    // PackageManager is reachable each entry is enriched with {"name", "icon"}
    // where icon is a base64-encoded PNG (192x192) of the app's launcher icon.
    // Enrichment failures never drop an entry: a missing name/icon simply
    // means the host falls back to pkg/activity for that row.
    private String listLauncherApps() throws Exception {
        List<String> output = execForLines(new String[] {
                "/system/bin/cmd", "package", "query-activities", "--components",
                "-a", "android.intent.action.MAIN",
                "-c", "android.intent.category.LAUNCHER"});
        boolean pmReady = ensurePackageManager();
        JSONArray apps = new JSONArray();
        for (String rawLine : output) {
            String candidate = rawLine.trim();
            int slash = candidate.indexOf('/');
            if (slash <= 0 || candidate.indexOf(' ') >= 0) {
                continue;
            }
            String pkg = candidate.substring(0, slash);
            String activity = candidate.substring(slash + 1);
            if (activity.startsWith(".")) {
                activity = pkg + activity;
            }
            JSONObject app = new JSONObject();
            app.put("pkg", pkg);
            app.put("activity", activity);
            if (pmReady) {
                enrichAppEntry(app, pkg);
            }
            apps.put(app);
        }
        return apps.toString();
    }

    // Best-effort: add "name" and "icon" to |app| for the given package. Any
    // failure is logged and swallowed so the caller still emits the row.
    private void enrichAppEntry(JSONObject app, String pkg) {
        try {
            ApplicationInfo info = pm.getApplicationInfo(pkg, 0);
            CharSequence label = pm.getApplicationLabel(info);
            if (label != null && label.length() > 0) {
                app.put("name", label.toString());
            }
            String icon = loadIconPngBase64(info);
            if (icon != null) {
                app.put("icon", icon);
            }
        } catch (Exception e) {
            e.printStackTrace(System.err);
        }
    }

    // Renders the app launcher icon into a 192x192 ARGB_8888 bitmap and returns
    // it as a base64 (NO_WRAP) PNG string. Returns null on any failure.
    // getApplicationIcon never throws (returns the default robot icon on miss),
    // so a null return means the bitmap/encode step failed.
    private String loadIconPngBase64(ApplicationInfo info) {
        Drawable drawable = pm.getApplicationIcon(info);
        final int size = 192;
        Bitmap bmp = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
        try {
            Canvas canvas = new Canvas(bmp);
            drawable.setBounds(0, 0, size, size);
            drawable.draw(canvas);
            ByteArrayOutputStream baos = new ByteArrayOutputStream(8 * 1024);
            if (!bmp.compress(Bitmap.CompressFormat.PNG, 100, baos)) {
                return null;
            }
            return Base64.encodeToString(baos.toByteArray(), Base64.NO_WRAP);
        } catch (Exception e) {
            e.printStackTrace(System.err);
            return null;
        } finally {
            bmp.recycle();
        }
    }

    // Lazily resolve the system Context via reflection on ActivityThread. The
    // bare app_process entry point has no Application or Context of its own.
    private synchronized boolean ensureSystemContext() {
        if (systemContext != null) {
            return true;
        }
        try {
            // MacMu runs as a bare app_process entry point, and application
            // discovery is handled on the macmu-ctrl thread. Unlike an Android
            // application thread, that thread has no Looper by default, while
            // ActivityThread.systemMain() creates Handlers as it initializes.
            if (Looper.myLooper() == null) {
                Looper.prepare();
            }
            Class<?> at = Class.forName("android.app.ActivityThread");
            Object thread = at.getDeclaredMethod("systemMain").invoke(null);
            Method getSystemContext = at.getDeclaredMethod("getSystemContext");
            systemContext = (Context) getSystemContext.invoke(thread);
            if (systemContext == null) {
                return false;
            }
            return true;
        } catch (Exception e) {
            e.printStackTrace(System.err);
            systemContext = null;
            return false;
        }
    }

    // Lazily resolve PackageManager for launcher labels and icons. A failure
    // only removes that enrichment; launcher discovery still returns entries.
    private synchronized boolean ensurePackageManager() {
        if (pm != null) {
            return true;
        }
        if (!ensureSystemContext()) {
            return false;
        }
        try {
            pm = systemContext.getPackageManager();
            return pm != null;
        } catch (Exception e) {
            e.printStackTrace(System.err);
            pm = null;
            return false;
        }
    }

    // Returns the Android logical size and rotation of one emulator-managed
    // VirtualDisplay. An application with a fixed/sensor landscape request can
    // rotate this logical display while the host surface is still portrait;
    // the shell uses this state to resize the physical virtual display and its
    // macOS window to match.
    private String displayState(int emulatorDisplayId) throws Exception {
        int androidDisplayId = resolveAndroidDisplayId(emulatorDisplayId);
        if (androidDisplayId < 0) {
            throw new IllegalStateException("display " + emulatorDisplayId + " not found");
        }
        if (!ensureSystemContext()) {
            throw new IllegalStateException("system context unavailable");
        }
        DisplayManager manager =
                (DisplayManager) systemContext.getSystemService(Context.DISPLAY_SERVICE);
        Display display = manager != null ? manager.getDisplay(androidDisplayId) : null;
        if (display == null) {
            invalidateDisplayId(emulatorDisplayId);
            throw new IllegalStateException("Android display " + androidDisplayId + " unavailable");
        }
        Point size = new Point();
        display.getRealSize(size);
        if (size.x <= 0 || size.y <= 0) {
            throw new IllegalStateException("display has invalid dimensions");
        }
        return size.x + " " + size.y + " " + display.getRotation();
    }

    // Returns null on success, an error message otherwise.
    private String launchComponent(String component, int displayId) throws Exception {
        if (component.indexOf('/') <= 0 || component.indexOf(' ') >= 0) {
            return "invalid component";
        }
        List<String> command = new ArrayList<>();
        command.add("/system/bin/cmd");
        command.add("activity");
        command.add("start-activity");
        if (displayId > 0) {
            // The host may call launch within a few hundred ms of DISPLAY_ADD_OK;
            // the guest VirtualDisplay is not guaranteed to be registered with
            // DisplayManager by then. Poll the dumpsys-backed cache for a few
            // seconds instead of failing on the first miss — otherwise
            // am start --display falls back to display 0 silently.
            int androidDisplayId = waitForAndroidDisplayId(displayId, 3000);
            System.err.println("MacMu [diag] launchComponent emulatorDisplayId=" + displayId
                    + " -> androidDisplayId=" + androidDisplayId + " cache=" + displayIdCache);
            if (androidDisplayId < 0) {
                return "display " + displayId + " not found in guest after waiting";
            }
            command.add("--display");
            command.add(Integer.toString(androidDisplayId));
        }
        command.add("-n");
        command.add(component);
        List<String> output = execForLines(command.toArray(new String[0]));
        for (String outLine : output) {
            if (outLine.contains("Error") || outLine.contains("Exception")) {
                return outLine.trim();
            }
        }
        return null;
    }

    // Returns null on success, an error message otherwise.
    private String closeComponent(String component, int displayId) throws Exception {
        String pkg = packageNameFromComponent(component);
        if (pkg == null) {
            return "invalid component";
        }
        String error = stopPackage(pkg, false);
        if (error != null && error.toLowerCase().contains("unknown command")) {
            error = stopPackage(pkg, true);
        }
        // The host removes the guest display right after this returns; the
        // cached emulator->Android id mapping is about to go stale, and the
        // slot may be reused by a later display with a different Android id.
        if (displayId > 0) {
            invalidateDisplayId(displayId);
        }
        return error;
    }

    private static String packageNameFromComponent(String component) {
        int slash = component.indexOf('/');
        if (slash <= 0 || component.indexOf(' ') >= 0) {
            return null;
        }
        String pkg = component.substring(0, slash);
        return pkg.isEmpty() ? null : pkg;
    }

    private String stopPackage(String pkg, boolean force) throws Exception {
        List<String> output = execForLines(new String[] {
                "/system/bin/cmd", "activity", force ? "force-stop" : "stop-app",
                "--user", "current", pkg});
        return firstCommandError(output);
    }

    private static String firstCommandError(List<String> output) {
        for (String outLine : output) {
            String lower = outLine.toLowerCase();
            if (lower.contains("error") || lower.contains("exception")
                    || lower.contains("unknown command")) {
                return outLine.trim();
            }
        }
        return null;
    }

    private void startMultiDisplayService() {
        try {
            execForLines(new String[] {
                    "/system/bin/cmd", "activity", "broadcast", "--user", "0",
                    "-a", "com.android.emulator.multidisplay.START",
                    "-n",
                    "com.android.emulator.multidisplay/.MultiDisplayServiceReceiver"});
        } catch (Exception e) {
            e.printStackTrace(System.err);
        }
    }

    private static List<String> execForLines(String[] command) throws Exception {
        ProcessBuilder builder = new ProcessBuilder(command);
        builder.redirectErrorStream(true);
        Process process = builder.start();
        List<String> lines = new ArrayList<>();
        try (BufferedReader reader =
                new BufferedReader(
                        new InputStreamReader(
                                process.getInputStream(), StandardCharsets.UTF_8))) {
            String outLine;
            while ((outLine = reader.readLine()) != null) {
                lines.add(outLine);
            }
        }
        process.waitFor();
        return lines;
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
            int displayId = resolveAndroidDisplayId(Integer.parseInt(fields[1]));
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
            int displayId = resolveAndroidDisplayId(Integer.parseInt(fields[1]));
            float x = Float.parseFloat(fields[2]);
            float y = Float.parseFloat(fields[3]);
            float hscroll = Integer.parseInt(fields[4]) / 1000.0f;
            float vscroll = Integer.parseInt(fields[5]) / 1000.0f;
            injectScroll(displayId, x, y, hscroll, vscroll);
            return;
        }
        if (fields.length == 6 && "t".equals(fields[0])) {
            int displayId = resolveAndroidDisplayId(Integer.parseInt(fields[1]));
            int pointerId = Integer.parseInt(fields[2]);
            String phase = fields[3];
            float x = Float.parseFloat(fields[4]);
            float y = Float.parseFloat(fields[5]);
            injectTouch(displayId, pointerId, phase, x, y);
            return;
        }
        if (fields.length == 5 && "m".equals(fields[0])) {
            int displayId = resolveAndroidDisplayId(Integer.parseInt(fields[1]));
            float x = Float.parseFloat(fields[2]);
            float y = Float.parseFloat(fields[3]);
            int buttons = Integer.parseInt(fields[4]);
            injectMouseMove(displayId, x, y, buttons);
            return;
        }
        if (fields.length == 5 && "b".equals(fields[0])) {
            int displayId = resolveAndroidDisplayId(Integer.parseInt(fields[1]));
            float x = Float.parseFloat(fields[2]);
            float y = Float.parseFloat(fields[3]);
            int buttons = Integer.parseInt(fields[4]);
            injectMouseButton(displayId, x, y, buttons);
        }
    }

    private synchronized void injectHover(int displayId, float x, float y) throws Exception {
        if (displayId < 0) {
            return;
        }
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
        if (phase.isEmpty() || displayId < 0) {
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
