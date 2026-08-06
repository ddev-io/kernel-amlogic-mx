/* One-shot append-only U-Boot environment restoration for the G04 NAND. */

#include <linux/atomic.h>
#include <linux/cred.h>
#include <linux/crc32.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/nand.h>
#include <linux/mtd/partitions.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#include <mach/nand.h>

#include "g343_bbt_restore.h"

#define G343_SOURCE_OUTER_CRC        0xe0cf7639U
#define G343_SOURCE_BBT_CRC          0xacbcfcadU
#define G343_FACTORY_ENV_CRC         0x4c8295b2U
#define G343_FACTORY_PREFIX_CRC      0x4a7e5ffdU
#define G343_NEW_OUTER_CRC           0xd2437f9fU
#define G343_NEW_RECORD_CRC          0x7d54bf78U
#define G343_TOTAL_BLOCKS            2048U
#define G343_ERASE_SIZE              0x00800000U
#define G343_WRITE_SIZE              0x00008000U
#define G343_OOB_SIZE                0x00000700U
#define G343_MTD_SIZE                0x400000000ULL
#define G343_ENV_BLOCK               2U
#define G343_SOURCE_PAGE             2U
#define G343_TARGET_PAGE             3U
#define G343_SOURCE_ADDR             0x01010000ULL
#define G343_TARGET_ADDR             0x01018000ULL
#define G343_SOURCE_TIMESTAMP        2U
#define G343_NEW_TIMESTAMP           3U
#define G343_TARGET_LOW_PAGE         2060U
#define G343_EXPECTED_PARTS          9U
#define G343_FACTORY_ENV_SIZE         4953U
#define G343_ENV_PREFIX_SIZE         0x6cccU
#define G343_NEW_BBT_CRC             G343_SOURCE_BBT_CRC
#define G343_COMMAND                 "append-vb100a-env-upgrade2-page3-g346"

/*
 * Exact default environment extracted from the VB100a U-Boot currently
 * present in mtd0 (SHA-256 f542c751...d1fb7).  The containing UCL stream is
 * byte-identical to vb100a_u-boot-comp.ucl, whose decompressed image has the
 * environment at offset 0x74244.  The only intentional change is
 * upgrade_step=2 so the stock preboot does not run defenv/save/update.
 *
 * Every explicit NUL terminates one variable; C appends the second final NUL.
 */
