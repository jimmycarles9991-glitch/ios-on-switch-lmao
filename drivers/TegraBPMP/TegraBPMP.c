/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * The EMC (DRAM) clock of the Switch OLED (T210B01), set the way switchroot's L4T kernel sets it: by asking the BPMP
 * firmware over the vendored L4T mailbox (linux/drivers/firmware/tegra).
 *
 * Who owns EMC on T210B01. hekate's launch_l4t() (bootloader/l4t/l4t.c) loads bpmpfw_b01.bin + mtc_tbl_b01.bin into
 * the BPMP carveout at 0xFFE00000 (SECFW_BASE), copies the three MTC tables of this console's DRAM (fuse DRAM id ->
 * mtc_table_idx_t210b01) into the firmware's DTB, sets CC_INIT_OP = OP_TRAIN and a 100 ms periodic-training period,
 * adds a fourth table at the DRAM's rated 1866 or 2133 MHz through Arachne when the ini sets no ram_oc
 * (_l4t_bpmpfw_b01_config), and finally jumps into the firmware on the BPMP itself. switchroot ATF keeps it running on
 * T210B01 (plat_late_platform_setup powers the BPMP off only with an sc7entry-fw, which hekate passes on T210 only),
 * and our loader keeps 0xFFD00000 up to 4 GiB out of XNU's RAM (build_native_loader.py 'secure-low-tail'). The
 * firmware trains every table itself and performs every EMC clock change and the periodic retraining, so the CPU
 * must never touch the EMC clock behind its back. L4T asks it: the T210B01 EMC clock is BPMP clock 0
 * (l4t-hardware-soc-t210 tegra210b01-soc-base.dtsi: bpmp_car "nvidia,tegra-bpmp-staged-clks", bpmp-emc clocks =
 * <&bpmp_car 0>; clk-tegra-bpmp.c tegra_bpmp_of_clk_init registers it as "emc"), and clk_set_rate() on it is
 * clk-nv-bpmp.c clk_bpmp_set_rate(): MRQ_CLK with BPMP_CLK_CMD(MRQ_CLK_SET_RATE, 0) and the rate in Hz.
 *
 * The sequence, each step as L4T does it:
 *   1. bpmp_mail_init(t210_chcfg, t210_mail_ops), as nv_bpmp.c bpmp_init(): per-CPU atomic channels 0-3, thread
 *      channels 4-7, inbound 8-11 (nv_bpmp.c t210_chcfg). mail_t210.c's connect maps the atomics block 0x70016000 and
 *      the arbitration semaphore 0x60001000 (tegra210-soc-base.dtsi bpmp reg), requires the firmware's handshake
 *      (semaphore status non-zero) and reads each channel's shared area from the atomics. Then the four inbound
 *      doorbells are claimed (shim/xnu_glue.cpp).
 *   2. MRQ_PING. nv_bpmp.c pings on a per-CPU atomic channel with interrupts off, busy-waiting for the answer; here
 *      it goes on a thread channel (tegra_bpmp_send_receive), which sleeps until the doorbell interrupt: same MRQ,
 *      same payload, no polling.
 *   3. MRQ_QUERY_ABI(MRQ_QUERY_FW_TAG), then MRQ_QUERY_FW_TAG: the firmware tag, for the log (nv_bpmp.c
 *      bpmp_get_fwtag; its fallback MRQ_QUERY_TAG needs a DMA buffer and is skipped).
 *   4. MRQ_EMC_DVFS_TABLE (tegra210-emc.c tegra210_bpmp_emc_table_get): the firmware's <kHz, mV> pairs; the rate
 *      asked for must be one of them. Their mV is not what the core rail needs. It is each MTC table's min_volt
 *      (hekate copies mtc_tbl_b01.bin into the firmware's DTB, l4t.c:756-763; rate_khz and min_volt sit at 0x40 and
 *      0x44, minerva mtc_table.h), and mtc_tbl_b01.bin (hekate 6.5.4) holds 1100 mV in every row of every DRAM, the
 *      204 MHz row the console boots at 1050 mV included: a placeholder. L4T does not gate on it on T210B01: its core
 *      DVFS builds the "emc" row from emcb01_dvb_dvfs_table (tegra210-dvfs.c:2212-2216; tegra210_predict_emc_rate,
 *      the firmware's mV, is only the fallback without a DVB table), and 1100 mV is above L4T's T210B01 VDD_CORE
 *      ceiling anyway (tegra210-dvfs.c get_coreb01_sku_max_mv: 1050, 1025 or 1000 mV by process). So the rail is
 *      checked against emc_core_mv below (675 mV for 1600 MHz) and never changed: hekate leaves VDD_CORE (MAX77620
 *      SD0) at 1050 mV on T210B01 (bdk/soc/hw_init.c:356).
 *   5. MRQ_CLK on clock 0: GET_ALL_INFO (it must be "emc"), GET_RATE, ENABLE (tegra210_clock_apply_init_table's
 *      clk_prepare_enable(emc)), ROUND_RATE then SET_RATE (clk_bpmp_determine_rate + clk_bpmp_set_rate), GET_RATE.
 *
 * SwitchPMIC.cpp asks for 1600 MHz. 1600 MHz is the
 * highest of hekate's stock MTC tables (mtc_tbl_b01.bin has 204, 1331.2 and 1600 MHz for every DRAM; l4t.c
 * DRAM_T210B01_TBL_MAX_FREQ 1600000) and the last stock row of emcb01_dvb_dvfs_table before "Custom". The Arachne
 * rated-speed table (1866 or 2133 MHz, l4t.c:765-801) shows up in the logged list when the firmware has it.
 */
