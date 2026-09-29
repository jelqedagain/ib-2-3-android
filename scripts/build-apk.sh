#!/usr/bin/env bash
# Builds the Android apps (arm64, Android 11+), one per game, from the same code:
#   dist/InfinityBladeIII-Android-<version>.apk   (com.ib3port.game)
#   dist/InfinityBladeII-Android-<version>.apk    (com.ib2port.game)
# The version is the manifest's versionName, so every released file says which build it is.
# Needs tools/android: the NDK, build-tools (aapt2, d8, zipalign, apksigner) and android-35/android.jar,
# plus a JDK (javac, keytool).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BT=tools/android/build-tools
JAR=tools/android/android-35/android.jar
KEYSTORE=tools/android/ib3-debug.keystore
COMMON=build-android/apk-common
VERSION="$(sed -n 's/.*android:versionName="\([^"]*\)".*/\1/p' android/AndroidManifest.xml)"

./build-android.sh ib3
rm -rf build-android/apk* && mkdir -p "$COMMON/classes" "$COMMON/lib/arm64-v8a" dist

# The Java activities (launcher/installer, dialogs), compiled to classes.dex.
javac --release 11 -classpath "$JAR" -d "$COMMON/classes" -Xlint:-options $(find android/java -name '*.java')
java -cp "$BT/lib/d8.jar" com.android.tools.r8.D8 --release --min-api 30 --lib "$JAR" --output "$COMMON" \
    $(find "$COMMON/classes" -name '*.class')

# The native library, without debug info (build-android/libib3.so keeps it for crash reports).
NDK="$(ls -d tools/android/android-ndk-* | head -1)"
"$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe" -o "$COMMON/lib/arm64-v8a/libib3.so" build-android/libib3.so
"$BT/aapt2.exe" compile --dir android/res -o "$COMMON/res.zip"

# Signed with a local key made on first use (kept out of the repository).
if [ ! -f "$KEYSTORE" ]; then
    keytool -genkeypair -keystore "$KEYSTORE" -storepass ib3port -keypass ib3port -alias ib3 \
        -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Infinity Blade III Port" >/dev/null 2>&1
fi

# package_app <id> <app name> <package> <.ipa bundle id> <.ipa version> <space needed> <apk name, without .apk>
package_app() {
    local OUT=build-android/apk-$1
    mkdir -p "$OUT"
    sed -e "s|@LABEL@|$2|" -e "s|@PACKAGE@|$3|" -e "s|@BUNDLE_ID@|$4|" -e "s|@IPA_VERSION@|$5|" -e "s|@SIZE@|$6|" \
        android/AndroidManifest.xml > "$OUT/AndroidManifest.xml"
    "$BT/aapt2.exe" compile --dir "android/res-$1" -o "$OUT/icon.zip"
    "$BT/aapt2.exe" link -o "$OUT/unsigned.apk" -I "$JAR" --manifest "$OUT/AndroidManifest.xml" \
        --min-sdk-version 30 --target-sdk-version 35 "$COMMON/res.zip" "$OUT/icon.zip"
    python3 - "$COMMON" "$OUT" <<'EOF'
import sys, zipfile
common, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out + "/unsigned.apk", "a", zipfile.ZIP_DEFLATED) as z:
    z.write(common + "/classes.dex", "classes.dex")
    z.write(common + "/lib/arm64-v8a/libib3.so", "lib/arm64-v8a/libib3.so")
EOF
    "$BT/zipalign.exe" -f -p 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"
    java -jar "$BT/lib/apksigner.jar" sign --ks "$KEYSTORE" --ks-pass pass:ib3port --key-pass pass:ib3port \
        --out "dist/$7-$VERSION.apk" "$OUT/aligned.apk"
    ls -la "dist/$7-$VERSION.apk"
}

package_app ib3 "Infinity Blade III" com.ib3port.game com.chairentertainment.IB3 1.4.4 "3 GB" InfinityBladeIII-Android
package_app ib2 "Infinity Blade II" com.ib2port.game com.chairentertainment.IB2 1.3.5 "1.5 GB" InfinityBladeII-Android
