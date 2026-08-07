/* Guarded no-erase restoration of the erased VB100a stock logo partition. */

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
#include "g347_logo_restore.h"

#define G347_MTD_SIZE                 0x400000000ULL
#define G347_ERASE_SIZE               0x00800000U
#define G347_WRITE_SIZE               0x00008000U
#define G347_OOB_SIZE                 0x00000700U
#define G347_PAGE_SHIFT               13U
#define G347_PLANE_NUM                2U

#define G347_LOGO_ADDR                0x02000000ULL
#define G347_LOGO_SIZE                0x00800000U
#define G347_AML_LOGO_ADDR            0x02800000ULL
#define G347_AML_LOGO_SIZE            0x00800000U
#define G347_LOGO_LOW_PAGE_FIRST      4096U
#define G347_LOGO_LOW_PAGE_STRIDE     4U
#define G347_LOGO_IMAGE_SIZE          4441280U
#define G347_LOGO_IMAGE_PAGES         136U
#define G347_LOGO_PADDED_SIZE         4456448U
#define G347_LOGO_TOTAL_PAGES         256U

#define G347_ERASED_LOGO_CRC          0x3de23e27U
#define G347_AML_LOGO_CRC             0x07912273U
#define G347_LOGO_IMAGE_CRC           0xf37d5623U
#define G347_LOGO_PADDED_CRC          0x9401101cU
#define G347_LOGO_FINAL_CRC           0xd68c5972U

#define G347_COMMAND \
	"restore-stock-logo-eec0bf84542c9a5c-g347\n"

static const u32 g347_logo_page_crc[G347_LOGO_IMAGE_PAGES] = {
	0x82d34250U, 0x49ca90daU, 0xdd856d8fU, 0xdd856d8fU,
	0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU,
	0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU,
	0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU,
	0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU,
	0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU,
	0xdd856d8fU, 0xdd856d8fU, 0xb5f5b0e1U, 0x1cc85404U,
	0xd052694eU, 0xeb52aca5U, 0x47690b17U, 0xaff4eb2dU,
	0x8b10a5d3U, 0x967babb4U, 0x193fab4cU, 0x58cc4afaU,
	0x72db017aU, 0x688e7753U, 0x8a2cac99U, 0x17f673c7U,
	0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU,
	0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU,
	0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU,
	0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU,
	0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU,
	0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU,
	0x91b3e1fdU, 0x66d7e5abU, 0x345d0b75U, 0xb2d757daU,
	0x72cea8fbU, 0xbca5ff3fU, 0xdd856d8fU, 0xdd856d8fU,
	0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU,
	0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU,
	0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU,
	0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU,
	0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU,
	0xdd856d8fU, 0x1462a06eU, 0x57cfbe5eU, 0xa7c7f5d3U,
	0x8c8e002aU, 0xe0d6d7b9U, 0x69d810d9U, 0x9737b772U,
	0xf955ce99U, 0x18041f3fU, 0x84dbeb2bU, 0xd827909fU,
	0x611db628U, 0x638027f6U, 0xc4f3f040U, 0xdd856d8fU,
	0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU,
	0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU,
	0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU,
	0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU,
	0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU,
	0xdd856d8fU, 0xdd856d8fU, 0xdd856d8fU, 0x99637446U,
	0x6e415283U, 0x9cca31faU, 0x8cd994d4U, 0x95858de7U,
};

struct g347_part_expect {
	const char *name;
	u64 offset;
	u64 size;
};