static const u8 g346_factory_env[] =
	"bootcmd=video dev bl_off; video clear; bmp display ${bootup_offset}; video dev bl_on;   nand read boot ${loadaddr} 0 500000; setenv bootargs ${bootargs} a9_clk_max=1512000000; bootm\0"
	"bootdelay=1\0"
	"baudrate=115200\0"
	"preboot=\0"
	"bootfile=uImage\0"
	"loadaddr=0x82000000\0"
	"testaddr=0x82400000\0"
	"loadaddr_misc=0x83000000\0"
	"usbtty=cdc_acm\0"
	"console=ttyS2,115200n8\0"
	"mmcargs=setenv bootargs console=${console} boardname=m6_g08\0"
	"chipname=8726m\0"
	"machid=4e21\0"
	"upgrade_step=2\0"
	"video_dev=panel\0"
	"display_width=1280\0"
	"display_height=800\0"
	"display_bpp=16\0"
	"display_color_format_index=16\0"
	"display_layer=osd2\0"
	"display_color_fg=0xffff\0"
	"display_color_bg=0\0"
	"fb_addr=0x85100000\0"
	"sleep_threshold=20\0"
	"batlow_threshold=5\0"
	"batfull_threshold=100\0"
	"bootargs=init=/init console=ttyS0,115200n8 hlt no_console_suspend vmalloc=256m mem=1024m logo=osd1,loaded,panel,debug hdmitx=vdacoff,powermode1,unplug_powerdown\0"
	"preboot=chk_all_regulators; board_special_init; run batlow_or_not;run upgrade_check; setenv sleep_count 0; saradc open 4;run updatekey_or_not; run usb_burning_or_not; set_chgcur 500; run switch_bootmode\0"
	"upgrade_check=if itest ${upgrade_step} == 0; then defenv; save; run update; else if itest ${upgrade_step} == 1; then defenv_without reboot_mode; setenv upgrade_step 2; save; fi; fi\0"
	"switch_bootmode=if check_reset; then run reset; fi; get_rebootmode; clear_rebootmode; echo reboot_mode=${reboot_mode}; if test ${reboot_mode} = normal; then run prepare; bmp display ${bootup_offset}; else if test ${reboot_mode} = factory_reset; then run recovery; else if test ${reboot_mode} = update; then run update; else run charging_or_not; fi; fi; fi\0"
	"prepare=nand read logo ${loadaddr_misc} 0 600000; unpackimg ${loadaddr_misc}; video open; video clear\0"
	"update=run prepare;video dev bl_off; bmp display ${bootup_offset}; video dev bl_on; if mmcinfo; then if fatload mmc 0 ${loadaddr} aml_autoscript; then autoscr ${loadaddr}; fi; if fatload mmc 0 ${loadaddr} uImage_recovery; then bootm; fi; fi; nand read recovery ${loadaddr} 0 500000; setenv bootargs ${bootargs} a9_clk_max=800000000; bootm\0"
	"recovery=run prepare; video dev bl_off; bmp display ${bootup_offset}; video dev bl_on; if nand read recovery ${loadaddr} 0 500000; then setenv bootargs ${bootargs} a9_clk_max=800000000; bootm; else echo no uImage_recovery in NAND; fi\0"
	"charging_or_not=if ac_online; then run prepare; run charging; else if getkey; then run prepare; run limit_charging; run bootcmd; else poweroff; fi; fi\0"
	"charging=video clear; run limit_charging; run display_loop\0"
	"display_loop=video dev bl_on; while itest 1 == 1; do get_batcap; if itest ${battery_cap} >= ${batfull_threshold}; then bmp display ${batteryfull_offset}; run custom_delay; poweroff; else bmp display ${battery0_offset}; run custom_delay; bmp display ${battery1_offset}; run custom_delay; bmp display ${battery2_offset}; run custom_delay; bmp display ${battery3_offset}; run custom_delay; fi; done\0"
	"custom_delay=setenv msleep_count 0; while itest ${msleep_count} < 800; do run aconline_or_not; run updatekey_or_not; run powerkey_or_not; msleep 1; calc ${msleep_count} + 1 msleep_count; done; run sleep_or_not\0"
	"sleep_or_not=if itest ${sleep_count} > ${sleep_threshold}; then run into_sleep; setenv sleep_count 0; else calc ${sleep_count} + 1 sleep_count; fi\0"
	"into_sleep=setenv sleep_enable 1; video dev disable; run normal_charging; while itest ${sleep_enable} == 1; do run sleep_get_key; done; run limit_charging; video dev enable; video dev bl_on\0"
	"sleep_get_key=run aconline_or_not;if getkey; then msleep 100; if getkey; then setenv sleep_enable 0; fi; fi; if saradc get_in_range 0x0 0x380; then msleep 100; if saradc get_in_range 0x0 0x380; then setenv sleep_enable 0; fi; fi\0"
	"powerkey_or_not=if getkey; then msleep 500; if getkey; then run limit_charging; run bootcmd; fi; fi\0"
	"updatekey_or_not=if saradc get_in_range 0x78 0xC8; then msleep 500; if getkey; then if saradc get_in_range 0x78 0xC8; then run update; fi; fi; fi\0"
	"usb_burning_or_not=if saradc get_in_range 0x1e 0x6e; then msleep 500; if getkey; then if saradc get_in_range 0x1e 0x6e; then run prepare;video dev bl_off; bmp display ${bootup_offset}; video dev bl_on; run usb_burning; fi; fi; fi\0"
	"aconline_or_not=if ac_online; then; else poweroff; fi\0"
	"batlow_or_not=if ac_online; then; else get_batcap; if itest ${battery_cap} < ${batlow_threshold}; then run prepare; run batlow_warning; poweroff; fi; fi\0"
	"batlow_warning=video dev bl_on; bmp display ${batterylow_offset}; msleep 500; bmp display ${batterylow_offset}; msleep 500; bmp display ${batterylow_offset}; msleep 500; bmp display ${batterylow_offset}; msleep 500; bmp display ${batterylow_offset}; msleep 1000\0"
	"usb_burning=tiny_usbtool 20000\0"
	"no_volume_usb_burning=if fatload mmc 0 ${loadaddr} novolume_usbburning; then run usb_burning; fi\0"
	"reset=run prepare;run bootcmd\0"
	"normal_charging=if itest ${ac_vbus} == 1; then set_chgcur 500; else set_chgcur 1200; fi; mw 0xc110419c 31;\0"
	"limit_charging=if itest ${ac_vbus} == 1; then set_chgcur 0; else set_chgcur 700; fi; mw 0xc110419c b1; \0";

struct g343_part_expect {
	const char *name;
	u64 offset;
	u64 size;
};

/* Board-table values in the corrected envx record before add_partition(). */
static const struct g343_part_expect g343_record_parts[G343_EXPECTED_PARTS] = {
	{ "logo",      0x04800000ULL, 0x00800000ULL },
	{ "aml_logo",  0x05800000ULL, 0x00800000ULL },
	{ "recovery",  0x06800000ULL, 0x00800000ULL },
	{ "boot",      0x08800000ULL, 0x00800000ULL },
	{ "system",    0x0a800000ULL, 0x30000000ULL },
	{ "factory",   0x3a800000ULL, 0x08000000ULL },
	{ "cache",     0x42800000ULL, 0x08000000ULL },
	{ "userdata",  0x4a800000ULL, 0x90000000ULL },
	{ "NFTL_Part", MTDPART_OFS_APPEND, MTDPART_SIZ_FULL },
};

