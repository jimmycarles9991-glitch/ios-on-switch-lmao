// SPDX-License-Identifier: MIT
// One place gives a writer promise up (SwitchGpuPromiseGiveUp.h): every driver and kit source outside the tests is read,
// and the calls that mark a promise abandoned, raise its word from the CPU or report it unkept are where they are
// allowed and nowhere else. A second give-up path (a cadence of its own, a raise in a hang check) fails here.
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace
{
using Sites = std::map<std::string,
		       unsigned>;  // a source's workspace path, and how often it names the call

Sites find(const std::string &call)
{
	Sites sites;
	for (const char *root : {"drivers", "kits"}) {
		const auto options = std::filesystem::directory_options::follow_directory_symlink;
		for (const auto &entry :
		     std::filesystem::recursive_directory_iterator(root, options)) {
			const std::string path = entry.path().generic_string();
			const std::string ext = entry.path().extension().string();
			if (!entry.is_regular_file() || path.find("/test/") != std::string::npos ||
			    path.find("/tests/") != std::string::npos ||
			    (ext != ".c" && ext != ".cpp" && ext != ".h" && ext != ".mm" &&
			     ext != ".m"))
				continue;
			std::ifstream file(path, std::ios::binary);
			std::ostringstream text;
			text << file.rdbuf();
			const std::string source = text.str();
			unsigned count = 0;
			for (size_t at = source.find(call); at != std::string::npos;
			     at = source.find(call, at + 1))
				++count;
			if (count)
				sites[path] = count;
		}
	}
	return sites;
}
}  // namespace

TEST(PromiseGiveUpSite, AbandonIsMarkedByTheGiveUpAndTheMakersOwnCloseOnly)
{
	EXPECT_EQ(find("tgm_promise_abandon("),
		  (Sites{
			  {"drivers/TegraGM20B/TegraGM20BGpu.h", 1},
			  {"drivers/TegraGM20B/TegraGM20BUserClientPromise.cpp", 1},
			  {"drivers/TegraGM20B/gpu_promise.c", 1},
			  {"drivers/TegraGM20B/gpu_promise_giveup.c", 1},
		  }))
		<< "the definition, its declaration, the give-up's RAISING step and a kill or "
		   "close of the maker";
	EXPECT_EQ(find("switch_gpu_promise_abandoned_add("),
		  (Sites{{"drivers/TegraGM20B/SwitchGpuWriterPromise.h", 1},
			 {"drivers/TegraGM20B/gpu_promise.c", 1}}));
}

TEST(PromiseGiveUpSite, OneRaiseAndOneLine)
{
	EXPECT_EQ(find("switch_gpu_promise_give_up("),
		  (Sites{
			  {"drivers/TegraGM20B/SwitchGpuPromiseGiveUp.h", 1},
			  {"drivers/TegraGM20B/SwitchGpuWriterPromise.h", 1},
		  }));
	EXPECT_EQ(find("promise_raise("), (Sites{{"drivers/TegraGM20B/gpu_promise_giveup.c", 1}}));
	EXPECT_EQ(find("->raise("), (Sites{{"drivers/TegraGM20B/SwitchGpuWriterPromise.h", 1}}))
		<< "the fence's raise, reached through switch_gpu_promise_give_up alone";
	EXPECT_EQ(find("writer promise at fence offset %u (value %u) was not kept"),
		  (Sites{{"drivers/TegraGM20B/gpu_promise_giveup.c", 1}}));
	EXPECT_EQ(find("giveup_run("), (Sites{
					       {"drivers/TegraGM20B/SwitchGpuPromiseGiveUp.h", 1},
					       {"drivers/TegraGM20B/gpu_promise_giveup.c", 2},
				       }))
		<< "the header's run, and the kernel's one call that runs it";
}
