#import "SwitchArchive.h"

#include <libkern/OSByteOrder.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>

BOOL switch_archive_read(struct archive *archive, void *bytes, size_t length, NSError **error)
{
    size_t remaining = length;
    while (remaining) {
        ssize_t count = archive_read_data(archive, bytes, remaining);
        if (count <= 0) {
            if (error)
                *error = switch_package_error(@"The IPA contains a damaged or incomplete executable.");
            return NO;
        }
        remaining -= count;
        bytes = (char *)bytes + count;
    }
    return YES;
}

static BOOL switch_executable_fail(NSError **error, NSString *reason)
{
    if (error)
        *error = switch_package_error(reason);
    return NO;
}

BOOL switch_validate_executable(struct archive *archive, uint64_t size, NSError **error)
{
    uint32_t prefix[2];
    if (size < sizeof(struct mach_header_64) || !switch_archive_read(archive, prefix, sizeof(prefix), error))
        return switch_executable_fail(error, @"The IPA has no complete app executable.");
    uint64_t position = sizeof(prefix), sliceSize = size;
    if (prefix[0] == FAT_CIGAM || prefix[0] == FAT_CIGAM_64) {
        const BOOL wide = prefix[0] == FAT_CIGAM_64;
        const uint32_t count = OSSwapBigToHostInt32(prefix[1]);
        if (!count || count > 64)
            return switch_executable_fail(error, @"The IPA has an invalid universal executable.");
        uint64_t selected = UINT64_MAX;
        for (uint32_t index = 0; index < count; ++index) {
            struct fat_arch_64 entry = {};
            const size_t length = wide ? sizeof(struct fat_arch_64) : sizeof(struct fat_arch);
            if (!switch_archive_read(archive, &entry, length, error))
                return NO;
            position += length;
            if (OSSwapBigToHostInt32(entry.cputype) != CPU_TYPE_ARM64)
                continue;
            uint64_t offset, bytes;
            if (wide) {
                offset = OSSwapBigToHostInt64(entry.offset);
                bytes = OSSwapBigToHostInt64(entry.size);
            } else {
                struct fat_arch narrow;
                memcpy(&narrow, &entry, sizeof(narrow));
                offset = OSSwapBigToHostInt32(narrow.offset);
                bytes = OSSwapBigToHostInt32(narrow.size);
            }
            if (offset < selected) {
                selected = offset;
                sliceSize = bytes;
            }
        }
        if (selected == UINT64_MAX)
            return switch_executable_fail(error, @"This IPA has no arm64 app. Choose an iPhone or iPad build.");
        if (selected < position || selected > size || sliceSize > size - selected ||
            sliceSize < sizeof(struct mach_header_64))
            return switch_executable_fail(error, @"The IPA has an invalid executable slice.");
        char skipped[65536];
        while (position < selected) {
            size_t length = (size_t)MIN(sizeof(skipped), selected - position);
            if (!switch_archive_read(archive, skipped, length, error))
                return NO;
            position += length;
        }
        if (!switch_archive_read(archive, prefix, sizeof(prefix), error))
            return NO;
    }
    struct mach_header_64 header = {};
    memcpy(&header, prefix, sizeof(prefix));
    if (!switch_archive_read(archive, (char *)&header + sizeof(prefix), sizeof(header) - sizeof(prefix), error))
        return NO;
    if (header.magic != MH_MAGIC_64 || header.cputype != CPU_TYPE_ARM64 || header.filetype != MH_EXECUTE)
        return switch_executable_fail(error, @"This IPA has no arm64 iPhone or iPad executable.");
    if (header.sizeofcmds > 1024 * 1024 || header.sizeofcmds > sliceSize - sizeof(header) ||
        header.ncmds > header.sizeofcmds / sizeof(struct load_command))
        return switch_executable_fail(error, @"The IPA has invalid executable metadata.");
    NSMutableData *commands = [NSMutableData dataWithLength:header.sizeofcmds];
    if (!switch_archive_read(archive, commands.mutableBytes, commands.length, error))
        return NO;
    size_t offset = 0;
    for (uint32_t index = 0; index < header.ncmds; ++index) {
        struct load_command command;
        if (offset > commands.length || commands.length - offset < sizeof(command))
            return switch_executable_fail(error, @"The IPA has truncated executable metadata.");
        memcpy(&command, (const char *)commands.bytes + offset, sizeof(command));
        if (command.cmdsize < sizeof(command) || command.cmdsize > commands.length - offset)
            return switch_executable_fail(error, @"The IPA has invalid executable commands.");
        if (command.cmd == LC_ENCRYPTION_INFO || command.cmd == LC_ENCRYPTION_INFO_64) {
            struct encryption_info_command encryption;
            if (command.cmdsize < sizeof(encryption))
                return switch_executable_fail(error, @"The IPA has invalid encryption metadata.");
            memcpy(&encryption, (const char *)commands.bytes + offset, sizeof(encryption));
            if (encryption.cryptid)
                return switch_executable_fail(error,
                                              @"This IPA is encrypted by the App Store. Choose an unencrypted IPA.");
        }
        if (command.cmd == LC_BUILD_VERSION) {
            struct build_version_command build;
            if (command.cmdsize < sizeof(build))
                return switch_executable_fail(error, @"The IPA has invalid platform metadata.");
            memcpy(&build, (const char *)commands.bytes + offset, sizeof(build));
            if (build.platform != PLATFORM_IOS)
                return switch_executable_fail(error, @"This is not an iPhone or iPad device build.");
        }
        offset += command.cmdsize;
    }
    return YES;
}