/* These are the post-add_partition() values in the platform table.  The MTD
 * slave expands the final FULL-size sentinel without changing this table. */
static const struct g343_part_expect g343_runtime_parts[G343_EXPECTED_PARTS] = {
	{ "logo",      0x02000000ULL, 0x00800000ULL },
	{ "aml_logo",  0x02800000ULL, 0x00800000ULL },
	{ "recovery",  0x03000000ULL, 0x00800000ULL },
	{ "boot",      0x03800000ULL, 0x00800000ULL },
	{ "system",    0x04000000ULL, 0x30000000ULL },
	{ "factory",   0x34000000ULL, 0x08000000ULL },
	{ "cache",     0x3c000000ULL, 0x08000000ULL },
	{ "userdata",  0x44000000ULL, 0x90000000ULL },
	{ "NFTL_Part", 0xd4000000ULL, MTDPART_SIZ_FULL },
};

struct g343_state {
	struct aml_nand_chip *aml_chip;
	struct mtd_info *mtd;
	struct mutex lock;
	u8 *old0;
	u8 *old1;
	u8 *empty0;
	u8 *empty1;
	u8 *raw_empty0;
	u8 *raw_empty1;
	u8 *new_record;
	u8 *verify;
	struct env_oobinfo_t old0_oob;
	struct env_oobinfo_t old1_oob;
	struct env_oobinfo_t empty0_oob;
	struct env_oobinfo_t empty1_oob;
	struct env_oobinfo_t new_oob;
	struct env_oobinfo_t verify_oob;
	u32 old_outer_crc;
	u32 old_bbt_crc;
	u32 new_outer_crc;
	u32 new_bbt_crc;
	int geometry_ok;
	int source_parts_ok;
	int preflight_result;
	int preflight_complete;
	int attempted;
	int gate_accepted;
	int write_result;
	int verify_result;
	int success;
};

struct g343_gate {
	atomic_t armed;
	struct mtd_info *mtd;
	const u8 *data;
	struct env_oobinfo_t oob;
	int page;
};

static struct g343_state *g343_singleton;
static struct g343_gate g343_gate = {
	.armed = ATOMIC_INIT(0),
};

static u32 g343_crc(const void *data, size_t size)
{
	return crc32((0 ^ 0xffffffffL), data, size) ^ 0xffffffffL;
}

struct g343_env_scan {
	unsigned int entries;
	unsigned int upgrade_step_entries;
	unsigned int printable;
	unsigned int terminated;
	size_t terminator;
};

static void g343_scan_env(const u8 *data, size_t limit,
			  struct g343_env_scan *scan)
{
	static const char upgrade[] = "upgrade_step=";
	size_t pos = 0;
	size_t end;
	size_t i;

	memset(scan, 0, sizeof(*scan));
	scan->printable = 1;
	while (pos < limit) {
		if (!data[pos]) {
			if (pos + 1 < limit && !data[pos + 1]) {
				scan->terminated = 1;
				scan->terminator = pos;
				return;
			}
			scan->printable = 0;
			pos++;
			continue;
		}
		end = pos;
		while (end < limit && data[end])
			end++;
		if (end == limit)
			return;
		for (i = pos; i < end; i++)
			if (data[i] < 0x20 || data[i] > 0x7e)
				scan->printable = 0;
		scan->entries++;
		if (end - pos >= sizeof(upgrade) - 1 &&
		    !memcmp(data + pos, upgrade, sizeof(upgrade) - 1))
			scan->upgrade_step_entries++;
		if (end + 1 < limit && !data[end + 1]) {
			scan->terminated = 1;
			scan->terminator = end;
			return;
		}
		pos = end + 1;
	}
}

static void g343_show_env_entries(struct seq_file *seq, const char *label,
				  const u8 *data, size_t limit)
{
	struct g343_env_scan scan;
	size_t pos = 0;
	size_t end;
	size_t i;
	unsigned int entry = 0;

	g343_scan_env(data, limit, &scan);
	seq_printf(seq,
		   "%s_summary entries=%u terminated=%u terminator=0x%lx "
		   "printable=%u upgrade_step_entries=%u prefix_crc=0x%08x\n",
		   label, scan.entries, scan.terminated,
		   (unsigned long)scan.terminator, scan.printable,
		   scan.upgrade_step_entries, g343_crc(data, limit));
	while (pos < limit && data[pos]) {
		end = pos;
		while (end < limit && data[end])
			end++;
		if (end == limit)
			break;
		seq_printf(seq, "%s[%03u]=", label, ++entry);
		for (i = pos; i < end; i++) {
			if (data[i] >= 0x20 && data[i] <= 0x7e)
				seq_putc(seq, data[i]);
			else
				seq_printf(seq, "\\x%02x", data[i]);
		}
		seq_putc(seq, '\n');
		pos = end + 1;
	}
}

