// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#import <Foundation/Foundation.h>

enum SwitchMigrationStatus {
    SwitchMigrationMissing,
    SwitchMigrationComplete,
    SwitchMigrationRepair,
    SwitchMigrationWorking,
    SwitchMigrationFailed,
};

int switch_migration_read(NSString *base, NSString *generation, SwitchMigrationStatus *status,
                          NSString **phase = nullptr, int *recordedError = nullptr, unsigned *attempt = nullptr);
// The worker holds activation.lock across the working receipt, the migration, and the final receipt.
int switch_migration_write(NSString *base, NSString *generation, SwitchMigrationStatus status, const char *phase,
                           int error, unsigned attempt = 1);
