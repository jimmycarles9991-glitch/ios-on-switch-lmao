// SPDX-License-Identifier: MIT
// Guards on NVK's side of the kernel's contracts (Mesa's nvkmd_darwin.c, nvk_queue.c, nvk_image.c and the Vulkan runtime's
// vk_queue.c), which go with m2v: tagged m2v, so the deletion's dry run (--config=nom2v) leaves them out.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cctype>
#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace
{
// The body of the function whose definition (not a declaration of it) starts with `signature`.
std::string definition(const std::string &source, const std::string &signature)
{
	for (size_t at = source.find(signature); at != std::string::npos;
	     at = source.find(signature, at + 1)) {
		const std::string rest = source.substr(at + signature.size());
		if (rest.substr(0, rest.find('{')).find(';') != std::string::npos)
			continue;
		return rest.substr(0, rest.find("\n}\n"));
	}
	ADD_FAILURE() << "no definition of " << signature;
	return {};
}

// `text` with every run of whitespace one space (the checks read code, not its line breaks).
std::string flat(const std::string &text)
{
	std::string out;
	bool space = false;
	for (const char c : text) {
		if (std::isspace((unsigned char)c)) {
			space = true;
			continue;
		}
		if (space)
			out += ' ';
		space = false;
		out += c;
	}
	if (space)
		out += ' ';
	return out;
}

// `first` occurs in `text`, and before `second`.
void expectBefore(const std::string &text, const char *first, const char *second)
{
	const size_t a = text.find(first), b = text.find(second);
	ASSERT_NE(a, std::string::npos) << first;
	ASSERT_NE(b, std::string::npos) << second;
	EXPECT_LT(a, b) << first << " before " << second;
}
}  // namespace

// A SurfaceWrites NVK could not deliver flags every later Exec TegraGM20BPushImportsWritten, which makes the connection
// mark every writable import at every Exec; the frontend and the raw path's kernel connection (raw_kernel.cpp) reach
// NVK's exports with dlsym.
TEST(GM20BCoherencySource, NvkFlagsEveryExecOnceSurfaceWritesFail)
{
	const std::string nvk = SourceText(
		"../../../third_party/mesa/src/nouveau/vulkan/nvkmd/darwin/nvkmd_darwin.c");
	EXPECT_THAT(flat(definition(nvk, "switch_nvk_surface_gpu_writes(uint32_t surface_id)")),
		    HasSubstr("p_atomic_set(&surface_writes_failed, true);"));
	EXPECT_THAT(flat(definition(nvk, "nvkmd_darwin_exec_ctx_flush(struct nvkmd_ctx *_ctx,")),
		    HasSubstr("ctx->req.pushes[0].flags |= TegraGM20BPushImportsWritten;"));
	EXPECT_THAT(nvk,
		    HasSubstr("if (req.surface_id && !p_atomic_read(&surface_writes_failed))"));
	// The frontend and the raw path's kernel connection (raw_kernel.cpp) reach these with dlsym; Mesa builds hidden by
	// default. Without the connection's, the raw path opens no device and the render server stays on m2v alone.
	for (const char *exported :
	     {"__attribute__((visibility(\"default\"))) void\nswitch_nvk_import_surface(",
	      "__attribute__((visibility(\"default\"))) bool\nswitch_nvk_surface_gpu_writes(",
	      "__attribute__((visibility(\"default\"))) bool\nswitch_nvk_connection(",
	      "__attribute__((visibility(\"default\"))) void\nswitch_nvk_connection_release(",
	      "__attribute__((visibility(\"default\"))) void\nswitch_nvk_import_foreign("})
		EXPECT_THAT(nvk, HasSubstr(exported));
}

