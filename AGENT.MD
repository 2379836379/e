
  1. op 被硬编码，且与参考实现含义冲突。
     当前 wire/arbor_wire.c:108 固定写入 OP_ALLREDUCE=2，request()/respond() 传入的 op 被丢弃。参考实现中
     SUM=1、MAX=2、MIN=3。因此当前包会被参考实现解释为 MAX，而不是 SUM。

  2. dtype 和 payload_kind 也没有按消息描述传递。
     当前 wire/arbor_wire.c:111 根据“是否有 payload”写 dtype，空 payload 时写 0；参考实现会在 credit/request/
     response 中保留 MessageDesc.dtype。另外当前无 payload 包固定写 COMPLETION，参考实现的普通 credit/control
     包默认是 DATA，只有携带 completion payload 时才使用 COMPLETION。

  4. 当前实现不支持参考实现的非对齐尾包。
     当前 protocol/requester.c:269 和 protocol/responder.c:1113 使用 size / PAYLOAD_LEN，尾部不足一个包的数据
     会被直接忽略。参考实现按向上取整计算 packet 数，并在 reference/arbor-host.cc:199 处理最后一个短 payload。

  5. AGG_MISS 的 repair 查找使用了错误的 offset。
     路由器改写 AGG_MISS 时保留 offsetA、清零 offsetB。当前 responder 在 protocol/responder.c:1296 却先用
     m.credit_offset（即 offsetB，通常为 0）查找 message；参考实现 reference/arbor-host.cc:881 使用 h.offsetA
     作为 credit offset。单 message、epoch=0 时可能偶然正常，跨 message 或非零 epoch 时会错误丢弃 repair。

  6. 修改 ECN 后没有更新 IPv4 checksum。
     当前 requester 在 protocol/requester.c:226 构帧后直接修改 ip->tos；router 的 protocol/router.c:628 也会修
     改聚合包的 ECN，但没有重算 checksum。参考实现通过 ns-3 的 IPv4 header 序列化重新生成校验和。


  9. 当前只支持 group 0，参考实现支持复合 (group, channel)。
     当前 wire/arbor_wire.c:32 固定 group_id=0，并对 channel 取模 128；参考实现端口编码包含 group，使用
     ChannelKey(group, channel)。

  11. 聚合运算语义不同。
     当前 router/responder 实际执行固定的 int32 加法（protocol/router.c:616、protocol/responder.c:646），不支
     持参考实现声明的 SUM/MAX/MIN 和 dtype gate。参考实现的数据面模型本身不计算数值，但协议字段和操作类型是可
     配置的。


