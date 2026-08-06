/* One-shot append-only BBT restoration for the G04 NAND. */

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

#define G343_OLD_BBT_CRC             0x0171b3b7U
#define G343_TOTAL_BLOCKS            2048U
#define G343_ERASE_SIZE              0x00800000U
#define G343_WRITE_SIZE              0x00008000U
#define G343_OOB_SIZE                0x00000700U
#define G343_MTD_SIZE                0x400000000ULL
#define G343_ENV_BLOCK               2U
#define G343_OLD_PAGE0               0U
#define G343_OLD_PAGE1               1U
#define G343_TARGET_PAGE             2U
#define G343_TARGET_ADDR             0x01010000ULL
#define G343_OLD_TIMESTAMP           1U
#define G343_NEW_TIMESTAMP           2U
#define G343_EXPECTED_PARTS          9U
#define G343_NEW_BBT_CRC             0xacbcfcadU
#define G343_COMMAND                 "append-empty-bbt-crc-0171b3b7-page-2-g344"

struct g343_part_expect {
	const char *name;
	u64 offset;
	u64 size;
};

/* These are the board-table values stored in envx before add_partition()
 * rewrites offsets sequentially around bad blocks. */
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
static int g343_read_raw_page(struct g343_state *state, u8 *buffer, u8 fill)
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
	error = state->mtd->read_oob(state->mtd, G343_TARGET_ADDR, &ops);
	if (error)
		return error;
	if (ops.retlen != state->mtd->writesize || ops.oobretlen)
		return -EIO;
	return 0;
}

static int g343_validate_old_bbt(struct g343_state *state, env_t *env)
{
	struct aml_nand_bbt_info *bbt;
	unsigned int entries = 0;
	unsigned int factory = 0;
	unsigned int runtime = 0;
	unsigned int out_of_range = 0;
	unsigned int parts = 0;
	unsigned int i;
	u16 value;
	u32 outer_crc;

	outer_crc = g343_crc(env->data, ENV_SIZE);
	if (outer_crc != env->crc || env->crc != 0x4d023923U)
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
	if (state->old_bbt_crc != G343_OLD_BBT_CRC ||
	    entries != MAX_BAD_BLK_NUM || factory != MAX_BAD_BLK_NUM ||
	    runtime || out_of_range || parts)
		return -EINVAL;
	return 0;
}

