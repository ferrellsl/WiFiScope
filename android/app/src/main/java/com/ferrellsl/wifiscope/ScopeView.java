package com.ferrellsl.wifiscope;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.os.SystemClock;
import android.view.GestureDetector;
import android.view.MotionEvent;
import android.view.View;
import android.widget.OverScroller;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

// The table and the graphs, drawn the way the Windows app draws them (main.cpp's
// drawTop, drawTable, drawPane, drawTime), for touch: the SSID column stays put
// while the others swipe sideways; on a narrow screen the graphs show one band
// at a time.
public final class ScopeView extends View {

    public interface Host {
        void rowLongPressed(Model.Row row);
        void settingsChanged();
        void bannerTapped();
    }

    private final Model model;
    private Host host;
    private final float u;                       // one dp in pixels
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint line = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint fontText = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint fontBold = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint fontSmall = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path path = new Path();
    private final GestureDetector gestures;
    private final OverScroller scroller;

    // the view's state (kept in the settings by MainActivity)
    public float split = 0.52f;                  // the table's share of the height
    public boolean timeView = false;             // signal over time instead of channels
    public int narrowBand = 5;                   // on a narrow screen: the band whose graph shows
    private String banner = "";

    // the layout, set by layout()
    private float W, H, top, bannerH, tableTop, tableBottom, rowH, headH, graphTop, frozen, plotTop, titleTop;
    private final RectF[] bandPill = { new RectF(), new RectF(), new RectF() };
    private final RectF groupPill = new RectF(), bannerBox = new RectF();
    private final List<RectF> tabs = new ArrayList<>();
    private final List<String> tabNames = new ArrayList<>();
    private boolean narrow;
    private float scrollY = 0, scrollX = 0;      // pixels
    private boolean dragging = false;
    private int gesture = 0;                     // 0 none, 1 table, 2 splitter

    private static final class Pane { int band; float x0, x1; double f0, f1; Pane(int b, double a, double z) { band = b; f0 = a; f1 = z; } }
    private final List<Pane> panes = new ArrayList<>();

    public ScopeView(Context context, Model model) {
        super(context);
        this.model = model;
        u = getResources().getDisplayMetrics().density;
        final float sp = getResources().getDisplayMetrics().scaledDensity;
        fontText.setTextSize(13.5f * sp);
        fontBold.setTextSize(13.5f * sp);
        fontBold.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
        fontSmall.setTextSize(11.5f * sp);
        line.setStyle(Paint.Style.STROKE);
        line.setStrokeJoin(Paint.Join.ROUND);
        scroller = new OverScroller(context);
        gestures = new GestureDetector(context, new Gestures());
        setBackgroundColor(0xFF0B1015);
    }

    public void setHost(Host h) { host = h; }

    public void setBanner(String s) {
        if (s == null) s = "";
        if (!s.equals(banner)) { banner = s; invalidate(); }
    }

    // ------------------------------------------------------------- drawing helpers
    private static int argb(int rgb, float a) { return (Math.round(Math.max(0, Math.min(1, a)) * 255) << 24) | (rgb & 0xFFFFFF); }

    private void rect(Canvas c, float l, float t, float r, float b, int rgb, float a) {
        fill.setColor(argb(rgb, a));
        c.drawRect(l, t, r, b, fill);
    }

    // text in a box: vertically centered, left, right or centered, cut off with "…" when too wide
    private void text(Canvas c, String s, Paint p, float l, float t, float r, float b, int rgb, float a, int align) {
        if (s == null || s.isEmpty() || r - l <= 0) return;
        String shown = s;
        final float room = r - l;
        if (p.measureText(shown) > room) {
            int n = p.breakText(s, true, Math.max(0, room - p.measureText("…")), null);
            shown = n <= 0 ? "" : s.substring(0, n) + "…";
        }
        final float w = p.measureText(shown);
        final float x = align == 0 ? l : align == 1 ? r - w : (l + r - w) / 2;
        final Paint.FontMetrics m = p.getFontMetrics();
        final float y = (t + b) / 2 - (m.ascent + m.descent) / 2;
        p.setColor(argb(rgb, a));
        c.drawText(shown, x, y, p);
    }

    private void pill(Canvas c, RectF r, String label, boolean on) {
        fill.setColor(argb(on ? 0x2E86DE : 0x2A3540, 1));
        c.drawRoundRect(r, r.height() / 2, r.height() / 2, fill);
        text(c, label, fontSmall, r.left, r.top, r.right, r.bottom, on ? 0xFFFFFF : 0x8FA3B8, 1, 2);
    }

    private static boolean inside(RectF r, float x, float y) { return r.contains(x, y); }

