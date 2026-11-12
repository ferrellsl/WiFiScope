package com.ferrellsl.wifiscope;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.location.LocationManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.provider.Settings;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.view.Menu;
import android.view.MenuItem;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.widget.Toolbar;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.Toast;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

// WiFiScope for Android: scans every 5 seconds while it is in front (as often as
// Android allows), and redraws every second.
public final class MainActivity extends Activity implements ScopeView.Host {

    private static final int SCAN_EVERY_MS = 5000, REFRESH_EVERY_MS = 1000;
    private static final int ASK_PERMISSIONS = 1;

    private final Model model = new Model();
    private final Handler handler = new Handler(Looper.getMainLooper());
    private Scanner scanner;
    private Model.Source source;                 // the scanner, or a recorded scan being played back
    private ScopeView view;
    private EditText filterBox;
    private SharedPreferences prefs;
    private boolean running = false;
    private String bannerAction = "";

    private final Runnable scanLoop = new Runnable() {
        @Override public void run() {
            if (!running) return;
            if (source == scanner) scanner.scan();
            handler.postDelayed(this, SCAN_EVERY_MS);
        }
    };
    private final Runnable refreshLoop = new Runnable() {
        @Override public void run() {
            if (!running) return;
            refresh();
            handler.postDelayed(this, REFRESH_EVERY_MS);
        }
    };

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        prefs = getSharedPreferences("WiFiScope", MODE_PRIVATE);
        scanner = new Scanner(this);
        source = scanner;
        loadSettings();

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(0xFF0B1015);
        // The title bar is ours, in the layout: Android 15 and later draw apps edge to
        // edge, under the status and navigation bars, so the layout is padded by them
        // (and by a camera cutout) instead.
        Toolbar bar = new Toolbar(this);
        bar.setBackgroundColor(0xFF141A21);
        bar.setTitle("WiFiScope");
        bar.setTitleTextColor(0xFFFFFFFF);
        root.addView(bar, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        setActionBar(bar);
        root.setOnApplyWindowInsetsListener((v, insets) -> {
            int l, t, r, b;
            if (Build.VERSION.SDK_INT >= 30) {
                android.graphics.Insets i = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                android.graphics.Insets ime = insets.getInsets(WindowInsets.Type.ime());
                l = i.left; t = i.top; r = i.right; b = Math.max(i.bottom, ime.bottom);
            } else {
                l = insets.getSystemWindowInsetLeft(); t = insets.getSystemWindowInsetTop();
                r = insets.getSystemWindowInsetRight(); b = insets.getSystemWindowInsetBottom();
            }
            v.setPadding(l, t, r, b);
            return insets;
        });
        if (Build.VERSION.SDK_INT >= 30) getWindow().setDecorFitsSystemWindows(false);
        filterBox = new EditText(this);
        filterBox.setHint("Filter by name, BSSID or vendor");
        filterBox.setSingleLine(true);
        filterBox.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        filterBox.setTextColor(0xFFE8EEF4);
        filterBox.setHintTextColor(0xFF6F8296);
        filterBox.setBackgroundColor(0xFF243039);
        final int pad = (int) (10 * getResources().getDisplayMetrics().density);
        filterBox.setPadding(pad, pad, pad, pad);
        filterBox.setVisibility(View.GONE);
        filterBox.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void onTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void afterTextChanged(Editable s) { model.filter = s.toString(); view.invalidate(); }
        });
        root.addView(filterBox, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        view = new ScopeView(this, model);
        view.setHost(this);
        view.split = prefs.getFloat("split", 0.52f);
        view.timeView = "time".equals(prefs.getString("graph", "channels"));
        view.narrowBand = prefs.getInt("narrowBand", 5);
        root.addView(view, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        setContentView(root);

        // the makers' registry (1.4 MB) is read once, away from the screen
        new Thread(() -> {
            try { Oui.load(getAssets().open("oui.txt")); } catch (Exception ignored) {}
            handler.post(() -> { for (Model.Entry e : model.entries.values()) e.vendor = ""; refresh(); });
        }, "oui").start();

        askPermissions();
    }

    @Override protected void onResume() {
        super.onResume();
        running = true;
        scanner.start(() -> { if (source == scanner) refresh(); });
        handler.post(scanLoop);
        handler.post(refreshLoop);
    }

    @Override protected void onPause() {
        super.onPause();
        running = false;
        handler.removeCallbacks(scanLoop);
        handler.removeCallbacks(refreshLoop);
        scanner.stop();
        saveSettings();
    }

    private void refresh() {
        model.refresh(source, SystemClock.elapsedRealtime());
        updateBanner();
        view.invalidate();
    }

    // ------------------------------------------------------------- what's in the way of scanning
    private boolean permitted() {
        if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission(Manifest.permission.NEARBY_WIFI_DEVICES) != PackageManager.PERMISSION_GRANTED)
            return false;
        return checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED;
    }

    private void askPermissions() {
        List<String> want = new ArrayList<>();
        want.add(Manifest.permission.ACCESS_FINE_LOCATION);
        want.add(Manifest.permission.ACCESS_COARSE_LOCATION);
        if (Build.VERSION.SDK_INT >= 33) want.add(Manifest.permission.NEARBY_WIFI_DEVICES);
        requestPermissions(want.toArray(new String[0]), ASK_PERMISSIONS);
    }

    @Override public void onRequestPermissionsResult(int code, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(code, permissions, results);
        scanner.scan();
        refresh();
    }

    private boolean locationOn() {
        LocationManager lm = (LocationManager) getSystemService(Context.LOCATION_SERVICE);
        if (lm == null) return true;
        if (Build.VERSION.SDK_INT >= 28) return lm.isLocationEnabled();
        return lm.isProviderEnabled(LocationManager.NETWORK_PROVIDER) || lm.isProviderEnabled(LocationManager.GPS_PROVIDER);
    }

    private void updateBanner() {
        String text = "", action = "";
        if (source != scanner) { text = "Playing back a recorded scan  -  tap to scan again"; action = "live"; }
        else if (!permitted()) { text = "WiFiScope needs the location permission to see the networks  -  tap to allow it"; action = "permission"; }
        else if (!scanner.wifiOn()) { text = "Wi-Fi is off  -  tap to open the Wi-Fi settings"; action = "wifi"; }
        else if (!locationOn() && model.entries.isEmpty()) { text = "Location is off: Android shows apps no networks without it  -  tap to turn it on"; action = "location"; }
        else if (scanner.throttled()) { text = "Android allows 4 scans in 2 minutes  -  tap for faster updates (Wi-Fi scan throttling)"; action = "throttle"; }
        bannerAction = action;
        view.setBanner(text);
    }

    @Override public void bannerTapped() {
        switch (bannerAction) {
            case "live": source = scanner; model.entries.clear(); scanner.scan(); refresh(); break;
            case "permission":
                if (shouldShowRequestPermissionRationale(Manifest.permission.ACCESS_FINE_LOCATION) || prefs.getInt("asked", 0) < 2) {
                    prefs.edit().putInt("asked", prefs.getInt("asked", 0) + 1).apply();
                    askPermissions();
                } else startActivity(new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.fromParts("package", getPackageName(), null)));
                break;
            case "wifi": startActivity(new Intent(Settings.ACTION_WIFI_SETTINGS)); break;
            case "location": startActivity(new Intent(Settings.ACTION_LOCATION_SOURCE_SETTINGS)); break;
            case "throttle":
                new AlertDialog.Builder(this)
                    .setTitle("Faster scans")
                    .setMessage("Android lets an app ask for only 4 Wi-Fi scans every 2 minutes; between them WiFiScope shows " +
                            "what the system's own scans heard.\n\nFor a scan every 5 seconds, turn off \"Wi-Fi scan throttling\" " +
                            "in Settings > System > Developer options (to show Developer options, tap Build number in " +
                            "Settings > About 7 times).")
                    .setPositiveButton("Developer options", (d, w) -> {
                        try { startActivity(new Intent(Settings.ACTION_APPLICATION_DEVELOPMENT_SETTINGS)); }
                        catch (Exception e) { startActivity(new Intent(Settings.ACTION_SETTINGS)); }
                    })
                    .setNegativeButton("Not now", null)
                    .show();
                break;
        }
    }

    // ------------------------------------------------------------- settings
    private void loadSettings() {
        final String sort = prefs.getString("sort", "Signal 1");
        final int space = sort.lastIndexOf(' ');
        if (space > 0)
            for (Model.Column c : Model.Column.values())
                if (sort.substring(0, space).equals(c.title)) { model.sortColumn = c; model.sortDown = !sort.endsWith(" 0"); }
        final String bands = prefs.getString("bands", "1 1 1");
        final String[] b = bands.split(" ");
        for (int i = 0; i < 3 && i < b.length; ++i) model.showBand[i] = !b[i].equals("0");
        model.grouped = prefs.getBoolean("grouped", false);
    }

    private void saveSettings() {
        prefs.edit()
            .putString("sort", model.sortColumn.title + " " + (model.sortDown ? 1 : 0))
            .putString("bands", (model.showBand[0] ? 1 : 0) + " " + (model.showBand[1] ? 1 : 0) + " " + (model.showBand[2] ? 1 : 0))
            .putBoolean("grouped", model.grouped)
            .putFloat("split", view.split)
            .putString("graph", view.timeView ? "time" : "channels")
            .putInt("narrowBand", view.narrowBand)
            .apply();
    }

    @Override public void settingsChanged() {
        saveSettings();
        invalidateOptionsMenu();
    }

    // ------------------------------------------------------------- the menu
    private static final int M_FILTER = 1, M_SCAN = 2, M_GROUP = 3, M_TIME = 4, M_COPY = 5, M_DEMO = 6, M_PLAYBACK = 7, M_ABOUT = 8;

    @Override public boolean onCreateOptionsMenu(Menu menu) {
        menu.add(0, M_FILTER, 0, "Filter").setIcon(android.R.drawable.ic_menu_search).setShowAsAction(MenuItem.SHOW_AS_ACTION_ALWAYS);
        menu.add(0, M_SCAN, 1, "Scan now").setIcon(android.R.drawable.ic_popup_sync).setShowAsAction(MenuItem.SHOW_AS_ACTION_IF_ROOM);
        menu.add(0, M_GROUP, 2, "Group by router").setCheckable(true);
        menu.add(0, M_TIME, 3, "Signal over time").setCheckable(true);
        menu.add(0, M_COPY, 4, "Copy all rows");
        menu.add(0, M_DEMO, 5, "Demo mode (disguise names and addresses)").setCheckable(true);
        menu.add(0, M_PLAYBACK, 6, "Play back a recorded scan");
        menu.add(0, M_ABOUT, 7, "About WiFiScope");
        return true;
    }

    @Override public boolean onPrepareOptionsMenu(Menu menu) {
        menu.findItem(M_GROUP).setChecked(model.grouped);
        menu.findItem(M_TIME).setChecked(view.timeView);
        menu.findItem(M_DEMO).setChecked(model.demo());
        menu.findItem(M_PLAYBACK).setVisible(captureFile().exists());
        return true;
    }

    private File captureFile() { return new File(getExternalFilesDir(null), "capture.txt"); }

    @Override public boolean onOptionsItemSelected(MenuItem item) {
        switch (item.getItemId()) {
            case M_FILTER: {
                final boolean show = filterBox.getVisibility() != View.VISIBLE;
                filterBox.setVisibility(show ? View.VISIBLE : View.GONE);
                InputMethodManager im = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
                if (show) { filterBox.requestFocus(); im.showSoftInput(filterBox, 0); }
                else { filterBox.setText(""); im.hideSoftInputFromWindow(filterBox.getWindowToken(), 0); }
                return true;
            }
            case M_SCAN:
                if (source != scanner) { source = scanner; model.entries.clear(); }
                if (!scanner.scan()) Toast.makeText(this, "Android refused the scan (4 scans in 2 minutes)", Toast.LENGTH_SHORT).show();
                refresh();
                return true;
            case M_GROUP: model.grouped = !model.grouped; settingsChanged(); view.invalidate(); return true;
            case M_TIME: view.timeView = !view.timeView; settingsChanged(); view.invalidate(); return true;
            case M_COPY: copy(model.allRowsText(), "Copied " + model.shown.size() + " rows"); return true;
            case M_DEMO: model.setDemo(!model.demo()); refresh(); invalidateOptionsMenu(); return true;
            case M_PLAYBACK:
                try {
                    source = Capture.read(captureFile());
                    model.entries.clear();
                    refresh();
                } catch (Exception e) {
                    Toast.makeText(this, "Can't read " + captureFile() + ": " + e.getMessage(), Toast.LENGTH_LONG).show();
                }
                return true;
            case M_ABOUT: about(); return true;
        }
        return super.onOptionsItemSelected(item);
    }

    private void about() {
        String version = "";
        try { version = getPackageManager().getPackageInfo(getPackageName(), 0).versionName; } catch (Exception ignored) {}
        new AlertDialog.Builder(this)
            .setTitle("WiFiScope " + version)
            .setMessage("Every Wi-Fi network this device hears: a table, and graphs of the 2.4, 5 and 6 GHz channels or of " +
                    "the signal over the last five minutes.\n\n" +
                    "Tap a row to pick it out on the graphs, press and hold it to copy it or see all its details; swipe the " +
                    "table sideways for more columns, tap a column's title to sort by it, drag the bar under the table to " +
                    "resize it. \"By router\" puts each router's radios together.\n\n" +
                    "The green band on a graph is the quietest channel for a router of your own (your own router left out).\n\n" +
                    "Vendors are from the IEEE registry, built in. Nothing leaves the device.\n\n" +
                    "github.com/ferrellsl/WiFiScope")
            .setPositiveButton("OK", null)
            .show();
    }

    // ------------------------------------------------------------- a row's actions
    @Override public void rowLongPressed(Model.Row row) {
        final Model.Group g = model.groups.get(row.group);
        final List<String> labels = new ArrayList<>();
        final List<Runnable> actions = new ArrayList<>();
        if (row.summary) {
            labels.add("Copy this router's radios");
            actions.add(() -> {
                StringBuilder s = new StringBuilder(Model.HEADER);
                for (long k : g.members) s.append(Model.rowText(model.entries.get(k))).append('\n');
                copy(s.toString(), "Copied " + g.members.size() + " radios");
            });
        } else {
            final Model.Entry e = model.entries.get(row.key);
            labels.add("Details"); actions.add(() -> details(e));
            labels.add("Copy BSSID"); actions.add(() -> copy(Bss.macText(e.bss.mac), "Copied " + Bss.macText(e.bss.mac)));
            labels.add("Copy this row"); actions.add(() -> copy(Model.HEADER + Model.rowText(e), "Copied the row"));
        }
        labels.add("Copy all rows"); actions.add(() -> copy(model.allRowsText(), "Copied " + model.shown.size() + " rows"));
        new AlertDialog.Builder(this)
            .setItems(labels.toArray(new String[0]), (d, which) -> actions.get(which).run())
            .show();
    }

    private void details(Model.Entry e) {
        final Bss b = e.bss;
        StringBuilder s = new StringBuilder();
        line(s, "BSSID", Bss.macText(b.mac) + (model.isConnected(b.mac) ? "  (connected)" : ""));
        line(s, "Vendor", e.vendor);
        line(s, "Signal", b.rssi + " dBm");
        line(s, "Channel", b.channel + "  (" + Model.bandName(b.band) + " GHz, " + b.frequency + " MHz)");
        line(s, "Width", b.split ? "80+80 MHz (" + b.center + " and " + b.center2 + " MHz)" : b.width + " MHz, centered on " + b.center + " MHz");
        line(s, "Wi-Fi", Model.wifiName(b) + (b.phy.isEmpty() ? "" : "  (802.11 " + b.phy + ")"));
        line(s, "Security", b.security);
        if (b.details) {
            line(s, "Streams", Integer.toString(b.streams));
            line(s, "Max rate", Model.rateText(b.maxRate) + " Mbps");
            line(s, "Min rate", Model.rateText(b.minRate) + " Mbps");
        }
        if (b.stations >= 0) line(s, "Clients", Integer.toString(b.stations));
        if (b.utilization >= 0) line(s, "Busy", b.utilization + "% of the time");
        if (b.multiLink) line(s, "Multi-link (MLO)", "MLD " + Bss.macText(b.mld));
        if (!b.country.isEmpty()) line(s, "Country", b.country);
        if (!b.details) s.append("\nAndroid passed on no beacon details for this network: width, security and generation are Android's own summary.");
        new AlertDialog.Builder(this)
            .setTitle(Model.nameOf(b))
            .setMessage(s.toString().trim())
            .setPositiveButton("OK", null)
            .show();
    }

    private static void line(StringBuilder s, String k, String v) { s.append(k).append(": ").append(v).append('\n'); }

    private void copy(String text, String toast) {
        ClipboardManager cm = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
        cm.setPrimaryClip(ClipData.newPlainText("WiFiScope", text));
        if (Build.VERSION.SDK_INT < 33) Toast.makeText(this, toast, Toast.LENGTH_SHORT).show();
    }
}
