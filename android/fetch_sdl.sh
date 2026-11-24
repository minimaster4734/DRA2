#!/bin/sh
# Fetches SDL2 (zlib licence) into app/jni/SDL and links its Java glue into both apps. Run once before building.
set -e
cd "$(dirname "$0")/app/jni"
[ -d SDL ] || git clone --depth 1 --branch release-2.32.8 https://github.com/libsdl-org/SDL.git SDL
for m in app megaman; do
  mkdir -p ../../$m/src/main/java/org
  rm -rf ../../$m/src/main/java/org/libsdl
  cp -r SDL/android-project/app/src/main/java/org/libsdl ../../$m/src/main/java/org/libsdl
done
# the megaman module builds against the same SDL checkout
rm -rf ../../megaman/jni/SDL; ln -s ../../app/jni/SDL ../../megaman/jni/SDL
echo "SDL ready."