    // ------------------------------------------------------------- layout
    private void layout() {
        W = getWidth(); H = getHeight();
        narrow = W < 720 * u;
        final float pillH = 30 * u;
        // the status line, and the switches: on one line when wide, the switches below when narrow
        float x = W - 10 * u;
        final float py = narrow ? 34 * u : 9 * u;
        for (int i = 2; i >= 0; --i) {
            final float w = 70 * u;
            bandPill[i].set(x - w, py, x, py + pillH);
            x -= w + 6 * u;
        }
        groupPill.set(x - 92 * u, py, x, py + pillH);
        top = py + pillH + 8 * u;
        bannerH = banner.isEmpty() ? 0 : 40 * u;
        bannerBox.set(0, top, W, top + bannerH);
        headH = 32 * u;
        rowH = 34 * u;
        tableTop = top + bannerH;
        final float rest = H - tableTop;
        final int fewest = rest < 420 * u ? 2 : 3;          // (a short screen: the graphs need the room)
        tableBottom = tableTop + Math.max(headH + rowH * fewest, Math.min(rest - (narrow ? 190 : 150) * u, rest * split));
        graphTop = tableBottom + 10 * u;
        // where the plots start: below the tabs and the band's line on a narrow screen;
        // beside the tabs (on the titles' line) otherwise, as on Windows
        plotTop = narrow ? graphTop + 56 * u : graphTop + 32 * u;
        titleTop = narrow ? graphTop + 30 * u : graphTop;
        frozen = 22 * u + Model.Column.SSID.width * u;

        // the graph tabs
        tabs.clear(); tabNames.clear();
        if (narrow) {
            tabNames.add("2.4 GHz"); tabNames.add("5 GHz");
            if (model.hasSixGhz) tabNames.add("6 GHz");
            tabNames.add("Over time");
        } else { tabNames.add("Channels"); tabNames.add("Signal over time"); }
        float tx = W - 10 * u;
        for (int i = tabNames.size() - 1; i >= 0; --i) {
            final float w = fontSmall.measureText(tabNames.get(i)) + 24 * u;
            tabs.add(0, new RectF(tx - w, graphTop, tx, graphTop + 26 * u));
            tx -= w + 5 * u;
        }

        panes.clear();
        List<Pane> want = new ArrayList<>();
        if (narrow) {
            int b = narrowBand == 6 && !model.hasSixGhz ? 5 : narrowBand;
            want.add(b == 2 ? new Pane(2, 2401, 2495) : b == 5 ? new Pane(5, 5150, 5895) : new Pane(6, 5925, 7125));
        } else {
            if (model.showBand[0]) want.add(new Pane(2, 2401, 2495));
            if (model.showBand[1]) want.add(new Pane(5, 5150, 5895));
            if (model.showBand[2] && model.hasSixGhz) want.add(new Pane(6, 5925, 7125));
        }
        final double[] weight = { 1.0, 1.7, 1.5 };
        double total = 0;
        for (Pane p : want) total += weight[Model.bandIndex(p.band)];
        float px = 10 * u;
        final float avail = W - 20 * u - (want.isEmpty() ? 0 : (want.size() - 1) * 10 * u);
        for (Pane p : want) {
            final float w = (float) (avail * weight[Model.bandIndex(p.band)] / total);
            p.x0 = px; p.x1 = px + w;
            px += w + 10 * u;
            panes.add(p);
        }
    }

    private float tableWidth() {
        float w = 0;
        for (Model.Column c : Model.Column.values()) if (c != Model.Column.SSID) w += c.width * u;
        return w + 10 * u;
    }

    private float maxScrollX() { return Math.max(0, tableWidth() - (W - frozen)); }
    private float maxScrollY() { return Math.max(0, model.rows.size() * rowH - (tableBottom - tableTop - headH)); }

    // a column's left edge on screen (the SSID column doesn't scroll)
    private float columnX(Model.Column col) {
        if (col == Model.Column.SSID) return 22 * u;
        float x = frozen - scrollX;
        for (Model.Column c : Model.Column.values()) {
            if (c == Model.Column.SSID) continue;
            if (c == col) return x;
            x += c.width * u;
        }
        return x;
    }

    // ------------------------------------------------------------- drawing
    @Override
    protected void onDraw(Canvas c) {
        if (scroller.computeScrollOffset()) {
            scrollY = scroller.getCurrY();
            scrollX = scroller.getCurrX();
            postInvalidateOnAnimation();
        }
        model.sortRows();
        layout();
        scrollY = Math.max(0, Math.min(scrollY, maxScrollY()));
        scrollX = Math.max(0, Math.min(scrollX, maxScrollX()));
        c.drawColor(0xFF0B1015);
        drawTop(c);
        drawTable(c);
        if (timeView) drawTime(c);
        else {
            for (Pane p : panes) drawPane(c, p);
            if (panes.isEmpty())
                text(c, "Choose a band at the top right.", fontText, 0, graphTop + 40 * u, W, graphTop + 70 * u, 0x8FA3B8, 1, 2);
        }
        for (int i = 0; i < tabs.size(); ++i) pill(c, tabs.get(i), tabNames.get(i), tabOn(i));
    }

