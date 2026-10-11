#pragma once
struct FakeWdt {
    int begin(unsigned long) { return 1; }
    void refresh() {}
};
extern FakeWdt WDT;
