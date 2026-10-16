// SPDX-License-Identifier: Apache-2.0
//
// 界面文案的多语言支持。
//
// 设计要点
// --------
// **用中文原文当 key，而不是自造一套 `menu.disconnect` 这样的符号键。**
//
// 理由：
//   1. 接入时改动最小 —— 把 `"断开"` 包成 `tr("断开")` 就够了，
//      不用同时想一个语义化的键名；
//   2. 漏翻时**不会显示成空白或键名**，而是原样显示中文，
//      用户看到的是"没翻译"，不是"坏了"；
//   3. 加新文案不用先去登记 key。
//
// 代价是英文表要靠中文原文来查 —— 但表就在下面这个文件里，
// 新增一条中文文案时顺手补一条英文即可，维护成本可控。
//
// **日志不翻译。** logLine 输出的都是排障信息，用户会把日志贴回来，
// 保持单一语言（中文）反而好搜、好对照。这个文件只管界面文案。
//
// 涉及文件编码：源码是 UTF-8，MSVC 用 /utf-8 编译（见 AGENTS.md C5）。

#pragma once

#include <string>

namespace i18n {

enum class Lang {
    En,   // 默认 —— 仓库面向英文用户
    Zh,
};

// 当前语言。程序启动时从配置读入，之后由用户随时切换。
Lang current();

// 切换语言。立即生效（菜单是每次弹出时重建的，下次打开就是新语言）。
void setLang(Lang lang);

// 配置里存的值与 Lang 的互转。未知值一律当作 En。
Lang fromConfigValue(const std::string& v);
const char* toConfigValue(Lang lang);

// 把中文界面文案翻成当前语言。
//
// 查表失败时**原样返回 key**（也就是中文原文）——
// 这样漏翻表现为"这句还是中文"，而不是空白，一眼就能看出来。
const char* tr(const char* zh);

// 宽字符版本，给 MessageBox 之类的 Win32 接口用。
std::wstring trw(const char* zh);

// 语言在菜单里的显示名（用该语言自己的写法，这是通行做法 ——
// 中文用户找「中文」、英文用户找「English」，都不需要先看懂另一种语言）。
const char* nativeName(Lang lang);

// 供校验用：当前语言下，全部词条里有多少条已经有了译文。
// 返回 {已翻译, 总数}。仅用于自检，不参与运行时逻辑。
void coverage(int* translated, int* total);

}  // namespace i18n
