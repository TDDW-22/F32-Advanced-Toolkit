#!/bin/sh
# fwguard: 9090(AT通道)/8080(短信API) 仅限内网，WAN 侧一律丢弃
for p in 9090 8080; do
  iptables -C INPUT -i wan1 -p tcp --dport $p -j DROP 2>/dev/null || iptables -I INPUT -i wan1 -p tcp --dport $p -j DROP
done
( sleep 25; sh /mnt/userdata/etc_rw/netfix.sh ) &
