#!/bin/sh
set -eu

ip netns add vlink-ns0 || true
ip netns add vlink-ns1 || true
ip link set vlinklp0 netns vlink-ns0
ip link set vlinklp1 netns vlink-ns1
ip -n vlink-ns0 addr add 10.0.0.1/24 dev vlinklp0
ip -n vlink-ns1 addr add 10.0.0.2/24 dev vlinklp1
ip -n vlink-ns0 link set vlinklp0 up
ip -n vlink-ns1 link set vlinklp1 up
ip netns exec vlink-ns0 ping -c 3 10.0.0.2
