#!/bin/sh

# swconfig模式: 通过交换机端口号检测链路
print_eth_port_status_swconfig() {
    local port=$1
    if [ -n "$(swconfig dev switch0 port $port show 2>/dev/null | grep 'link:up')" ]; then
        echo 1
    else
        echo 0
    fi
}

# DSA模式: 通过网络接口carrier状态检测链路
print_eth_port_status_dsa() {
    local iface=$1
    if [ -f "/sys/class/net/$iface/carrier" ]; then
        local carrier=$(cat "/sys/class/net/$iface/carrier" 2>/dev/null)
        [ "$carrier" = "1" ] && echo 1 || echo 0
    else
        echo 0
    fi
}

print_usb_port_status() {
    if [ "`ls -1 /sys/bus/usb/devices | wc -l`" -gt 8 ]; then
        echo 1
    else
        echo 0
    fi
}

# 自动检测: swconfig可用则用旧模式, 否则走DSA
# 输出顺序按config.h定义: LAN1, LAN2, LAN3, WAN, USB
if swconfig list 2>/dev/null | grep -q "switch0"; then
    print_eth_port_status_swconfig 1  # Port 1 is LAN1 on label
    print_eth_port_status_swconfig 0  # Port 0 is LAN2 on label
    print_eth_port_status_swconfig 2  # LAN3
    print_eth_port_status_swconfig 3  # WAN
else
    print_eth_port_status_dsa lan1    # LAN1 on label
    print_eth_port_status_dsa lan2    # LAN2 on label
    print_eth_port_status_dsa lan3    # LAN3
    print_eth_port_status_dsa wan     # WAN
fi
print_usb_port_status
