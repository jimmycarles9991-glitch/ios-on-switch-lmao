// SPDX-License-Identifier: MIT
// No CPU atomic read-modify-write reaches write-combined or uncached memory, in any driver or kit. T210 answers an
// exclusive access (ldxr/stxr, a compare-and-swap, any LSE atomic) to Normal Non-cacheable or Device memory with an
// external abort, an SError at the next unmask: boot 17's T-v4a panic, a compare-and-swap on a fence-pool word
// (0d581ccca). Every source outside the tests is read with its comments and strings blanked. Every mapping that makes
// memory write-combined or uncached is in kMappings with the names its memory is reached through, and a mapping that is
// not fails here until it is. Each RMW call's operand, each argument of a function that RMWs through one, and each
// exclusive or atomic instruction in assembly or inline asm, is checked against those names.
#include <gtest/gtest.h>
#include <cctype>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <map>
#include <utility>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>
#include "switch_gpu_fence_model.h"
#include "switch_wc_atomic_scan.h"

namespace
{
using namespace wc_atomic_scan;

// Every write-combined or uncached mapping in drivers/ and kits/, by the token that makes it so and how often the file
// names it, and the names its memory is reached through. A row without carriers is memory the code reaches only through
// accessors that load and store (MMIO read/write helpers, nvkm_rd32/wo32, IOMemoryDescriptor reads and writes), or a
// declaration or definition of the mapping call itself.
struct Mapping {
	const char *file, *token;
	int count;
	const char *carriers;  // a regex an RMW operand reaching this memory matches; nullptr: none
	const char *what;
};

const Mapping kMappings[] = {
	{"drivers/TegraGM20B/gpu_chan.c", "tgm_pool_alloc", 1,
	 R"(\bfence_cpu\b|\btgm_fence_pages\.va\b|\btgm_fence_word\s*\(|\bpromiseWord\s*\(|\bfence_va_\b|\bvisible_cpu\b|\bbind_cpu\b|\bsemantic_public\b|\bword->cpu\b|\bmirror\b|(->|\b)word\s*\()",
	 "the fence pool (fences_alloc): every connection's fence page, channel fences, promise, "
	 "event, set and bind words"},
	{"drivers/TegraGM20B/gpu_chan.c", "DMA_ATTR_WRITE_COMBINE", 1, R"(->ring\b)",
	 "a channel's ring, GPFIFO and stamps (the shim's dma_alloc_attrs returns pool VA)"},
	{"drivers/TegraGM20B/gpu_mem.c", "tgm_pool_alloc", 2,
	 R"(\btgm_pfence\.va\b|\btgm_present\.fence\b)",
	 "tgm_pool_alloc's definition, and the present's fence page"},
	{"drivers/TegraGM20B/gpu_scrub.c", "tgm_pool_alloc", 1, nullptr,
	 "the scrubber's fence page, reached as its channel's fence_cpu"},
	{"drivers/TegraGM20B/gpu_bringup.c", "tgm_pool_alloc", 1, nullptr,
	 "bring-up's self-test page, loads and stores only"},
	{"drivers/TegraGM20B/SwitchGpuKqueue.h", "tgm_pool_alloc", 1, R"(\bwords\b)",
	 "a kernel queue's push pages"},
	{"drivers/TegraGM20B/TegraGM20BGpu.h", "tgm_pool_alloc", 1, nullptr, "its declaration"},
	{"drivers/TegraGM20B/shim/xnu_glue.cpp", "VM_WIMG_WCOMB", 1, R"(\bphystokv\s*\()",
	 "the GPU pool's kernel mapping"},
	{"drivers/TegraGM20B/shim/xnu_glue.cpp", "pmap_set_cache_attributes", 2,
	 R"(\bphystokv\s*\()", "the GPU pool's kernel mapping"},
	{"drivers/TegraGM20B/shim/xnu_glue.cpp", "kIOMapWriteCombineCache", 1,
	 R"(\bnvxnu_vmap\s*\()", "nvxnu_vmap"},
	{"drivers/TegraGM20B/TegraGM20BUserClient.cpp", "kIOMapWriteCombineCache", 2, nullptr,
	 "a client's coherent BOs and read-only fence view, mapped into its task"},
	{"drivers/TegraGM20B/TegraGM20BUserClientBo.cpp", "kIOMapWriteCombineCache", 1, nullptr,
	 "a coherent BO from system memory, mapped into its task"},
	{"drivers/TegraGM20B/nouveau/nvkm/subdev/instmem/gk20a.c", "DMA_ATTR_WRITE_COMBINE", 1,
	 nullptr, "nvkm instance memory, through nvkm_wo32/ro32"},
	{"drivers/TegraGM20B/nouveau/nvkm/engine/device/base.c", "ioremap", 1, nullptr,
	 "the GPU's MMIO, nvkm_rd32/wr32"},
	{"drivers/TegraGM20B/shim/nvif_nvkm.c", "ioremap", 1, nullptr,
	 "the GPU's MMIO, nvkm_rd32/wr32"},
	{"drivers/TegraGM20B/shim/include/nvif/os.h", "DMA_ATTR_WRITE_COMBINE", 1, nullptr,
	 "its definition"},
	{"drivers/TegraGM20B/shim/include/nvif/os.h", "ioremap", 1, nullptr, "its declaration"},
	{"drivers/TegraGM20B/shim/include/nvif/os.h", "ioremap_wc", 1, nullptr, "its declaration"},
	{"drivers/TegraGM20B/shim/linux.c", "ioremap", 2, nullptr, "its definition"},
	{"drivers/TegraGM20B/shim/linux.c", "ioremap_wc", 1, nullptr, "its definition"},
	{"drivers/TegraGM20B/shim/linux.c", "switch_linux_ioremap", 2, nullptr,
	 "the definitions above"},
	{"drivers/TegraGM20B/shim/include/nvxnu.h", "nvxnu_ioremap", 1, nullptr, "its definition"},
	{"drivers/TegraGM20B/shim/include/nvxnu.h", "switch_linux_ioremap", 1, nullptr,
	 "nvxnu_ioremap's body"},
	{"drivers/TegraGM20B/platform.c", "nvxnu_ioremap", 4, R"(\b(car|pmc|fuse|mc)\b)",
	 "CAR, PMC, fuse and MC registers"},
	{"drivers/TegraBPMP/linux/drivers/firmware/tegra/mail_t210.c", "ioremap", 1, nullptr,
	 "BPMP mailboxes, readl/writel"},
	{"drivers/TegraBPMP/shim/include/linux/io.h", "ioremap", 1, nullptr, "its definition"},
	{"drivers/TegraBPMP/shim/include/linux/io.h", "switch_linux_ioremap", 1, nullptr,
	 "ioremap's body"},
	{"drivers/TegraBPMP/shim/linux.c", "switch_linux_ioremap", 1, nullptr,
	 "the shim's mapping"},
	{"drivers/TegraDFLL/platform.c", "switch_linux_ioremap", 5, nullptr,
	 "DFLL, CAR and fuse registers, readl/writel"},
	{"drivers/TegraDFLL/shim/linux.c", "switch_linux_ioremap", 1, nullptr,
	 "the shim's mapping"},
	{"drivers/TegraI2C/TegraI2C.cpp", "ml_io_map", 5, R"(\b(gCar|gPinmux|gApbMisc)\b)",
	 "CAR, pinmux and APB_MISC"},
	{"kits/SwitchLinux/SwitchLinux.cpp", "ml_io_map", 2, nullptr,
	 "switch_linux_ioremap's definition"},
	{"kits/SwitchLinux/SwitchLinux.cpp", "ml_io_map_wcomb", 2, nullptr,
	 "switch_linux_ioremap's definition"},
	{"kits/SwitchLinux/SwitchLinux.cpp", "switch_linux_ioremap", 1, nullptr, "its definition"},
	{"kits/SwitchLinux/SwitchLinux.h", "switch_linux_ioremap", 1, nullptr, "its declaration"},
	{"kits/SwitchMMIO/SwitchMMIOKernel.h", "ml_io_map", 2, R"(\bswitch_mmio_map\s*\()",
	 "switch_mmio maps, switch_mmio_read32/write32"},
	{"kits/qemu/SharedWindow.cpp", "kIOMapInhibitCache", 1, nullptr,
	 "QEMU's shared window, copies only"},
	{"kits/qemu/SharedWindow.cpp", "ml_io_map", 2, nullptr,
	 "QEMU's shared window, copies only"},
	{"kits/qemu/VirtioInput.cpp", "mapDeviceMemoryWithIndex", 1, nullptr,
	 "virtio MMIO, read/write helpers"},
	{"drivers/ArmCpu/ArmCpu.cpp", "kIOMapInhibitCache", 2, nullptr,
	 "GIC distributor and redistributors"},
	{"drivers/ArmGIC/ArmGIC.cpp", "kIOMapInhibitCache", 2, nullptr, "GIC distributor and LIC"},
	{"drivers/NativeKeyStoreEvents/AKSDeviceKey.cpp", "kIOMapInhibitCache", 1, nullptr,
	 "SE registers"},
	{"drivers/NativeKeyStoreEvents/TegraSecurityEngine.cpp", "kIOMapInhibitCache", 1, nullptr,
	 "SE registers"},
	{"drivers/NativeMobileDisplay/NativeGOPPresenter.cpp", "kIOMapInhibitCache", 1, nullptr,
	 "the GOP framebuffer"},
	{"drivers/NativeMobileDisplay/NativeGOPPresenter.cpp", "mapDeviceMemoryWithIndex", 1,
	 nullptr, "the GOP framebuffer"},
	{"drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp", "kIOMapInhibitCache", 1, nullptr,
	 "DC registers"},
	{"drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp", "kIOMapWriteCombineCache", 2,
	 nullptr, "the scanout and the boot framebuffer, pixel copies only"},
	{"drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp", "mapDeviceMemoryWithIndex", 1,
	 nullptr, "the boot framebuffer"},
	{"drivers/SwitchUSB/SwitchXHCI.cpp", "kIOMapWriteCombineCache", 1, nullptr,
	 "XHCI rings, Apple's AppleUSBXHCI"},
	{"drivers/SwitchUSB/UsbDmaMapper.cpp", "kIOMapWriteCombineCache", 1, nullptr,
	 "the bounce pool, readBytes/writeBytes"},
};

// Functions that read, modify and write through a pointer argument: their calls are checked argument by argument
const char *const kWrappers[] = {"nvxnu_word_wake",
				 "switch_gpu_irq_arm",
				 "switch_gpu_irq_complete",
				 "switch_gpu_waiter_want",
				 "switch_gpu_waiter_reached",
				 "switch_gpu_quiet_need",
				 "stats_add",
				 "take",
				 "raise",
				 "once",
				 "lazyLock"};

struct Hit {
	std::string where, what;
};

bool startsWith(const std::string &name, const char *prefix)
{
	return name.rfind(prefix, 0) == 0;
}

bool endsWith(const std::string &name, const char *suffix)
{
	const size_t n = std::strlen(suffix);
	return name.size() >= n && name.compare(name.size() - n, n, suffix) == 0;
}

// A C call that reads, modifies and writes its first operand atomically
bool readModifyWrite(const std::string &name)
{
	if (startsWith(name, "__atomic_"))
		return name != "__atomic_load_n" && name != "__atomic_load" &&
		       name != "__atomic_store_n" && name != "__atomic_store" &&
		       name != "__atomic_thread_fence" && name != "__atomic_signal_fence" &&
		       name != "__atomic_always_lock_free" && name != "__atomic_is_lock_free" &&
		       name != "__atomic_clear";
	if (startsWith(name, "__sync_") && name != "__sync_synchronize")
		return true;
	if (startsWith(name, "OSAtomic") || startsWith(name, "OSCompareAndSwap") ||
	    startsWith(name, "OSIncrementAtomic") || startsWith(name, "OSDecrementAtomic") ||
	    startsWith(name, "OSAddAtomic") || startsWith(name, "OSBitOrAtomic") ||
	    startsWith(name, "OSBitAndAtomic") || startsWith(name, "OSBitXorAtomic") ||
	    startsWith(name, "OSTestAndSet") || startsWith(name, "OSTestAndClear"))
		return true;
	if (startsWith(name, "atomic_") || startsWith(name, "atomic64_"))
		return !endsWith(name, "_read") && !endsWith(name, "_set") &&
		       !endsWith(name, "_init");
	return name == "set_bit" || name == "clear_bit" || name == "change_bit" ||
	       name == "test_and_set_bit" || name == "test_and_clear_bit" ||
	       name == "test_and_change_bit";
}

// An AArch64 exclusive or LSE atomic mnemonic
bool exclusiveMnemonic(const std::string &word)
{
	static const char *const prefixes[] = {
		"ldxr",  "ldxp",  "stxr",  "stxp",  "ldaxr",  "ldaxp",  "stlxr",  "stlxp",
		"ldadd", "ldclr", "ldset", "ldeor", "ldsmax", "ldsmin", "ldumax", "ldumin"};
	for (const char *prefix : prefixes)
		if (startsWith(word, prefix))
			return true;
	static const std::regex casOrSwap(R"(^(cas|swp)(a|l|al)?[bhp]?$)");
	return std::regex_match(word, casOrSwap);
}

const char *const kMappingTokens[] = {"tgm_pool_alloc",
				      "DMA_ATTR_WRITE_COMBINE",
				      "kIOMapWriteCombineCache",
				      "kIOMapInhibitCache",
				      "mapDeviceMemoryWithIndex",
				      "ml_io_map_wcomb",
				      "ml_io_map",
				      "ioremap_wc",
				      "ioremap",
				      "nvxnu_ioremap",
				      "switch_linux_ioremap",
				      "VM_WIMG_WCOMB",
				      "VM_WIMG_IO",
				      "pmap_set_cache_attributes"};

struct Scan {
	std::vector<Hit> hits;
	std::map<std::pair<std::string, std::string>, int> mappings;  // (file, token): occurrences
};

Scan scan()
{
	std::vector<std::pair<std::regex, const Mapping *>> carriers;
	for (const auto &mapping : kMappings)
		if (mapping.carriers)
			carriers.emplace_back(std::regex(mapping.carriers), &mapping);
	Scan result;
	std::vector<Hit> &found = result.hits;
	for (const char *root : {"drivers", "kits"}) {
		const auto options = std::filesystem::directory_options::follow_directory_symlink;
		for (const auto &entry :
		     std::filesystem::recursive_directory_iterator(root, options)) {
			const std::string path = entry.path().generic_string();
			const std::string ext = entry.path().extension().string();
			if (!entry.is_regular_file() || path.find("/test/") != std::string::npos ||
			    path.find("/tests/") != std::string::npos)
				continue;
			const bool assembly = ext == ".S";
			if (!assembly && ext != ".c" && ext != ".cpp" && ext != ".h" &&
			    ext != ".mm" && ext != ".m")
				continue;
			std::ifstream file(path, std::ios::binary);
			std::ostringstream text;
			text << file.rdbuf();
			const std::string raw = text.str();
			const std::string source = assembly ? raw : blanked(raw);
			size_t line = 1;
			for (size_t i = 0; i < source.size();) {
				if (source[i] == '\n')
					++line;
				if (!identifierChar(source[i]) ||
				    (i && identifierChar(source[i - 1]))) {
					++i;
					continue;
				}
				size_t end = i;
				while (end < source.size() && identifierChar(source[end]))
					++end;
				const std::string word = source.substr(i, end - i);
				i = end;
				if (!assembly &&
				    std::find_if(std::begin(kMappingTokens),
						 std::end(kMappingTokens), [&](const char *token) {
							 return word == token;
						 }) != std::end(kMappingTokens))
					++result.mappings[{path, word}];
				if (assembly) {
					if (exclusiveMnemonic(word))
						found.push_back(
							{path + ":" + std::to_string(line), word});
					continue;
				}
				size_t open = end;
				while (open < source.size() &&
				       (source[open] == ' ' || source[open] == '\t'))
					++open;
				if (word == "asm" || word == "__asm__" || word == "__asm") {
					const size_t close = source.find(';', open);
					const std::string statement = raw.substr(
						open, close == std::string::npos ? std::string::npos
										 : close - open);
					std::string mnemonic;
					for (const char c : statement + " ") {
						if (identifierChar(c)) {
							mnemonic += c;
						} else if (!mnemonic.empty()) {
							if (exclusiveMnemonic(mnemonic))
								found.push_back(
									{path + ":" +
										 std::to_string(
											 line),
									 "asm " + mnemonic});
							mnemonic.clear();
						}
					}
					continue;
				}
				const size_t start = i - word.size();
				const size_t before =
					start ? source.find_last_not_of(" \t\n", start - 1)
					      : std::string::npos;
				const bool member = before != std::string::npos &&
						    (source[before] == '.' ||
						     (source[before] == '>' && before &&
						      source[before - 1] == '-'));
				const bool wrapper =
					!member &&
					std::find_if(std::begin(kWrappers), std::end(kWrappers),
						     [&](const char *w) {
							     return word == w;
						     }) != std::end(kWrappers);
				if (open == source.size() || source[open] != '(' ||
				    !(wrapper || readModifyWrite(word)))
					continue;
				const std::vector<std::string> operands =
					wrapper ? arguments(source, open + 1)
						: std::vector<std::string>{
							  firstArgument(source, open + 1)};
				for (const std::string &operand : operands) {
					if (operand.find_first_not_of(" \t\n") !=
						    std::string::npos &&
					    operand[operand.find_first_not_of(" \t\n")] == '[')
						continue;  // a lambda, never the operand
					const std::string name = local(operand);
					const std::string from =
						name.empty() ? std::string()
							     : assigned(source, open, name);
					for (const auto &[carrier, mapping] : carriers)
						if (reaches(operand, carrier) ||
						    (!from.empty() && reaches(from, carrier)))
							found.push_back(
								{path + ":" + std::to_string(line),
								 word + "(" + operand +
									 (from.empty()
										  ? ""
										  : " =" + from) +
									 ") on " + mapping->what +
									 " (" + mapping->file +
									 " " + mapping->token +
									 ")"});
				}
			}
		}
	}
	return result;
}
}  // namespace

