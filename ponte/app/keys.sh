#!/bin/sh
# One time: makes the app's adb key (secrets/ponte-adbkey, assets/*.der) and authorizes it on the Pixel over USB:
# a private adb server with only this key asks "Allow USB debugging?" on the phone -> "Always allow".
set -e
cd "$(dirname "$0")"
S=../../secrets/ponte-adb
A=$HOME/Android/Sdk/platform-tools/adb
mkdir -p $S/.android assets
[ -f $S/.android/adbkey ] || openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out $S/.android/adbkey
openssl pkcs8 -topk8 -nocrypt -in $S/.android/adbkey -outform DER -out assets/adbkey.der
openssl req -new -x509 -key $S/.android/adbkey -subj /CN=ponte -days 36500 -outform DER -out assets/adbcert.der
$A kill-server
HOME=$(cd $S && pwd) $A start-server
echo "sul Pixel: 'Consenti sempre da questo computer' + Consenti"
HOME=$(cd $S && pwd) $A wait-for-device
HOME=$(cd $S && pwd) $A shell echo autorizzata
$A kill-server
$A start-server