static int g343_validate_old_oob(const struct env_oobinfo_t *oob)
{
	if (memcmp(oob->name, ENV_NAND_MAGIC, 4) || oob->ec != -1 ||
	    oob->timestamp != G343_OLD_TIMESTAMP || !oob->status_page)
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
			       "G344 rescue: partition preflight mismatch index=%u "
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
	struct aml_nand_part_info *part;
	unsigned int i;

	memcpy(state->new_record, state->old1, state->mtd->writesize);
	bbt = (struct aml_nand_bbt_info *)
		(env->data + ENV_SIZE - sizeof(*bbt));
	memset(bbt, 0, sizeof(*bbt));
	memcpy(bbt->bbt_head_magic, BBT_HEAD_MAGIC, 4);
	memcpy(bbt->bbt_tail_magic, BBT_TAIL_MAGIC, 4);
	for (i = 0; i < G343_EXPECTED_PARTS; i++) {
		part = &bbt->aml_nand_part[i];
		memcpy(part->mtd_part_magic, MTD_PART_MAGIC, 4);
		strncpy(part->mtd_part_name, g343_record_parts[i].name,
			MAX_MTD_PART_NAME_LEN - 1);
		part->size = g343_record_parts[i].size;
		part->offset = g343_record_parts[i].offset;
		part->mask_flags = 0;
	}
	env->crc = g343_crc(env->data, ENV_SIZE);
	state->new_outer_crc = env->crc;
	state->new_bbt_crc = g343_crc(bbt, sizeof(*bbt));
	if (state->new_bbt_crc != G343_NEW_BBT_CRC)
		printk(KERN_ERR
		       "G344 rescue: unexpected generated BBT CRC 0x%08x\n",
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
}

static int g343_preflight(struct g343_state *state)
{
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
		(state->mtd->size / state->mtd->erasesize) == G343_TOTAL_BLOCKS;
	if (!state->geometry_ok)
		return -EINVAL;
	state->source_parts_ok = !g343_validate_source_parts(state);
	if (!state->source_parts_ok)
		return -EINVAL;
	error = g343_read_page(state, G343_OLD_PAGE0,
				state->old0, &state->old0_oob);
	if (error)
		return error;
	error = g343_read_page(state, G343_OLD_PAGE1,
				state->old1, &state->old1_oob);
	if (error)
		return error;
	if (memcmp(state->old0, state->old1, state->mtd->writesize) ||
	    memcmp(&state->old0_oob, &state->old1_oob,
		   sizeof(state->old0_oob)))
		return -EINVAL;
	if (g343_validate_old_oob(&state->old0_oob) ||
	    g343_validate_old_bbt(state, (env_t *)state->old0))
		return -EINVAL;
	if (memcmp(state->old0, state->old1, 0x6ccc))
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
	error = g343_read_raw_page(state, state->raw_empty0, 0xa5);
	if (error)
		return error;
	error = g343_read_raw_page(state, state->raw_empty1, 0x5a);
	if (error)
		return error;
	if (memcmp(state->raw_empty0, state->raw_empty1,
		   state->mtd->writesize + state->mtd->oobsize) ||
	    !g343_all_ff(state->raw_empty0,
			 state->mtd->writesize + state->mtd->oobsize))
		return -EINVAL;
	g343_build_new_record(state);
	if (state->new_bbt_crc != G343_NEW_BBT_CRC)
		return -EINVAL;
	if (memcmp(state->new_record, state->old1, 0x6ccc))
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
	    !mtd->ecclayout || !oob)
		return 0;
	oob_offset = mtd->ecclayout->oobfree[0].offset;
	if (oob_offset + sizeof(g343_gate.oob) > mtd->oobsize ||
	    memcmp(oob + oob_offset, &g343_gate.oob,
		   sizeof(g343_gate.oob)))
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
	error = g343_read_page(state, G343_OLD_PAGE0,
				state->empty0, &second_oob);
	if (error || memcmp(state->empty0, state->old0, state->mtd->writesize) ||
	    memcmp(&second_oob, &state->old0_oob, sizeof(second_oob)))
		return -EIO;
	error = g343_read_page(state, G343_OLD_PAGE1,
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
		"g344=one_shot_append_only target=0x%llx block=%u page=%u "
		"erase=disabled markbad=disabled general_write=disabled\n",
		G343_TARGET_ADDR, G343_ENV_BLOCK, G343_TARGET_PAGE);
	seq_printf(seq,
		"geometry_ok=%d source_parts_ok=%d preflight_complete=%d "
		"preflight_result=%d\n",
		state->geometry_ok, state->source_parts_ok,
		state->preflight_complete, preflight);
	seq_printf(seq,
		"old_outer_crc=0x%08x old_bbt_crc=0x%08x "
		"new_outer_crc=0x%08x new_bbt_crc=0x%08x\n",
		state->old_outer_crc, state->old_bbt_crc,
		state->new_outer_crc, state->new_bbt_crc);
	seq_printf(seq,
		"old_slots=2:0,2:1 old_timestamp=%u new_slot=2:2 "
		"new_timestamp=%u bbt_entries=0 partition_entries=%u\n",
		G343_OLD_TIMESTAMP, G343_NEW_TIMESTAMP, G343_EXPECTED_PARTS);
	seq_printf(seq,
		"attempted=%d gate_accepted=%d write_result=%d verify_result=%d "
		"success=%d\n",
		state->attempted, state->gate_accepted, state->write_result,
		state->verify_result, state->success);
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
	       "G344 rescue: append-only BBT restore verified at 0x%llx\n",
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
	if (!proc_create_data("g344_bbt_restore", S_IRUSR | S_IWUSR, NULL,
			      &g343_fops, state)) {
		g343_singleton = NULL;
		g343_free_state(state);
		return -ENOMEM;
	}
	printk(KERN_WARNING
	       "G344 rescue: one-shot append-only BBT restore control ready; "
	       "no write is automatic\n");
	return 0;
}