static const struct g347_part_expect g347_parts[] = {
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

struct g347_state {
	struct aml_nand_chip *aml_chip;
	struct mtd_info *mtd;
	struct mutex lock;
	u8 *page0;
	u8 *image;
	int geometry_ok;
	int layout_ok;
	int runtime_bbt_ok;
	int logo_empty;
	int aml_logo_ok;
	u32 logo_crc1;
	u32 logo_crc2;
	u32 aml_logo_crc1;
	u32 aml_logo_crc2;
	int preflight_result;
	int preflight_complete;
	int attempted;
	int gate_accepted;
	int write_result;
	int verify_result;
	int success;
};

struct g347_gate {
	atomic_t armed;
	atomic_t next;
	struct mtd_info *mtd;
	const u8 *image;
};

static struct g347_state *g347_singleton;
static struct g347_gate g347_gate = {
	.armed = ATOMIC_INIT(0),
	.next = ATOMIC_INIT(0),
};

static u32 g347_crc(const void *data, size_t size)
{
	return crc32((0 ^ 0xffffffffL), data, size) ^ 0xffffffffL;
}

static int g347_all_value(const u8 *data, size_t size, u8 value)
{
	size_t i;

	for (i = 0; i < size; i++)
		if (data[i] != value)
			return 0;
	return 1;
}

static int g347_validate_layout(struct g347_state *state)
{
	struct platform_nand_chip *chip;
	struct mtd_partition *part;
	unsigned int i;

	chip = &state->aml_chip->platform->platform_nand_data.chip;
	if (!chip->partitions || chip->nr_partitions != ARRAY_SIZE(g347_parts))
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(g347_parts); i++) {
		part = &chip->partitions[i];
		if (!part->name || strcmp(part->name, g347_parts[i].name) ||
		    part->offset != g347_parts[i].offset ||
		    part->size != g347_parts[i].size ||
		    part->mask_flags != MTD_WRITEABLE)
			return -EINVAL;
	}
	return 0;
}

static int g347_crc_range(struct g347_state *state, u64 addr, size_t size,
			  u32 *result)
{
	size_t done = 0;
	size_t retlen;
	size_t amount;
	u32 crc = 0xffffffffU;
	int error;

	while (done < size) {
		amount = min_t(size_t, state->mtd->writesize, size - done);
		memset(state->page0, 0xa5, state->mtd->writesize);
		retlen = 0;
		error = state->mtd->read(state->mtd, addr + done, amount,
					 &retlen, state->page0);
		if (error && error != -EUCLEAN)
			return error;
		if (retlen != amount)
			return -EIO;
		crc = crc32(crc, state->page0, amount);
		done += amount;
	}
	*result = crc ^ 0xffffffffU;
	return 0;
}

static int g347_preflight(struct g347_state *state)
{
	int error;

	state->preflight_complete = 0;
	state->geometry_ok = 0;
	state->layout_ok = 0;
	state->runtime_bbt_ok = 0;
	state->logo_empty = 0;
	state->aml_logo_ok = 0;
	state->logo_crc1 = 0;
	state->logo_crc2 = 0;
	state->aml_logo_crc1 = 0;
	state->aml_logo_crc2 = 0;
	state->geometry_ok = state->mtd->size == G347_MTD_SIZE &&
		state->mtd->erasesize == G347_ERASE_SIZE &&
		state->mtd->writesize == G347_WRITE_SIZE &&
		state->mtd->oobsize == G347_OOB_SIZE &&
		state->aml_chip->plane_num == G347_PLANE_NUM &&
		state->aml_chip->chip.page_shift == G347_PAGE_SHIFT &&
		(G347_LOGO_ADDR >> G347_PAGE_SHIFT) ==
			G347_LOGO_LOW_PAGE_FIRST;
	if (!state->geometry_ok)
		return -EINVAL;
	state->layout_ok = !g347_validate_layout(state);
	if (!state->layout_ok || !state->aml_chip->block_status ||
	    state->aml_chip->block_status[G347_LOGO_ADDR / G347_ERASE_SIZE] !=
		NAND_BLOCK_GOOD ||
	    state->aml_chip->block_status[G347_AML_LOGO_ADDR /
					 G347_ERASE_SIZE] != NAND_BLOCK_GOOD)
		return -EINVAL;
	state->runtime_bbt_ok = 1;
	error = g347_crc_range(state, G347_LOGO_ADDR, G347_LOGO_SIZE,
				 &state->logo_crc1);
	if (error)
		return error;
	error = g347_crc_range(state, G347_LOGO_ADDR, G347_LOGO_SIZE,
				 &state->logo_crc2);
	if (error || state->logo_crc1 != G347_ERASED_LOGO_CRC ||
	    state->logo_crc2 != G347_ERASED_LOGO_CRC)
		return error ? error : -EINVAL;
	state->logo_empty = 1;
	error = g347_crc_range(state, G347_AML_LOGO_ADDR, G347_AML_LOGO_SIZE,
				 &state->aml_logo_crc1);
	if (error)
		return error;
	error = g347_crc_range(state, G347_AML_LOGO_ADDR, G347_AML_LOGO_SIZE,
				 &state->aml_logo_crc2);
	if (error || state->aml_logo_crc1 != G347_AML_LOGO_CRC ||
	    state->aml_logo_crc2 != G347_AML_LOGO_CRC)
		return error ? error : -EINVAL;
	state->aml_logo_ok = 1;
	state->preflight_complete = 1;
	return 0;
}

