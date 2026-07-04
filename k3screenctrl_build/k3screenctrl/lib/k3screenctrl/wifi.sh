#!/bin/sh

COMPLETE_STAT=$(wifi status 2>/dev/null)

print_wifi_info() {
    local ucidev=$1
    local status device_json ssid psk client_count=0 enabled=0
    local ifname

    device_json=$(echo "$COMPLETE_STAT" | jsonfilter -e "@.$ucidev" 2>/dev/null)
    if [ -n "$device_json" ]; then
        status=$(echo "$device_json" | jsonfilter -e "@.disabled" 2>/dev/null)
        local iface_check=$(echo "$device_json" | jsonfilter -e "@.interfaces[0]" 2>/dev/null)
        if [ "x$status" = "xfalse" -a -n "$iface_check" ]; then
            ssid=$(echo "$COMPLETE_STAT" | jsonfilter -e "@.$ucidev.interfaces[0].config.ssid" 2>/dev/null)
            psk=$(echo "$COMPLETE_STAT" | jsonfilter -e "@.$ucidev.interfaces[0].config.key" 2>/dev/null)
            ifname=$(echo "$device_json" | jsonfilter -e "@.interfaces[0].ifname" 2>/dev/null)
            if [ -n "$ifname" ]; then
                client_count=$(iwinfo "$ifname" assoclist 2>/dev/null | grep -c "dBm")
            fi
            enabled=1
        fi
    fi

    echo "$ssid"

    local psk_hide=$(uci get k3screenctrl.@general[0].psk_hide 2>/dev/null)
    psk_hide=${psk_hide:-0}
    if [ "$psk_hide" -eq 1 ] 2>/dev/null; then
        echo "$psk" | sed 's/./*/g'
    else
        echo "$psk"
    fi
    echo $enabled
    echo $client_count
}

echo 0 # Band mix
print_wifi_info radio0 # 2.4GHz
print_wifi_info radio1 # 5GHZ
print_wifi_info radiox # Visitor - not implemented
