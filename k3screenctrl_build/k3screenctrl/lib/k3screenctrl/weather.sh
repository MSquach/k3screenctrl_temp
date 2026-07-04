#!/bin/sh

. /lib/network/config.sh
. /lib/functions.sh

update_weather=0

update_time=$(uci get k3screenctrl.@general[0].update_time 2>/dev/null)
[ -z "$update_time" ] && update_time=3600

# 预先从uci获取city (供OFF模式显示)
city=$(uci get k3screenctrl.@general[0].city 2>/dev/null)

DATE=$(date "+%Y-%m-%d %H:%M")
DATE_DATE=$(echo "$DATE" | awk '{print $1}')
DATE_TIME=$(echo "$DATE" | awk '{print $2}')
DATE_WEEK=$(date "+%u")
if [ "$DATE_WEEK" = "7" ]; then
	DATE_WEEK=0
fi

if [ "$update_time" -eq 0 ] 2>/dev/null; then
	echo "OFF"$city
	echo "$WENDU"
	echo "$DATE_DATE"
	echo "$DATE_TIME"
	echo "$TYPE"
	echo "$DATE_WEEK"
	echo 0
	exit
fi

cur_time=$(date +%s)
last_time=$(cat /tmp/weather_time 2>/dev/null)
if [ -z "$last_time" ]; then
	update_weather=1
	echo $cur_time > /tmp/weather_time
else
	time_tmp=$((cur_time - last_time))
	if [ "$time_tmp" -ge "$update_time" ] 2>/dev/null; then
		update_weather=1
		echo $cur_time > /tmp/weather_time
	fi
fi

city_checkip=$(uci get k3screenctrl.@general[0].city_checkip 2>/dev/null)

if [ "$city_checkip" = "1" ]; then
	city_tmp=$(cat /tmp/weather_city 2>/dev/null)
	if [ -z "$city_tmp" ]; then
		# 用wget替代curl (OpenWrt默认自带wget)
		wget -q -T 3 -O /tmp/wanip.json "http://pv.sohu.com/cityjson" 2>/dev/null
		wanip=$(grep -oE "([0-9]{1,3}\.){3}[0-9]{1,3}" /tmp/wanip.json 2>/dev/null | head -1)
		rm -f /tmp/wanip.json
		if [ -n "$wanip" ]; then
			wget -q -T 3 -O /tmp/city_json "http://ip.taobao.com/service/getIpInfo.php?ip=$wanip" 2>/dev/null
			city_json=$(cat /tmp/city_json 2>/dev/null)
			rm -f /tmp/city_json
			ip_city=$(echo "$city_json" | jsonfilter -e '@.data.city' 2>/dev/null)
			ip_county=$(echo "$city_json" | jsonfilter -e '@.data.county' 2>/dev/null)
			if [ "$ip_county" != "XX" -a -n "$ip_county" ]; then
				city="$ip_county"
			else
				city="$ip_city"
			fi
		fi
		if [ -n "$city" ]; then
			echo "$city" > /tmp/weather_city
			uci set k3screenctrl.@general[0].city="$city"
			uci commit k3screenctrl
		fi
	else
		city="$city_tmp"
	fi
fi

weather_info=$(cat /tmp/k3-weather.json 2>/dev/null)
if [ -z "$weather_info" ]; then
	update_weather=1
fi

key=$(uci get k3screenctrl.@general[0].key 2>/dev/null)
if [ -z "$key" ]; then
	update_weather=0
fi

if [ "$update_weather" = "1" ]; then
	rm -rf /tmp/k3-weather.json
	wget "http://api.seniverse.com/v3/weather/now.json?key=$key&location=$city&language=zh-Hans&unit=c" -T 3 -O /tmp/k3-weather.json 2>/dev/null
fi

weather_json=$(cat /tmp/k3-weather.json 2>/dev/null)
WENDU=$(echo "$weather_json" | jsonfilter -e '@.results[0].now.temperature' 2>/dev/null)
TYPE=$(echo "$weather_json" | jsonfilter -e '@.results[0].now.code' 2>/dev/null)

#output weather data
echo "$city"
echo "$WENDU"
echo "$DATE_DATE"
echo "$DATE_TIME"
echo "$TYPE"
echo "$DATE_WEEK"
echo 0
