#!/bin/bash
# rtl_dab_time.sh - Get UTC time from DAB using welle-cli
# Usage: rtl_dab_time.sh [-c channel] [-d rtl_tcp_host:port] [-s]
#
# Requires: welle-cli in PATH or /usr/local/bin/welle-cli
# Options:
#   -c channel   DAB channel (e.g., 12C, 5A, 10B). Default: auto-scan
#   -d host:port rtl_tcp server address. Default: use local RTL-SDR
#   -s           Step clock (instead of slew)
#   -1           One-shot: exit after setting time

CHANNEL=""
DEVICE_ARG=""
STEP_ONLY=0
ONE_SHOT=0
WELLE_CLI="${WELLE_CLI:-welle-cli}"

# DAB Band III channels
CHANNELS="5A 5B 5C 5D 6A 6B 6C 6D 7A 7B 7C 7D 8A 8B 8C 8D 9A 9B 9C 9D 10A 10B 10C 10D 11A 11B 11C 11D 12A 12B 12C 12D"

while getopts "c:d:s1h" opt; do
    case $opt in
        c) CHANNEL="$OPTARG" ;;
        d) DEVICE_ARG="-F rtl_tcp,$OPTARG" ;;
        s) STEP_ONLY=1 ;;
        1) ONE_SHOT=1 ;;
        h|*) echo "Usage: $0 [-c channel] [-d host:port] [-s] [-1]"; exit 1 ;;
    esac
done

# Find welle-cli
if ! command -v "$WELLE_CLI" &>/dev/null; then
    if [ -x "/usr/local/bin/welle-cli" ]; then
        WELLE_CLI="/usr/local/bin/welle-cli"
    elif [ -x "/tmp/welle.io/build-cli/welle-cli" ]; then
        WELLE_CLI="/tmp/welle.io/build-cli/welle-cli"
    else
        echo "Error: welle-cli not found. Install welle.io or set WELLE_CLI env var." >&2
        exit 1
    fi
fi

apply_time() {
    local year=$1 month=$2 day=$3 hour=$4 min=$5 sec=$6
    local dab_time="${year}-${month}-${day} ${hour}:${min}:${sec} UTC"
    echo "DAB time: $dab_time" >&2

    if [ $STEP_ONLY -eq 1 ]; then
        sudo date -u -s "$dab_time" >/dev/null 2>&1
        echo "Clock stepped to $dab_time" >&2
    else
        # Calculate offset and use adjtimex if available
        local dab_epoch=$(date -u -d "$dab_time" +%s 2>/dev/null)
        local sys_epoch=$(date -u +%s)
        local offset=$((dab_epoch - sys_epoch))
        if [ ${offset#-} -gt 0 ]; then
            if [ ${offset#-} -gt 1 ]; then
                sudo date -u -s "$dab_time" >/dev/null 2>&1
                echo "Clock stepped by ${offset}s" >&2
            else
                echo "Clock within 1s, no adjustment needed" >&2
            fi
        fi
    fi
}

scan_and_decode() {
    local ch="$1"
    local timeout=30

    echo "Trying channel $ch..." >&2
    # Run welle-cli for up to $timeout seconds, capture UTCTime JSON
    local time_json
    time_json=$("$WELLE_CLI" -c "$ch" $DEVICE_ARG -D 2>&1 | \
        timeout "$timeout" grep -m1 '"UTCTime"' 2>/dev/null)

    if [ -n "$time_json" ]; then
        # Parse JSON: {"UTCTime":{"day":5,"hour":20,"minutes":32,"month":5,"seconds":20,"year":2026}}
        local year month day hour minutes seconds
        year=$(echo "$time_json" | grep -o '"year":[0-9]*' | cut -d: -f2)
        month=$(echo "$time_json" | grep -o '"month":[0-9]*' | cut -d: -f2)
        day=$(echo "$time_json" | grep -o '"day":[0-9]*' | cut -d: -f2)
        hour=$(echo "$time_json" | grep -o '"hour":[0-9]*' | cut -d: -f2)
        minutes=$(echo "$time_json" | grep -o '"minutes":[0-9]*' | cut -d: -f2)
        seconds=$(echo "$time_json" | grep -o '"seconds":[0-9]*' | cut -d: -f2)

        if [ -n "$year" ] && [ -n "$month" ] && [ -n "$day" ]; then
            printf -v month "%02d" "$month"
            printf -v day "%02d" "$day"
            printf -v hour "%02d" "$hour"
            printf -v minutes "%02d" "$minutes"
            printf -v seconds "%02d" "$seconds"
            apply_time "$year" "$month" "$day" "$hour" "$minutes" "$seconds"
            return 0
        fi
    fi
    return 1
}

# Main
if [ -n "$CHANNEL" ]; then
    # Single channel mode
    while true; do
        if scan_and_decode "$CHANNEL"; then
            [ $ONE_SHOT -eq 1 ] && exit 0
        else
            echo "No time data on channel $CHANNEL, retrying..." >&2
        fi
        sleep 5
    done
else
    # Auto-scan mode
    while true; do
        for ch in $CHANNELS; do
            if scan_and_decode "$ch"; then
                [ $ONE_SHOT -eq 1 ] && exit 0
                # Found a working channel, keep using it
                CHANNEL="$ch"
                break
            fi
        done
        if [ -n "$CHANNEL" ]; then
            # Keep using the channel that worked
            while true; do
                if scan_and_decode "$CHANNEL"; then
                    [ $ONE_SHOT -eq 1 ] && exit 0
                else
                    echo "Lost signal on $CHANNEL, rescanning..." >&2
                    CHANNEL=""
                    break
                fi
                sleep 5
            done
        fi
    done
fi