static int g343_all_ff(const u8 *data, size_t size)
{
	size_t i;

	for (i = 0; i < size; i++)
		if (data[i] != 0xff)
			return 0;
	return 1;
}

static u64 g343_addr(unsigned int page)
{
	return (u64)G343_ENV_BLOCK * G343_ERASE_SIZE +
		(u64)page * G343_WRITE_SIZE;
}

static int g343_read_page(struct g343_state *state, unsigned int page,
			  u8 *data, struct env_oobinfo_t *oob)
{
	struct mtd_oob_ops ops;
	int error;

	memset(data, 0xa5, state->mtd->writesize);
	memset(oob, 0xa5, sizeof(*oob));
	memset(&ops, 0, sizeof(ops));
	ops.mode = MTD_OOB_AUTO;
	ops.len = state->mtd->writesize;
	ops.ooblen = sizeof(*oob);
	ops.ooboffs = state->mtd->ecclayout->oobfree[0].offset;
	ops.datbuf = data;
	ops.oobbuf = (u8 *)oob;
	error = state->mtd->read_oob(state->mtd, g343_addr(page), &ops);
	if (error && error != -EUCLEAN)
		return error;
	if (ops.retlen != state->mtd->writesize ||
	    ops.oobretlen != sizeof(*oob))
		return -EIO;
	return 0;
}

/*
 * The factory two-plane raw callback returns each physical data+spare tuple
 * consecutively through datbuf.  datbuf therefore deliberately has room for
 * writesize + oobsize even though the MTD core reports only writesize bytes in
 * retlen.  Two reads with inverse fill values detect a partial/stale capture.
 */
static int g343_read_raw_page(struct g343_state *state, unsigned int page,
			      u8 *buffer, u8 fill)
{
	struct nand_chip *chip = state->mtd->priv;
	struct mtd_oob_ops ops;
	unsigned int raw_size = state->mtd->writesize + state->mtd->oobsize;
	int error;

	memset(buffer, fill, raw_size);
	memset(chip->oob_poi, fill, state->mtd->oobsize);
	memset(&ops, 0, sizeof(ops));
	ops.mode = MTD_OOB_RAW;
	ops.len = state->mtd->writesize;
	ops.ooblen = state->mtd->oobsize;
	ops.ooboffs = 0;
	ops.datbuf = buffer;
	ops.oobbuf = NULL;
	chip->pagebuf = -1;
	error = state->mtd->read_oob(state->mtd, g343_addr(page), &ops);
	if (error)
		return error;
	if (ops.retlen != state->mtd->writesize || ops.oobretlen)
		return -EIO;
	return 0;
}

static int g343_validate_source_record(struct g343_state *state, env_t *env)
{
	struct aml_nand_bbt_info *bbt;
	struct aml_nand_part_info *part;
	unsigned int entries = 0;
	unsigned int factory = 0;
	unsigned int runtime = 0;
	unsigned int out_of_range = 0;
	unsigned int parts = 0;
	unsigned int i;
	u16 value;
	u32 outer_crc;

	outer_crc = g343_crc(env->data, ENV_SIZE);
	if (outer_crc != env->crc || env->crc != G343_SOURCE_OUTER_CRC)
		return -EINVAL;
	bbt = (struct aml_nand_bbt_info *)
		(env->data + ENV_SIZE - sizeof(*bbt));
	if (memcmp(bbt->bbt_head_magic, BBT_HEAD_MAGIC, 4) ||
	    memcmp(bbt->bbt_tail_magic, BBT_TAIL_MAGIC, 4))
		return -EINVAL;
	for (i = 0; i < MAX_BAD_BLK_NUM; i++) {
		value = (u16)bbt->nand_bbt[i];
		if (!value)
			continue;
		entries++;
		if (value & 0x8000)
			factory++;
		else
			runtime++;
		if ((value & 0x7fff) >= G343_TOTAL_BLOCKS)
			out_of_range++;
	}
	for (i = 0; i < MAX_MTD_PART_NUM; i++)
		if (!memcmp(bbt->aml_nand_part[i].mtd_part_magic,
			    MTD_PART_MAGIC, 4))
			parts++;
	state->old_outer_crc = outer_crc;
	state->old_bbt_crc = g343_crc(bbt, sizeof(*bbt));
	if (state->old_bbt_crc != G343_SOURCE_BBT_CRC || entries || factory ||
	    runtime || out_of_range || parts != G343_EXPECTED_PARTS)
		return -EINVAL;
	for (i = 0; i < G343_EXPECTED_PARTS; i++) {
		part = &bbt->aml_nand_part[i];
		if (strncmp(part->mtd_part_name, g343_record_parts[i].name,
			    MAX_MTD_PART_NAME_LEN) ||
		    part->offset != g343_record_parts[i].offset ||
		    part->size != g343_record_parts[i].size || part->mask_flags)
			return -EINVAL;
	}
	return 0;
}

