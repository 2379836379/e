# Arbor Lab

当前实现采用论文语义的 `requester + switch + responder` 协议路径：
- requester 在每个 subchannel 上发送 `REGISTER`，收到 `credit` 后发送 `REQUEST`
- switch 旁路 `REGISTER / RESPONSE / REPAIR_TRIGGER / REPAIR_REQUEST / END / END_ACK`，仅对 normal `REQUEST` 按 `(channel_id, subchannel_id, credit_offset, agg_loc)` 进行 slot 聚合
- responder 发放 credit、处理正常完成、repair trigger 和 END 握手

线上的包格式为：
- `Ethernet + IPv4 + UDP + arbor_header_t + payload`

目录约定：
- 容器内统一以 `/app` 为工作目录
- 二进制来自 `/app/build/inc`
- 测试配置来自 `/app/tests/config/`
- 测试输入来自 `/app/tests/data/current/`
- 测试输出和日志写回 `/app/tests/out/`

当前测试入口：
- `make test_allreduce`