#include <linux/kernel.h>
#include <linux/of_address.h>
#include <soc/tegra/bpmp_abi.h>
#include <soc/tegra/bpmp_t210_abi.h>
#include <soc/tegra/tegra_bpmp.h>
#include "bpmp.h"
#include "TegraI2C/TegraI2C.h"
#include "TegraBPMP.h"

static const struct channel_cfg t210_chcfg = {
	.channel_mask = 0xfff,
	.per_cpu_ch_0 = 0,
	.per_cpu_ch_cnt = 4,
	.thread_ch_0 = 4,
	.thread_ch_cnt = 4,
	.ib_ch_0 = 8,
	.ib_ch_cnt = 4,
};

static struct device_node bpmp_node = {
	.full_name = "/bpmp",
	.reg = {{0x70016000, 0x2000}, {0x60001000, 0x1000}},
	.nreg = 2,
};

#define EMC_CLOCK 0
#define PMIC_BUS 5
#define MAX77620 0x3c
#define MAX77620_REG_SD0 0x16 /* VDD_CORE: 600 mV + 12.5 mV * bits 6:0 (hekate max7762x.c "sd0") */

/*
 * The VDD_CORE L4T requires for an EMC rate on T210B01: emcb01_dvb_dvfs_table (switch-l4t-kernel-4.9
 * drivers/soc/tegra/tegra210b01-core-dvfs.c:237-262; NVIDIA's rows up to 1600 MHz, CTCaer's above), process 0
 * column, the highest of the three, so the process fuse need not be read. A rate between rows needs the next row up
 * (tegra210-dvfs.c dvb_predict_rate).
 */
static const struct {
	unsigned int khz, mv;
} emc_core_mv[] = {
	{1065600, 637}, {1331200, 650},  {1600000, 675},  {1866000, 700},
	{2133000, 725}, {2400000, 750},  {2533000, 800},  {2666000, 825},
	{2800000, 900}, {2933000, 1000}, {3066000, 1025}, {3200000, 1050},
};

static int mailbox_state = 1; /* 1 not tried, 0 up, < 0 the error it failed with */
static char fw_tag[sizeof(struct mrq_query_fw_tag_response) + 1];

static int clk_call(unsigned int cmd, const void *args, int args_size, void *reply, int reply_size)
{
	u8 request[4 + sizeof(struct cmd_clk_set_rate_request)];
	const u32 cmd_and_id = (cmd << 24) | EMC_CLOCK;

	memcpy(request, &cmd_and_id, sizeof(cmd_and_id));
	if (args_size)
		memcpy(request + 4, args, args_size);
	return tegra_bpmp_send_receive(MRQ_CLK, request, 4 + args_size, reply, reply_size);
}