static int g347_validate_image(struct g347_state *state)
{
	unsigned int i;

	if (g347_crc(state->image, G347_LOGO_IMAGE_SIZE) !=
	    G347_LOGO_IMAGE_CRC ||
	    g347_crc(state->image, G347_LOGO_PADDED_SIZE) !=
	    G347_LOGO_PADDED_CRC)
		return -EINVAL;
	for (i = 0; i < G347_LOGO_IMAGE_PAGES; i++)
		if (g347_crc(state->image + i * G347_WRITE_SIZE,
			     G347_WRITE_SIZE) != g347_logo_page_crc[i])
			return -EINVAL;
	return 0;
}

static void g347_gate_arm(struct g347_state *state)
{
	g347_gate.mtd = state->mtd;
	g347_gate.image = state->image;
	atomic_set(&g347_gate.next, 0);
	smp_wmb();
	atomic_set(&g347_gate.armed, 1);
}

static void g347_gate_disarm(void)
{
	atomic_set(&g347_gate.armed, 0);
}

int g347_logo_write_gate_take(struct mtd_info *mtd,
			      const unsigned char *data,
			      const unsigned char *oob, int page,
			      int cached, int raw)
{
	unsigned int index;
	unsigned int expected_page;
	unsigned int oob_offset;

	if (!atomic_read(&g347_gate.armed) || mtd != g347_gate.mtd || raw ||
	    cached ||
	    !oob || !data)
		return 0;
	index = atomic_read(&g347_gate.next);
	if (index >= G347_LOGO_IMAGE_PAGES)
		return 0;
	if (!mtd->ecclayout)
		return 0;
	oob_offset = mtd->ecclayout->oobfree[0].offset;
	if (oob_offset + mtd->oobavail > mtd->oobsize)
		return 0;
	expected_page = G347_LOGO_LOW_PAGE_FIRST +
		index * G347_LOGO_LOW_PAGE_STRIDE;
	if (page != expected_page ||
	    data != g347_gate.image + index * G347_WRITE_SIZE ||
	    g347_crc(data, G347_WRITE_SIZE) != g347_logo_page_crc[index] ||
	    !g347_all_value(oob + oob_offset,
			    mtd->oobavail, 0xff))
		return 0;
	if (atomic_cmpxchg(&g347_gate.next, index, index + 1) != index)
		return 0;
	if (g347_singleton)
		g347_singleton->gate_accepted++;
	return 1;
}

static int g347_write_logo(struct g347_state *state)
{
	size_t retlen;
	size_t total = 0;
	size_t amount;
	unsigned int i;
	int error;

	state->gate_accepted = 0;
	g347_gate_arm(state);
	for (i = 0; i < G347_LOGO_IMAGE_PAGES; i++) {
		retlen = 0;
		error = state->mtd->write(state->mtd,
					  G347_LOGO_ADDR + total,
					  G347_WRITE_SIZE, &retlen,
					  state->image + total);
		if (error || retlen != G347_WRITE_SIZE)
			break;
		total += retlen;
	}
	/* Leave all remaining pages untouched: they were verified FF. */
	g347_gate_disarm();
	if (error)
		return error;
	if (total != G347_LOGO_PADDED_SIZE ||
	    state->gate_accepted != G347_LOGO_IMAGE_PAGES ||
	    atomic_read(&g347_gate.next) != G347_LOGO_IMAGE_PAGES)
		return -EIO;
	if (state->mtd->sync)
		state->mtd->sync(state->mtd);
	return 0;
}

