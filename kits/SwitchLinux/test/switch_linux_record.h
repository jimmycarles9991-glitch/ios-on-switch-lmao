// What the stand-ins recorded, for the test to read.
#ifndef SWITCH_TEST_SWITCH_LINUX_RECORD_H
#define SWITCH_TEST_SWITCH_LINUX_RECORD_H

#include <string>
#include <vector>

extern std::vector<std::string> gSwitchLinuxCalls;

void switch_linux_stub_fire(void *call);

#endif
