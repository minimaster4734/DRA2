package org.dra2;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Bundle;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import java.io.File;
import java.io.RandomAccessFile;
import android.widget.TextView;
import android.widget.Toast;

/** Asks the user for their own disc image (ISO/GCM) via the system file picker, remembers it, and starts the game. */
public class LauncherActivity extends Activity {
    private static final int REQ_PICK = 1;
    private SharedPreferences prefs;

    @Override protected void onResume() { super.onResume(); build(); }

    private String readLog() {
        try (RandomAccessFile f = new RandomAccessFile(new File(getFilesDir(), "last.log"), "r")) {
            long n = Math.min(f.length(), 4000); byte[] buf = new byte[(int) n]; f.seek(f.length() - n); f.readFully(buf); return new String(buf);
        } catch (Exception e) { return null; }
    }

    private void build() {
        prefs = getSharedPreferences("dra2", MODE_PRIVATE);
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL); l.setGravity(Gravity.CENTER); l.setPadding(48, 48, 48, 48);
        TextView t = new TextView(this);
        t.setText("This app does not contain the game.\nChoose your own legally owned disc image\n(Digimon Rumble Arena 2, Europe, .iso / .gcm).");
        t.setGravity(Gravity.CENTER); t.setTextSize(16);
        Button pick = new Button(this); pick.setText("Choose disc image");
        pick.setOnClickListener(v -> pick());
        l.addView(t); l.addView(pick);
        final String log = readLog();
        String saved = prefs.getString("disc", null);
        if (saved != null) {
            Button play = new Button(this); play.setText("Play (last disc)");
            play.setOnClickListener(v -> start(Uri.parse(saved)));
            l.addView(play);
        }
        if (log != null) {
            TextView lt = new TextView(this); lt.setTextSize(10); lt.setTypeface(android.graphics.Typeface.MONOSPACE); lt.setText("Log of the last run:\n" + log);
            Button copy = new Button(this); copy.setText("Copy log");
            copy.setOnClickListener(v -> { ((ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE)).setPrimaryClip(ClipData.newPlainText("log", log)); Toast.makeText(this, "Copied", Toast.LENGTH_SHORT).show(); });
            l.addView(copy); l.addView(lt);
        }
        ScrollView sv = new ScrollView(this); sv.addView(l);
        setContentView(sv);
    }

    private void pick() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE); i.setType("*/*");
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(i, REQ_PICK);
    }

    @Override protected void onActivityResult(int req, int res, Intent data) {
        if (req != REQ_PICK || res != RESULT_OK || data == null || data.getData() == null) return;
        Uri u = data.getData();
        try { getContentResolver().takePersistableUriPermission(u, Intent.FLAG_GRANT_READ_URI_PERMISSION); } catch (Exception ignored) {}
        prefs.edit().putString("disc", u.toString()).apply();
        start(u);
    }

    private void start(Uri u) {
        Intent i = new Intent(this, GameActivity.class); i.setData(u);
        try { startActivity(i); } catch (Exception e) { Toast.makeText(this, "Cannot start: " + e, Toast.LENGTH_LONG).show(); }
    }
}