    private boolean tabOn(int i) {
        final String n = tabNames.get(i);
        if (!narrow) return (i == 1) == timeView;
        if (n.equals("Over time")) return timeView;
        return !timeView && n.startsWith(Model.bandName(narrowBand == 6 && !model.hasSixGhz ? 5 : narrowBand) + " ");
    }

    private void drawTop(Canvas c) {
        rect(c, 0, 0, W, top, 0x141A21, 1);
        final long now = SystemClock.elapsedRealtime();
        String status = model.networksHeard() + " networks";
        if (model.grouped) status += ", " + model.routersHeard() + " routers";
        // (Android scans seldom when throttled: say how old the newest results are)
        final long age = model.lastHeard > 0 ? (now - Math.min(now, model.lastHeard)) / 1000 : 0;
        if (age >= 15) status += ", last scan " + (age < 120 ? age + " s" : age / 60 + " min") + " ago";
        if (!model.adapter.isEmpty()) status += "   •   " + model.adapter;
        if (model.demo()) status += "   •   demo: names and addresses disguised";
        if (narrow) text(c, status, fontSmall, 12 * u, 6 * u, W - 12 * u, 28 * u, 0x9FB0C2, 1, 0);
        else text(c, status, fontSmall, 12 * u, groupPill.top, groupPill.left - 12 * u, groupPill.bottom, 0x9FB0C2, 1, 0);
        pill(c, groupPill, "By router", model.grouped);
        final String[] names = { "2.4 GHz", "5 GHz", "6 GHz" };
        for (int i = 0; i < 3; ++i) pill(c, bandPill[i], names[i], model.showBand[i]);
        if (bannerH > 0) {
            rect(c, 0, bannerBox.top, W, bannerBox.bottom, 0x3A2F12, 1);
            text(c, banner, fontSmall, 12 * u, bannerBox.top, W - 12 * u, bannerBox.bottom, 0xFFD98A, 1, 0);
        }
    }

