// SPDX-License-Identifier: Apache-2.0
//
// version_test -- 版本比较 + JSON 取值。
//
// 这两个都是**纯函数**：不依赖声卡、网络、设备，几毫秒跑完。
// 2026-10-18 外部审查（P3-3）点的就是这个缺口 —— 项目里最容易出错、
// 又最容易测的一类函数，一个测试都没有。
//
// 「1.2.9 vs 1.2.10」那条是刻意放的：字符串比较会得到**相反**结论
// （'9' > '1'），那正是当初把比较写成「按段拆数字」的原因。

#include "version.h"
#include "config_store.h"

#include <cstdio>
#include <string>

using namespace fxchain;

namespace {

int g_fail = 0;
int g_checks = 0;

void check(bool condValue, const std::string& what) {
    ++g_checks;
    if (!condValue) {
        ++g_fail;
        std::printf("      FAIL  %s\n", what.c_str());
    }
}

// expected = 「remote 比 local 新吗」
void vt(const char* remote, const char* local, bool expected) {
    const bool got = isNewerVersion(remote, local);
    ++g_checks;
    if (got != expected) {
        ++g_fail;
        std::printf("      FAIL  isNewerVersion(\"%s\", \"%s\") = %d，期望 %d\n",
                    remote, local, int(got), int(expected));
    }
}

}  // namespace

int main() {
    std::printf("\n1. 版本比较\n");
    // 常规
    vt("1.5.22", "1.5.21", true);
    vt("1.5.21", "1.5.22", false);
    vt("1.5.22", "1.5.22", false);        // 相同不算更新
    // ★ 字符串比较会在这里出错："1.2.9" > "1.2.10"（字符 9 > 1）
    vt("1.2.9", "1.2.10", false);
    vt("1.2.10", "1.2.9", true);
    // v 前缀（GitHub 的 tag 就是 v1.5.22）
    vt("v1.5.22", "1.5.22", false);
    vt("V1.6.0", "1.5.22", true);
    // 段数不等：缺的段按 0 算
    vt("1.5", "1.5.0", false);
    vt("1.5.1", "1.5", true);
    vt("1.5.0.1", "1.5", true);
    // 大版本
    vt("2.0.0", "1.9.9", true);
    vt("1.9.9", "2.0.0", false);
    // 预发布后缀：遇到非数字就停，所以同号时不算更新
    vt("1.5.22-rc1", "1.5.22", false);
    vt("1.5.23-rc1", "1.5.22", true);
    // 两位数进位（最容易踩的边界）
    vt("1.10.0", "1.9.0", true);
    vt("1.9.0", "1.10.0", false);

    std::printf("\n2. JSON 取值（手写解析器）\n");
    {
        const std::string j = "{\"a\": \"hello\", \"n\": 42, \"b\": true}";
        check(jsonGetString(j, "a") == "hello", "取字符串");
        check(jsonGetString(j, "missing") == "", "缺键 -> 空串");
        check(jsonGetInt(j, "n", -1) == 42, "取整数");
        check(jsonGetInt(j, "missing", 7) == 7, "缺键 -> 用默认值");
        check(jsonGetBool(j, "b", false) == true, "取布尔");
        check(jsonGetBool(j, "missing", true) == true, "缺键 -> 用默认值");
    }
    {
        // 转义：设备名里含反斜杠（saveConfig 会写成 \\）
        const std::string j = "{\"name\": \"a\\\\b\"}";
        check(jsonGetString(j, "name") == "a\\b", "解析转义的反斜杠");
    }
    {
        // 键名互为前缀时不能取错
        const std::string j = "{\"vol\": 1, \"volume\": 2}";
        check(jsonGetInt(j, "volume", -1) == 2, "键名互为前缀时取对");
    }
    // 空串 / 垃圾输入不崩
    check(jsonGetString("", "a") == "", "空串输入");
    check(jsonGetInt("garbage", "a", 9) == 9, "垃圾输入 -> 默认值");
    check(jsonGetBool("{}", "a", true) == true, "空对象 -> 默认值");

    std::printf("\n");
    if (g_fail) {
        std::printf("   %d/%d 项失败\n", g_fail, g_checks);
        return 1;
    }
    std::printf("   全部通过（%d 项）\n", g_checks);
    return 0;
}
