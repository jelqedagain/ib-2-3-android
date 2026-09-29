#!/usr/bin/env bash
# Builds the Android app: dist/InfinityBladeIII-Android.apk (arm64, Android 11+).
# Needs tools/android: the NDK, build-tools (aapt2, d8, zipalign, apksigner) and android-35/android.jar,
# plus a JDK (javac, keytool).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BT=tools/android/build-tools
JAR=tools/android/android-35/android.jar
OUT=build-android/apk
KEYSTORE=tools/android/ib3-debug.keystore

./build-android.sh ib3
rm -rf "$OUT" && mkdir -p "$OUT/lib/arm64-v8a" dist

# Resources and the manifest.
"$BT/aapt2.exe" compile --dir android/res -o "$OUT/res.zip"
"$BT/aapt2.exe" link -o "$OUT/unsigned.apk" -I "$JAR" --manifest android/AndroidManifest.xml \
    --min-sdk-version 30 --target-sdk-version 35 "$OUT/res.zip"

# The Java activities (launcher/installer, dialogs), compiled to classes.dex.
mkdir -p "$OUT/classes"
javac --release 11 -classpath "$JAR" -d "$OUT/classes" -Xlint:-options $(find android/java -name '*.java')
java -cp "$BT/lib/d8.jar" com.android.tools.r8.D8 --release --min-api 30 --lib "$JAR" --output "$OUT"     $(find "$OUT/classes" -name '*.class')

# The native library, without debug info (build-android/libib3.so keeps it for crash reports).
NDK="$(ls -d tools/android/android-ndk-* | head -1)"
"$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe" -o "$OUT/lib/arm64-v8a/libib3.so" build-android/libib3.so
python3 - "$OUT" <<'EOF'
import sys, zipfile
out = sys.argv[1]
with zipfile.ZipFile(out + "/unsigned.apk", "a", zipfile.ZIP_DEFLATED) as z:
    z.write(out + "/classes.dex", "classes.dex")
    z.write(out + "/lib/arm64-v8a/libib3.so", "lib/arm64-v8a/libib3.so")
EOF
"$BT/zipalign.exe" -f -p 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"

# Signed with a local key made on first use (kept out of the repository).
if [ ! -f "$KEYSTORE" ]; then
    keytool -genkeypair -keystore "$KEYSTORE" -storepass ib3port -keypass ib3port -alias ib3 \
        -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Infinity Blade III Port" >/dev/null 2>&1
fi
java -jar "$BT/lib/apksigner.jar" sign --ks "$KEYSTORE" --ks-pass pass:ib3port --key-pass pass:ib3port \
    --out dist/InfinityBladeIII-Android.apk "$OUT/aligned.apk"
ls -la dist/InfinityBladeIII-Android.apk