    private void drawTable(Canvas c) {
        final long now = SystemClock.elapsedRealtime();
        rect(c, 0, tableTop, W, tableBottom, 0x0E1318, 1);
        rect(c, 0, tableTop, W, tableTop + headH, 0x1C2630, 1);
        final byte[] ourMac = model.connectedMac();

        c.save();
        c.clipRect(0, tableTop, W, tableBottom);
        // the header: the scrolling columns, then the fixed SSID column over them
        c.save();
        c.clipRect(frozen, tableTop, W, tableTop + headH);
        for (Model.Column col : Model.Column.values()) if (col != Model.Column.SSID) header(c, col);
        c.restore();
        header(c, Model.Column.SSID);

        final int first = (int) (scrollY / rowH);
        final float bodyTop = tableTop + headH;
        c.clipRect(0, bodyTop, W, tableBottom);
        for (int i = first; i < model.rows.size(); ++i) {
            final float y = bodyTop + i * rowH - scrollY;
            if (y > tableBottom) break;
            final Model.Row row = model.rows.get(i);
            final Model.Group g = model.groups.get(row.group);
            final Model.Entry e = model.entryOf(row);
            final Bss b = e.bss;
            final boolean child = !row.summary && g.members.size() > 1;
            final boolean faded = model.faded(e);
            boolean mine = ourMac != null && java.util.Arrays.equals(ourMac, b.mac);
            if (row.summary) for (long k : g.members) mine = mine || (ourMac != null && java.util.Arrays.equals(ourMac, model.entries.get(k).bss.mac));
            final boolean picked = row.summary ? model.selectedGroup == g.id
                    : (row.key == model.selected || (model.selectedGroup != 0 && model.selectedGroup == g.id));
            if (picked) { rect(c, 0, y, W, y + rowH, 0x0E1318, 1); rect(c, 0, y, W, y + rowH, 0x2E86DE, row.summary || row.key == model.selected ? 0.35f : 0.18f); }
            else if (row.summary) rect(c, 0, y, W, y + rowH, 0xFFFFFF, 0.05f);
            else if (i % 2 == 1) rect(c, 0, y, W, y + rowH, 0xFFFFFF, 0.025f);
            final float alpha = faded ? 0.45f : 1;
            final int fg = 0xE8EEF4, dim = 0x9FB0C2;

            // the scrolling columns
            c.save();
            c.clipRect(frozen, y, W, y + rowH);
            if (row.summary) {
                cell(c, Model.Column.BSSID, y, g.members.size() + " radios", dim, alpha, null);
                cell(c, Model.Column.CHANNEL, y, g.channels, fg, alpha, null);
                cell(c, Model.Column.BAND, y, g.bands, dim, alpha, null);
            } else {
                cell(c, Model.Column.BSSID, y, Bss.macText(b.mac) + (b.multiLink ? " ≡" : ""), dim, alpha, null);
                cell(c, Model.Column.CHANNEL, y, Integer.toString(b.channel), fg, alpha, null);
                cell(c, Model.Column.BAND, y, Model.bandName(b.band), dim, alpha, null);
            }
            cell(c, Model.Column.VENDOR, y, e.vendor, Oui.local(b.mac) ? 0x8FA3B8 : 0xC8D6E5, alpha, null);
            {   // signal: a bar from -95 to -35 dBm, and the number
                final float x = columnX(Model.Column.SIGNAL) + 4 * u, bw = 54 * u;
                final float t = Math.max(0, Math.min(1, (b.rssi + 95) / 60.0f));
                rect(c, x, y + 12 * u, x + bw, y + rowH - 12 * u, 0x2A3540, alpha);
                final int barColor = b.rssi >= -60 ? 0x2ECC71 : b.rssi >= -70 ? 0xF1C40F : b.rssi >= -80 ? 0xE67E22 : 0xE74C3C;
                rect(c, x, y + 12 * u, x + bw * t, y + rowH - 12 * u, barColor, alpha);
                text(c, Integer.toString(b.rssi), fontText, x + bw + 6 * u, y, x + Model.Column.SIGNAL.width * u, y + rowH, fg, alpha, 0);
            }
            cell(c, Model.Column.WIDTH, y, b.split ? "80+80" : Integer.toString(b.width), fg, alpha, null);
            cell(c, Model.Column.WIFI, y, Model.wifiName(b), b.generation >= 7 ? 0xFFD166 : fg, alpha, null);
            cell(c, Model.Column.SECURITY, y, b.security, b.security.equals("Open") || b.security.equals("WEP") ? 0xFFB4A2 : fg, alpha, null);
            cell(c, Model.Column.MAX, y, Model.rateText(b.maxRate), fg, alpha, null);
            cell(c, Model.Column.MIN, y, Model.rateText(b.minRate), dim, alpha, null);
            cell(c, Model.Column.CLIENTS, y, b.stations >= 0 ? Integer.toString(b.stations) : "", fg, alpha, null);
            cell(c, Model.Column.UTIL, y, b.utilization >= 0 ? b.utilization + "%" : "",
                    b.utilization >= 70 ? 0xFF7B6B : b.utilization >= 40 ? 0xF1C40F : fg, alpha, null);
            final long seen = (now - Math.min(now, e.heard)) / 1000;
            cell(c, Model.Column.SEEN, y, seen < 2 ? "now" : seen < 120 ? seen + " s" : (seen / 60) + " m", dim, alpha, null);
            c.restore();

            // the fixed column: the color mark and the name
            c.save();
            c.clipRect(0, y, frozen, y + rowH);
            if (scrollX > 0) rect(c, frozen - 1 * u, y, frozen, y + rowH, 0x2A3540, 1);
            rect(c, (child ? 12 : 6) * u, y + 7 * u, (child ? 16 : 14) * u, y + rowH - 7 * u, e.color, faded ? 0.35f : 1);
            if (row.summary) {
                final boolean open = model.opened.contains(g.id);
                cell(c, Model.Column.SSID, y, (open ? "▾ " : "▸ ") + (mine ? "● " : "") + g.names, fg, alpha, fontBold);
            } else {
                String name = Model.nameOf(b);
                if (mine) name = "● " + name;
                cellAt(c, Model.Column.SSID, y, name, b.ssid.isEmpty() ? dim : fg, alpha, mine ? fontBold : null, child ? 16 * u : 0);
            }
            c.restore();
        }
        c.restore();

        // the scroll bars when there are more rows or columns than fit
        final float h = tableBottom - bodyTop;
        final float content = model.rows.size() * rowH;
        if (content > h) {
            final float th = Math.max(20 * u, h * h / content), ty = bodyTop + (h - th) * scrollY / Math.max(1, maxScrollY());
            fill.setColor(argb(0x5C7080, 0.7f));
            c.drawRoundRect(new RectF(W - 6 * u, ty, W - 2 * u, ty + th), 2 * u, 2 * u, fill);
        }
        if (maxScrollX() > 0) {
            final float vw = W - frozen, total = tableWidth();
            final float tw = Math.max(30 * u, vw * vw / total), tx = frozen + (vw - tw) * scrollX / maxScrollX();
            fill.setColor(argb(0x5C7080, 0.5f));
            c.drawRoundRect(new RectF(tx, tableTop + headH - 4 * u, tx + tw, tableTop + headH - 1 * u), 2 * u, 2 * u, fill);
        }
        // the splitter, with a grip
        rect(c, 0, tableBottom, W, tableBottom + 3 * u, 0x2A3540, 1);
        fill.setColor(argb(0x5C7080, 1));
        c.drawRoundRect(new RectF(W / 2 - 22 * u, tableBottom - 1 * u, W / 2 + 22 * u, tableBottom + 4 * u), 2 * u, 2 * u, fill);
    }

