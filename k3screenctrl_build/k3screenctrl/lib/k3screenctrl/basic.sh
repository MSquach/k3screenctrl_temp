#!/bin/sh
. /etc/os-release
. /etc/openwrt_release

PRODUCT_NAME_FULL=$(cat /etc/board.json | jsonfilter -e "@.model.name")
PRODUCT_NAME=${PRODUCT_NAME_FULL#* } # Remove first word to save space

# DSA兼容: 新版OpenWrt使用device替代ifname
WAN_IFNAME=$(uci get network.wan.device 2>/dev/null)
[ -z "$WAN_IFNAME" ] && WAN_IFNAME=$(uci get network.wan.ifname 2>/dev/null)

if [ -n "$WAN_IFNAME" ]; then
    MAC_ADDR=$(ip link show "$WAN_IFNAME" 2>/dev/null | grep -oE '([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}' | head -1)
fi
[ -z "$MAC_ADDR" ] && MAC_ADDR="00:00:00:00:00:00"

HW_VERSION="A1"
FW_VERSION=${DISTRIB_REVISION:0:17}

echo $PRODUCT_NAME

# 安全读取cputemp配置，不存在时默认为0
CPU_TEMP_CFG=$(uci get k3screenctrl.@general[0].cputemp 2>/dev/null)
CPU_TEMP_CFG=${CPU_TEMP_CFG:-0}

if [ "$CPU_TEMP_CFG" -eq 1 ] 2>/dev/null; then
    CPU_TEMP=$(($(cat /sys/class/thermal/thermal_zone0/temp) / 1000))

    used=`free | grep Mem | awk '{print$3}'`
    all=`free | grep Mem | awk '{print$2}'`
    LOAD=`uptime | awk -F "average:" '{print$2}' | awk -F "," '{print$1}'`
    UPTIME=`uptime | awk -F "," '{print$1}' | awk '{print$3$4}'`

    echo T: $CPU_TEMP*C
    echo $FW_VERSION, $UPTIME
    echo U:$LOAD, R: $((100*$used/$all))%
else
    echo $HW_VERSION
    echo $FW_VERSION
    echo $MAC_ADDR
fi
