/*
 * BleManager - gestion Bluetooth Low Energy des simulateurs de pannes.
 * Plusieurs boitiers connectes simultanement, file d'operations GATT par boitier,
 * reconnexion automatique. Les evenements sont transmis au JavaScript (window.bleEvent).
 *
 * Phoebus du F.L.A.L. - Lycee de la Borde Basse - STI2D / NSI
 */
package fr.flal.simpanne;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanFilter;
import android.bluetooth.le.ScanRecord;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.Context;
import android.content.SharedPreferences;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelUuid;
import android.os.Vibrator;
import android.webkit.JavascriptInterface;
import android.webkit.WebView;

import org.json.JSONObject;

import java.nio.charset.Charset;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedList;
import java.util.List;
import java.util.UUID;

public class BleManager {

    // ----- UUID du service du simulateur (identiques au firmware) -----
    static final UUID SVC  = UUID.fromString("5f6d0001-1d9c-4c7e-a7b1-53494d50414e");
    static final UUID CFG  = UUID.fromString("5f6d0002-1d9c-4c7e-a7b1-53494d50414e");
    static final UUID ETAT = UUID.fromString("5f6d0003-1d9c-4c7e-a7b1-53494d50414e");
    static final UUID CMD  = UUID.fromString("5f6d0004-1d9c-4c7e-a7b1-53494d50414e");
    static final UUID REP  = UUID.fromString("5f6d0005-1d9c-4c7e-a7b1-53494d50414e");
    static final UUID CCCD = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");
    static final Charset UTF8 = Charset.forName("UTF-8");

    static final long CONNECT_TIMEOUT = 12000;
    static final long OP_TIMEOUT = 6000;
    static final long RECONNECT_DELAY = 15000;

    final MainActivity act;
    final WebView web;
    final Handler h = new Handler(Looper.getMainLooper());
    final HashMap<String, Sim> sims = new HashMap<String, Sim>();
    final LinkedList<Sim> connectQueue = new LinkedList<Sim>();
    Sim connecting = null;
    boolean permOk = false, btOn = false, scanning = false;
    ScanCallback scanCb = null;

    BleManager(MainActivity a, WebView w) { act = a; web = w; }

    BluetoothAdapter adapter() {
        BluetoothManager m = (BluetoothManager) act.getSystemService(Context.BLUETOOTH_SERVICE);
        return m == null ? null : m.getAdapter();
    }

    // =====================================================================
    //  Evenements vers le JavaScript
    // =====================================================================
    void emit(final JSONObject o) {
        h.post(new Runnable() {
            public void run() {
                web.evaluateJavascript("window.bleEvent && window.bleEvent(" + JSONObject.quote(o.toString()) + ")", null);
            }
        });
    }

    void emit(String t, String mac, String key, Object val) {
        try {
            JSONObject o = new JSONObject();
            o.put("t", t);
            if (mac != null) o.put("mac", mac);
            if (key != null) o.put(key, val);
            emit(o);
        } catch (Exception ignored) { }
    }

    void toastJs(String m) { emit("toast", null, "m", m); }

    void setPermissions(boolean ok) { permOk = ok; emit("perm", null, "ok", ok); if (ok) pumpConnect(); }

    void setBluetooth(boolean on) { btOn = on; emit("bt", null, "on", on); if (on) pumpConnect(); }

    void onResume() {
        BluetoothAdapter ad = adapter();
        boolean on = ad != null && ad.isEnabled();
        if (on != btOn) setBluetooth(on);
    }

    // =====================================================================
    //  Interface JavaScript (appelee depuis le thread JavaBridge)
    // =====================================================================
    @JavascriptInterface public String loadData() {
        return act.getSharedPreferences("simpanne", Context.MODE_PRIVATE).getString("data", "");
    }

    @JavascriptInterface public void saveData(String json) {
        SharedPreferences.Editor e = act.getSharedPreferences("simpanne", Context.MODE_PRIVATE).edit();
        e.putString("data", json);
        e.apply();
    }

    @JavascriptInterface public String getStatus() {
        BluetoothAdapter ad = adapter();
        return "{\"perm\":" + permOk + ",\"bt\":" + (ad != null && ad.isEnabled()) + "}";
    }

    @JavascriptInterface public void requestPerms() {
        h.post(new Runnable() { public void run() { act.checkPermissions(); } });
    }

    @JavascriptInterface public void requestBt() {
        h.post(new Runnable() { public void run() { act.requestBluetooth(); } });
    }

    @JavascriptInterface public void vibrate(final int ms) {
        try {
            Vibrator v = (Vibrator) act.getSystemService(Context.VIBRATOR_SERVICE);
            if (v != null) v.vibrate(ms);
        } catch (Exception ignored) { }
    }