    private void header(Canvas c, Model.Column col) {
        final float x = columnX(col), w = col.width * u;
        if (col == Model.Column.SSID) rect(c, 0, tableTop, frozen, tableTop + headH, 0x1C2630, 1);
        String name = col.title;
        if (col == model.sortColumn) name += model.sortDown ? " ▾" : " ▴";
        text(c, name, fontBold, x + 4 * u, tableTop, x + w - 6 * u, tableTop + headH,
                col == model.sortColumn ? 0xFFFFFF : 0xB8C6D6, 1, col.right ? 1 : 0);
    }

    private void cell(Canvas c, Model.Column col, float y, String s, int rgb, float alpha, Paint f) { cellAt(c, col, y, s, rgb, alpha, f, 0); }

    private void cellAt(Canvas c, Model.Column col, float y, String s, int rgb, float alpha, Paint f, float indent) {
        final float x = columnX(col) + indent, w = col.width * u - indent;
        text(c, s, f != null ? f : fontText, x + 4 * u, y, x + w - 6 * u, y + rowH, rgb, alpha, col.right ? 1 : 0);
    }

    // The names on a graph: the selected and strongest first, just above their point;
    // one that would overlap another moves up or down a line, with a thin line to its
    // point.  Names with no room left are skipped (the table has them).
    private static final class Label { long key; float x, y, alpha; boolean right; Label(long k, float x, float y, float a, boolean r) { key = k; this.x = x; this.y = y; alpha = a; right = r; } }

    private void placeLabels(Canvas c, List<Label> names, float left, float right, float top, float bottom) {
        names.sort((a, b) -> {
            final boolean ha = model.highlighted(a.key), hb = model.highlighted(b.key);
            if (ha != hb) return ha ? -1 : 1;
            return Float.compare(a.y, b.y);
        });
        List<RectF> placed = new ArrayList<>();
        final float lh = 17 * u;
        for (Label l : names) {
            final Model.Entry e = model.entries.get(l.key);
            final String name = Model.nameOf(e.bss);
            final float tw = fontSmall.measureText(name) + 8 * u;
            final float x = l.right ? Math.min(right - tw - 1, l.x + 4 * u) : Math.max(left + 1, Math.min(right - tw - 1, l.x - tw / 2));
            RectF box = null;
            for (int step : new int[] { 0, -1, -2, -3, 1, 2, 3, 4, -4, 5, 6 }) {
                final float y = (l.right ? l.y - lh / 2 : l.y - 19 * u) + step * (lh + 1 * u);
                if (y < top || y + lh > bottom) continue;
                RectF r = new RectF(x, y, x + tw, y + lh);
                boolean clash = false;
                for (RectF q : placed) if (RectF.intersects(r, q)) { clash = true; break; }
                if (!clash) { box = r; break; }
            }
            if (box == null) continue;
            placed.add(box);
            final float home = l.right ? l.y - lh / 2 : l.y - 19 * u;
            if (Math.abs(box.top - home) > 1 * u) {
                line.setColor(argb(e.color, 0.5f * l.alpha));
                line.setStrokeWidth(1 * u);
                if (l.right) c.drawLine(l.x, l.y, box.left, box.centerY(), line);
                else c.drawLine(l.x, box.bottom < l.y ? box.bottom : box.top, l.x, l.y, line);
            }
            rect(c, box.left, box.top, box.right, box.bottom, 0x0E1318, 0.8f * l.alpha);
            text(c, name, fontSmall, box.left, box.top, box.right, box.bottom, e.color, l.alpha, 2);
        }
    }

    private static final double DB_TOP = -20, DB_BOTTOM = -100;

    private float dbY(double db, float y0, float y1) {
        db = Math.max(DB_BOTTOM, Math.min(DB_TOP, db));
        return (float) (y0 + (DB_TOP - db) / (DB_TOP - DB_BOTTOM) * (y1 - y0));
    }

    private void drawDbGrid(Canvas c, float x0, float ax, float x1, float y0, float y1) {
        rect(c, ax, y0, x1, y1, 0x0E1318, 1);
        line.setStrokeWidth(1);
        final boolean tight = y1 - y0 < 130 * u;           // (a short graph: every other label)
        for (int db = -30; db >= -90; db -= 10) {
            line.setColor(argb(0xFFFFFF, 0.07f));
            final float y = dbY(db, y0, y1);
            c.drawLine(ax, y, x1, y, line);
            if (!tight || db % 20 == 0) text(c, Integer.toString(db), fontSmall, x0, y - 9 * u, ax - 4 * u, y + 9 * u, 0x8FA3B8, 1, 1);
        }
    }

