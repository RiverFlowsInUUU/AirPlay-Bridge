// SPDX-License-Identifier: Apache-2.0
//
// tooltip_text.h 的回归测试。
//
// 钉住的是一类「短名字机器上看不出来」的错误：
// szTip 只有 128 字符，单行写「已连接（设备名：IP）」时名字一长就把 IP 挤掉，
// 而 IP 正是区分两台同名 HomePod 的依据。
//
// Windows 控制台默认代码页解不了 UTF-8 中文，所以断言失败时打印的是
// 字节数/长度而不是原文 —— 够定位就行。

#include "tooltip_text.h"

#include <cstdio>
#include <string>

using namespace fxchain;

namespace {

int g_fail = 0;
int g_checks = 0;

// 只按结构断言：行数、每行内容。避免依赖控制台编码。
void expectLines(const std::string& got, const std::string& want,
                 const char* why) {
    ++g_checks;
    if (got != want) {
        ++g_fail;
        std::printf("      FAIL  %s\n", why);
        std::printf("        实际 %d 字节 / 期望 %d 字节\n",
                    int(got.size()), int(want.size()));
    }
}

int countLines(const std::string& s) {
    int n = 1;
    for (char c : s) {
        if (c == '\n') ++n;
    }
    return n;
}

void expectLineCount(const std::string& got, int want, const char* why) {
    ++g_checks;
    const int n = countLines(got);
    if (n != want) {
        ++g_fail;
        std::printf("      FAIL  %s：%d 行，期望 %d 行\n", why, n, want);
    }
}

}  // namespace

int main() {
    const char* kLabel = "已连接";

    std::printf("\n1. 名字 + IP -> 三行\n");
    {
        const std::string got =
            connectedTipBody("客厅 HomePod", "192.168.1.50", kLabel);
        expectLineCount(got, 3, "名字和 IP 不同");
        expectLines(got, std::string(kLabel) + "\n客厅 HomePod\n192.168.1.50",
                    "三行内容");
    }

    std::printf("\n2. 只有名字（拿不到 IP）-> 两行，不留空行\n");
    {
        const std::string got = connectedTipBody("客厅 HomePod", "", kLabel);
        expectLineCount(got, 2, "无 IP");
        expectLines(got, std::string(kLabel) + "\n客厅 HomePod", "两行内容");
    }

    std::printf("\n3. ★ 名字为空 -> 退回 IP，且 IP 只出现一次\n");
    {
        // 关键点：deviceName 空时 who 就是 host，
        // 若照「名字 + IP」的规则继续补 IP 行，同一个地址会出现两次。
        const std::string got = connectedTipBody("", "192.168.1.50", kLabel);
        expectLineCount(got, 2, "名字为空");
        expectLines(got, std::string(kLabel) + "\n192.168.1.50", "两行内容");
    }

    std::printf("\n4. 名字和 IP 都没有 -> 只剩一行状态\n");
    {
        const std::string got = connectedTipBody("", "", kLabel);
        expectLineCount(got, 1, "两者皆空");
        expectLines(got, std::string(kLabel), "只有状态行");
    }

    std::printf("\n5. ★ 长名字截断，IP 不能被挤掉\n");
    {
        const std::string longName(40, 'A');   // 40 字节，远超上限 22
        const std::string got =
            connectedTipBody(longName, "192.168.1.50", kLabel);
        expectLineCount(got, 3, "长名字仍要保持三行");
        expectLines(got, std::string(kLabel) + "\n" +
                             longName.substr(0, 22) + "...\n192.168.1.50",
                    "名字截到 22 字节后接 ...，IP 完整保留");
    }

    std::printf("\n6. 恰好等于上限的名字不截断\n");
    {
        const std::string name(22, 'B');       // 正好 22 字节
        const std::string got = connectedTipBody(name, "", kLabel);
        expectLines(got, std::string(kLabel) + "\n" + name, "22 字节边界");
    }

    std::printf("\n7. 边界：上限放宽后长名字不再截断\n");
    {
        const std::string name(40, 'C');
        const std::string got = connectedTipBody(name, "", kLabel, 100);
        expectLines(got, std::string(kLabel) + "\n" + name, "maxBytes=100");
    }

    std::printf("\n8. ★ 中文名按字符边界截断，不能切出半个字\n");
    {
        // 中文一个字 3 字节，22 字节会切在某个字中间。
        // 切坏了界面上是乱码「特别特�...」—— 纯 ASCII 用例永远发现不了。
        const std::string cn = "一个名字特别特别长的 AirPlay 音箱设备";
        const std::string got = connectedTipBody(cn, "192.168.1.50", kLabel);

        const std::size_t nl = got.find('\n');
        const std::size_t nl2 = got.find('\n', nl + 1);
        const std::string nameLine = got.substr(nl + 1, nl2 - nl - 1);
        const std::size_t ell = nameLine.rfind("...");
        const std::string prefix =
            (ell == std::string::npos) ? nameLine : nameLine.substr(0, ell);

        ++g_checks;
        // ★ 判据：截出来的前缀必须能被原串接受，且接上原串的下一段才完整。
        //   只检查「最后一个字节不是 10xxxxxx」是错的 —— 一个完整汉字的
        //   首字节是 1110xxxx，用它当判据会把合法结果报成乱码。
        //   真正的判据是「prefix 是 cn 的前缀，且 cn[prefix.size()] 处
        //   正好是一个字符的开头」。
        if (cn.compare(0, prefix.size(), prefix) != 0) {
            ++g_fail;
            std::printf("      FAIL  截断结果不是原串的前缀\n");
        } else if (prefix.size() < cn.size() &&
                   (static_cast<unsigned char>(cn[prefix.size()]) & 0xC0) == 0x80) {
            // 原串在截断点处的字节是续字节 -> 说明把某个字切开了
            ++g_fail;
            std::printf("      FAIL  截断点落在多字节字符中间（会显示成乱码）\n");
        }

        ++g_checks;
        if (got.find("192.168.1.50") == std::string::npos) {
            ++g_fail;
            std::printf("      FAIL  中文长名字把 IP 挤掉了\n");
        }

        ++g_checks;
        // 截断后不能超过字节上限（"..." 另计）
        if (prefix.size() > 22) {
            ++g_fail;
            std::printf("      FAIL  截断后仍超上限：%d 字节\n", int(prefix.size()));
        }
    }

    std::printf("\n9. label 为空指针不崩（防御）\n");
    {
        const std::string got = connectedTipBody("x", "", nullptr);
        expectLineCount(got, 2, "label 为空仍给名字");
        expectLines(got, std::string("\nx"), "第一行是空状态");
    }

    std::printf("\n");
    if (g_fail) {
        std::printf("   %d/%d 项失败\n", g_fail, g_checks);
        return 1;
    }
    std::printf("   全部通过（%d 项）\n", g_checks);
    return 0;
}
