#!/usr/bin/env bash
# Headless Android build (no Android Studio needed).
# Requires: JDK 17, ANDROID_HOME, Gradle 8.x
set -euo pipefail

cd "$(dirname "$0")"

export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-17-openjdk}"
export ANDROID_HOME="${ANDROID_HOME:-/opt/android-sdk}"
export ANDROID_SDK_ROOT="$ANDROID_HOME"
export PATH="$HOME/.local/bin:$JAVA_HOME/bin:$PATH"

if [ ! -f local.properties ]; then
  echo "sdk.dir=$ANDROID_HOME" > local.properties
fi

GRADLE_CMD="${GRADLE_CMD:-gradle}"
if [ -x ./gradlew ]; then
  GRADLE_CMD=./gradlew
elif ! command -v "$GRADLE_CMD" >/dev/null 2>&1; then
  echo "Gradle not found. Install Gradle 8.x or set GRADLE_CMD." >&2
  exit 1
fi

TASK="${1:-assembleDebug}"
shift || true
exec "$GRADLE_CMD" "$TASK" "$@"