    @JavascriptInterface public void startScan(final int ms) {
        h.post(new Runnable() { public void run() { doStartScan(ms); } });
    }

    @JavascriptInterface public void stopScan() {
        h.post(new Runnable() { public void run() { doStopScan(); } });
    }

    @JavascriptInterface public void connect(final String mac, final String pin) {
        h.post(new Runnable() { public void run() { doConnect(mac, pin); } });
    }

    @JavascriptInterface public void disconnect(final String mac) {
        h.post(new Runnable() { public void run() { doDisconnect(mac); } });
    }

    @JavascriptInterface public void command(final String mac, final String cmd) {
        h.post(new Runnable() { public void run() { doWrite(mac, CMD, cmd); } });
    }

    @JavascriptInterface public void writeConfig(final String mac, final String cfg) {
        h.post(new Runnable() { public void run() { doWrite(mac, CFG, cfg); } });
    }

    @JavascriptInterface public void refresh(final String mac) {
        h.post(new Runnable() {
            public void run() {
                Sim s = sims.get(mac);
                if (s == null || s.state != Sim.READY) return;
                s.enqueue(new ReadOp(s.cCfg));
                s.enqueue(new ReadOp(s.cEtat));
            }
        });
    }

    // =====================================================================
    //  Recherche des boitiers
    // =====================================================================
    void doStartScan(int ms) {
        BluetoothAdapter ad = adapter();
        if (!permOk || ad == null || !ad.isEnabled()) {
            emit("scanend", null, null, null);
            toastJs("Bluetooth indisponible ou autorisations manquantes");
            return;
        }
        doStopScan();
        final BluetoothLeScanner sc = ad.getBluetoothLeScanner();
        if (sc == null) { emit("scanend", null, null, null); return; }
        scanCb = new ScanCallback() {
            @Override public void onScanResult(int type, ScanResult r) { reportScan(r); }
            @Override public void onBatchScanResults(List<ScanResult> l) { for (ScanResult r : l) reportScan(r); }
            @Override public void onScanFailed(int code) {
                scanning = false;
                toastJs("Échec de la recherche Bluetooth (code " + code + ")");
                emit("scanend", null, null, null);
            }
        };
        List<ScanFilter> f = new ArrayList<ScanFilter>();
        f.add(new ScanFilter.Builder().setServiceUuid(new ParcelUuid(SVC)).build());
        ScanSettings st = new ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build();
        try {
            sc.startScan(f, st, scanCb);
            scanning = true;
        } catch (SecurityException e) {
            setPermissions(false);
            emit("scanend", null, null, null);
            return;
        }
        h.postDelayed(new Runnable() {
            public void run() { if (scanning) { doStopScan(); emit("scanend", null, null, null); } }
        }, Math.max(3000, ms));
    }

    void doStopScan() {
        if (!scanning || scanCb == null) return;
        scanning = false;
        try {
            BluetoothAdapter ad = adapter();
            if (ad != null && ad.getBluetoothLeScanner() != null) ad.getBluetoothLeScanner().stopScan(scanCb);
        } catch (Exception ignored) { }
    }

    void reportScan(ScanResult r) {
        try {
            String name = null;
            ScanRecord rec = r.getScanRecord();
            if (rec != null) name = rec.getDeviceName();
            if (name == null) { try { name = r.getDevice().getName(); } catch (SecurityException ignored) { } }
            JSONObject o = new JSONObject();
            o.put("t", "scan");
            o.put("mac", r.getDevice().getAddress());
            o.put("name", name == null ? "Simulateur" : name);
            o.put("rssi", r.getRssi());
            emit(o);
        } catch (Exception ignored) { }
    }

    // =====================================================================
    //  Connexions
    // =====================================================================
    void doConnect(String mac, String pin) {
        Sim s = sims.get(mac);
        if (s == null) { s = new Sim(mac); sims.put(mac, s); }
        s.pin = pin;
        s.want = true;
        if (s.state == Sim.READY) {               // deja connecte : re-authentifie et relit tout
            s.enqueue(new WriteOp(s.cCmd, "AUTH " + pin));
            s.enqueue(new ReadOp(s.cCfg));
            s.enqueue(new ReadOp(s.cEtat));
            emit("conn", mac, "s", "ready");
            return;
        }
        if (s.state == Sim.OFF && !connectQueue.contains(s) && connecting != s) connectQueue.add(s);
        emit("conn", mac, "s", "connecting");
        pumpConnect();
    }

