package com.dualscreen.companion;

import android.content.Context;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
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
