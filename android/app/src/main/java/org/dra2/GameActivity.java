package org.dra2;

import android.net.Uri;
import android.os.ParcelFileDescriptor;
import org.libsdl.app.SDLActivity;

/** SDL host activity. The chosen disc is opened through the content resolver; the native side reads it via /proc/self/fd/N. */
public class GameActivity extends SDLActivity {
    private ParcelFileDescriptor disc;

    @Override protected String[] getLibraries() { return new String[] { "SDL2", "main" }; }

    @Override protected String[] getArguments() {
        Uri u = getIntent().getData();
        try {
            disc = getContentResolver().openFileDescriptor(u, "r");
            String[] fsVals = {"auto", "0", "1", "2"};
            int fs = Math.max(0, Math.min(3, getSharedPreferences("dra2", MODE_PRIVATE).getInt("fs", 0)));
            return new String[] { "/proc/self/fd/" + disc.getFd(), "-fs", fsVals[fs] };
        } catch (Exception e) {
            return new String[] { "/nonexistent.iso" };
        }
    }
}
