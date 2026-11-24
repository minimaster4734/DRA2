package org.megarun;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Bundle;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.RandomAccessFile;

/** Asks for the user's own WiiWare .wad and their own Wii common key file, then starts the game. Nothing proprietary is bundled. */
public class LauncherActivity extends Activity {
    private static final int REQ_WAD = 1, REQ_KEY = 2;
    private SharedPreferences prefs;

    @Override protected void onResume() { super.onResume(); build(); }

    private String readLog() {
        try (RandomAccessFile f = new RandomAccessFile(new File(getFilesDir(), "last.log"), "r")) {
            long n = Math.min(f.length(), 4000); byte[] buf = new byte[(int) n]; f.seek(f.length() - n); f.readFully(buf); return new String(buf);
        } catch (Exception e) { return null; }
    }

    private boolean haveKey() { File k = new File(getFilesDir(), "wii.key"); return k.exists() && k.length() >= 16; }

    private Button cycle(LinearLayout l, String title, String[] names, String key) {
        final int idx = Math.max(0, Math.min(names.length - 1, prefs.getInt(key, 0)));
        Button b = new Button(this); b.setText(title + ": " + names[idx] + " (tap to change)");
        b.setOnClickListener(v -> { prefs.edit().putInt(key, (idx + 1) % names.length).apply(); build(); });
        l.addView(b); return b;
    }

    private void build() {
        prefs = getSharedPreferences("mm", MODE_PRIVATE);
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL); l.setGravity(Gravity.CENTER); l.setPadding(48, 48, 48, 48);
        TextView t = new TextView(this);
        t.setText("Blue Bomber Runtime\n\nThis app does not contain any game or Nintendo key.\nChoose your own legally owned WiiWare .wad\n(Mega Man 9 / Mega Man 10) and a file with your own\nWii common key (16 raw bytes, or 32 hex digits).");
        t.setGravity(Gravity.CENTER); t.setTextSize(16);
        l.addView(t);
        Button kb = new Button(this); kb.setText(haveKey() ? "Common key: loaded (tap to replace)" : "Choose common key file");
        kb.setOnClickListener(v -> pick(REQ_KEY)); l.addView(kb);
        Button pick = new Button(this); pick.setText("Choose WAD and play"); pick.setOnClickListener(v -> pick(REQ_WAD)); l.addView(pick);
        String saved = prefs.getString("wad", null);
        if (saved != null) { Button play = new Button(this); play.setText("Play (last WAD)"); play.setOnClickListener(v -> start(Uri.parse(saved))); l.addView(play); }
        cycle(l, "Screen", new String[] {"Landscape", "Portrait", "Auto-rotate"}, "orient");
        final boolean fps = prefs.getBoolean("fps", true);
        Button fb = new Button(this); fb.setText("FPS counter: " + (fps ? "On" : "Off") + " (tap to change)");
        fb.setOnClickListener(v -> { prefs.edit().putBoolean("fps", !fps).apply(); build(); }); l.addView(fb);
        cycle(l, "Frameskip", new String[] {"Auto", "Off", "1 of 2", "2 of 3"}, "fs");
        cycle(l, "Filter", new String[] {"Off", "Scanlines", "Sharp pixels", "Scale2x", "Scale2x + scanlines"}, "filter");
        cycle(l, "Internal resolution", new String[] {"100%", "75%", "50%"}, "res");
        final String log = readLog();
        if (log != null) {
            TextView lt = new TextView(this); lt.setTextSize(10); lt.setTypeface(android.graphics.Typeface.MONOSPACE); lt.setText("Log of the last run:\n" + log);
            Button copy = new Button(this); copy.setText("Copy log");
            copy.setOnClickListener(v -> { ((ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE)).setPrimaryClip(ClipData.newPlainText("log", log)); Toast.makeText(this, "Copied", Toast.LENGTH_SHORT).show(); });
            l.addView(copy); l.addView(lt);
        }
        ScrollView sv = new ScrollView(this); sv.addView(l);
        setContentView(sv);
    }

    private void pick(int req) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE); i.setType("*/*");
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(i, req);
    }

    /** Stores the key in the app's private storage as 16 raw bytes (accepts raw or hex text). */
    private boolean importKey(Uri u) {
        try (InputStream in = getContentResolver().openInputStream(u)) {
            ByteArrayOutputStream bo = new ByteArrayOutputStream(); byte[] b = new byte[256]; int n; while ((n = in.read(b)) > 0 && bo.size() < 4096) bo.write(b, 0, n);
            byte[] d = bo.toByteArray(), key = new byte[16];
            String s = new String(d).trim().replaceAll("[^0-9A-Fa-f]", "");
            if (d.length == 16) System.arraycopy(d, 0, key, 0, 16);
            else if (s.length() >= 32) for (int k = 0; k < 16; k++) key[k] = (byte) Integer.parseInt(s.substring(2 * k, 2 * k + 2), 16);
            else return false;
            try (FileOutputStream fo = new FileOutputStream(new File(getFilesDir(), "wii.key"))) { fo.write(key); }
            return true;
        } catch (Exception e) { return false; }
    }

    @Override protected void onActivityResult(int req, int res, Intent data) {
        if (res != RESULT_OK || data == null || data.getData() == null) return;
        Uri u = data.getData();
        if (req == REQ_KEY) { Toast.makeText(this, importKey(u) ? "Key saved" : "Not a valid key file (need 16 bytes or 32 hex digits)", Toast.LENGTH_LONG).show(); build(); return; }
        if (req != REQ_WAD) return;
        try { getContentResolver().takePersistableUriPermission(u, Intent.FLAG_GRANT_READ_URI_PERMISSION); } catch (Exception ignored) {}
        prefs.edit().putString("wad", u.toString()).apply();
        start(u);
    }

    private void start(Uri u) {
        if (!haveKey()) { Toast.makeText(this, "Choose your common key file first", Toast.LENGTH_LONG).show(); return; }
        Intent i = new Intent(this, GameActivity.class); i.setData(u);
        try { startActivity(i); } catch (Exception e) { Toast.makeText(this, "Cannot start: " + e, Toast.LENGTH_LONG).show(); }
    }
}
