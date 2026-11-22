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
            return new String[] { "/proc/self/fd/" + disc.getFd() };
        } catch (Exception e) {
            return new String[] { "/nonexistent.iso" };
        }
    }
}
