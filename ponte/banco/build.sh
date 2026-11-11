#!/bin/sh
# Banco Remoto: a test app for the Remoto bench (PLAN §65). Content like the user's benchmarks without touching
# their apps: "pagine" (Kindle: pages of text, a tap turns the page with a short slide), "gioco" (a DS game: tile
# maps scrolling at 60 fps on two screens), "flipper" (a fast ball over the whole screen). With "install" it installs.
#   am start --display <id> -n dumbphone.ponte.banco/.Banco --es mode pagine|gioco|flipper
set -e
cd "$(dirname "$0")"
SDK=$HOME/Android/Sdk
BT=$SDK/build-tools/35.0.0
JAR=$SDK/platforms/android-36/android.jar
rm -rf build && mkdir -p build/classes
$BT/aapt2 link -o build/base.apk -I "$JAR" --manifest AndroidManifest.xml
javac --release 17 -nowarn -cp "$JAR" -d build/classes $(find src -name '*.java')
$BT/d8 --min-api 33 --lib "$JAR" --output build $(find build/classes -name '*.class')
(cd build && zip -q base.apk classes.dex)
$BT/zipalign -f 4 build/base.apk build/aligned.apk
$BT/apksigner sign --ks $HOME/.android/debug.keystore --ks-pass pass:android --out build/Banco.apk build/aligned.apk
[ "$1" = install ] && $SDK/platform-tools/adb install -r build/Banco.apk
exit 0