    /** Les connexions sont etablies une par une (plus fiable sur Android). */
    void pumpConnect() {
        if (connecting != null || !permOk || !btOn) return;
        final Sim s = connectQueue.poll();
        if (s == null) return;
        if (!s.want || s.state != Sim.OFF) { pumpConnect(); return; }
        connecting = s;
        s.state = Sim.CONNECTING;
        s.gen++;
        final int gen = s.gen;
        try {
            BluetoothDevice dev = adapter().getRemoteDevice(s.mac);
            s.gatt = dev.connectGatt(act, false, new Cb(s), BluetoothDevice.TRANSPORT_LE);
        } catch (Exception e) {
            fail(s, "connexion impossible");
            return;
        }
        h.postDelayed(new Runnable() {
            public void run() { if (s.gen == gen && s.state == Sim.CONNECTING) fail(s, "délai dépassé"); }
        }, CONNECT_TIMEOUT);
    }

    void fail(final Sim s, String why) {
        closeGatt(s);
        emit2conn(s.mac, "offline", why);
        if (connecting == s) connecting = null;
        if (s.want) {
            final int gen = s.gen;
            h.postDelayed(new Runnable() {
                public void run() {
                    if (s.want && s.state == Sim.OFF && s.gen == gen && !connectQueue.contains(s)) {
                        connectQueue.add(s);
                        emit("conn", s.mac, "s", "connecting");
                        pumpConnect();
                    }
                }
            }, RECONNECT_DELAY);
        }
        pumpConnect();
    }

    void emit2conn(String mac, String st, String err) {
        try {
            JSONObject o = new JSONObject();
            o.put("t", "conn"); o.put("mac", mac); o.put("s", st);
            if (err != null) o.put("err", err);
            emit(o);
        } catch (Exception ignored) { }
    }

    void closeGatt(Sim s) {
        s.state = Sim.OFF;
        s.ops.clear();
        s.cur = null;
        if (s.gatt != null) {
            try { s.gatt.disconnect(); } catch (Exception ignored) { }
            try { s.gatt.close(); } catch (Exception ignored) { }
            s.gatt = null;
        }
    }

    void doDisconnect(String mac) {
        Sim s = sims.get(mac);
        if (s == null) return;
        s.want = false;
        s.gen++;
        connectQueue.remove(s);
        closeGatt(s);
        if (connecting == s) { connecting = null; pumpConnect(); }
        emit("conn", mac, "s", "offline");
    }

    void doWrite(String mac, UUID ch, String value) {
        Sim s = sims.get(mac);
        if (s == null || s.state != Sim.READY) { toastJs("Simulateur non connecté"); return; }
        s.enqueue(new WriteOp(ch == CFG ? s.cCfg : s.cCmd, value));
    }

    void closeAll() {
        doStopScan();
        for (Sim s : sims.values()) { s.want = false; closeGatt(s); }
    }

    // =====================================================================
    //  Un simulateur et sa file d'operations GATT
    // =====================================================================
    class Sim {
        static final int OFF = 0, CONNECTING = 1, READY = 2;
        final String mac;
        String pin = "0000";
        boolean want = false;
        int state = OFF, gen = 0, opGen = 0;
        BluetoothGatt gatt;
        BluetoothGattCharacteristic cCfg, cEtat, cCmd, cRep;
        final LinkedList<Op> ops = new LinkedList<Op>();
        Op cur = null;

        Sim(String m) { mac = m; }

        void enqueue(Op o) { ops.add(o); next(); }

        void next() {
            if (cur != null || gatt == null) return;
            while (!ops.isEmpty()) {
                cur = ops.poll();
                boolean started;
                try { started = cur.start(this); } catch (Exception e) { started = false; }
                if (started) {
                    final Op watched = cur;
                    final int g = ++opGen;
                    h.postDelayed(new Runnable() {
                        public void run() { if (cur == watched && opGen == g) { cur = null; next(); } }
                    }, OP_TIMEOUT);
                    return;
                }
                cur = null;
            }
        }

        void done() { cur = null; next(); }
    }

    abstract class Op { abstract boolean start(Sim s); }

    class ReadOp extends Op {
        final BluetoothGattCharacteristic c;
        ReadOp(BluetoothGattCharacteristic c) { this.c = c; }
        boolean start(Sim s) { return c != null && s.gatt.readCharacteristic(c); }
    }

    class WriteOp extends Op {
        final BluetoothGattCharacteristic c;
        final String v;
        WriteOp(BluetoothGattCharacteristic c, String v) { this.c = c; this.v = v; }
        boolean start(Sim s) {
            if (c == null) return false;
            c.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
            c.setValue(v.getBytes(UTF8));
            return s.gatt.writeCharacteristic(c);
        }
    }

