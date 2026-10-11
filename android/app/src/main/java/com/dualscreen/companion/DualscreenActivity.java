package com.dualscreen.companion;

import android.Manifest;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.provider.Settings;
import android.util.Log;
import android.view.InputDevice;
import android.view.WindowManager;
import android.widget.Toast;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;

import org.libsdl.app.SDLActivity;

/**
 * The companion's Android entry point.
 *
 * Everything the app does lives in the native library, which is the same
 * program the desktop and Switch builds run. What is here is the handful of
 * things only the Java side can ask for.
 */
public class DualscreenActivity extends SDLActivity {
    private static final String TAG = "dualscreen";

    private WifiManager.MulticastLock multicastLock;
    private WifiManager.WifiLock wifiLock;

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // A companion is watched, not touched, for minutes at a time.
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        WifiManager wifi = (WifiManager)
                getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        if (wifi == null) {
            return;
        }

        // Without this the Wi-Fi chipset drops broadcast and multicast packets
        // before they reach the app, to save power. Hosts announce themselves
        // by broadcast, so the effect is not a slow search or a warning -- the
        // list simply stays empty for ever, on a network where everything is
        // working.
        try {
            multicastLock = wifi.createMulticastLock("dualscreen-discovery");
            multicastLock.setReferenceCounted(false);
            multicastLock.acquire();
            Log.i(TAG, "holding the multicast lock, so host beacons arrive");
        } catch (Exception error) {
            Log.w(TAG, "no multicast lock: hosts may not be found", error);
        }

