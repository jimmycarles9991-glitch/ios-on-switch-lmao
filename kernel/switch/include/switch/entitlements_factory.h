// SPDX-License-Identifier: GPL-2.0-only
#ifndef SWITCH_ENTITLEMENTS_FACTORY_H
#define SWITCH_ENTITLEMENTS_FACTORY_H

#include <sys/cdefs.h>
#include <stddef.h>
#include <kern/cs_blobs.h>

__BEGIN_DECLS
struct cs_blob;
void *switch_app_code_native_dictionary(const void *der, size_t size);
void *switch_app_code_native_entitlements(const void *der, size_t size, const char *identity,
                                         struct cs_blob *blob);
__END_DECLS

#endif