    private void drawPane(Canvas c, Pane p) {
        final long now = SystemClock.elapsedRealtime();
        final float x0 = p.x0, x1 = p.x1;
        final float y0 = plotTop, y1 = H - 28 * u;
        final float ax = x0 + 34 * u;
        if (y1 - y0 < 30 * u) return;

        // the title, and the quietest channel for a router of yours (left of the tabs)
        final String title = p.band == 2 ? "2.4 GHz" : p.band == 5 ? "5 GHz" : "6 GHz";
        final float tb = titleTop + 26 * u;
        final float limit = !narrow && !tabs.isEmpty() && x1 >= tabs.get(0).left - 4 * u ? tabs.get(0).left - 8 * u : x1;
        float tx = x0;
        if (!narrow) { text(c, title, fontBold, x0, titleTop, limit, tb, 0xB8C6D6, 1, 0); tx = x0 + fontBold.measureText(title) + 12 * u; }
        final List<Advice.Choice> best = model.suggestions(p.band);
        if (!best.isEmpty()) {
            final Advice.Choice ch = best.get(0);
            String hint = ch.first == ch.last ? "quietest: " + ch.first
                    : String.format(Locale.US, "quietest: %d–%d (%d MHz)", ch.first, ch.last, ch.width);
            if (p.band == 5 && ch.dfs)
                for (Advice.Choice o : best) if (!o.dfs) { hint += String.format(Locale.US, ", radar rules; or %d–%d", o.first, o.last); break; }
            text(c, hint, fontSmall, tx, titleTop, limit, tb, 0x7FD88F, 1, 0);
            drawDbGrid(c, x0, ax, x1, y0, y1);
            final float l = fx(p, ch.center - ch.width / 2.0, ax, x1), r = fx(p, ch.center + ch.width / 2.0, ax, x1);
            rect(c, Math.max(ax, l), y0, Math.min(x1, r), y1, 0x2ECC71, 0.07f);
        } else drawDbGrid(c, x0, ax, x1, y0, y1);

        List<Integer> labels = new ArrayList<>();
        if (p.band == 2) for (int ch = 1; ch <= 14; ++ch) labels.add(ch);
        else if (p.band == 5) for (int ch : narrow ? new int[] { 36, 52, 100, 116, 132, 149, 165 } : new int[] { 36, 44, 52, 60, 100, 108, 116, 124, 132, 140, 149, 157, 165, 173 }) labels.add(ch);
        else for (int ch = 1; ch <= 233; ch += narrow ? 32 : 32) labels.add(ch);
        line.setStrokeWidth(1);
        for (int ch : labels) {
            final float x = fx(p, Decoder.frequencyOf(p.band, ch), ax, x1);
            line.setColor(argb(0xFFFFFF, 0.05f));
            c.drawLine(x, y0, x, y1, line);
            text(c, Integer.toString(ch), fontSmall, x - 20 * u, y1 + 3 * u, x + 20 * u, y1 + 24 * u, 0x8FA3B8, 1, 2);
        }

        // the networks: one shape across each one's channel, as high as its signal
        List<Long> order = new ArrayList<>();
        for (long k : model.shown) if (model.entries.get(k).bss.band == p.band) order.add(k);
        order.sort((a, b) -> {
            final boolean ha = model.highlighted(a), hb = model.highlighted(b);
            if (ha != hb) return hb ? -1 : 1;
            return Integer.compare(model.entries.get(a).bss.rssi, model.entries.get(b).bss.rssi);
        });
        List<Label> names = new ArrayList<>();
        c.save();
        c.clipRect(ax, y0, x1, y1);
        for (long k : order) {
            final Model.Entry e = model.entries.get(k);
            final Bss b = e.bss;
            final boolean faded = model.faded(e);
            final boolean lit = model.highlighted(k), dimmed = model.anySelection() && !lit;
            final float alpha = faded ? 0.3f : dimmed ? 0.35f : 1;
            final int parts = b.split ? 2 : 1;
            for (int part = 0; part < parts; ++part) {
                final int center = part == 0 ? b.center : b.center2, width = b.split ? 80 : b.width;
                final double half = width / 2.0, slope = Math.min(width * 0.12, 4.0);
                final float l = fx(p, center - half, ax, x1), r = fx(p, center + half, ax, x1);
                final float lt = fx(p, center - half + slope, ax, x1), rt = fx(p, center + half - slope, ax, x1);
                final float yt = dbY(b.rssi, y0, y1), yb = y1;
                path.reset();
                path.moveTo(l, yb); path.lineTo(lt, yt); path.lineTo(rt, yt); path.lineTo(r, yb);
                fill.setColor(argb(e.color, 0.10f * alpha + (lit ? 0.12f : 0)));
                c.drawPath(path, fill);
                line.setColor(argb(e.color, alpha));
                line.setStrokeWidth((lit ? 2.5f : 1.3f) * u);
                c.drawPath(path, line);
                if (!dimmed) names.add(new Label(k, (lt + rt) / 2, yt, alpha, false));
            }
        }
        placeLabels(c, names, ax, x1, y0, y1);
        c.restore();
        line.setColor(argb(0x2A3540, 1));
        line.setStrokeWidth(1);
        c.drawRect(ax, y0, x1, y1, line);
    }