        // Wi-Fi power saving parks the radio between packets, which a video
        // stream feels as stutter. Asking for the high-performance mode costs
        // battery and is what this app is for.
        try {
            int mode = Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q
                    ? WifiManager.WIFI_MODE_FULL_LOW_LATENCY
                    : WifiManager.WIFI_MODE_FULL_HIGH_PERF;
            wifiLock = wifi.createWifiLock(mode, "dualscreen-stream");
            wifiLock.setReferenceCounted(false);
            wifiLock.acquire();
        } catch (Exception error) {
            Log.w(TAG, "no wifi lock: the stream may stutter", error);
        }
    }

    /**
     * Whether the native side may read the device's shared storage, which is
     * where games are kept. Called from native code (see ask_activity() in
     * companion/source/main.cpp) when Load a game is chosen.
     *
     * Android 11 and later: "access to all files", a switch in the system's
     * own settings and nothing an app can grant itself. Before that, the
     * ordinary storage permission. Before Android 6, installing was enough.
     */
    /**
     * Which way the picture is turned from the way this device is built, as
     * Surface.ROTATION_0 to ROTATION_270 (0 to 3). Read from native code
     * (src/input/motion.cpp): a device's own motion sensors report in its
     * built-in frame, and a device whose panel is built portrait -- a G
     * Cloud's is -- shows this app turned a quarter of the way round from
     * that.
     */
    public int displayRotation() {
        try {
            return getWindowManager().getDefaultDisplay().getRotation();
        } catch (RuntimeException error) {
            return 0;
        }
    }

    /**
     * What this device is called, for other devices to show: the name set
     * under Settings > About (Android 7.1 and later), else its Bluetooth
     * name, else its maker and model. Read from native code
     * (src/config/settings.cpp) -- the host name an Android device reports
     * is "localhost", whichever device it is.
     */
    public String deviceName() {
        String name = null;
        try {
            if (Build.VERSION.SDK_INT >= 25) {
                name = Settings.Global.getString(getContentResolver(),
                        Settings.Global.DEVICE_NAME);
            }
            if (name == null || name.trim().isEmpty()) {
                name = Settings.Secure.getString(getContentResolver(), "bluetooth_name");
            }
        } catch (RuntimeException error) {
            name = null;
        }
        if (name == null || name.trim().isEmpty()) {
            String maker = Build.MANUFACTURER == null ? "" : Build.MANUFACTURER;
            String model = Build.MODEL == null ? "" : Build.MODEL;
            name = model.toLowerCase().startsWith(maker.toLowerCase()) ? model
                    : (maker + " " + model).trim();
        }
        return name.trim();
    }

    /**
     * Runs every game controller's motor for {@code ms} milliseconds, or stops
     * them at 0. Called from native code (src/input/input_manager.cpp): SDL's
     * Android port has no controller rumble at all. A controller with no motor
     * of its own -- a handheld's built-in controls, a G Cloud's among them --
     * buzzes the device itself, which is where those controls are.
     */
    public void rumble(int ms) {
        try {
            boolean any = false;
            for (int id : InputDevice.getDeviceIds()) {
                InputDevice device = InputDevice.getDevice(id);
                if (device == null || device.isVirtual()) continue;
                int sources = device.getSources();
                boolean pad = (sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                        || (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
                if (!pad) continue;
                Vibrator motor = device.getVibrator();
                if (motor != null && motor.hasVibrator()) {
                    buzz(motor, ms);
                    any = true;
                }
            }
            if (!any) {
                Vibrator motor = (Vibrator) getSystemService(Context.VIBRATOR_SERVICE);
                if (motor != null && motor.hasVibrator()) buzz(motor, ms);
            }
        } catch (RuntimeException error) {
            Log.w(TAG, "could not rumble", error);
        }
    }

    private static void buzz(Vibrator motor, int ms) {
        if (ms <= 0) {
            motor.cancel();
        } else if (Build.VERSION.SDK_INT >= 26) {
            motor.vibrate(VibrationEffect.createOneShot(ms, VibrationEffect.DEFAULT_AMPLITUDE));
        } else {
            motor.vibrate(ms);
        }
    }

    public boolean hasFileAccess() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            return Environment.isExternalStorageManager();
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            return checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE)
                    == PackageManager.PERMISSION_GRANTED;
        }
        return true;
    }

    /**
     * Asks for what hasFileAccess() checks: the system's page for this app's
     * "access to all files" switch, or the ordinary permission prompt. Returns
     * at once; the native side asks hasFileAccess() again when this activity
     * comes back to the front. Always true -- the return value only lets the
     * native side call every method here the same way.
     */
    public boolean requestFileAccess() {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    try {
                        Intent intent = new Intent(
                                Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                Uri.parse("package:" + getPackageName()));
                        startActivity(intent);
                    } catch (Exception error) {
                        // Some builds of Android have no per-app page; the
                        // list of every app is the next best thing.
                        try {
                            startActivity(new Intent(
                                    Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                        } catch (Exception again) {
                            Log.w(TAG, "nowhere to ask for access to all files", again);
                        }
                    }
                } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                    requestPermissions(new String[] {
                            Manifest.permission.READ_EXTERNAL_STORAGE,
                            Manifest.permission.WRITE_EXTERNAL_STORAGE,
                    }, 1);
                }
            }
        });
        return true;
    }

    // ---- Updates -------------------------------------------------------
    //
    // Called from native code on the updater's own thread (see
    // companion/source/android_updater.cpp): fetching what the newest build
    // is, downloading it, and handing it to Android's package installer.
    // Each of them blocks, and each says what went wrong through
    // updateError() rather than an exception crossing into native code.

    /** Where the package installer's answer comes back to, as a new intent. */
    private static final String ACTION_INSTALLED = "com.dualscreen.companion.UPDATE_INSTALLED";

    private volatile String updateError = "";

    /** Why the last of the methods below failed. */
    public String updateError() {
        return updateError;
    }

    /** How far a download has got; false to stop it. */
    private static native boolean updateProgress(long done, long total);

    private static HttpURLConnection open(String address) throws IOException {
        HttpURLConnection connection = (HttpURLConnection) new URL(address).openConnection();
        connection.setConnectTimeout(15000);
        connection.setReadTimeout(30000);
        connection.setInstanceFollowRedirects(true);
        connection.setRequestProperty("User-Agent", "Dualscreen");
        int status = connection.getResponseCode();
        if (status != HttpURLConnection.HTTP_OK) {
            connection.disconnect();
            throw new IOException(status == HttpURLConnection.HTTP_NOT_FOUND
                    ? "the release is not there" : "GitHub answered " + status);
        }
        return connection;
    }

    private static String describe(Exception error) {
        if (error instanceof java.net.UnknownHostException
                || error instanceof java.net.ConnectException
                || error instanceof java.net.SocketTimeoutException) {
            return "GitHub could not be reached";
        }
        String message = error.getMessage();
        return message == null || message.isEmpty() ? error.getClass().getSimpleName() : message;
    }

    /** A small file, whole -- the release's version.json -- or null. */
    public String httpGet(String address) {
        HttpURLConnection connection = null;
        try {
            connection = open(address);
            ByteArrayOutputStream body = new ByteArrayOutputStream();
            byte[] buffer = new byte[8192];
            try (InputStream in = connection.getInputStream()) {
                for (int read; (read = in.read(buffer)) > 0; ) {
                    body.write(buffer, 0, read);
                    if (body.size() > 65536) throw new IOException("the answer is too long");
                }
            }
            return body.toString("UTF-8");
        } catch (Exception error) {
            updateError = describe(error);
            return null;
        } finally {
            if (connection != null) connection.disconnect();
        }
    }

    /** A release's file, into {@code path}. */
    public boolean downloadFile(String address, String path) {
        HttpURLConnection connection = null;
        File file = new File(path);
        try {
            connection = open(address);
            long total = connection.getContentLength();
            long done = 0;
            byte[] buffer = new byte[65536];
            try (InputStream in = connection.getInputStream();
                 OutputStream out = new FileOutputStream(file)) {
                for (int read; (read = in.read(buffer)) > 0; ) {
                    out.write(buffer, 0, read);
                    done += read;
                    if (!updateProgress(done, Math.max(0, total))) {
                        throw new IOException("cancelled");
                    }
                }
            }
            return true;
        } catch (Exception error) {
            updateError = describe(error);
            file.delete();
            return false;
        } finally {
            if (connection != null) connection.disconnect();
        }
    }

    /**
     * Hands the APK at {@code path} to the package installer. It asks the
     * player -- and, the first time, for this app to be allowed to install
     * apps at all -- then replaces this one; its answer comes back through
     * onNewIntent(). True once it has been handed over.
     */
    public boolean installPackage(String path) {
        File file = new File(path);
        PackageInstaller.Session session = null;
        try {
            PackageInstaller installer = getPackageManager().getPackageInstaller();
            PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(
                    PackageInstaller.SessionParams.MODE_FULL_INSTALL);
            params.setAppPackageName(getPackageName());
            int id = installer.createSession(params);
            session = installer.openSession(id);
            try (InputStream in = new FileInputStream(file);
                 OutputStream out = session.openWrite("dualscreen.apk", 0, file.length())) {
                byte[] buffer = new byte[65536];
                for (int read; (read = in.read(buffer)) > 0; ) {
                    out.write(buffer, 0, read);
                }
                session.fsync(out);
            }
            Intent answer = new Intent(this, DualscreenActivity.class).setAction(ACTION_INSTALLED);
            int flags = PendingIntent.FLAG_UPDATE_CURRENT;
            // The installer adds its answer to the intent, which Android 12
            // and later allow only into one marked mutable.
            if (Build.VERSION.SDK_INT >= 31) flags |= PendingIntent.FLAG_MUTABLE;
            PendingIntent pending = PendingIntent.getActivity(this, id, answer, flags);
            session.commit(pending.getIntentSender());
            Log.i(TAG, "handed the update to the package installer");
            return true;
        } catch (Exception error) {
            updateError = describe(error);
            if (session != null) session.abandon();
            return false;
        } finally {
            if (session != null) session.close();
            file.delete();
        }
    }

    @Override
    protected void onNewIntent(Intent intent) {
        if (intent != null && ACTION_INSTALLED.equals(intent.getAction())) {
            int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS,
                    PackageInstaller.STATUS_FAILURE);
            if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
                // The installer's own screen, asking the player.
                Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT);
                if (confirm != null) startActivity(confirm);
            } else if (status != PackageInstaller.STATUS_SUCCESS) {
                String message = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
                Log.w(TAG, "the update was not installed: " + message);
                Toast.makeText(this, "The update was not installed"
                        + (message == null ? "." : ": " + message), Toast.LENGTH_LONG).show();
            }
            return;
        }
        super.onNewIntent(intent);
    }

    @Override
    protected void onDestroy() {
        if (multicastLock != null && multicastLock.isHeld()) {
            multicastLock.release();
        }
        if (wifiLock != null && wifiLock.isHeld()) {
            wifiLock.release();
        }
        super.onDestroy();
    }
}
