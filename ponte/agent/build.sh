#!/bin/sh
# Builds agent.dex (javac against android.jar + d8) and, with "push", copies it to the phone.
set -e
cd "$(dirname "$0")"
SDK=$HOME/Android/Sdk
JAR=$SDK/platforms/android-36/android.jar
rm -rf build && mkdir -p build/classes
javac --release 17 -nowarn -cp "$JAR" -d build/classes $(find src -name '*.java')
$SDK/build-tools/35.0.0/d8 --min-api 30 --lib "$JAR" --output build $(find build/classes -name '*.class')
mv build/classes.dex build/agent.dex
if [ "$1" = push ]; then
    ADB=$SDK/platform-tools/adb
    $ADB shell mkdir -p /data/local/tmp/ponte
    $ADB push build/agent.dex run.sh /data/local/tmp/ponte/
    $ADB shell chmod 755 /data/local/tmp/ponte/run.sh
fi