static int g343_validate_source_oob(const struct env_oobinfo_t *oob)
{
	if (memcmp(oob->name, ENV_NAND_MAGIC, 4) || oob->ec != -1 ||
	    oob->timestamp != G343_SOURCE_TIMESTAMP || !oob->status_page)
		return -EINVAL;
	return 0;
}

static int g343_validate_source_parts(struct g343_state *state)
{
	struct platform_nand_chip *chip;
	struct mtd_partition *part;
	unsigned int i;

	chip = &state->aml_chip->platform->platform_nand_data.chip;
	if (chip->nr_partitions != G343_EXPECTED_PARTS || !chip->partitions)
		return -EINVAL;
	for (i = 0; i < G343_EXPECTED_PARTS; i++) {
		part = &chip->partitions[i];
		if (!part->name ||
		    strcmp(part->name, g343_runtime_parts[i].name) ||
		    part->offset != g343_runtime_parts[i].offset ||
		    part->size != g343_runtime_parts[i].size ||
		    part->mask_flags != MTD_WRITEABLE) {
			printk(KERN_ERR
			       "G346 rescue: partition preflight mismatch index=%u "
			       "name=%s offset=0x%llx size=0x%llx mask=0x%x\n",
			       i, part->name ? part->name : "<null>", part->offset,
			       part->size, part->mask_flags);
			return -EINVAL;
		}
	}
	return 0;
}

static void g343_build_new_record(struct g343_state *state)
{
	env_t *env = (env_t *)state->new_record;
	struct aml_nand_bbt_info *bbt;

	memset(state->new_record, 0, state->mtd->writesize);
	memcpy(env->data, g346_factory_env, sizeof(g346_factory_env));
	bbt = (struct aml_nand_bbt_info *)
		(env->data + ENV_SIZE - sizeof(*bbt));
	memcpy(bbt, ((env_t *)state->old1)->data + ENV_SIZE - sizeof(*bbt),
	       sizeof(*bbt));
	env->crc = g343_crc(env->data, ENV_SIZE);
	state->new_outer_crc = env->crc;
	state->new_bbt_crc = g343_crc(bbt, sizeof(*bbt));
	if (state->new_bbt_crc != G343_NEW_BBT_CRC ||
	    state->new_outer_crc != G343_NEW_OUTER_CRC)
		printk(KERN_ERR
		       "G346 rescue: unexpected generated CRCs outer=0x%08x "
		       "bbt=0x%08x\n", state->new_outer_crc,
		       state->new_bbt_crc);
	memset(&state->new_oob, 0, sizeof(state->new_oob));
	memcpy(state->new_oob.name, ENV_NAND_MAGIC, 4);
	state->new_oob.ec = -1;
	state->new_oob.timestamp = G343_NEW_TIMESTAMP;
	state->new_oob.status_page = 1;
}

/* Compile-time guards for the exact legacy on-flash ABI used by the CRC. */
static inline void g343_layout_assertions(void)
{
	BUILD_BUG_ON(sizeof(struct aml_nand_part_info) != 56);
	BUILD_BUG_ON(sizeof(struct aml_nand_bbt_info) != 0x1330);
	BUILD_BUG_ON(offsetof(struct aml_nand_bbt_info, nand_bbt) != 4);
	BUILD_BUG_ON(offsetof(struct aml_nand_bbt_info, aml_nand_part) != 0xfa8);
	BUILD_BUG_ON(offsetof(struct aml_nand_bbt_info, bbt_tail_magic) != 0x1328);
	BUILD_BUG_ON(sizeof(struct env_oobinfo_t) != 8);
	BUILD_BUG_ON(sizeof(g346_factory_env) != G343_FACTORY_ENV_SIZE);
	BUILD_BUG_ON(G343_ENV_PREFIX_SIZE + sizeof(struct aml_nand_bbt_info) !=
		     ENV_SIZE);
}