const Scan &tree()
{
	static const Scan scanned = scan();
	return scanned;
}

TEST(WriteCombinedAtomics, NoReadModifyWriteReachesWriteCombinedOrUncachedMemory)
{
	for (const Hit &hit : tree().hits)
		ADD_FAILURE() << hit.where << ": " << hit.what;
}

TEST(WriteCombinedAtomics, AnOperandReachesTheMemoryNotTheVariableHoldingItsPointer)
{
	const std::regex fence(kMappings[0].carriers), push(R"(\bwords\b)");
	EXPECT_TRUE(reaches("chan->fence_cpu", fence));
	EXPECT_TRUE(reaches(" &chan->fence_cpu[1] ", fence));
	EXPECT_TRUE(reaches("tgm_fence_word(offset)", fence));
	EXPECT_TRUE(reaches("&queue.space.words[slot]", push));
	EXPECT_FALSE(reaches("&chan->fence_cpu", fence));
	EXPECT_FALSE(reaches("&chan->promise_count", fence));
	// 0d581ccca's compare-and-swap, one hop back through its local
	const std::string before =
		"volatile uint32_t *at = wait ? word((uint32_t)(wait >> 32)) : 0;\n"
		"raised = __atomic_compare_exchange_n(at, &now, value, 0, 0, 0);";
	EXPECT_EQ(local("at"), "at");
	EXPECT_EQ(local("&at[2]"), "at");
	EXPECT_EQ(local("&chan->fence_cpu"), "");
	EXPECT_TRUE(reaches(assigned(before, before.find("__atomic"), "at"), fence));
}

