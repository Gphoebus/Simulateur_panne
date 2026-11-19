/*
 * Simulateurs de pannes - application enseignant (Android)
 * Phoebus du F.L.A.L. - Lycee de la Borde Basse - STI2D / NSI
 *
 * Interface : page HTML (assets/index.html) dans une WebView.
 * Bluetooth Low Energy : classe BleManager, exposee au JavaScript sous le nom "SPBLE".
 */
package fr.flal.simpanne;

import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.os.Build;
import android.os.Bundle;
import android.view.Window;
import android.view.WindowManager;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import java.util.ArrayList;

public class MainActivity extends Activity {

    static final int REQ_PERM = 1;
    static final int REQ_BT = 2;

    WebView web;
    BleManager ble;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Window w = getWindow();
        w.addFlags(WindowManager.LayoutParams.FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS);
        w.setStatusBarColor(Color.parseColor("#0a2e50"));
        w.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);   // tableau de bord toujours visible

        web = new WebView(this);
        setContentView(web);
        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setAllowFileAccess(true);
        web.setWebViewClient(new WebViewClient());
        web.setWebChromeClient(new WebChromeClient());

        ble = new BleManager(this, web);
        web.addJavascriptInterface(ble, "SPBLE");
        web.loadUrl("file:///android_asset/index.html");
        checkPermissions();
    }

    /** Permissions Bluetooth : Android 12+ (SCAN/CONNECT) et localisation pour Android 6 a 11. */
    String[] neededPermissions() {
        ArrayList<String> p = new ArrayList<String>();
        if (Build.VERSION.SDK_INT >= 31) {
            p.add("android.permission.BLUETOOTH_SCAN");
            p.add("android.permission.BLUETOOTH_CONNECT");
        }
        p.add("android.permission.ACCESS_FINE_LOCATION");
        return p.toArray(new String[0]);
    }

    boolean hasPermissions() {
        for (String p : neededPermissions())
            if (checkSelfPermission(p) != PackageManager.PERMISSION_GRANTED) return false;
        return true;
    }

    void checkPermissions() {
        if (hasPermissions()) onPermissionsOk();
        else requestPermissions(neededPermissions(), REQ_PERM);
    }

    void onPermissionsOk() {
        ble.setPermissions(true);
        BluetoothAdapter ad = ble.adapter();
        if (ad == null) { ble.toastJs("Ce téléphone n'a pas de Bluetooth"); return; }
        if (!ad.isEnabled()) {
            ble.setBluetooth(false);
            requestBluetooth();
        } else ble.setBluetooth(true);
    }

    void requestBluetooth() {
        try {
            startActivityForResult(new Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE), REQ_BT);
        } catch (Exception e) {
            ble.toastJs("Activez le Bluetooth dans les réglages");
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        if (requestCode != REQ_PERM) return;
        if (hasPermissions()) onPermissionsOk();
        else ble.setPermissions(false);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode == REQ_BT) {
            BluetoothAdapter ad = ble.adapter();
            ble.setBluetooth(ad != null && ad.isEnabled());
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        ble.onResume();
    }

    @Override
    public void onBackPressed() {
        web.evaluateJavascript("app.back()", new ValueCallback<String>() {
            @Override
            public void onReceiveValue(String value) {
                if (!"true".equals(value)) finish();
            }
        });
    }

    @Override
    protected void onDestroy() {
        ble.closeAll();
        super.onDestroy();
    }
}
