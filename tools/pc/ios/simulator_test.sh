#!/bin/bash
# Run the iPhone simulator build (package_ios.py) in an iOS Simulator and
# keep what it shows: screenshots, the game's console, crash reports.
# usage: simulator_test.sh <YFM.app> <output folder> [seconds]
set -uo pipefail
app=$1
out=$2
seconds=${3:-60}
mkdir -p "$out"

device=$(xcrun simctl list devices available -j | python3 -c '
import json, sys
devices = json.load(sys.stdin)["devices"]
best = None
for runtime, entries in devices.items():
    if "iOS" not in runtime:
        continue
    version = tuple(int(x) for x in runtime.rsplit("iOS-", 1)[-1].split("-") if x.isdigit())
    for d in entries:
        if d["name"].startswith("iPhone") and (best is None or version > best[0]):
            best = (version, d["udid"], d["name"], runtime)
print(best[1])
print(best[2] + " (" + best[3] + ")", file=sys.stderr)
')
echo "simulator: $device"
xcrun simctl boot "$device"
xcrun simctl bootstatus "$device" -b > /dev/null
xcrun simctl install "$device" "$app"
bundle=$(/usr/libexec/PlistBuddy -c 'Print CFBundleIdentifier' "$app/Info.plist")
xcrun simctl launch --console-pty --terminate-running-process "$device" "$bundle" > "$out/console.txt" 2>&1 &
launcher=$!
shot=0
for wait in 10 20 $((seconds - 30)); do
    sleep "$wait"
    shot=$((shot + 1))
    xcrun simctl io "$device" screenshot "$out/screen-$shot.png" > /dev/null 2>&1 || true
done
running=yes
kill -0 "$launcher" 2> /dev/null || running=no
echo "still running after ${seconds}s: $running" | tee "$out/result.txt"
kill "$launcher" 2> /dev/null || true
documents=$(xcrun simctl get_app_container "$device" "$bundle" data 2> /dev/null)/Documents
[ -d "$documents" ] && ls -laR "$documents" > "$out/documents.txt" 2>&1
find ~/Library/Logs/DiagnosticReports -name 'memories*' -newer "$app/Info.plist" -exec cp {} "$out/" \; 2> /dev/null
xcrun simctl shutdown "$device" > /dev/null 2>&1 || true
tail -40 "$out/console.txt"
[ "$running" = yes ]
