// ipc_kmsg.c's use of switch_render_senders.h: ipc_kmsg_send counts a message to the watched space under the
// destination port's lock (ip_in_space asserts it), before the message is queued, never for the kernel's own reply; the
// render server's own messages to other tasks go to the frame diagnostics (<switch/frames.h>) under the same lock.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

using testing::HasSubstr;

namespace {
std::string kmsg()
{
    std::ifstream file(IPC_KMSG, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}
}  // namespace

TEST(RenderSendersSource, CapturedUnderThePortLockAndCountedAfterUnlock)
{
    const std::string s = kmsg();
    const size_t send = s.find("\nipc_kmsg_send(\n");
    ASSERT_NE(send, std::string::npos);
    const std::string body = s.substr(send, s.find("\n}\n", send) - send);
    const size_t send_locked = body.find("ipc_mqueue_send_locked(&port->ip_messages, kmsg,");
    const size_t check = body.find("ip_in_space(port, (ipc_space_t)switch_render_server_space)");
    const size_t note = body.find("switch_render_sender_note(switch_render_id);");
    ASSERT_NE(check, std::string::npos);
    ASSERT_NE(note, std::string::npos);
    EXPECT_LT(body.find("ip_mq_lock(port);"), check);
    EXPECT_LT(check, send_locked);
    EXPECT_LT(send_locked, note);
    EXPECT_THAT(body,
                HasSubstr("!kernel_reply) {\n\t\t\tif (ip_in_space(port, (ipc_space_t)switch_render_server_space))"));
    const size_t sent = body.find("switch_frames_render_server_sent_to(switch_recv_pid, switch_recv_name);");
    ASSERT_NE(sent, std::string::npos);
    EXPECT_LT(send_locked, sent);
    EXPECT_THAT(body, HasSubstr("} else if (current_task()->itk_space == switch_render_server_space &&"));
    EXPECT_THAT(s, HasSubstr("static LCK_SPIN_DECLARE(switch_render_senders_lock"));
}