static int g347_verify_logo(struct g347_state *state)
{
	size_t retlen;
	unsigned int i;
	u64 addr;
	u32 crc;
	int error;

	for (i = 0; i < G347_LOGO_IMAGE_PAGES; i++) {
		addr = G347_LOGO_ADDR + (u64)i * G347_WRITE_SIZE;
		memset(state->page0, 0xa5, G347_WRITE_SIZE);
		retlen = 0;
		error = state->mtd->read(state->mtd, addr, G347_WRITE_SIZE,
					 &retlen, state->page0);
		if ((error && error != -EUCLEAN) || retlen != G347_WRITE_SIZE ||
		    memcmp(state->page0, state->image + i * G347_WRITE_SIZE,
			   G347_WRITE_SIZE))
			return error ? error : -EIO;
	}
	for (i = G347_LOGO_IMAGE_PAGES; i < G347_LOGO_TOTAL_PAGES; i++) {
		addr = G347_LOGO_ADDR + (u64)i * G347_WRITE_SIZE;
		memset(state->page0, 0xa5, G347_WRITE_SIZE);
		retlen = 0;
		error = state->mtd->read(state->mtd, addr, G347_WRITE_SIZE,
					 &retlen, state->page0);
		if ((error && error != -EUCLEAN) || retlen != G347_WRITE_SIZE ||
		    !g347_all_value(state->page0, G347_WRITE_SIZE, 0xff))
			return error ? error : -EIO;
	}
	error = g347_crc_range(state, G347_LOGO_ADDR, G347_LOGO_SIZE, &crc);
	if (error || crc != G347_LOGO_FINAL_CRC)
		return error ? error : -EIO;
	error = g347_crc_range(state, G347_AML_LOGO_ADDR, G347_AML_LOGO_SIZE,
				 &crc);
	if (error || crc != G347_AML_LOGO_CRC)
		return error ? error : -EIO;
	return 0;
}

static int g347_show(struct seq_file *seq, void *unused)
{
	struct g347_state *state = seq->private;
	int preflight;
	int env_current;

	mutex_lock(&state->lock);
	env_current = g343_env_restore_current();
	if (!state->attempted) {
		preflight = g347_preflight(state);
		state->preflight_result = preflight;
	} else {
		preflight = state->preflight_result;
	}
	seq_printf(seq,
		"g347=one_shot_no_erase_logo target=0x%llx size=0x%x "
		"image_size=%u pages=%u erase=disabled markbad=disabled "
		"aml_logo_write=disabled\n",
		G347_LOGO_ADDR, G347_LOGO_SIZE, G347_LOGO_IMAGE_SIZE,
		G347_LOGO_IMAGE_PAGES);
	seq_printf(seq,
		"geometry_ok=%d layout_ok=%d runtime_bbt_ok=%d logo_empty=%d "
		"aml_logo_ok=%d preflight_complete=%d preflight_result=%d\n",
		state->geometry_ok, state->layout_ok, state->runtime_bbt_ok,
		state->logo_empty, state->aml_logo_ok,
		state->preflight_complete, preflight);
	seq_puts(seq,
		 "factory_oob_policy=preserve_not_classify logical_ecc_double_read=1\n");
	seq_printf(seq,
		"logo_read_crc1=0x%08x logo_read_crc2=0x%08x "
		"expected_erased_crc=0x%08x proposed_image_crc=0x%08x "
		"proposed_final_crc=0x%08x\n",
		state->logo_crc1, state->logo_crc2, G347_ERASED_LOGO_CRC,
		G347_LOGO_IMAGE_CRC, G347_LOGO_FINAL_CRC);
	seq_printf(seq,
		"aml_logo_crc1=0x%08x aml_logo_crc2=0x%08x "
		"expected_aml_logo_crc=0x%08x\n",
		state->aml_logo_crc1, state->aml_logo_crc2,
		G347_AML_LOGO_CRC);
	seq_printf(seq,
		"attempted=%d gate_accepted=%d write_result=%d verify_result=%d "
		"success=%d env_restore_current=%d logo_write_unlocked=%d\n",
		state->attempted,
		state->gate_accepted, state->write_result, state->verify_result,
		state->success, env_current, env_current && !preflight);
	if (!state->attempted && !preflight && env_current) {
		seq_printf(seq, "payload_prefix=%s", G347_COMMAND);
		seq_printf(seq, "payload_total_bytes=%u\n",
			   (unsigned int)(sizeof(G347_COMMAND) - 1 +
					  G347_LOGO_IMAGE_SIZE));
		seq_puts(seq,
			 "asset_sha256=eec0bf84542c9a5cee341844c736a3d114a5e48e3a10a3e8f8d3d073814ea3e6\n");
	}
	mutex_unlock(&state->lock);
	return 0;
}

