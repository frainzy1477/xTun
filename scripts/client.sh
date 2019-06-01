#!/bin/sh

IFACE=tun0
CIDR=10.0.0.2/24
SERVER=216.250.96.80
PORT=1082
PASSWORD=password

IP_ROUTE_TABLE=xTun
FWMARK="0x023/0x023"
SETNAME=wall
CHAIN=xTun
DNS=8.8.8.8
BLACK_LIST=black_list

start() {
    xTun -i $IFACE -I $CIDR -k $PASSWORD -c $SERVER -p $PORT
    net_start
    acl add
}

stop() {
    net_stop
    acl del
    xTun --signal stop
}

shutdown() {
    net_stop
    acl del
    xTun --signal quit
}

net_start() {
    sysctl -w net.ipv4.ip_forward=1 >> /dev/null

    for f in /proc/sys/net/ipv4/conf/*/rp_filter; do
        echo 0 > $f
    done

    iptables -t nat -N $CHAIN > /dev/null 2>&1 || (
        iptables -t nat -D POSTROUTING -j $CHAIN
        iptables -t nat -F $CHAIN
        iptables -t nat -Z $CHAIN
    )
    iptables -t nat -A $CHAIN -o $IFACE -j MASQUERADE
    iptables -t nat -A POSTROUTING -j $CHAIN

    iptables -N $CHAIN > /dev/null 2>&1 || (
        iptables -D FORWARD -j $CHAIN
        iptables -F $CHAIN
        iptables -Z $CHAIN
    )
    iptables -I $CHAIN 1 -i $IFACE -m state --state RELATED,ESTABLISHED -j ACCEPT
    iptables -I $CHAIN 1 -o $IFACE -j ACCEPT
    iptables -I FORWARD -j $CHAIN

    iptables -t mangle -N $CHAIN > /dev/null 2>&1 || (
        iptables -t mangle -D PREROUTING -j $CHAIN
        iptables -t mangle -D OUTPUT -j $CHAIN
        iptables -t mangle -F $CHAIN
        iptables -t mangle -Z $CHAIN
    )
    ipset -N $SETNAME iphash -exist
    iptables -t mangle -A $CHAIN -m set --match-set $SETNAME dst -j MARK --set-mark $FWMARK
    iptables -t mangle -A PREROUTING -j $CHAIN
    iptables -t mangle -A OUTPUT -j $CHAIN

    xTun_rule_ids=`ip rule list | grep "lookup $IP_ROUTE_TABLE" | sed 's/://g' | awk '{print $1}'`
    for rule_id in $xTun_rule_ids
    do
        ip rule del prio $rule_id
    done

    CHKIPROUTE=$(grep $IP_ROUTE_TABLE /etc/iproute2/rt_tables)
    if [ -z "$CHKIPROUTE" ]; then
        echo "200 $IP_ROUTE_TABLE" >> /etc/iproute2/rt_tables
    fi

    ip route add default dev $IFACE table $IP_ROUTE_TABLE
    ip route list | grep -q "$DNS dev $IFACE" || ip route add $DNS dev $IFACE
    ip rule list | grep -q "fwmark $FWMARK lookup $IP_ROUTE_TABLE" || ip rule add fwmark $FWMARK table $IP_ROUTE_TABLE

    ip route flush cache
}

net_stop() {
    iptables -t nat -D POSTROUTING -j $CHAIN > /dev/null 2>&1
    iptables -t nat -F $CHAIN > /dev/null 2>&1
    iptables -t nat -X $CHAIN > /dev/null 2>&1

    iptables -D FORWARD -j $CHAIN > /dev/null 2>&1
    iptables -F $CHAIN > /dev/null 2>&1
    iptables -X $CHAIN > /dev/null 2>&1

    iptables -t mangle -D PREROUTING -j $CHAIN > /dev/null 2>&1
    iptables -t mangle -D OUTPUT -j $CHAIN > /dev/null 2>&1
    iptables -t mangle -F $CHAIN > /dev/null 2>&1
    iptables -t mangle -X $CHAIN > /dev/null 2>&1

    ip route del default dev $IFACE table $IP_ROUTE_TABLE > /dev/null 2>&1
    ip route del $DNS dev $IFACE > /dev/null 2>&1
    xTun_rule_ids=`ip rule list | grep "lookup $IP_ROUTE_TABLE" | sed 's/://g' | awk '{print $1}'`
    for rule_id in $xTun_rule_ids
    do
        ip rule del prio $rule_id
    done

    ip route flush cache
}

acl() {
    if [ ! -f $BLACK_LIST ]; then
        return
    fi

    while read line;do
        [ -z "$line" ] && continue
        case "$line" in \#*) continue ;; esac
        ipset $1 $SETNAME $line --exist
    done < $BLACK_LIST
}

show_help() {
    echo "Usage: $ProgName <command> [options]"
    echo "Commands:"
    echo "    start   start tun"
    echo "    stop    stop tun"
    echo ""
    echo "For help with each command run:"
    echo "$ProgName <command> -h|--help"
    echo ""
}

ProgName=$(basename $0)

command=$1
case $command in
    "" | "-h" | "--help")
        show_help
        ;;
    *)
        shift
        ${command} $@
        if [ $? = 127 ]; then
            echo "Error: '$command' is not a known command." >&2
            echo "       Run '$ProgName --help' for a list of known commands." >&2
            exit 1
        fi
        ;;
esac
