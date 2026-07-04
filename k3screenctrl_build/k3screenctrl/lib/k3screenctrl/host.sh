#!/bin/sh

SPEED_DIR="/tmp/lan_speed"
OUI_FILE="/lib/k3screenctrl/oui/oui.txt"
mkdir -p "$SPEED_DIR" 2>/dev/null

# Setup iptables chains for traffic counting
iptables -L UPSP >/dev/null 2>&1 || iptables -N UPSP
iptables -L DWSP >/dev/null 2>&1 || iptables -N DWSP

# Load custom device config
uci show k3screenctrl 2>/dev/null > /tmp/k3_custom

# Get online devices from ARP table
DEVICES=$(grep -v "0x0" /proc/net/arp | grep "br-lan")
[ -z "$DEVICES" ] && { echo 0; echo 0; exit 0; }

COUNT=$(echo "$DEVICES" | wc -l)
echo "$COUNT"

# Process each device
echo "$DEVICES" | while IFS= read -r line; do
    ip=$(echo "$line" | awk '{print $1}')
    mac=$(echo "$line" | awk '{print $4}')
    [ -z "$ip" ] && continue

    # Add iptables rules if needed
    iptables -nvx -L FORWARD 2>/dev/null | grep -q "UPSP.*$ip" || {
        iptables -I FORWARD 1 -s "$ip" -j UPSP
        iptables -I FORWARD 1 -d "$ip" -j DWSP
        date +%s > "$SPEED_DIR/$ip"
        echo 0 >> "$SPEED_DIR/$ip"
        echo 0 >> "$SPEED_DIR/$ip"
    }

    # Get hostname from DHCP leases
    hostname=$(grep -w "$ip" /tmp/dhcp.leases 2>/dev/null | awk '{print $4}')

    # Get logo from OUI database
    mac_upper=$(echo "$mac" | tr 'a-z' 'A-Z')
    mac_prefix=$(echo "$mac_upper" | tr -d ':' | cut -c1-6)
    logo=$(grep -i "$mac_prefix" "$OUI_FILE" 2>/dev/null | awk '{print $1}')

    # Check for custom config (user-defined name/icon)
    tmp_uci=$(grep "$mac_upper" /tmp/k3_custom 2>/dev/null | awk -F'=' '{print $1}' | awk -F'.' '{print $1"."$2}')
    if [ -n "$tmp_uci" ]; then
        hostname=$(uci get "$tmp_uci.name" 2>/dev/null)
        logo=$(uci get "$tmp_uci.icon" 2>/dev/null)
    fi

    # Read last speed stats
    last_time=0; last_up=0; last_dw=0
    if [ -f "$SPEED_DIR/$ip" ]; then
        last_time=$(sed -n '1p' "$SPEED_DIR/$ip")
        last_up=$(sed -n '2p' "$SPEED_DIR/$ip")
        last_dw=$(sed -n '3p' "$SPEED_DIR/$ip")
    fi

    # Get current iptables counters
    now_time=$(date +%s)
    now_up=$(iptables -nvx -L FORWARD 2>/dev/null | grep "UPSP" | grep -w "$ip" | awk '{print $2}')
    now_dw=$(iptables -nvx -L FORWARD 2>/dev/null | grep "DWSP" | grep -w "$ip" | awk '{print $2}')

    [ -z "$last_time" ] && last_time=0
    [ -z "$last_up" ] && last_up=0
    [ -z "$last_dw" ] && last_dw=0
    [ -z "$now_up" ] && now_up=0
    [ -z "$now_dw" ] && now_dw=0

    # Calculate speed (bytes/sec)
    delta=$((now_time - last_time))
    [ "$delta" -eq 0 ] && delta=1
    up_sp=$(( (now_up - last_up) / delta ))
    dw_sp=$(( (now_dw - last_dw) / delta ))

    # Save current stats
    printf '%s\n%s\n%s\n' "$now_time" "$now_up" "$now_dw" > "$SPEED_DIR/$ip"

    # Defaults
    [ -z "$hostname" ] || [ "$hostname" = "*" ] && hostname="Unknown"
    [ -z "$logo" ] && logo=0

    # Output: hostname, download_speed, upload_speed, logo
    echo "$hostname"
    echo "$dw_sp"
    echo "$up_sp"
    echo "$logo"
done

echo 0