static int g347_open(struct inode *inode, struct file *file)
{
	return single_open(file, g347_show, PDE(inode)->data);
}

static ssize_t g347_write(struct file *file, const char __user *buffer,
			  size_t count, loff_t *position)
{
	struct g347_state *state = PDE(file->f_path.dentry->d_inode)->data;
	char command[sizeof(G347_COMMAND) - 1];
	size_t expected = sizeof(G347_COMMAND) - 1 + G347_LOGO_IMAGE_SIZE;
	int error;

	if (!state || current_uid() != 0 || count != expected)
		return -EINVAL;
	if (copy_from_user(command, buffer, sizeof(command)) ||
	    memcmp(command, G347_COMMAND, sizeof(command)))
		return -EPERM;
	mutex_lock(&state->lock);
	if (state->attempted) {
		error = -EPERM;
		goto out;
	}
	if (!g343_env_restore_current()) {
		error = -EPERM;
		goto out;
	}
	error = g347_preflight(state);
	state->preflight_result = error;
	if (error)
		goto out;
	memset(state->image, 0xff, G347_LOGO_PADDED_SIZE);
	if (copy_from_user(state->image, buffer + sizeof(command),
			   G347_LOGO_IMAGE_SIZE)) {
		error = -EFAULT;
		goto out;
	}
	error = g347_validate_image(state);
	if (error)
		goto out;
	state->attempted = 1;
	state->write_result = g347_write_logo(state);
	if (state->write_result) {
		error = state->write_result;
		goto out;
	}
	state->verify_result = g347_verify_logo(state);
	if (state->verify_result) {
		error = state->verify_result;
		goto out;
	}
	state->success = 1;
	error = count;
	printk(KERN_ALERT
	       "G347 rescue: stock logo restored and verified; aml_logo "
	       "remained unchanged\n");
out:
	mutex_unlock(&state->lock);
	return error;
}

static const struct file_operations g347_fops = {
	.owner = THIS_MODULE,
	.open = g347_open,
	.read = seq_read,
	.write = g347_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static void g347_free(struct g347_state *state)
{
	if (!state)
		return;
	vfree(state->page0);
	vfree(state->image);
	kfree(state);
}

int g347_logo_restore_register(struct aml_nand_chip *aml_chip)
{
	struct g347_state *state;

	BUILD_BUG_ON(ARRAY_SIZE(g347_logo_page_crc) !=
		     G347_LOGO_IMAGE_PAGES);
	if (g347_singleton)
		return -EBUSY;
	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;
	state->aml_chip = aml_chip;
	state->mtd = &aml_chip->mtd;
	mutex_init(&state->lock);
	state->write_result = -EAGAIN;
	state->verify_result = -EAGAIN;
	state->page0 = vmalloc(aml_chip->mtd.writesize);
	state->image = vmalloc(G347_LOGO_PADDED_SIZE);
	if (!state->page0 || !state->image) {
		g347_free(state);
		return -ENOMEM;
	}
	g347_singleton = state;
	if (!proc_create_data("g347_logo_restore", S_IRUSR | S_IWUSR, NULL,
			      &g347_fops, state)) {
		g347_singleton = NULL;
		g347_free(state);
		return -ENOMEM;
	}
	printk(KERN_WARNING
	       "G347 rescue: one-shot no-erase stock logo restore ready; "
	       "no write is automatic and aml_logo is blocked\n");
	return 0;
}
