# mycc Android 实机联调清单

## 验证项（环境就位后逐项跑）

| # | 检查项 | 命令 / 方法 | 预期 |
|---|--------|------------|------|
| 1 | `--target` 经 extra 传递 | 用 NDK clang++ 编译 mycc，`file mycc` | ELF 64-bit LSB, ARM aarch64 |
| 2 | `/bin/sh` 可用 | `ls -l /bin/sh` | 符号链接到 /system/bin/sh 或存在 |
| 3 | 缓存目录落点 | cwd 下跑一次构建，`ls -d .mycc` | `.mycc/` 在 cwd 创建成功且可写 |
| 4 | `-static-libstdc++` 后 std::filesystem 完整 | 编译 mycc 后直接跑 `--help` | 不报 `__cxa_*` / `__gxx_*` 缺失 |
| 5 | `/data/local/tmp` 执行权限 | chmod +x 后执行 | 可运行（/sdcard 不行） |
| 6 | `.myccfp` 写权限（缓存模式核心 I/O 面） | 构建一次，`ls .mycc/*.myccfp` | 文件存在且内容为 16 位 hex |
| 7 | `.myccfp` 在 app 沙箱内 | 同上，跑在 app 域内 | SELinux 域允许 fopen(w) |

## 实测 vs 预测差异定位表

| 现象 | 先看哪里 |
|------|----------|
| mycc 报 `__cxa_*` / `__gxx_*` 缺失 | `-static-libstdc++` 没加，或 NDK sysroot 不完整 |
| `system()` 返回 -1 | `/bin/sh` 缺失或 SELinux 域限制；run_cmd 会打 errno |
| `.mycc/` 创建失败 | cwd 不可写 → 换 `--obj-dir` 到可写位置，或 cd 到可写目录再跑 |
| 编译成功但 `.myccfp` 没生成 | worker 的 write_fingerprint 分支，看是否打了 `cannot write fingerprint` 警告 |
| 一次命中后第二次又全量 | `.myccfp` 写了但读不到 → SELinux 禁 open-for-read，或 cwd 被清 |
| `mv --obj-dir` 后全量重编 | `build_compile_cmd` 的 fp_cmd 分支，检查 `-o` 是否被误拼进指纹 |

## 备注

- 缓存默认开启，全部中间产物在 cwd 的 `.mycc/` 下；任何时候
  `rm -rf .mycc` 安全，下次自动重建。
- 第 6/7 项写失败时功能上 fail-safe（警告 + 永远重编），但若每次都失败
  会退化为"永远全量"，需确认 SELinux 是否放行。
- 头文件依赖追踪（depfile 路线）是否可行，依赖本联调确认 clang 在
  Android 上的 `-MD -MF` 行为，见 mycc.cpp 头部 ARCHIVED NOTES 第 3 条。