static int g343_preflight(struct g343_state *state)
{
	struct g343_env_scan proposed;
	int error;

	state->preflight_complete = 0;
	state->geometry_ok = 0;
	state->source_parts_ok = 0;
	state->old_outer_crc = 0;
	state->old_bbt_crc = 0;
	state->new_outer_crc = 0;
	state->new_bbt_crc = 0;
	state->geometry_ok = state->mtd->size == G343_MTD_SIZE &&
		state->mtd->erasesize == G343_ERASE_SIZE &&
		state->mtd->writesize == G343_WRITE_SIZE &&
		state->mtd->oobsize == G343_OOB_SIZE &&
		state->aml_chip->plane_num == 2 &&
		state->aml_chip->chip.page_shift == 13 &&
		(G343_TARGET_ADDR >> state->aml_chip->chip.page_shift) ==
			G343_TARGET_LOW_PAGE &&
		(state->mtd->size / state->mtd->erasesize) == G343_TOTAL_BLOCKS;
	if (!state->geometry_ok)
		return -EINVAL;
	state->source_parts_ok = !g343_validate_source_parts(state);
	if (!state->source_parts_ok)
		return -EINVAL;
	error = g343_read_page(state, G343_SOURCE_PAGE,
					state->old0, &state->old0_oob);
	if (error)
		return error;
	error = g343_read_page(state, G343_SOURCE_PAGE,
					state->old1, &state->old1_oob);
	if (error)
		return error;
	if (memcmp(state->old0, state->old1, state->mtd->writesize) ||
	    memcmp(&state->old0_oob, &state->old1_oob,
		   sizeof(state->old0_oob)))
		return -EINVAL;
	if (g343_validate_source_oob(&state->old0_oob) ||
	    g343_validate_source_record(state, (env_t *)state->old0))
		return -EINVAL;
	error = g343_read_page(state, G343_TARGET_PAGE,
				state->empty0, &state->empty0_oob);
	if (error)
		return error;
	error = g343_read_page(state, G343_TARGET_PAGE,
				state->empty1, &state->empty1_oob);
	if (error)
		return error;
	if (memcmp(state->empty0, state->empty1, state->mtd->writesize) ||
	    memcmp(&state->empty0_oob, &state->empty1_oob,
		   sizeof(state->empty0_oob)) ||
	    !g343_all_ff(state->empty0, state->mtd->writesize) ||
	    !g343_all_ff((u8 *)&state->empty0_oob,
			 sizeof(state->empty0_oob)))
		return -EINVAL;
	error = g343_read_raw_page(state, G343_TARGET_PAGE,
				     state->raw_empty0, 0xa5);
	if (error)
		return error;
	error = g343_read_raw_page(state, G343_TARGET_PAGE,
				     state->raw_empty1, 0x5a);
	if (error)
		return error;
	if (memcmp(state->raw_empty0, state->raw_empty1,
		   state->mtd->writesize + state->mtd->oobsize) ||
	    !g343_all_ff(state->raw_empty0,
			 state->mtd->writesize + state->mtd->oobsize))
		return -EINVAL;
	g343_build_new_record(state);
	g343_scan_env(((env_t *)state->new_record)->data,
		      G343_ENV_PREFIX_SIZE, &proposed);
	if (state->new_bbt_crc != G343_NEW_BBT_CRC ||
	    state->new_outer_crc != G343_NEW_OUTER_CRC ||
	    g343_crc(g346_factory_env, sizeof(g346_factory_env)) !=
		G343_FACTORY_ENV_CRC ||
	    g343_crc(((env_t *)state->new_record)->data,
		     G343_ENV_PREFIX_SIZE) != G343_FACTORY_PREFIX_CRC ||
	    g343_crc(state->new_record, state->mtd->writesize) !=
		G343_NEW_RECORD_CRC || proposed.entries != 51 ||
	    proposed.upgrade_step_entries != 1 || !proposed.printable ||
	    !proposed.terminated || proposed.terminator !=
		G343_FACTORY_ENV_SIZE - 2)
		return -EINVAL;
	if (memcmp(((env_t *)state->new_record)->data + G343_ENV_PREFIX_SIZE,
		   ((env_t *)state->old1)->data + G343_ENV_PREFIX_SIZE,
		   sizeof(struct aml_nand_bbt_info)))
		return -EINVAL;
	state->preflight_complete = 1;
	return 0;
}

static void g343_gate_arm(struct g343_state *state)
{
	struct nand_chip *chip = &state->aml_chip->chip;

	g343_gate.mtd = state->mtd;
	g343_gate.data = state->new_record;
	g343_gate.oob = state->new_oob;
	g343_gate.page = (int)(G343_TARGET_ADDR >> chip->page_shift);
	smp_wmb();
	atomic_set(&g343_gate.armed, 1);
}

static void g343_gate_disarm(void)
{
	atomic_set(&g343_gate.armed, 0);
}

