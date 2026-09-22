#!/bin/bash

set -euo pipefail

MTU=9000
TOPO_DIR=$(cd "$(dirname "$0")" && pwd)
hosts=(host1 host2 host3 host4 host5 host6 host7 host8)
routers=(router-root router-l router-r router-ll router-lr router-rl router-rr)
nodes=("${routers[@]}" "${hosts[@]}")

links=(
  host1 host1-eth0 router-ll rll-h1s0
  host1 host1-eth1 router-ll rll-h1s1
  host2 host2-eth0 router-ll rll-h2s0
  host2 host2-eth1 router-ll rll-h2s1
  host3 host3-eth0 router-lr rlr-h3s0
  host3 host3-eth1 router-lr rlr-h3s1
  host4 host4-eth0 router-lr rlr-h4s0
  host4 host4-eth1 router-lr rlr-h4s1
  host5 host5-eth0 router-rl rrl-h5s0
  host5 host5-eth1 router-rl rrl-h5s1
  host6 host6-eth0 router-rl rrl-h6s0
  host6 host6-eth1 router-rl rrl-h6s1
  host7 host7-eth0 router-rr rrr-h7s0
  host7 host7-eth1 router-rr rrr-h7s1
  host8 host8-eth0 router-rr rrr-h8s0
  host8 host8-eth1 router-rr rrr-h8s1
  router-root r0-l-down router-l rl-up
  router-l rl-down router-root r0-l-up
  router-root r0-r-down router-r rr-up
  router-r rr-down router-root r0-r-up
  router-l rl-ll-down router-ll rll-up
  router-ll rll-down router-l rl-ll-up
  router-l rl-lr-down router-lr rlr-up
  router-lr rlr-down router-l rlr-up
  router-r rr-rl-down router-rl rrl-up
  router-rl rrl-down router-r rr-rl-up
  router-r rr-rr-down router-rr rrr-up
  router-rr rrr-down router-r rr-rr-up
)

mac_for() {
  printf '02:62:%02x:%02x:%02x:%02x' $(((1 + $1 >> 24) & 255)) $(((1 + $1 >> 16) & 255)) $(((1 + $1 >> 8) & 255)) $(((1 + $1) & 255))
}

generate_tree_cfg() {
  local cfg="$TOPO_DIR/tree.generated.cfg"
  local r rank sub leaf major leaf_port parent_port
  : > "$cfg"

  cat >> "$cfg" <<'EOF'
# Generated three-level complete binary tree configuration.
EOF
  for r in router-root router-l router-r router-ll router-lr router-rl router-rr; do
    case "$r" in
      router-root) for p in r0-l-down r0-l-up r0-r-down r0-r-up; do echo "dev,$r,$p" >> "$cfg"; done ;;
      router-l) for p in rl-up rl-down rl-ll-down rl-ll-up rl-lr-down rl-lr-up; do echo "dev,$r,$p" >> "$cfg"; done ;;
      router-r) for p in rr-up rr-down rr-rl-down rr-rl-up rr-rr-down rr-rr-up; do echo "dev,$r,$p" >> "$cfg"; done ;;
      router-ll) for p in rll-up rll-down rll-h1s0 rll-h1s1 rll-h2s0 rll-h2s1; do echo "dev,$r,$p" >> "$cfg"; done ;;
      router-lr) for p in rlr-up rlr-down rlr-h3s0 rlr-h3s1 rlr-h4s0 rlr-h4s1; do echo "dev,$r,$p" >> "$cfg"; done ;;
      router-rl) for p in rrl-up rrl-down rrl-h5s0 rrl-h5s1 rrl-h6s0 rrl-h6s1; do echo "dev,$r,$p" >> "$cfg"; done ;;
      router-rr) for p in rrr-up rrr-down rrr-h7s0 rrr-h7s1 rrr-h8s0 rrr-h8s1; do echo "dev,$r,$p" >> "$cfg"; done ;;
    esac
  done

  for r in router-root router-l router-r router-ll router-lr router-rl router-rr; do
    for rank in {0..7}; do
      case "$r:$rank" in
        router-root:0|router-root:1|router-root:2|router-root:3) leaf_port=r0-l-down ;;
        router-root:*) leaf_port=r0-r-down ;;
        router-l:0|router-l:1) leaf_port=rl-ll-down ;;
        router-l:2|router-l:3) leaf_port=rl-lr-down ;;
        router-l:*) leaf_port=rl-up ;;
        router-r:4|router-r:5) leaf_port=rr-rl-down ;;
        router-r:6|router-r:7) leaf_port=rr-rr-down ;;
        router-r:*) leaf_port=rr-up ;;
        router-ll:0) leaf_port=rll-h1s0 ;;
        router-ll:1) leaf_port=rll-h2s0 ;;
        router-ll:*) leaf_port=rll-down ;;
        router-lr:2) leaf_port=rlr-h3s0 ;;
        router-lr:3) leaf_port=rlr-h4s0 ;;
        router-lr:*) leaf_port=rlr-down ;;
        router-rl:4) leaf_port=rrl-h5s0 ;;
        router-rl:5) leaf_port=rrl-h6s0 ;;
        router-rl:*) leaf_port=rrl-down ;;
        router-rr:6) leaf_port=rrr-h7s0 ;;
        router-rr:7) leaf_port=rrr-h8s0 ;;
        router-rr:*) leaf_port=rrr-down ;;
      esac
      case "$r" in
        router-root) parent_port=${leaf_port/down/up} ;;
        router-l) parent_port=${leaf_port/down/up} ;;
        router-r) parent_port=${leaf_port/down/up} ;;
        router-ll|router-lr|router-rl|router-rr) parent_port=${leaf_port/down/up} ;;
      esac
      for sub in 0 1; do echo "route,$r,$rank,$sub,$leaf_port" >> "$cfg"; done
      for sub in 0 1; do echo "tree,$r,$rank,$sub,LEVEL,$leaf_port,$parent_port" >> "$cfg"; done
      case "$r" in
        router-root) for sub in 0 1; do echo "mcast,$r,$rank,$sub,r0-l-down" >> "$cfg"; echo "mcast,$r,$rank,$sub,r0-r-down" >> "$cfg"; done ;;
        router-l) for sub in 0 1; do echo "mcast,$r,$rank,$sub,rl-ll-down" >> "$cfg"; echo "mcast,$r,$rank,$sub,rl-lr-down" >> "$cfg"; done ;;
        router-r) for sub in 0 1; do echo "mcast,$r,$rank,$sub,rr-rl-down" >> "$cfg"; echo "mcast,$r,$rank,$sub,rr-rr-down" >> "$cfg"; done ;;
        router-ll) for sub in 0 1; do echo "mcast,$r,$rank,$sub,rll-h1s$sub" >> "$cfg"; echo "mcast,$r,$rank,$sub,rll-h2s$sub" >> "$cfg"; done ;;
        router-lr) for sub in 0 1; do echo "mcast,$r,$rank,$sub,rlr-h3s$sub" >> "$cfg"; echo "mcast,$r,$rank,$sub,rlr-h4s$sub" >> "$cfg"; done ;;
        router-rl) for sub in 0 1; do echo "mcast,$r,$rank,$sub,rrl-h5s$sub" >> "$cfg"; echo "mcast,$r,$rank,$sub,rrl-h6s$sub" >> "$cfg"; done ;;
        router-rr) for sub in 0 1; do echo "mcast,$r,$rank,$sub,rrr-h7s$sub" >> "$cfg"; echo "mcast,$r,$rank,$sub,rrr-h8s$sub" >> "$cfg"; done ;;
      esac
    done
  done

  for rank in {0..7}; do
    for r in {0..7}; do
      [ "$rank" -eq "$r" ] && continue
      leaf=$((rank / 2)); major=$((rank / 4));
      local_leaf=$((r / 2)); local_major=$((r / 4));
      if [ "$leaf" -eq "$local_leaf" ]; then fan0=1; else fan0=2; fi
      if [ "$major" -eq "$local_major" ]; then fan1=3; else fan1=4; fi
      for sub in 0 1; do echo "req,$r,$rank,$sub,3,$fan0,$fan1,7" >> "$cfg"; done
    done
  done
}

