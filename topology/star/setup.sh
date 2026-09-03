#!/bin/bash
set -euo pipefail

hosts=(host1 host2 host3 host4)
routers=(router1 router2)
nodes=("${routers[@]}" "${hosts[@]}")
links=(
  host1 host1-eth0 router1 r1-h1s0
  host1 host1-eth1 router2 r2-h1s1
  host2 host2-eth0 router1 r1-h2s0
  host2 host2-eth1 router2 r2-h2s1
  host3 host3-eth0 router1 r1-h3s0
  host3 host3-eth1 router2 r2-h3s1
  host4 host4-eth0 router1 r1-h4s0
  host4 host4-eth1 router2 r2-h4s1
)
ips=(
  host1-eth0 10.0.0.1/24 host1-eth1 10.0.0.1/24
  host2-eth0 10.0.0.2/24 host2-eth1 10.0.0.2/24
  host3-eth0 10.0.0.3/24 host3-eth1 10.0.0.3/24
  host4-eth0 10.0.0.4/24 host4-eth1 10.0.0.4/24
)
mac_for() { printf '02:42:%02x:%02x:%02x:%02x' $((($1 >> 24) & 255)) $((($1 >> 16) & 255)) $((($1 >> 8) & 255)) $(($1 & 255)); }
setup() {
  local i c1 v1 c2 v2
  for node in "${nodes[@]}"; do
    docker container rm -f "$node" >/dev/null 2>&1 || true
    docker container create --cap-add NET_ADMIN --name "$node" -v "$(pwd)":/app node >/dev/null
    docker container start "$node" >/dev/null
  done
  for ((i=0; i<${#links[@]}; i+=4)); do
    c1=${links[i]}; v1=${links[i+1]}; c2=${links[i+2]}; v2=${links[i+3]}
    sudo ip link add "${v1}_tmp" type veth peer name "${v2}_tmp"
    sudo ip link set "${v1}_tmp" address "$(mac_for $((i+1)))"
    sudo ip link set "${v2}_tmp" address "$(mac_for $((i+2)))"
    sudo ip link set "${v1}_tmp" netns "$(docker inspect -f '{{.State.Pid}}' "$c1")"
    sudo ip link set "${v2}_tmp" netns "$(docker inspect -f '{{.State.Pid}}' "$c2")"
    docker exec "$c1" ip link set "${v1}_tmp" name "$v1"
    docker exec "$c2" ip link set "${v2}_tmp" name "$v2"
    docker exec "$c1" ip link set "$v1" up
    docker exec "$c2" ip link set "$v2" up
    docker exec "$c1" ip link set dev "$v1" promisc on
    docker exec "$c2" ip link set dev "$v2" promisc on
  done
  for ((i=0; i<${#ips[@]}; i+=2)); do
    iface=${ips[i]}; addr=${ips[i+1]}; container=${iface%-*}
    docker exec "$container" ip addr add "$addr" dev "$iface"
  done
}
clean() { for node in "${nodes[@]}"; do docker container rm -f "$node" >/dev/null 2>&1 || true; done; }
case "${1:-}" in setup) setup ;; clean) clean ;; *) echo "Usage: $0 {setup|clean}"; exit 1 ;; esac
