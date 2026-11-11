#!/bin/sh
# Builds FotoPixel.apk (aapt2 + javac + d8, no Gradle, like ponte/app) and, with "install", installs it on the
# Pixel 2 XL, grants storage, exempts it from battery optimization and starts the service.
# The token (X-Token) comes from secrets/pixelfoto-token and goes into the APK's assets.
set -e
cd "$(dirname "$0")"
SDK=$HOME/Android/Sdk
BT=$SDK/build-tools/35.0.0
JAR=$SDK/platforms/android-36/android.jar
TOKEN=../../secrets/pixelfoto-token
[ -f $TOKEN ] || { echo "manca $TOKEN" >&2; exit 1; }
rm -rf build && mkdir -p build/classes build/assets
cp assets/* build/assets/
tr -d '\n' < $TOKEN > build/assets/token
# AdbGuard accepts only this PC's adb key (MD5 fingerprint as SystemUI shows it)
cut -d' ' -f1 $HOME/.android/adbkey.pub | base64 -d | md5sum | cut -d' ' -f1 | tr a-f A-F | sed 's/../&:/g; s/:$//' | tr -d '\n' > build/assets/adbfp
# tailscaled + CLI 1.104.2 static arm64 as native libs (exec allowed from nativeLibraryDir), PLAN.md §58.3
TS=../../ref/tailscale/tailscale_1.104.2_arm64
mkdir -p build/lib/arm64-v8a
cp $TS/tailscaled build/lib/arm64-v8a/libtailscaled.so
cp $TS/tailscale build/lib/arm64-v8a/libtailscale.so
$BT/aapt2 compile --dir res -o build/res.zip
$BT/aapt2 link -o build/base.apk -I "$JAR" --manifest AndroidManifest.xml -A build/assets build/res.zip
javac --release 17 -nowarn -cp "$JAR" -d build/classes $(find src -name '*.java')
$BT/d8 --min-api 29 --lib "$JAR" --output build $(find build/classes -name '*.class')
(cd build && zip -q base.apk classes.dex && zip -q -r base.apk lib)
$BT/zipalign -p -f 4 build/base.apk build/aligned.apk
$BT/apksigner sign --ks $HOME/.android/debug.keystore --ks-pass pass:android --out build/FotoPixel.apk build/aligned.apk
if [ "$1" = install ]; then
    A="$SDK/platform-tools/adb -s ${PIXEL:-SERIAL}"
    P=dumbphone.pixelfoto
    $A install -r build/FotoPixel.apk
    $A shell pm grant $P android.permission.READ_EXTERNAL_STORAGE
    $A shell pm grant $P android.permission.WRITE_EXTERNAL_STORAGE
    $A shell dumpsys deviceidle whitelist +$P >/dev/null
    # Google Foto too: the screen is broken, so it is never opened, Android drops it to the RARE bucket and backups stall
    $A shell dumpsys deviceidle whitelist +com.google.android.apps.photos >/dev/null
    $A shell am set-standby-bucket com.google.android.apps.photos active
    # AdbGuard: accepts the "Allow USB debugging?" dialog by itself (always allow), the screen is broken
    $A shell settings put secure enabled_accessibility_services $P/.AdbGuard
    $A shell settings put secure accessibility_enabled 1
    $A shell am broadcast -n $P/.Boot >/dev/null
fi