/* ROUND_RATE / SET_RATE: {int32 unused, int64 rate} in, int64 rate (or a negative BPMP error) out. */
static int clk_rate(unsigned int cmd, s64 hz, s64 *out)
{
	struct cmd_clk_set_rate_request args;
	s64 reply = 0;
	int err;

	memset(&args, 0, sizeof(args));
	args.rate = hz;
	err = clk_call(cmd, &args, sizeof(args), &reply, sizeof(reply));
	if (!err && reply < 0)
		err = (int)reply;
	if (!err)
		*out = reply;
	return err;
}

static int clk_get_rate(s64 *out)
{
	s64 reply = 0;
	int err = clk_call(CMD_CLK_GET_RATE, NULL, 0, &reply, sizeof(reply));

	if (!err && reply < 0)
		err = (int)reply;
	if (!err)
		*out = reply;
	return err;
}

static int mailbox_up(char *line, unsigned long size)
{
	struct mrq_ping_request ping = {.challenge = 0x10101010};
	struct mrq_ping_response pong = {0};
	struct mrq_query_abi_request abi = {.mrq = MRQ_QUERY_FW_TAG};
	struct mrq_query_abi_response abi_reply = {0};
	int err;

	if (mailbox_state <= 0) {
		if (mailbox_state < 0)
			snprintf(line, size, "BPMP mailbox unavailable (%d), EMC left as is",
				 mailbox_state);
		return mailbox_state;
	}
	err = bpmp_mail_init(&t210_chcfg, &t210_mail_ops, &bpmp_node);
	if (err) {
		mailbox_state = err;
		snprintf(line, size,
			 "no BPMP firmware handshake (%d; hekate's bpmpfw_b01.bin not running?), "
			 "EMC left as is",
			 err);
		return err;
	}
	bpxnu_doorbells_start();
	err = tegra_bpmp_send_receive(MRQ_PING, &ping, sizeof(ping), &pong, sizeof(pong));
	if (!err && pong.reply != ping.challenge << 1)
		err = -EINVAL;
	if (err) {
		mailbox_state = err;
		snprintf(line, size,
			 "BPMP firmware did not answer MRQ_PING (%d, reply %#x), EMC left as is",
			 err, pong.reply);
		return err;
	}
	if (!tegra_bpmp_send_receive(MRQ_QUERY_ABI, &abi, sizeof(abi), &abi_reply,
				     sizeof(abi_reply)) &&
	    !abi_reply.status) {
		struct mrq_query_fw_tag_response tag;

		memset(&tag, 0, sizeof(tag));
		if (!tegra_bpmp_send_receive(MRQ_QUERY_FW_TAG, NULL, 0, &tag, sizeof(tag)))
			memcpy(fw_tag, tag.tag, sizeof(tag.tag));
	}
	if (!fw_tag[0])
		memcpy(fw_tag, "?", 2);
	mailbox_state = 0;
	return 0;
}

