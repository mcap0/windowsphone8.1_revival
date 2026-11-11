#!/bin/sh
# Builds Ponte.apk (aapt2 + javac + d8, no Gradle) and, with "install", installs it and grants its permissions.
# One-time setup: ./keys.sh (app's adb key, authorized once like a PC over USB).
set -e
cd "$(dirname "$0")"
SDK=$HOME/Android/Sdk
BT=$SDK/build-tools/35.0.0
JAR=$SDK/platforms/android-36/android.jar
[ -f assets/adbkey.der ] || { echo "manca la chiave: ./keys.sh" >&2; exit 1; }
rm -rf build && mkdir -p build/classes
$BT/aapt2 compile --dir res -o build/res.zip
$BT/aapt2 link -o build/base.apk -I "$JAR" --manifest AndroidManifest.xml -A assets build/res.zip
javac --release 17 -nowarn -cp "$JAR" -d build/classes $(find src -name '*.java')
$BT/d8 --min-api 33 --lib "$JAR" --output build $(find build/classes -name '*.class')
(cd build && zip -q base.apk classes.dex)
$BT/zipalign -f 4 build/base.apk build/aligned.apk
$BT/apksigner sign --ks $HOME/.android/debug.keystore --ks-pass pass:android --out build/Ponte.apk build/aligned.apk
if [ "$1" = install ]; then
    A=$SDK/platform-tools/adb
    $A install -r build/Ponte.apk
    $A shell pm grant dumbphone.ponte.app android.permission.WRITE_SECURE_SETTINGS
    $A shell pm grant dumbphone.ponte.app android.permission.POST_NOTIFICATIONS
    # no battery limits: the 15-minute check must run, in Doze too
    $A shell dumpsys deviceidle whitelist +dumbphone.ponte.app
    $A shell cmd appops set dumbphone.ponte.app RUN_ANY_IN_BACKGROUND allow
    $A shell cmd appops set dumbphone.ponte.app RUN_IN_BACKGROUND allow
    $A shell cmd appops set dumbphone.ponte.app AUTO_REVOKE_PERMISSIONS_IF_UNUSED ignore
    $A shell am set-standby-bucket dumbphone.ponte.app active
fi
