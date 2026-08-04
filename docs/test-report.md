# 压测报告 — iot-gateway

> 来源：`tests/integration/stress_test.py` 实测输出（历史数据存档）。

---

## 1. 测试环境

- WSL2 Ubuntu 22.04，GCC 13.3.0，Release（`-O2`）
- 本机回环 `127.0.0.1`，Mosquitto Broker 本地
- 复现命令：

```bash
# 小规模
python3 tests/integration/stress_test.py -c 10 -f 10 -d 3 -r /tmp/stress_small.txt
# 大规模
python3 tests/integration/stress_test.py -c 100 -f 10 -d 60 -r /tmp/stress_large.txt
```

## 2. 结果

| 指标 | 小规模 (10 conn / 3s) | 大规模 (100 conn / 60s) |
|---|---|---|
| 吞吐量 (QPS) | 99.9 | 991.1 |
| 总帧数 | 300 | 59,500 |
| P50 延迟 | 0.044 ms | 0.043 ms |
| P99 延迟 | 0.370 ms | 0.241 ms |
| 平均延迟 | 0.065 ms | 0.061 ms |
| 最大延迟 | 0.444 ms | 4.161 ms |
| 错误率 | 0.00% | 0.00% |
| 连接成功率 | 100% (10/10) | 100% (100/100) |

## 3. 大规模原始输出（存档）

```
Target:          127.0.0.1:8080
Connections:     100
Target FPS:      10.0 per connection
Target Duration: 60.0s
Actual Duration: 60.04s

Total frames sent:     59500
Send errors:           0
Connect failures:      0
Effective QPS:         991.1
Error rate:            0.00%

Avg latency:           0.061 ms
P50 latency:           0.043 ms
P99 latency:           0.241 ms
Min latency:           0.021 ms
Max latency:           4.161 ms

Successful connections: 100
Failed connections:     0
Avg frames/conn:        595.0
```

## 4. 结论

- 100 并发下稳态 QPS ≈ 991，P99 延迟 < 0.25ms，零错误率，满足传感器高频上报场景。
- 延迟几乎不随并发增长（P50 0.044 → 0.043ms），说明任务队列 + 线程池未形成瓶颈。
