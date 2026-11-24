package org.megarun;

import android.content.pm.ActivityInfo;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.view.View;
import android.view.WindowManager;
import java.io.File;
import java.util.ArrayList;
import org.libsdl.app.SDLActivity;

/** SDL host activity: true fullscreen (immersive, over the display cutout), orientation from the launcher, WAD opened through the content resolver. */
public class GameActivity extends SDLActivity {
    private ParcelFileDescriptor wad;

    @Override protected String[] getLibraries() { return new String[] { "SDL2", "main" }; }

    @Override protected void onCreate(Bundle b) {
        int o = getSharedPreferences("mm", MODE_PRIVATE).getInt("orient", 0);   // 0 landscape, 1 portrait, 2 auto
        setRequestedOrientation(o == 1 ? ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT : o == 2 ? ActivityInfo.SCREEN_ORIENTATION_FULL_SENSOR : ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        if (Build.VERSION.SDK_INT >= 28) getWindow().getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON | WindowManager.LayoutParams.FLAG_FULLSCREEN);
        super.onCreate(b);
        immersive();
    }

    private void immersive() {
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }

    @Override public void onWindowFocusChanged(boolean hasFocus) { super.onWindowFocusChanged(hasFocus); if (hasFocus) immersive(); }

    @Override protected String[] getArguments() {
        ArrayList<String> a = new ArrayList<>();
        android.content.SharedPreferences p = getSharedPreferences("mm", MODE_PRIVATE);
        try {
            wad = getContentResolver().openFileDescriptor(getIntent().getData(), "r");
            a.add("/proc/self/fd/" + wad.getFd());
        } catch (Exception e) { a.add("/nonexistent.wad"); }
        a.add("-key"); a.add(new File(getFilesDir(), "wii.key").getAbsolutePath());
        String[] fsVals = {"auto", "0", "1", "2"};
        a.add("-fs"); a.add(fsVals[Math.max(0, Math.min(3, p.getInt("fs", 0)))]);
        a.add("-filter"); a.add(String.valueOf(Math.max(0, Math.min(4, p.getInt("filter", 0)))));
        a.add("-scale"); a.add(new String[] {"1.0", "0.75", "0.5"}[Math.max(0, Math.min(2, p.getInt("res", 0)))]);
        if (p.getBoolean("fps", true)) a.add("-fps");
        return a.toArray(new String[0]);
    }
}
