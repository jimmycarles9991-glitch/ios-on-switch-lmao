// SPDX-License-Identifier: MIT
// The commit test run against a gpu_exec.c with its commit moved (switch_gpu_exec_mutant.py): a failing test is the
// mutation caught, so the run passes; every test passing means the suite no longer sees that move, and it fails.
#include <gtest/gtest.h>
#include <cstdio>

int main(int argc, char **argv)
{
	testing::InitGoogleTest(&argc, argv);
	if (RUN_ALL_TESTS() != 0)
		return 0;
	std::fprintf(stderr, "the mutation survived: every commit test passed against it\n");
	return 1;
}
