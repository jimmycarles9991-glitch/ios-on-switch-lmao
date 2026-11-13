// SPDX-License-Identifier: MIT
#include "../raw/runtime/raw_use_memo.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
unsigned checks;

void check(bool value)
{
    if (!value)
        std::abort();
    ++checks;
}
}  // namespace

int main()
{
    raw::UseMemo memo;
    int a = 0, b = 0;
    check(!memo.named(&a, 0x1000, 64, nullptr, false));
    check(memo.named(&a, 0x1000, 64, nullptr, false));
    check(!memo.named(&a, 0x1000, 64, nullptr, true));
    check(memo.named(&a, 0x1000, 64, nullptr, true));
    check(memo.named(&a, 0x1000, 64, nullptr, false));
    check(!memo.named(&a, 0x2000, 64, nullptr, false));
    check(!memo.named(&b, 0x1000, 64, nullptr, false));
    // the same object and address, another extent or import: a texture remade in place
    check(!memo.named(&b, 0x1000, 128, nullptr, false));
    check(!memo.named(&b, 0x1000, 128, &a, false));
    check(memo.named(&b, 0x1000, 128, &a, false));
    memo.clear();
    check(!memo.named(&a, 0x2000, 64, nullptr, false));
    check(!memo.named(&b, 0x1000, 64, nullptr, false));
    // more names than slots: an evicted one is named in full again, never answered from another's slot
    std::vector<int> many(1000);
    for (int &x : many)
        check(!memo.named(&x, 1, 64, nullptr, false));
    unsigned again = 0;
    for (int &x : many)
        again += !memo.named(&x, 1, 64, nullptr, false);
    check(again > 0);
    for (int round = 0; round < 70000; ++round)
        memo.clear();
    check(!memo.named(&many[0], 1, 64, nullptr, false));
    printf("use memo: %u checks passed\n", checks);
    return 0;
}