int g343_bbt_write_gate_take(struct mtd_info *mtd, const unsigned char *data,
			     const unsigned char *oob, int page,
			     int cached, int raw)
{
	unsigned int oob_offset;

	if (!atomic_read(&g343_gate.armed) || mtd != g343_gate.mtd ||
	    data != g343_gate.data || page != g343_gate.page || cached || raw ||
	    !mtd->ecclayout || !oob ||
	    g343_crc(data, G343_WRITE_SIZE) != G343_NEW_RECORD_CRC)
		return 0;
	oob_offset = mtd->ecclayout->oobfree[0].offset;
	if (oob_offset + sizeof(g343_gate.oob) > mtd->oobsize ||
	    memcmp(oob + oob_offset, &g343_gate.oob,
		   sizeof(g343_gate.oob)) ||
	    memcmp(g343_gate.oob.name, ENV_NAND_MAGIC, 4) ||
	    g343_gate.oob.ec != -1 ||
	    g343_gate.oob.timestamp != G343_NEW_TIMESTAMP ||
	    !g343_gate.oob.status_page)
		return 0;
	if (atomic_cmpxchg(&g343_gate.armed, 1, 0) != 1)
		return 0;
	if (g343_singleton)
		g343_singleton->gate_accepted++;
	return 1;
}

static int g343_write_record(struct g343_state *state)
{
	struct mtd_oob_ops ops;
	int error;

	memset(&ops, 0, sizeof(ops));
	ops.mode = MTD_OOB_AUTO;
	ops.len = state->mtd->writesize;
	ops.ooblen = sizeof(state->new_oob);
	ops.ooboffs = state->mtd->ecclayout->oobfree[0].offset;
	ops.datbuf = state->new_record;
	ops.oobbuf = (u8 *)&state->new_oob;
	g343_gate_arm(state);
	error = state->mtd->write_oob(state->mtd, G343_TARGET_ADDR, &ops);
	g343_gate_disarm();
	if (error)
		return error;
	if (ops.retlen != state->mtd->writesize ||
	    ops.oobretlen != sizeof(state->new_oob) ||
	    state->gate_accepted != 1)
		return -EIO;
	if (state->mtd->sync)
		state->mtd->sync(state->mtd);
	return 0;
}

static int g343_verify_record(struct g343_state *state)
{
	struct env_oobinfo_t second_oob;
	int error;

	error = g343_read_page(state, G343_TARGET_PAGE,
				state->verify, &state->verify_oob);
	if (error)
		return error;
	if (memcmp(state->verify, state->new_record, state->mtd->writesize) ||
	    memcmp(&state->verify_oob, &state->new_oob,
		   sizeof(state->new_oob)))
		return -EIO;
	error = g343_read_page(state, G343_TARGET_PAGE,
				state->empty0, &second_oob);
	if (error)
		return error;
	if (memcmp(state->empty0, state->new_record, state->mtd->writesize) ||
	    memcmp(&second_oob, &state->new_oob, sizeof(state->new_oob)))
		return -EIO;
	error = g343_read_page(state, G343_SOURCE_PAGE,
					state->empty0, &second_oob);
	if (error || memcmp(state->empty0, state->old0, state->mtd->writesize) ||
	    memcmp(&second_oob, &state->old0_oob, sizeof(second_oob)))
		return -EIO;
	error = g343_read_page(state, G343_SOURCE_PAGE,
					state->empty0, &second_oob);
	if (error || memcmp(state->empty0, state->old1, state->mtd->writesize) ||
	    memcmp(&second_oob, &state->old1_oob, sizeof(second_oob)))
		return -EIO;
	return 0;
}

static int g343_show(struct seq_file *seq, void *unused)
{
	struct g343_state *state = seq->private;
	int preflight;

	mutex_lock(&state->lock);
	if (!state->attempted) {
		preflight = g343_preflight(state);
		state->preflight_result = preflight;
	} else {
		preflight = state->preflight_result;
	}
	seq_printf(seq,
			"g346=one_shot_append_only_env target=0x%llx block=%u page=%u "
			"low_page=%u "
			"erase=disabled markbad=disabled general_write=disabled\n",
		G343_TARGET_ADDR, G343_ENV_BLOCK, G343_TARGET_PAGE,
		G343_TARGET_LOW_PAGE);
	seq_printf(seq,
		"geometry_ok=%d source_parts_ok=%d preflight_complete=%d "
		"preflight_result=%d\n",
		state->geometry_ok, state->source_parts_ok,
		state->preflight_complete, preflight);
	seq_printf(seq,
			"source_outer_crc=0x%08x source_bbt_crc=0x%08x "
			"new_outer_crc=0x%08x new_bbt_crc=0x%08x\n",
		state->old_outer_crc, state->old_bbt_crc,
		state->new_outer_crc, state->new_bbt_crc);
	seq_printf(seq,
			"source=0x%llx source_slot=2:2 source_timestamp=%u "
			"new_slot=2:3 "
			"new_timestamp=%u bbt_preserved=1 upgrade_step=2 "
			"partition_entries=%u\n",
			G343_SOURCE_ADDR, G343_SOURCE_TIMESTAMP, G343_NEW_TIMESTAMP,
			G343_EXPECTED_PARTS);
	seq_printf(seq,
		"attempted=%d gate_accepted=%d write_result=%d verify_result=%d "
		"success=%d\n",
		state->attempted, state->gate_accepted, state->write_result,
		state->verify_result, state->success);
	if (state->preflight_complete) {
		g343_show_env_entries(seq, "source_env",
				      ((env_t *)state->old1)->data,
				      G343_ENV_PREFIX_SIZE);
		g343_show_env_entries(seq, "proposed_env",
				      ((env_t *)state->new_record)->data,
				      G343_ENV_PREFIX_SIZE);
	} else {
		seq_puts(seq, "env_dump_available=0\n");
	}
	if (!state->attempted && !preflight)
		seq_printf(seq, "confirmation=%s\n", G343_COMMAND);
	mutex_unlock(&state->lock);
	return 0;
}

