# resources/fuzz — 坏输入样本集（OPT-14）

定向损坏的 PNG 变体（签名截断/IHDR 截断/中段截断/缺 IEND/伪造 40000x40000
尺寸/坏 CRC/字节翻转/全零），由 tests/test_fuzz_smoke.cpp 的内存变异互补
（JPEG/HDR 的随机形态在测试内生成，不入文件集）。新样本请保持「损坏模式
可一句话描述」并同步登记在测试里。
