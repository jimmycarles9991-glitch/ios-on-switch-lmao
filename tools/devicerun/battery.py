"""iPadOS 27's battery pack (drivers/SwitchBattery/SwitchBatteryPack.h): dasd and powerd find it.

Boot 2 (22-23 Nov, iOS 27) is the baseline. With no AppleSmartBatteryPack, dasd logged "No match for
AppleSmartBatteryPack IOService" 24,494 times in 39 min, once for each activity it scored. powerd logged "Failed to find
battery pack with ID 0" and its health failures every 2 min. 21H461's dasd and powerd read no pack and log neither line.
"""

from tools.devicerun.rules import Check

OWNER = 'perf1-daemons'
SOURCE = 'drivers/SwitchBattery/SwitchBatteryPack.cpp'

BATTERY = [
    Check(
        'battery.pack-unmatched',
        OWNER,
        'No match for AppleSmartBatteryPack IOService',
        process='dasd',
        source=SOURCE,
        note="dasd's battery temperature reader found no pack; boot 2: 24,494 lines in 39 min",
    ),
    Check(
        'battery.pack-not-found',
        OWNER,
        'Failed to find battery pack with ID',
        process='powerd',
        source=SOURCE,
        note="powerd's battery health found no pack 0; boot 2: one every 2 min",
    ),
]
