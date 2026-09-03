#!/bin/bash
set -euo pipefail
hosts=(host1 host2 host3 host4)
routers=(router1)
nodes=("${routers[@]}" "${hosts[@]}")
links=(
  host1 host1-eth0 router1 r1-h1s0
  host2 host2-eth0 router1 r1-h2s0
  host3 host3-eth0 router1 r1-h3s0
  host4 host4-eth0 router1 r1-h4s0
)
mac_for() { printf '02:52:%02x:%02x:%02x:%02x' $((($1 >> 24) & 255)) $((($1 >> 16) & 255)) $((($1 >> 8) & 255)) $(($1 & 255)); }
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
  done
  for r in 1 2 3 4; do docker exec host$r ip addr add "10.1.0.$r/24" dev host$r-eth0; done
}
clean() { for node in "${nodes[@]}"; do docker container rm -f "$node" >/dev/null 2>&1 || true; done; }
case "${1:-}" in setup) setup ;; clean) clean ;; *) echo "Usage: $0 {setup|clean}"; exit 1 ;; esac