int tegra_bpmp_emc_boot(unsigned int khz, char *line, unsigned long size)
{
	struct mrq_emc_dvfs_table_response table;
	struct cmd_clk_get_all_info_response info;
	char rates[EMC_DVFS_TABLE_MAX_SIZE * 6 + 1] = "";
	unsigned int i, fw_mv = 0, need_mv = 0, have_mv = 0, n = 0;
	s64 before = 0, rounded = 0, after = 0;
	uint8_t sd0 = 0;
	int found = 0, err;

	err = mailbox_up(line, size);
	if (err)
		return err;

	memset(&table, 0, sizeof(table));
	err = tegra_bpmp_send_receive(MRQ_EMC_DVFS_TABLE, NULL, 0, &table, sizeof(table));
	if (!err && (table.num_pairs == 0 || table.num_pairs > EMC_DVFS_TABLE_MAX_SIZE))
		err = -EINVAL;
	if (err) {
		snprintf(
			line, size,
			"BPMP firmware %s has no EMC table (MRQ_EMC_DVFS_TABLE %d), EMC left as is",
			fw_tag, err);
		return err;
	}
	for (i = 0; i < table.num_pairs; i++) {
		n += snprintf(rates + n, sizeof(rates) - n, i ? " %u" : "%u",
			      table.pairs[i].freq / 1000);
		if (table.pairs[i].freq == khz) {
			found = 1;
			fw_mv = table.pairs[i].mv;
		}
	}

	memset(&info, 0, sizeof(info));
	err = clk_call(CMD_CLK_GET_ALL_INFO, NULL, 0, &info, sizeof(info));
	info.name[sizeof(info.name) - 1] = 0;
	if (!err && strcmp((const char *)info.name, "emc"))
		err = -ENODEV;
	if (!err)
		err = clk_get_rate(&before);
	if (err) {
		snprintf(line, size,
			 "BPMP clock 0 is not a usable EMC clock (%d, \"%s\"), EMC left as is", err,
			 (const char *)info.name);
		return err;
	}
	if (!khz) {
		snprintf(line, size,
			 "%lld MHz (BPMP firmware %s; EMC tables %s MHz; no rate asked)",
			 before / 1000000, fw_tag, rates);
		return 0;
	}
	if (!found) {
		snprintf(line, size,
			 "%lld MHz kept: %u kHz is not in the BPMP's EMC tables (%s MHz)",
			 before / 1000000, khz, rates);
		return -EINVAL;
	}
	for (i = 0; i < ARRAY_SIZE(emc_core_mv) && !need_mv; i++)
		if (khz <= emc_core_mv[i].khz)
			need_mv = emc_core_mv[i].mv;
	if (!need_mv) {
		snprintf(line, size,
			 "%lld MHz kept: %u MHz is above L4T's T210B01 EMC DVFS table (3200 MHz)",
			 before / 1000000, khz / 1000);
		return -ERANGE;
	}

	err = tegra_i2c_init(PMIC_BUS);
	if (!err)
		err = tegra_i2c_read(PMIC_BUS, MAX77620, MAX77620_REG_SD0, &sd0, 1);
	have_mv = 600 + (sd0 & 0x7f) * 25 / 2;
	if (err || have_mv < need_mv) {
		snprintf(line, size,
			 "%lld MHz kept: %u MHz needs VDD_CORE %u mV (L4T T210B01), MAX77620 SD0 "
			 "reads %u mV "
			 "(I2C5 %d)",
			 before / 1000000, khz / 1000, need_mv, have_mv, err);
		return err ? err : -ERANGE;
	}

	err = clk_call(CMD_CLK_ENABLE, NULL, 0, NULL, 0);
	if (!err)
		err = clk_rate(CMD_CLK_ROUND_RATE, (s64)khz * 1000, &rounded);
	if (!err && rounded != before)
		err = clk_rate(CMD_CLK_SET_RATE, rounded, &after);
	if (!err)
		err = clk_get_rate(&after);
	if (err) {
		snprintf(line, size, "%lld MHz: the BPMP refused %u MHz (MRQ_CLK %d)",
			 before / 1000000, khz / 1000, err);
		return err;
	}
	snprintf(line, size,
		 "%lld -> %lld MHz (BPMP firmware %s; EMC tables %s MHz; VDD_CORE %u mV, L4T needs "
		 "%u, "
		 "firmware table says %u)",
		 before / 1000000, after / 1000000, fw_tag, rates, have_mv, need_mv, fw_mv);
	return 0;
}

/*
 * SC7, as L4T runs it around the BPMP: pm-tegra210.c's syscore suspend tells the firmware which state the next CCPLEX
 * power-down is (MRQ_ENABLE_SUSPEND {TEGRA_PM_SC7, 0}); tegra210-emc.c tegra210b01_emc_suspend drops EMC to 204 MHz
 * (EMC_SUSPEND_RATE, the rate hekate's saved SDRAM parameters bring the DRAM back at) and its resume asks for the saved
 * rate again. ATF restarts the firmware at PMC_SCRATCH39 after SC7 and both sides keep the channel areas, so the
 * mailbox needs nothing new (t210_mail_ops has no resume).
 */
#define EMC_SUSPEND_HZ 204000000LL
static s64 emc_resume_hz;
/* From SC7 entry to its resume: the governor's rate changes are refused, so SC7 enters and leaves at the rates above. */
static int emc_frozen;

