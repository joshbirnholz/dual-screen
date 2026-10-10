package com.dualscreen.companion;

import android.Manifest;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.util.Log;
import android.view.WindowManager;

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