// NVK names a submission's host imports (method 17) before every Exec of it, a retry after kIOReturnBusy included, and a
// GPFIFO the kernel found full sleeps in WaitFence, never a timed poll.
TEST(GM20BFencesSource, NvkSendsItsSurfacesBeforeEachExecAndSleepsWhenFull)
{
	const std::string nvk = SourceText(
		"../../../third_party/mesa/src/nouveau/vulkan/nvkmd/darwin/nvkmd_darwin.c");
	const std::string flush =
		flat(definition(nvk, "nvkmd_darwin_exec_ctx_flush(struct nvkmd_ctx *_ctx,"));
	expectBefore(flush, "for (;;) {", "TegraGM20BMethodExecSurfaces");
	expectBefore(flush, "TegraGM20BMethodExecSurfaces", "TegraGM20BMethodExec,");
	EXPECT_THAT(flush, Not(HasSubstr("usleep")));
	expectBefore(flush, "if (!ctx->submitted) darwin_exec_busy_unsubmitted(ctx->channel);",
		     "darwin_wait_seqno(dev, ctx->channel, ctx->last_seqno,");
	const std::string queue =
		SourceText("../../../third_party/mesa/src/nouveau/vulkan/nvk_queue.c");
	const std::string submit =
		flat(definition(queue, "nvk_queue_submit(struct vk_queue *vk_queue,"));
	expectBefore(submit, "nvkmd_darwin_submit_surfaces(queue->exec_ctx, submit->driver_data);",
		     "result = nvk_queue_submit_exec(queue, submit);");
	expectBefore(submit, "result = nvk_queue_submit_exec(queue, submit);",
		     "nvkmd_darwin_submit_surfaces_end(queue->exec_ctx);");
	EXPECT_THAT(queue, HasSubstr("__attribute__((visibility(\"default\"))) "
				     "void\nswitch_nvk_submit_surfaces("));
	const std::string call = flat(definition(queue, "nvk_QueueSubmit2(VkQueue _queue,"));
	expectBefore(call, "nvkmd_darwin_submit_call(queue, true);", "vk_common_QueueSubmit2(");
	expectBefore(call, "vk_common_QueueSubmit2(", "nvkmd_darwin_submit_call(queue, false);");
	EXPECT_THAT(flat(queue),
		    HasSubstr("queue->vk.driver_submit_data = nvk_queue_submit_data;"));
	// Mesa gives each submission its data on the client's thread, before a submit thread can take it, and frees it with
	// the submission.
	const std::string runtime =
		SourceText("../../../third_party/mesa/src/vulkan/runtime/vk_queue.c");
	expectBefore(flat(definition(runtime, "vk_queue_submit(struct vk_queue *queue,")),
		     "submit->driver_data = queue->driver_submit_data(queue);",
		     "VK_QUEUE_SUBMIT_MODE_THREADED");
	EXPECT_THAT(flat(definition(runtime, "vk_queue_submit_free(struct vk_queue *queue,")),
		    HasSubstr("queue->driver_submit_data_free(submit->driver_data);"));
}

// NVK binds a plane with a PTE kind over a VA rounded to the bind alignment (16 KiB: the page size), so a D32_SFLOAT_S8_UINT
// image's stencil copy temp starts on it, like its planes, and its bind ends inside the image's memory (TegraGM20B refuses a
// bind past its BO: RenderBox's memoryless depth/stencil targets came back nil and the widget drew black).
TEST(NvkImageSource, StencilCopyTempStartsOnTheBindAlignment)
{
	const std::string image =
		flat(SourceText("../../../third_party/mesa/src/nouveau/vulkan/nvk_image.c"));
	const std::string temp =
		image.substr(image.find("&image->stencil_copy_temp.nil, &stencil_nil_info);"));
	EXPECT_THAT(temp.substr(0, temp.find("uint64_t plane_offset_B = 0;")),
		    HasSubstr("if (image->stencil_copy_temp.nil.pte_kind) "
			      "image->stencil_copy_temp.plane_align_B = "
			      "MAX2(image->stencil_copy_temp.plane_align_B, "
			      "pdev->nvkmd->bind_align_B);"));
	EXPECT_THAT(flat(definition(image, "nvk_image_plane_alloc_va(struct nvk_device *dev,")),
		    HasSubstr("const uint64_t va_align_B = plane->plane_align_B;"));
}
