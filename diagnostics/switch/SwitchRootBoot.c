/* The root boot, in every kernel config (kernel/switch/boot/bsd_init.c, once IOBSD exists): the SD card's block device,
 * the NVRAM controller and the board drivers' thread. */
#include <stdint.h>
#include <kern/debug.h>
#include <kern/kern_types.h>
#include <kern/misc_protos.h>
#include "SwitchDiag.h"

extern int switch_block_publish(void);
extern kern_return_t kernel_thread_start(thread_continue_t continuation, void *parameter, thread_t *new_thread);
extern void thread_deallocate(thread_t thread);
extern void thread_terminate_self(void);
extern int tegra_gpio_start(void);
extern int switch_thermal_start(void);
extern int switch_battery_start(void);
extern int switch_touch_start(void);
extern int switch_buttons_start(void);
extern int switch_motion_start(void);
extern int
switch_sensors_start(void);        /* thermalmonitord's HID temperature sensors, after SwitchThermal/SwitchBattery */
extern int switch_als_start(void); /* after SwitchTouch, which powers the sensor (LDO6) */
extern int switch_bluetooth_start(void); /* no-op with switch-no-bt; runs its bring-up on its own thread */
extern int switch_joycon_start(void);
extern int switch_wlan_publish(void); /* no-op with switch-no-wlan; runs its bring-up on its own thread */
extern int tegra_gm20b_probe(void);   /* no-op with switch-no-gpu; runs its bring-up on its own thread */
extern int tegra_nvdec_probe(void);   /* no-op with switch-no-nvdec; own thread, starts host1x itself */
extern int tegra_nvenc_probe(void);   /* on with NVDEC (switch-no-nvdec turns both off); own thread */
extern int tegra_vic_probe(void);     /* on by default; own thread, waits for NVDEC's bring-up to end */
extern int tegra_nvjpg_probe(void);   /* on by default; own thread, waits for NVDEC's bring-up to end */
extern int
switch_audio_publish(void); /* null sink with switch-no-audio; the Tegra backend sleeps on I2C1, PZ4 and the GIC */
extern int switch_suspend_start(void); /* after every board driver it suspends */
extern int switch_usb_start(void); /* no-op without the ADT's /arm-io/usb-device; runs its bring-up on its own thread */
extern int switch_watchdog_publish(void); /* the IOWatchdog launchd starts watchdogd for (NativeSwitchWatchdog) */
extern int tegra_se_start(void);          /* SE2: known answers, kernel PRNG reseed, a timing run later; own thread */

/* Board drivers (GPIO, fan/thermal, battery) sleep on I2C transfers and wait for the GIC, so they
 * start on their own thread: the root mount never waits behind them. A driver whose interrupt is missing panics with
 * its own reason. */
static void switch_board_thread(void *parameter __unused, wait_result_t result __unused)
{
    /* First: returns at once and publishes the per-unit BD address before anything here can sleep. */
    if (switch_bluetooth_start())
        printf("Switch: SwitchBluetooth did not start\n");
    /* Before anything that can sleep long: launchd must find the IOWatchdog when it reads its LaunchEvents. */
    if (switch_watchdog_publish())
        printf("Switch: NativeSwitchWatchdog did not start\n");
    /* Early: returns at once; its thread reseeds the kernel PRNG before most of userspace has asked it for anything. */
    if (tegra_se_start())
        printf("Switch: TegraSE did not start\n");
    /* After Bluetooth: rail pairing hands Joy-Cons the console's (fuse-derived) BD address. */
    if (switch_joycon_start())
        printf("Switch: SwitchJoyCon did not start\n");
    if (switch_usb_start())
        printf("Switch: SwitchUSB did not start\n");
    if (tegra_gpio_start())
        printf("Switch: TegraGPIO did not start\n");
    if (switch_thermal_start())
        printf("Switch: SwitchThermal did not start\n");
    if (switch_battery_start())
        printf("Switch: SwitchBattery did not start\n");
    /* Right after its sources: the 38 services should exist before thermalmonitord first looks (it rebuilds a partial
     * list only every 20 s) and before watchdogd's open asks for them (it waits for them, at most 20 s). */
    if (switch_sensors_start())
        printf("Switch: SwitchSensors did not start\n");
    if (switch_touch_start())
        printf("Switch: SwitchTouch did not start\n");
    if (switch_buttons_start())
        printf("Switch: SwitchButtons did not start\n");
    if (switch_als_start())
        printf("Switch: SwitchALS did not start\n");
    if (switch_motion_start())
        printf("Switch: SwitchMotion did not start\n");
    if (switch_wlan_publish())
        printf("Switch: Wi-Fi driver not started\n");
    if (tegra_gm20b_probe())
        printf("Switch: TegraGM20B did not start\n");
    if (tegra_nvdec_probe())
        printf("Switch: TegraNVDEC did not start\n");
    if (tegra_nvenc_probe())
        printf("Switch: TegraNVENC did not start\n");
    if (tegra_vic_probe())
        printf("Switch: TegraVIC did not start\n");
    if (tegra_nvjpg_probe())
        printf("Switch: TegraNVJPG did not start\n");
    if (switch_audio_publish())
        printf("Switch: SwitchAudio did not start\n");
    (void)switch_suspend_start();
    thread_terminate_self();
}

static void switch_board_start(void)
{
    thread_t thread = NULL;
    if (kernel_thread_start(switch_board_thread, NULL, &thread) != KERN_SUCCESS)
        panic("Switch: cannot start the board driver thread");
    thread_deallocate(thread);
}

void switch_root_boot_prepare(void)
{
    switch_root_record_begin();
    /* The SD block driver owns the controller and finds the APFS partition;
     * IOKit and APFS publish the container and volumes from there. */
    int error = switch_block_publish();
    if (error)
        panic("Switch: SD block device publish failed (%d); see SwitchSD lines above", error);
    /* NVRAM controller: IODTNVRAM gains a backend and the "IONVRAM" resource is
     * published (AppleImage4's NVRAM writes wait on it forever). It keeps the
     * NVRAM in RAM until Data is mounted, then in /private/var/Switch/nvram.bin. */
    extern int switch_nvram_publish(void);
    if (switch_nvram_publish())
        printf("Switch: NVRAM controller not registered\n");
    switch_board_start();
    switch_root_record_stage(5);
}
