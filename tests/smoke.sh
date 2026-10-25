#!/usr/bin/env bash
# mycc 冒烟测试 — Linux 桌面 (CLI 简化版: 默认缓存 .mycc/, 默认输出名派生)
# 覆盖: 两词调用 / 默认输出名 / 缓存命中 / mtime 失效 / 参数指纹 /
#       -o 无关性 / 失败语义 / --no-cache
#
# ⚠️ 预期失败提示: 测试 6 的第一条 mycc 命令【预期失败】(b.cpp 被故意写坏)。
#    断言只看退出码, stderr 保持可见以便确认失败原因是 b.cpp 编译错误;
#    若该命令意外成功, 说明失败检测失效, 才是 bug。
#
# 用法: 仓库根目录下执行  bash tests/smoke.sh
set -euo pipefail
cd "$(dirname "$0")/.."          # 始终在仓库根目录执行

# 0) 构建 mycc 本体 (-Wall -Wextra, 有残留警告顺手收掉)
g++ -std=c++17 -O2 mycc.cpp -o mycc -Wall -Wextra

# 0) 最小测试项目
rm -rf t && mkdir -p t/src
cat > t/src/a.cpp <<'EOF'
#include <cstdio>
int b();
int main(){ std::printf("%d\n", b()); }
EOF
cat > t/src/b.cpp <<'EOF'
int b(){ return 42; }
EOF
cd t
MYCC=../mycc

echo "=== 1) 两词调用 + 首次全量 (无 --obj-dir, 缓存默认开) ==="
$MYCC src -o app -v
test -d .mycc && test -f .mycc/a.o && test -f .mycc/a.o.myccfp

echo "=== 2) 原样重跑: 期望 cache hit x2, 直接链接 ==="
$MYCC src -o app -v

echo "=== 3) touch b.cpp: 期望仅 b.cpp 重编, a.cpp 命中 ==="
touch src/b.cpp
$MYCC src -o app -v

echo "=== 4) 追加 -O2: 期望全量重编 (参数指纹变化) ==="
$MYCC src -o app -v -O2

echo "=== 5) mv .mycc .mycc2 + --obj-dir .mycc2: 期望仍全部命中 (指纹不含 -o) ==="
mv .mycc .mycc2
$MYCC src -o app -v --obj-dir .mycc2

echo "=== 6) 失败语义 (接 .mycc2 缓存) ==="
echo 'syntax error' >> src/b.cpp
# 注意: 断言用 if, 不用 "cmd && echo" 短路形式 —
#   set -e 只对 && 列表的末位命令生效, 非末位失败会被静默跳过
if $MYCC src -o app -v --obj-dir .mycc2; then
    echo "UNEXPECTED: bad source compiled successfully — failure detection broken!"
    exit 1
fi
echo "expect_fail_here: b.cpp 编译失败 (见上方 stderr), 符合预期; a.o 与指纹应保留"
sed -i '$d' src/b.cpp
$MYCC src -o app -v --obj-dir .mycc2   # 期望 a.cpp 命中, 仅 b.cpp 重编

# 端到端断言: 验证输出内容而非仅退出码
#   赋值形式 out=$(...) 不受 && 豁免影响: app 崩溃时 set -e 直接中断
out="$(./app)"
if [ "$out" != "42" ]; then
    echo "UNEXPECTED: app printed '$out' (expected 42)"
    exit 1
fi
echo "RUN OK (42)"

echo "=== 7) 单文件默认输出名: src/hello.c -> ./hello ==="
cat > src/hello.c <<'EOF'
#include <cstdio>
int main(){ std::printf("hi\n"); return 0; }
EOF
$MYCC src/hello.c -v        # 注意: 无 -o, 应产出 ./hello (全 .c -> cc 链接)
test -x hello
./hello

echo "=== 8) --no-cache: 强制全量重编 ==="
$MYCC src -o app -v --no-cache

cd ..
echo "=== ALL SMOKE TESTS PASSED ==="
