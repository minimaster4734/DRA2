#!/bin/sh
# Fetches SDL2 (zlib licence) into app/jni/SDL and links its Java glue into the app. Run once before building.
set -e
cd "$(dirname "$0")/app/jni"
[ -d SDL ] || git clone --depth 1 --branch release-2.32.8 https://github.com/libsdl-org/SDL.git SDL
mkdir -p ../src/main/java/org
rm -rf ../src/main/java/org/libsdl
cp -r SDL/android-project/app/src/main/java/org/libsdl ../src/main/java/org/libsdl
echo "SDL ready."
