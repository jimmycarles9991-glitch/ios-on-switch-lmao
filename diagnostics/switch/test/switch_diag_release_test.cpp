// SwitchDiag.h as a release kernel's XNU includes it (no DEVELOPMENT, no DEBUG): every debug hook is a macro that never
// evaluates its arguments, so XNU's hooks compile to nothing and nothing calls into the debug units. The debug view
// (switch_diag_debug_view.c) holds the other half. switch_diag_link_test checks what a release kernel links.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "bazel/testing/source_text.h"
#include "diagnostics/switch/test/switch_diag_debug_view.h"

// The host toolchain's debug builds define DEBUG; a release kernel defines neither.
#undef DEBUG
#undef DEVELOPMENT
extern "C" {
#include "diagnostics/switch/SwitchDiag.h"
}

#if !defined(switch_diag_user_syscall) || !defined(switch_diag_stage) || !defined(switch_diag_process_event) ||  \
    !defined(switch_diag_console_active) || !defined(switch_diag_console_putc) || !defined(switch_root_event) || \
    !defined(switch_root_data_ownership) || !defined(switch_root_record_begin) || !defined(switch_root_record_stage)
#error "a release kernel's debug hooks are macros"
#endif
#if defined(switch_diag_panic) || defined(switch_diag_init) || defined(switch_diag_early_stage) || \
    defined(switch_native_text_console_active) || defined(switch_root_boot_prepare)
#error "a release kernel keeps the panic entry, the early exception screen and the root boot"
#endif

using testing::HasSubstr;

namespace {
int evaluated;

int Evaluate()
{
    return ++evaluated;
}
}  // namespace

TEST(SwitchDiagRelease, DebugHooksNeverEvaluateTheirArguments)
{
    evaluated = 0;
    switch_diag_user_syscall(Evaluate(), Evaluate(), Evaluate(), uint64_t{0}, uint64_t{0}, Evaluate());
    switch_diag_stage(Evaluate() ? "stage" : "");
    switch_diag_process_event(Evaluate(), "launchd", "exec load_machfile enter", Evaluate());
    switch_diag_console_putc(static_cast<char>(Evaluate()));
    switch_root_event(Evaluate(), Evaluate(), uint64_t{0});
    switch_root_data_ownership(uint64_t{0}, Evaluate(), uint64_t{0});
    switch_root_record_stage(Evaluate());
    switch_root_record_begin();
    EXPECT_EQ(evaluated, 0);
    EXPECT_EQ(Evaluate(), 1) << "a call outside the hooks counts";
    EXPECT_EQ(switch_diag_console_active(), 0);
    EXPECT_EQ(switch_diag_debug_view_hooks(), 1);
}

TEST(SwitchDiagRelease, TheHudIsBuiltForDebugKernelsOnly)
{
    const std::string hud = switch_ios::test::SourceText("../SwitchDiagHud.c");
    const size_t gate = hud.find("\n#if DEVELOPMENT || DEBUG\n");
    ASSERT_NE(gate, std::string::npos);
    EXPECT_EQ(hud.find('{'), hud.find('{', gate)) << "every definition is inside the gate";
    EXPECT_THAT(hud.substr(hud.rfind('\n', hud.size() - 2)), HasSubstr("#endif"));
}
