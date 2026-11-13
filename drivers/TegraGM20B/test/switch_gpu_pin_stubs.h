// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <cstdlib>
#define IONew(type, count) static_cast<type *>(calloc((count), sizeof(type)))
#define IODelete(pointer, type, count) free(pointer)
constexpr int kIOReturnSuccess = 0;

struct IOMemoryDescriptor {
	unsigned refs = 1, prepared = 0;
	bool refuse = false;

	int prepare()
	{
		if (refuse)
			return -1;
		++prepared;
		return 0;
	}

	void complete()
	{
		--prepared;
	}

	void retain()
	{
		++refs;
	}

	void release()
	{
		--refs;
	}
};