    private static float fx(Pane p, double mhz, float ax, float x1) { return (float) (ax + (mhz - p.f0) / (p.f1 - p.f0) * (x1 - ax)); }

    // signal over the last five minutes: a line for each network, broken where it wasn't heard
    private void drawTime(Canvas c) {
        final long now = SystemClock.elapsedRealtime();
        final float x0 = 10 * u, x1 = W - 10 * u - (narrow ? 96 : 150) * u;
        final float y0 = plotTop, y1 = H - 28 * u;
        final float ax = x0 + 34 * u;
        if (y1 - y0 < 30 * u) return;
        final double span = Model.HISTORY_SECONDS * 1000.0;

        text(c, "Signal over the last 5 minutes", fontBold, x0, titleTop, narrow || tabs.isEmpty() ? W : tabs.get(0).left - 8 * u,
             titleTop + 26 * u, 0xB8C6D6, 1, 0);
        drawDbGrid(c, x0, ax, x1, y0, y1);
        line.setStrokeWidth(1);
        for (int s = 0; s <= Model.HISTORY_SECONDS; s += narrow ? 60 : 30) {
            final float x = (float) (ax + (1 - s * 1000.0 / span) * (x1 - ax));
            line.setColor(argb(0xFFFFFF, 0.05f));
            c.drawLine(x, y0, x, y1, line);
            final String label = s == 0 ? "now" : s % 60 == 0 ? (s / 60) + " min" : String.format(Locale.US, "%d:%02d", s / 60, s % 60);
            text(c, label, fontSmall, x - 30 * u, y1 + 3 * u, x + 30 * u, y1 + 24 * u, 0x8FA3B8, 1, 2);
        }

        List<Long> order = new ArrayList<>(model.shown);
        order.sort((a, b) -> {
            final boolean ha = model.highlighted(a), hb = model.highlighted(b);
            if (ha != hb) return hb ? -1 : 1;
            return Integer.compare(model.entries.get(a).bss.rssi, model.entries.get(b).bss.rssi);
        });
        List<Label> names = new ArrayList<>();
        c.save();
        c.clipRect(ax, y0, W - 10 * u, y1);
        for (long k : order) {
            final Model.Entry e = model.entries.get(k);
            if (e.history.isEmpty()) continue;
            final boolean lit = model.highlighted(k), dimmed = model.anySelection() && !lit;
            final boolean faded = model.faded(e);
            final float alpha = dimmed ? 0.22f : faded ? 0.4f : 0.9f;
            final float width = (lit ? 2.6f : 1.4f) * u;
            line.setColor(argb(e.color, alpha));
            line.setStrokeWidth(width);
            fill.setColor(argb(e.color, alpha));
            long[] prev = null;
            for (long[] pt : e.history) {
                final float x = tx(pt[0], now, span, ax, x1), y = dbY(pt[1], y0, y1);
                if (prev != null && pt[0] - prev[0] <= Model.GAP_MS) c.drawLine(tx(prev[0], now, span, ax, x1), dbY(prev[1], y0, y1), x, y, line);
                else c.drawCircle(x, y, width, fill);
                prev = pt;
            }
            if (!faded && prev != null) {
                final float y = dbY(prev[1], y0, y1);
                c.drawLine(tx(prev[0], now, span, ax, x1), y, x1, y, line);
            }
            if (!dimmed && !faded && prev != null) names.add(new Label(k, x1, dbY(prev[1], y0, y1), alpha, true));
        }
        placeLabels(c, names, ax, W - 10 * u, y0, y1);
        c.restore();
        line.setColor(argb(0x2A3540, 1));
        line.setStrokeWidth(1);
        c.drawRect(ax, y0, x1, y1, line);
    }

    private static float tx(long t, long now, double span, float ax, float x1) {
        return (float) (ax + (1 - (double) (now - Math.min(now, t)) / span) * (x1 - ax));
    }

    // ------------------------------------------------------------- touch
    private int rowAt(float y) {
        if (y < tableTop + headH || y >= tableBottom) return -1;
        final int i = (int) ((y - tableTop - headH + scrollY) / rowH);
        return i < model.rows.size() ? i : -1;
    }