setup() {
  local i c1 v1 c2 v2 r
  generate_tree_cfg
  for node in "${nodes[@]}"; do
    echo "Creating and starting $node"
    docker container rm -f "$node" >/dev/null 2>&1 || true
    docker container create --cap-add NET_ADMIN --name "$node" -v "$(cd "$TOPO_DIR/../.." && pwd)":/app node >/dev/null
    docker container start "$node" >/dev/null
  done
  for ((i=0; i<${#links[@]}; i+=4)); do
    c1=${links[i]}; v1=${links[i+1]}; c2=${links[i+2]}; v2=${links[i+3]}
    echo "Link $c1($v1) <-> $c2($v2)"
    sudo ip link add "${v1}_tmp" type veth peer name "${v2}_tmp"
    sudo ip link set "${v1}_tmp" address "$(mac_for $((i+1)))"
    sudo ip link set "${v2}_tmp" address "$(mac_for $((i+2)))"
    sudo ip link set "${v1}_tmp" netns "$(docker inspect -f '{{.State.Pid}}' "$c1")"
    sudo ip link set "${v2}_tmp" netns "$(docker inspect -f '{{.State.Pid}}' "$c2")"
    docker exec "$c1" ip link set "${v1}_tmp" name "$v1"
    docker exec "$c2" ip link set "${v2}_tmp" name "$v2"
    docker exec "$c1" ip link set "$v1" up
    docker exec "$c2" ip link set "$v2" up
    docker exec "$c1" ip link set dev "$v1" mtu "$MTU"
    docker exec "$c2" ip link set dev "$v2" mtu "$MTU"
    docker exec "$c1" ip link set dev "$v1" promisc on
    docker exec "$c2" ip link set dev "$v2" promisc on
  done
  for r in {1..8}; do docker exec "host$r" ip addr add "10.3.0.$r/24" dev "host$r-eth0"; done
}

clean() {
  for node in "${nodes[@]}"; do docker container rm -f "$node" >/dev/null 2>&1 || true; done
  rm -f "$TOPO_DIR/tree.generated.cfg"
}

case "${1:-}" in
  setup) setup ;;
  clean) clean ;;
  *) echo "Usage: $0 {setup|clean}"; exit 1 ;;
esac