    class NotifyOp extends Op {
        final BluetoothGattCharacteristic c;
        NotifyOp(BluetoothGattCharacteristic c) { this.c = c; }
        boolean start(Sim s) {
            if (c == null) return false;
            s.gatt.setCharacteristicNotification(c, true);
            BluetoothGattDescriptor d = c.getDescriptor(CCCD);
            if (d == null) return false;
            d.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
            return s.gatt.writeDescriptor(d);
        }
    }

    /** Marque la fin de la sequence d'ouverture : le boitier est pret. */
    class ReadyOp extends Op {
        boolean start(Sim s) {
            s.state = Sim.READY;
            if (connecting == s) connecting = null;
            emit("conn", s.mac, "s", "ready");
            pumpConnect();
            return false;   // operation instantanee
        }
    }

    // =====================================================================
    //  Callbacks GATT (thread Binder -> repasse sur le thread principal)
    // =====================================================================
    class Cb extends BluetoothGattCallback {
        final Sim s;
        final int gen;
        Cb(Sim s) { this.s = s; this.gen = s.gen; }

        boolean stale(BluetoothGatt g) { return s.gen != gen || s.gatt != g; }

        @Override public void onConnectionStateChange(final BluetoothGatt g, final int status, final int newState) {
            h.post(new Runnable() {
                public void run() {
                    if (stale(g)) { try { g.close(); } catch (Exception ignored) { } return; }
                    if (newState == BluetoothProfile.STATE_CONNECTED && status == BluetoothGatt.GATT_SUCCESS) {
                        if (!g.requestMtu(247)) g.discoverServices();
                    } else {
                        boolean wasReady = s.state == Sim.READY;
                        fail(s, wasReady ? "liaison perdue" : "erreur " + status);
                    }
                }
            });
        }

        @Override public void onMtuChanged(final BluetoothGatt g, int mtu, int status) {
            h.post(new Runnable() { public void run() { if (!stale(g)) g.discoverServices(); } });
        }

        @Override public void onServicesDiscovered(final BluetoothGatt g, final int status) {
            h.post(new Runnable() {
                public void run() {
                    if (stale(g)) return;
                    BluetoothGattService svc = g.getService(SVC);
                    if (status != BluetoothGatt.GATT_SUCCESS || svc == null) { fail(s, "service introuvable"); return; }
                    s.cCfg = svc.getCharacteristic(CFG);
                    s.cEtat = svc.getCharacteristic(ETAT);
                    s.cCmd = svc.getCharacteristic(CMD);
                    s.cRep = svc.getCharacteristic(REP);
                    if (s.cCfg == null || s.cEtat == null || s.cCmd == null || s.cRep == null) { fail(s, "firmware incompatible"); return; }
                    s.enqueue(new NotifyOp(s.cRep));
                    s.enqueue(new NotifyOp(s.cEtat));
                    s.enqueue(new WriteOp(s.cCmd, "AUTH " + s.pin));
                    s.enqueue(new ReadOp(s.cCfg));
                    s.enqueue(new ReadOp(s.cEtat));
                    s.enqueue(new ReadyOp());
                }
            });
        }

        @Override public void onCharacteristicRead(final BluetoothGatt g, final BluetoothGattCharacteristic c, final int status) {
            final byte[] v = c.getValue();
            h.post(new Runnable() {
                public void run() {
                    if (stale(g)) return;
                    if (status == BluetoothGatt.GATT_SUCCESS && v != null) deliver(c.getUuid(), v);
                    s.done();
                }
            });
        }

        @Override public void onCharacteristicWrite(final BluetoothGatt g, BluetoothGattCharacteristic c, final int status) {
            h.post(new Runnable() {
                public void run() {
                    if (stale(g)) return;
                    if (status != BluetoothGatt.GATT_SUCCESS) toastJs("Écriture refusée par le boîtier (" + status + ")");
                    s.done();
                }
            });
        }

        @Override public void onDescriptorWrite(final BluetoothGatt g, BluetoothGattDescriptor d, int status) {
            h.post(new Runnable() { public void run() { if (!stale(g)) s.done(); } });
        }

        @Override public void onCharacteristicChanged(final BluetoothGatt g, final BluetoothGattCharacteristic c) {
            final byte[] v = c.getValue();
            h.post(new Runnable() { public void run() { if (!stale(g) && v != null) deliver(c.getUuid(), v); } });
        }

        void deliver(UUID u, byte[] v) {
            String d = new String(v, UTF8);
            String t = u.equals(CFG) ? "cfg" : u.equals(ETAT) ? "st" : u.equals(REP) ? "rsp" : null;
            if (t != null) emit(t, s.mac, "d", d);
        }
    }
}
