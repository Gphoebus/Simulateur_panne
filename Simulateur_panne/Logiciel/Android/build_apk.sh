#!/bin/bash
# Construction de l'APK sans Gradle (outils Android SDK : aapt, dx, zipalign, apksigner)
# Prerequis Ubuntu : sudo apt install android-sdk-platform-23 android-sdk-build-tools apksigner zipalign default-jdk
set -e
cd "$(dirname "$0")"
SDK=${ANDROID_HOME:-/usr/lib/android-sdk}
JAR=$SDK/platforms/android-23/android.jar
BT=$(ls -d $SDK/build-tools/* | sort -V | tail -1)
SRC=app/src/main
OUT=build
rm -rf $OUT && mkdir -p $OUT/gen $OUT/classes
echo "[1/6] Ressources (aapt)"
$BT/aapt package -f -m -J $OUT/gen -M $SRC/AndroidManifest.xml -S $SRC/res -I $JAR
echo "[2/6] Compilation Java"
javac -nowarn -Xlint:-options -source 8 -target 8 -bootclasspath $JAR -classpath $JAR -d $OUT/classes \
  $(find $SRC/java $OUT/gen -name '*.java')
echo "[3/6] Dalvik (dx)"
$BT/dx --dex --min-sdk-version=23 --output=$OUT/classes.dex $OUT/classes
echo "[4/6] Paquet APK"
$BT/aapt package -f -M $SRC/AndroidManifest.xml -S $SRC/res -A $SRC/assets -I $JAR -F $OUT/unsigned.apk
(cd $OUT && $BT/aapt add unsigned.apk classes.dex >/dev/null)
echo "[5/6] Alignement"
$BT/zipalign -f -p 4 $OUT/unsigned.apk $OUT/aligned.apk
echo "[6/6] Signature"
if [ ! -f simpanne.keystore ]; then
  keytool -genkeypair -keystore simpanne.keystore -storepass simpanne -keypass simpanne -alias simpanne \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Phoebus du F.L.A.L., O=Lycee de la Borde Basse, C=FR" >/dev/null 2>&1
fi
$BT/apksigner sign --ks simpanne.keystore --ks-pass pass:simpanne --key-pass pass:simpanne --ks-key-alias simpanne \
  --min-sdk-version 23 --out SimuPannes.apk $OUT/aligned.apk
$BT/apksigner verify --print-certs SimuPannes.apk | head -2
echo "APK : $(pwd)/SimuPannes.apk ($(du -k SimuPannes.apk | cut -f1) Ko)"