static int g343_open(struct inode *inode, struct file *file)
{
	return single_open(file, g343_show, PDE(inode)->data);
}

static ssize_t g343_write(struct file *file, const char __user *buffer,
			  size_t count, loff_t *position)
{
	struct g343_state *state = PDE(file->f_path.dentry->d_inode)->data;
	char command[64];
	size_t length;
	int error;

	if (!state || !count || count >= sizeof(command))
		return -EINVAL;
	if (current_uid() != 0)
		return -EPERM;
	if (copy_from_user(command, buffer, count))
		return -EFAULT;
	command[count] = '\0';
	length = count;
	while (length && (command[length - 1] == '\n' ||
			  command[length - 1] == '\r'))
		command[--length] = '\0';
	if (strcmp(command, G343_COMMAND))
		return -EPERM;
	mutex_lock(&state->lock);
	if (state->attempted) {
		error = -EPERM;
		goto out;
	}
	error = g343_preflight(state);
	state->preflight_result = error;
	if (error)
		goto out;
	state->attempted = 1;
	state->write_result = g343_write_record(state);
	if (state->write_result) {
		error = state->write_result;
		goto out;
	}
	state->verify_result = g343_verify_record(state);
	if (state->verify_result) {
		error = state->verify_result;
		goto out;
	}
	state->success = 1;
	error = count;
	printk(KERN_ALERT
		       "G346 rescue: append-only U-Boot environment restore "
		       "verified at 0x%llx\n",
	       G343_TARGET_ADDR);
out:
	mutex_unlock(&state->lock);
	return error;
}

static const struct file_operations g343_fops = {
	.owner = THIS_MODULE,
	.open = g343_open,
	.read = seq_read,
	.write = g343_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static void g343_free_state(struct g343_state *state)
{
	if (!state)
		return;
	vfree(state->old0);
	vfree(state->old1);
	vfree(state->empty0);
	vfree(state->empty1);
	vfree(state->raw_empty0);
	vfree(state->raw_empty1);
	vfree(state->new_record);
	vfree(state->verify);
	kfree(state);
}

int g343_bbt_restore_register(struct aml_nand_chip *aml_chip)
{
	struct g343_state *state;
	unsigned int size = aml_chip->mtd.writesize;
	unsigned int raw_size = size + aml_chip->mtd.oobsize;

	g343_layout_assertions();

	if (g343_singleton)
		return -EBUSY;
	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;
	state->aml_chip = aml_chip;
	state->mtd = &aml_chip->mtd;
	mutex_init(&state->lock);
	state->write_result = -EAGAIN;
	state->verify_result = -EAGAIN;
	state->old0 = vmalloc(size);
	state->old1 = vmalloc(size);
	state->empty0 = vmalloc(size);
	state->empty1 = vmalloc(size);
	state->raw_empty0 = vmalloc(raw_size);
	state->raw_empty1 = vmalloc(raw_size);
	state->new_record = vmalloc(size);
	state->verify = vmalloc(size);
	if (!state->old0 || !state->old1 || !state->empty0 ||
	    !state->empty1 || !state->raw_empty0 || !state->raw_empty1 ||
	    !state->new_record || !state->verify) {
		g343_free_state(state);
		return -ENOMEM;
	}
	g343_singleton = state;
	if (!proc_create_data("g346_env_restore", S_IRUSR | S_IWUSR, NULL,
				      &g343_fops, state)) {
		g343_singleton = NULL;
		g343_free_state(state);
		return -ENOMEM;
	}
	printk(KERN_WARNING
		       "G346 rescue: one-shot append-only U-Boot environment "
		       "restore control ready; "
	       "no write is automatic\n");
	return 0;
}