int tegra_bpmp_sc7_suspend(char *line, unsigned long size)
{
	u32 enable[2] = {TEGRA_PM_SC7, 0};
	s64 now = 0, rounded = 0, after = 0;
	unsigned int channels;
	int err;

	emc_resume_hz = 0;
	__atomic_store_n(&emc_frozen, 1, __ATOMIC_RELEASE);
	err = mailbox_up(line, size);
	if (err)
		return err;
	err = clk_get_rate(&now);
	if (!err && now > EMC_SUSPEND_HZ) {
		err = clk_rate(CMD_CLK_ROUND_RATE, EMC_SUSPEND_HZ, &rounded);
		if (!err)
			err = clk_rate(CMD_CLK_SET_RATE, rounded, &after);
		if (!err)
			emc_resume_hz = now;
	}
	if (err) {
		snprintf(line, size, "EMC not lowered to 204 MHz for SC7 (MRQ_CLK %d, at %lld MHz)",
			 err, now / 1000000);
		return err;
	}
	err = tegra_bpmp_send_receive(MRQ_ENABLE_SUSPEND, enable, sizeof(enable), NULL, 0);
	if (!err)
		err = tegra_bpmp_suspend();
	channels = tegra_bpmp_semaphore();
	snprintf(line, size,
		 "EMC %lld -> %lld MHz, MRQ_ENABLE_SUSPEND SC7 %s (%d), mailbox channels %#x%s",
		 now / 1000000, (emc_resume_hz ? after : now) / 1000000, err ? "failed" : "sent",
		 err, channels,
		 channels == TEGRA_BPMP_SIGN_OF_LIFE
			 ? ""
			 : " (not all free: ATF will not see the firmware)");
	if (!err && channels != TEGRA_BPMP_SIGN_OF_LIFE)
		err = -EBUSY;
	return err;
}

int tegra_bpmp_sc7_resume(char *line, unsigned long size)
{
	s64 now = 0, after = 0;
	int err;

	if (mailbox_state) {
		__atomic_store_n(&emc_frozen, 0, __ATOMIC_RELEASE);
		snprintf(line, size, "BPMP mailbox not up");
		return 0;
	}
	tegra_bpmp_resume();
	err = clk_get_rate(&now);
	if (!err && emc_resume_hz && now != emc_resume_hz)
		err = clk_rate(CMD_CLK_SET_RATE, emc_resume_hz, &after);
	if (!err)
		err = clk_get_rate(&after);
	__atomic_store_n(&emc_frozen, 0, __ATOMIC_RELEASE);
	snprintf(line, size, "EMC %lld -> %lld MHz after SC7 (MRQ_CLK %d)", now / 1000000,
		 after / 1000000, err);
	return err;
}

int tegra_bpmp_emc_rates(unsigned int *khz, unsigned int max)
{
	struct mrq_emc_dvfs_table_response table;
	unsigned int i, n = 0;
	int err;

	if (mailbox_state)
		return mailbox_state < 0 ? mailbox_state : -ENODEV;
	memset(&table, 0, sizeof(table));
	err = tegra_bpmp_send_receive(MRQ_EMC_DVFS_TABLE, NULL, 0, &table, sizeof(table));
	if (err)
		return err;
	if (table.num_pairs == 0 || table.num_pairs > EMC_DVFS_TABLE_MAX_SIZE)
		return -EINVAL;
	for (i = 0; i < table.num_pairs && n < max; i++)
		khz[n++] = table.pairs[i].freq;
	return (int)n;
}

int tegra_bpmp_emc_get(unsigned int *khz)
{
	s64 now = 0;
	int err;

	if (mailbox_state)
		return mailbox_state < 0 ? mailbox_state : -ENODEV;
	err = clk_get_rate(&now);
	if (!err)
		*khz = (unsigned int)(now / 1000);
	return err;
}

int tegra_bpmp_emc_set(unsigned int khz, unsigned int *now_khz)
{
	s64 after = 0;
	int err;

	if (mailbox_state)
		return mailbox_state < 0 ? mailbox_state : -ENODEV;
	if (__atomic_load_n(&emc_frozen, __ATOMIC_ACQUIRE))
		return -EBUSY;
	err = clk_rate(CMD_CLK_SET_RATE, (s64)khz * 1000, &after);
	if (!err)
		*now_khz = (unsigned int)(after / 1000);
	return err;
}