    private boolean onSplitter(float y) { return y >= tableBottom - 14 * u && y <= tableBottom + 14 * u; }

    @Override
    public boolean onTouchEvent(MotionEvent ev) {
        if (ev.getActionMasked() == MotionEvent.ACTION_DOWN) {
            final float y = ev.getY();
            gesture = onSplitter(y) ? 2 : (y >= tableTop && y < tableBottom) ? 1 : 0;
            if (gesture == 2) dragging = true;
        }
        if (dragging && ev.getActionMasked() == MotionEvent.ACTION_MOVE) {
            final float rest = Math.max(1, H - tableTop);
            split = Math.max(0.15f, Math.min(0.85f, (ev.getY() - tableTop) / rest));
            invalidate();
        }
        if (dragging && (ev.getActionMasked() == MotionEvent.ACTION_UP || ev.getActionMasked() == MotionEvent.ACTION_CANCEL)) {
            dragging = false;
            if (host != null) host.settingsChanged();
            return true;
        }
        if (dragging) return true;
        gestures.onTouchEvent(ev);
        return true;
    }

    private final class Gestures extends GestureDetector.SimpleOnGestureListener {
        private int axis = 0;                    // a drag's direction, once clear: 1 sideways, 2 up and down

        @Override public boolean onDown(MotionEvent e) {
            scroller.forceFinished(true);
            axis = 0;
            return true;
        }

        @Override public boolean onScroll(MotionEvent e1, MotionEvent e2, float dx, float dy) {
            if (gesture != 1) return false;
            if (axis == 0) axis = Math.abs(dx) > Math.abs(dy) ? 1 : 2;
            if (axis == 1) scrollX = Math.max(0, Math.min(maxScrollX(), scrollX + dx));
            else scrollY = Math.max(0, Math.min(maxScrollY(), scrollY + dy));
            invalidate();
            return true;
        }

        @Override public boolean onFling(MotionEvent e1, MotionEvent e2, float vx, float vy) {
            if (gesture != 1) return false;
            if (axis == 1) vy = 0; else vx = 0;
            scroller.fling((int) scrollX, (int) scrollY, (int) -vx, (int) -vy, 0, (int) maxScrollX(), 0, (int) maxScrollY());
            postInvalidateOnAnimation();
            return true;
        }

        @Override public boolean onSingleTapUp(MotionEvent ev) {
            final float x = ev.getX(), y = ev.getY();
            for (int i = 0; i < 3; ++i)
                if (inside(bandPill[i], x, y)) { model.showBand[i] = !model.showBand[i]; changed(); return true; }
            if (inside(groupPill, x, y)) { model.grouped = !model.grouped; scrollY = 0; changed(); return true; }
            if (bannerH > 0 && inside(bannerBox, x, y)) { if (host != null) host.bannerTapped(); return true; }
            for (int i = 0; i < tabs.size(); ++i)
                if (inside(tabs.get(i), x, y)) {
                    final String n = tabNames.get(i);
                    if (!narrow) timeView = i == 1;
                    else if (n.equals("Over time")) timeView = true;
                    else { timeView = false; narrowBand = n.startsWith("2.4") ? 2 : n.startsWith("5") ? 5 : 6; }
                    changed();
                    return true;
                }
            if (y >= tableTop && y < tableTop + headH) {
                Model.Column hit = null;
                if (x < frozen) hit = Model.Column.SSID;
                else for (Model.Column c : Model.Column.values())
                    if (c != Model.Column.SSID && x >= columnX(c) && x < columnX(c) + c.width * u) hit = c;
                if (hit != null) { model.setSort(hit); changed(); }
                return true;
            }
            final int r = rowAt(y);
            if (r >= 0) {
                final Model.Row row = model.rows.get(r);
                if (row.summary) {
                    // a router's row: open or close it (and pick it out on the graphs)
                    final long id = model.groups.get(row.group).id;
                    if (!model.opened.remove(id)) model.opened.add(id);
                    model.selectedGroup = id;
                    model.selected = 0;
                } else if (row.key == model.selected) model.selected = 0;
                else { model.selected = row.key; model.selectedGroup = 0; }
            } else if (y > tableBottom) { model.selected = 0; model.selectedGroup = 0; }
            invalidate();
            return true;
        }

        @Override public void onLongPress(MotionEvent ev) {
            final int r = rowAt(ev.getY());
            if (r < 0 || host == null) return;
            final Model.Row row = model.rows.get(r);
            if (row.summary) { model.selectedGroup = model.groups.get(row.group).id; model.selected = 0; }
            else { model.selected = row.key; model.selectedGroup = 0; }
            invalidate();
            performHapticFeedback(android.view.HapticFeedbackConstants.LONG_PRESS);
            host.rowLongPressed(row);
        }
    }

    private void changed() {
        invalidate();
        if (host != null) host.settingsChanged();
    }
}