// A mapping kMappings does not name, or names wrongly, fails: its memory's names join the table first.
TEST(WriteCombinedAtomics, EveryWriteCombinedOrUncachedMappingIsInTheTable)
{
	std::map<std::pair<std::string, std::string>, int> expected;
	for (const auto &mapping : kMappings)
		expected[{mapping.file, mapping.token}] = mapping.count;
	for (const auto &[at, count] : tree().mappings) {
		const auto known = expected.find(at);
		if (known == expected.end())
			ADD_FAILURE() << at.first << " maps with " << at.second << " (" << count
				      << " times), which kMappings does not name";
		else if (known->second != count)
			ADD_FAILURE() << at.first << " names " << at.second << " " << count
				      << " times, kMappings " << known->second;
	}
	for (const auto &[at, count] : expected)
		if (!tree().mappings.count(at))
			ADD_FAILURE() << "kMappings names " << at.first << " " << at.second
				      << ", which no longer maps";
}

// The host tests' fence page (switch_gpu_fence_model.h) answers every read-modify-write with a fault, as T210's bus
// does, and lets the one plain store through.
TEST(WriteCombinedAtomics, TheHostFenceModelFaultsOnEveryReadModifyWrite)
{
	fence_model::Page fence;
	fence.store(&fence.page[3], 7);
	EXPECT_EQ(fence.page[3], 7u);
	EXPECT_DEATH(__atomic_fetch_add(&fence.page[3], 1u, __ATOMIC_RELAXED), "");
	EXPECT_DEATH(__atomic_exchange_n(&fence.page[3], 1u, __ATOMIC_RELAXED), "");
	EXPECT_DEATH(
		{
			uint32_t expected = 7;
			__atomic_compare_exchange_n(&fence.page[3], &expected, 8u, false,
						    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
		},
		"");
	EXPECT_DEATH(__sync_fetch_and_or(&fence.page[3], 1u), "");
	EXPECT_EQ(fence.page[3], 7u);
}
