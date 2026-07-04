#!/bin/sh

# Basic vars
TEMP_FILE="/tmp/k3screenctrl/wan_speed"
mkdir -p "$(dirname "$TEMP_FILE")"

WAN_STAT=$(ifstatus wan 2>/dev/null)
WAN6_STAT=$(ifstatus wan6 2>/dev/null)

# Internet connectivity
IPV4_ADDR=$(echo "$WAN_STAT" | jsonfilter -e "@['ipv4-address'][0].address" 2>/dev/null)
IPV6_ADDR=$(echo "$WAN6_STAT" | jsonfilter -e "@['ipv6-address'][0].address" 2>/dev/null)

if [ -n "$IPV4_ADDR" -o -n "$IPV6_ADDR" ]; then
    CONNECTED=1
else
    CONNECTED=0
fi

# Get WAN interface name with multiple fallbacks
WAN_IFNAME=$(echo "$WAN_STAT" | jsonfilter -e "@.l3_device" 2>/dev/null)
if [ -z "$WAN_IFNAME" ]; then
    WAN_IFNAME=$(echo "$WAN_STAT" | jsonfilter -e "@.device" 2>/dev/null)
fi
if [ -z "$WAN_IFNAME" ]; then
    WAN_IFNAME=$(uci get network.wan.device 2>/dev/null)
fi
if [ -z "$WAN_IFNAME" ]; then
    WAN_IFNAME=$(uci get network.wan.ifname 2>/dev/null)
fi

# Calculate speed by traffic delta / time delta
# NOTE: /proc/net/dev updates every ~1s.
# You must call this script with longer interval!
CURR_TIME=$(date +%s)
CURR_DOWNLOAD_BYTES=0
CURR_UPLOAD_BYTES=0

if [ -n "$WAN_IFNAME" ]; then
    CURR_STAT=$(grep "$WAN_IFNAME" /proc/net/dev 2>/dev/null | sed -e 's/^ *//' -e 's/  */ /g')
    if [ -n "$CURR_STAT" ]; then
        CURR_DOWNLOAD_BYTES=$(echo "$CURR_STAT" | cut -d " " -f 2)
        CURR_UPLOAD_BYTES=$(echo "$CURR_STAT" | cut -d " " -f 10)
        CURR_DOWNLOAD_BYTES=${CURR_DOWNLOAD_BYTES:-0}
        CURR_UPLOAD_BYTES=${CURR_UPLOAD_BYTES:-0}
    fi
fi

if [ -e "$TEMP_FILE" ]; then
    LINE_NUM=0
    while read line; do
        case "$LINE_NUM" in
            0)
                LAST_TIME=$line
                ;;
            1)
                LAST_UPLOAD_BYTES=$line
                ;;
            2)
                LAST_DOWNLOAD_BYTES=$line
                ;;
            *)
                ;;
        esac
        LINE_NUM=$(($LINE_NUM+1))
    done < "$TEMP_FILE"
fi

echo "$CURR_TIME" > "$TEMP_FILE"
echo "$CURR_UPLOAD_BYTES" >> "$TEMP_FILE"
echo "$CURR_DOWNLOAD_BYTES" >> "$TEMP_FILE"

if [ -z "$LAST_TIME" -o -z "$LAST_UPLOAD_BYTES" -o -z "$LAST_DOWNLOAD_BYTES" ]; then
    # First time of launch
    UPLOAD_BPS=0
    DOWNLOAD_BPS=0
else
    TIME_DELTA_S=$(($CURR_TIME-$LAST_TIME))
    if [ $TIME_DELTA_S -eq 0 ]; then
        TIME_DELTA_S=1
    fi
    UPLOAD_BPS=$((($CURR_UPLOAD_BYTES-$LAST_UPLOAD_BYTES)/$TIME_DELTA_S))
    DOWNLOAD_BPS=$((($CURR_DOWNLOAD_BYTES-$LAST_DOWNLOAD_BYTES)/$TIME_DELTA_S))
fi

echo $CONNECTED
echo $UPLOAD_BPS
echo $DOWNLOAD_BPS
