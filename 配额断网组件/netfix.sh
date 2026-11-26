#!/bin/sh
# netfix.sh —— 开机把 DHCP 下发的 DNS 从"设备自身"改成公共 DNS
# 原因: 本机 dnsmasq 的上游需经 wan1 出网, 而 wan1 是 NOARP 的 ECM 口, 数据靠
#       fastnat 转发, 设备自身没有可用 L3 路径 -> 客户端把 DNS 指向 192.168.0.1
#       时解析必然超时/REFUSED, 表现为"网速慢、连不上"。
# 做法: /etc/rc 会先删掉 /etc_rw/udhcpd.conf 再由固件重写, 所以开机后等它出现,
#       再改 option dns 并重启 udhcpd。
i=0
while [ $i -lt 90 ]; do
    if [ -f /etc_rw/udhcpd.conf ] && grep -q "option dns 192.168.0.1" /etc_rw/udhcpd.conf; then
        sed -i "s/^option dns.*/option dns 223.5.5.5 114.114.114.114 180.76.76.76/" /etc_rw/udhcpd.conf
        pid=$(cat /etc_rw/udhcpd.pid 2>/dev/null)
        [ -n "$pid" ] && kill $pid 2>/dev/null
        sleep 1
        udhcpd /etc_rw/udhcpd.conf
        echo "$(date) netfix: dns patched, udhcpd restarted" >> /mnt/userdata/etc_rw/quota.log
        exit 0
    fi
    i=`expr $i + 1`
    sleep 2
done
echo "$(date) netfix: udhcpd.conf not found/unchanged" >> /mnt/userdata/etc_rw/quota.log
