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

CPU_TEMP=$(($(cat /sys/class/thermal/thermal_zone0/temp) / 1000))

HW_VERSION="A1"
#${LEDE_DEVICE_REVISION:0:2}
FW_VERSION=${DISTRIB_REVISION:0:17}

echo $PRODUCT_NAME

# 安全读取showmore配置，不存在时默认为0
SHOWMORE=$(uci get k3screenctrl.@general[0].showmore 2>/dev/null)
SHOWMORE=${SHOWMORE:-0}

if [ "$SHOWMORE" -eq 1 ] 2>/dev/null; then
    echo U:$CPU_TEMP *C
    used=`free | grep Mem | awk '{print$3}'`
    all=`free | grep Mem | awk '{print$2}'`
    LOAD=`uptime | awk -F "average:" '{print$2}' | awk -F "," '{print$1}'`
    UPTIME=`uptime | awk -F "," '{print$1}'|awk '{print"up " $3" " $4}'`
    echo U:$LOAD R:$((100*$used/$all))%
    echo $UPTIME
    echo $DISTRIB_DESCRIPTION
else
    echo $HW_VERSION
    echo $FW_VERSION
    echo $FW_VERSION
    echo $MAC_ADDR
fi
