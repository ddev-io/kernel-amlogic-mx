/* Read-only recovery of historic Amlogic BBT copies from service records. */

#include <linux/atomic.h>
#include <linux/crc32.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/nand.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include <mach/nand.h>

#include "g339_service_bbt.h"

/*
 * Do not call the normal key or secure initializers here.  Their error paths
 * can erase, rewrite, or mark service blocks bad.  G340 only issues read_oob.
 *
 * The key record is 64 KiB: a four-byte CRC followed by a 0xfffc-byte payload.
 * struct aml_nand_bbt_info occupies the payload tail.  On this G04, one record
 * is two 32-KiB virtual pages and the BBT starts at 0x6cd0 of page two.
 */
#define G340_EARLY_START_BLOCK       2
#define G340_EARLY_END_BLOCK         64
#define G340_TAIL_BLOCKS             16
#define G339_KEY_RECORD_SIZE         0x10000
#define G339_KEY_PAYLOAD_SIZE        (G339_KEY_RECORD_SIZE - sizeof(u32))
#define G339_MAX_UNIQUE_BBTS         64
#define G339_KEY_MAGIC               "keyx"
#define G339_SECURE_MAGIC            0x9fe7d05cU

struct g339_bbt_candidate {
	struct aml_nand_bbt_info bbt;
	u32 bbt_crc;
	u32 occurrences;
	u32 stable_occurrences;
	u32 exact_key_occurrences;
	u32 key_magic_occurrences;
	u32 env_magic_occurrences;
	u32 secure_magic_occurrences;
	u32 outer_crc_checked;
	u32 outer_crc_ok;
	u32 first_stored_crc;
	u32 first_calculated_crc;
	u32 first_block;
	u32 first_page;
	u32 first_offset;
	u32 last_block;
	u32 last_page;
	u32 last_offset;
	u32 min_timestamp;
	u32 max_timestamp;
	u32 entries;
	u32 factory;
	u32 runtime;
	u32 out_of_range;
	u32 part_count;
};

struct g339_scan_state {
	struct aml_nand_chip *aml_chip;
	struct g339_bbt_candidate *candidates;
	unsigned int total_blocks;
	unsigned int start_block;
	unsigned int early_end_block;
	unsigned int tail_start_block;
	unsigned int pages_per_block;
	unsigned int bbt_page_offset;
	unsigned int scan_pages;
	unsigned int total_reads;
	unsigned int read_errors;
	unsigned int euclean_reads;
	unsigned int key_magic_pages;
	unsigned int env_magic_pages;
	unsigned int secure_magic_pages;
	unsigned int head_hits;
	unsigned int cross_page_hits;
	unsigned int invalid_tail_hits;
	unsigned int valid_bbt_hits;
	unsigned int unstable_hits;
	unsigned int exact_key_hits;
	unsigned int outer_crc_checked;
	unsigned int outer_crc_ok;
	unsigned int unique_candidates;
	unsigned int dropped_candidates;
	int scan_error;
};

static atomic_t g339_scan_open = ATOMIC_INIT(0);

static int g339_read_page(struct g339_scan_state *scan,
			  unsigned int block, unsigned int page,
			  u8 *data, u8 *oob)
{
	struct mtd_info *mtd = &scan->aml_chip->mtd;
	struct mtd_oob_ops ops;
	u64 addr;
	int error;

	addr = (u64)block * mtd->erasesize + (u64)page * mtd->writesize;
	memset(data, 0xa5, mtd->writesize);
	memset(oob, 0xa5, sizeof(struct env_oobinfo_t));
	memset(&ops, 0, sizeof(ops));
	ops.mode = MTD_OOB_AUTO;
	ops.len = mtd->writesize;
	ops.ooblen = sizeof(struct env_oobinfo_t);
	ops.ooboffs = mtd->ecclayout->oobfree[0].offset;
	ops.datbuf = data;
	ops.oobbuf = oob;
	error = mtd->read_oob(mtd, addr, &ops);
	scan->total_reads++;
	if (error == -EUCLEAN)
		scan->euclean_reads++;
	if (((error != 0) && (error != -EUCLEAN)) ||
	    ops.retlen != mtd->writesize ||
	    ops.oobretlen != sizeof(struct env_oobinfo_t)) {
		scan->read_errors++;
		if (!error)
			error = -EIO;
		return error;
	}
	return error;
}

static void g339_classify_bbt(struct g339_scan_state *scan,
			      struct g339_bbt_candidate *candidate)
{
	struct aml_nand_part_info *part;
	unsigned int i;
	u16 value;

	for (i = 0; i < MAX_BAD_BLK_NUM; i++) {
		value = (u16)candidate->bbt.nand_bbt[i];
		if (!value)
			continue;
		candidate->entries++;
		if (value & 0x8000)
			candidate->factory++;
		else
			candidate->runtime++;
		if ((value & 0x7fff) >= scan->total_blocks)
			candidate->out_of_range++;
	}
	for (i = 0; i < MAX_MTD_PART_NUM; i++) {
		part = &candidate->bbt.aml_nand_part[i];
		if (memcmp(part->mtd_part_magic, MTD_PART_MAGIC, 4))
			break;
		candidate->part_count++;
	}
}

static void g339_record_candidate(struct g339_scan_state *scan,
				  const struct aml_nand_bbt_info *bbt,
				  unsigned int block, unsigned int page,
				  unsigned int offset, unsigned int stable,
				  unsigned int exact_key,
				  unsigned int key_magic,
				  unsigned int env_magic,
				  unsigned int secure_magic,
				  unsigned int timestamp,
				  unsigned int outer_checked,
				  unsigned int outer_ok, u32 stored_crc,
				  u32 calculated_crc)
{
	struct g339_bbt_candidate *candidate;
	unsigned int i;

	for (i = 0; i < scan->unique_candidates; i++) {
		candidate = &scan->candidates[i];
		if (!memcmp(&candidate->bbt, bbt, sizeof(*bbt)))
			goto update;
	}
	if (scan->unique_candidates >= G339_MAX_UNIQUE_BBTS) {
		scan->dropped_candidates++;
		return;
	}
	candidate = &scan->candidates[scan->unique_candidates++];
	memcpy(&candidate->bbt, bbt, sizeof(*bbt));
	candidate->bbt_crc = crc32((0 ^ 0xffffffffL),
				     (u8 *)&candidate->bbt,
				     sizeof(candidate->bbt)) ^ 0xffffffffL;
	candidate->first_block = block;
	candidate->first_page = page;
	candidate->first_offset = offset;
	candidate->first_stored_crc = stored_crc;
	candidate->first_calculated_crc = calculated_crc;
	g339_classify_bbt(scan, candidate);

update:
	candidate->occurrences++;
	candidate->stable_occurrences += !!stable;
	candidate->exact_key_occurrences += !!exact_key;
	candidate->outer_crc_checked += !!outer_checked;
	candidate->outer_crc_ok += !!outer_ok;
	candidate->last_block = block;
	candidate->last_page = page;
	candidate->last_offset = offset;
	if (key_magic) {
		if (!candidate->key_magic_occurrences) {
			candidate->min_timestamp = timestamp;
			candidate->max_timestamp = timestamp;
		} else {
			candidate->min_timestamp = min(candidate->min_timestamp,
						       timestamp);
			candidate->max_timestamp = max(candidate->max_timestamp,
						       timestamp);
		}
		candidate->key_magic_occurrences++;
	}
	candidate->env_magic_occurrences += !!env_magic;
	candidate->secure_magic_occurrences += !!secure_magic;
}

static int g339_scan_nand(struct g339_scan_state *scan)
{
	struct aml_nand_chip *aml_chip = scan->aml_chip;
	struct mtd_info *mtd = &aml_chip->mtd;
	struct env_oobinfo_t *oobinfo;
	struct aml_nand_bbt_info *bbt;
	u8 *page_data = NULL, *verify_data = NULL;
	u8 *previous_data = NULL, *record = NULL;
	u8 page_oob[sizeof(struct env_oobinfo_t)];
	u8 verify_oob[sizeof(struct env_oobinfo_t)];
	u8 previous_oob[sizeof(struct env_oobinfo_t)];
	u8 *cursor, *hit, *end;
	u32 secure_magic_value, stored_crc, calculated_crc;
	unsigned int block, page, offset, timestamp;
	unsigned int key_magic, env_magic, secure_magic, stable, exact_key;
	unsigned int outer_checked, outer_ok;
	unsigned int bbt_record_offset;
	int error, verify_error, previous_error;

	if (!mtd->writesize || !mtd->erasesize ||
	    mtd->erasesize % mtd->writesize)
		return -EINVAL;
	if (mtd->writesize * 2 != G339_KEY_RECORD_SIZE)
		return -EINVAL;
	scan->pages_per_block = mtd->erasesize / mtd->writesize;
	scan->total_blocks = div_u64(mtd->size, mtd->erasesize);
	if (!scan->total_blocks || scan->pages_per_block < 2)
		return -EINVAL;
	scan->start_block = min_t(unsigned int, G340_EARLY_START_BLOCK,
				  scan->total_blocks);
	scan->early_end_block = min_t(unsigned int, G340_EARLY_END_BLOCK,
				      scan->total_blocks);
	scan->tail_start_block = scan->total_blocks > G340_TAIL_BLOCKS ?
				 scan->total_blocks - G340_TAIL_BLOCKS : 0;
	bbt_record_offset = sizeof(u32) + G339_KEY_PAYLOAD_SIZE -
			    sizeof(struct aml_nand_bbt_info);
	scan->bbt_page_offset = bbt_record_offset - mtd->writesize;
	if (bbt_record_offset < mtd->writesize ||
	    scan->bbt_page_offset + sizeof(*bbt) != mtd->writesize)
		return -EINVAL;

	page_data = vmalloc(mtd->writesize);
	verify_data = vmalloc(mtd->writesize);
	previous_data = vmalloc(mtd->writesize);
	record = vmalloc(G339_KEY_RECORD_SIZE);
	if (!page_data || !verify_data || !previous_data || !record) {
		error = -ENOMEM;
		goto free_buffers;
	}

	printk(KERN_INFO
	       "G340 scan: read-only service blocks %u..%u and %u..%u\n",
	       scan->start_block,
	       scan->early_end_block ? scan->early_end_block - 1 : 0,
	       scan->tail_start_block, scan->total_blocks - 1);
	for (block = scan->start_block; block < scan->total_blocks; block++) {
		if (block >= scan->early_end_block &&
		    block < scan->tail_start_block)
			continue;
		for (page = 0; page < scan->pages_per_block; page++) {
			scan->scan_pages++;
			error = g339_read_page(scan, block, page, page_data,
					       page_oob);
			if ((error != 0) && (error != -EUCLEAN))
				continue;
			oobinfo = (struct env_oobinfo_t *)page_oob;
			key_magic = !memcmp(oobinfo->name, G339_KEY_MAGIC, 4);
			env_magic = !memcmp(oobinfo->name, ENV_NAND_MAGIC, 4);
			if (key_magic)
				scan->key_magic_pages++;
			if (env_magic)
				scan->env_magic_pages++;
			memcpy(&secure_magic_value, page_oob,
			       sizeof(secure_magic_value));
			secure_magic = secure_magic_value == G339_SECURE_MAGIC;
			if (secure_magic)
				scan->secure_magic_pages++;

			cursor = page_data;
			end = page_data + mtd->writesize - 4;
			while (cursor <= end) {
				hit = memchr(cursor, BBT_HEAD_MAGIC[0],
					      end - cursor + 1);
				if (!hit)
					break;
				cursor = hit + 1;
				if (memcmp(hit, BBT_HEAD_MAGIC, 4))
					continue;
				scan->head_hits++;
				offset = hit - page_data;
				if (offset + sizeof(*bbt) > mtd->writesize) {
					scan->cross_page_hits++;
					continue;
				}
				bbt = (struct aml_nand_bbt_info *)hit;
				if (memcmp(bbt->bbt_tail_magic,
					   BBT_TAIL_MAGIC, 4)) {
					scan->invalid_tail_hits++;
					continue;
				}

				scan->valid_bbt_hits++;
				verify_error = g339_read_page(scan, block, page,
							 verify_data, verify_oob);
				stable = ((verify_error == 0) ||
					  (verify_error == -EUCLEAN)) &&
					 !memcmp(page_data + offset,
						 verify_data + offset, sizeof(*bbt));
				if (!stable)
					scan->unstable_hits++;

				exact_key = key_magic && page > 0 &&
					offset == scan->bbt_page_offset;
				outer_checked = 0;
				outer_ok = 0;
				stored_crc = 0;
				calculated_crc = 0;
				if (exact_key) {
					scan->exact_key_hits++;
					previous_error = g339_read_page(scan, block,
						page - 1, previous_data, previous_oob);
					if ((previous_error == 0) ||
					    (previous_error == -EUCLEAN)) {
						memcpy(record, previous_data,
						       mtd->writesize);
						memcpy(record + mtd->writesize,
						       page_data, mtd->writesize);
						memcpy(&stored_crc, record,
						       sizeof(stored_crc));
						calculated_crc = crc32(
							(0 ^ 0xffffffffL),
							record + sizeof(u32),
							G339_KEY_PAYLOAD_SIZE) ^
							0xffffffffL;
						outer_checked = 1;
						outer_ok = stored_crc == calculated_crc;
						scan->outer_crc_checked++;
						scan->outer_crc_ok += outer_ok;
					}
				}
				timestamp = key_magic ? oobinfo->timestamp : 0;
				g339_record_candidate(scan, bbt, block, page,
						      offset, stable, exact_key,
						      key_magic, env_magic,
						      secure_magic, timestamp,
						      outer_checked, outer_ok,
						      stored_crc,
						      calculated_crc);
			}
		}
	}
	error = 0;
	printk(KERN_INFO
	       "G340 scan: complete pages=%u reads=%u errors=%u valid_hits=%u unique=%u\n",
	       scan->scan_pages, scan->total_reads, scan->read_errors,
	       scan->valid_bbt_hits, scan->unique_candidates);

free_buffers:
	vfree(record);
	vfree(previous_data);
	vfree(verify_data);
	vfree(page_data);
	return error;
}

static int g339_service_bbt_show(struct seq_file *seq, void *unused)
{
	struct g339_scan_state *scan = seq->private;
	struct g339_bbt_candidate *candidate;
	unsigned int i, j, emitted;
	u16 value;

	(void)unused;
	seq_printf(seq,
		"g340=read_only scan=early_and_tail_all_pages exact_key_format=64k_two_page "
		"no_block_status=1 no_block_isbad=1 no_key_init=1 no_secure_init=1 "
		"no_write=1\n");
	seq_printf(seq,
		"scan_error=%d early_start=%u early_end=%u tail_start=%u "
		"tail_end=%u total_blocks=%u "
		"pages_per_block=%u bbt_second_page_offset=0x%x\n",
		scan->scan_error, scan->start_block,
		scan->early_end_block ? scan->early_end_block - 1 : 0,
		scan->tail_start_block,
		scan->total_blocks ? scan->total_blocks - 1 : 0,
		scan->total_blocks, scan->pages_per_block,
		scan->bbt_page_offset);
	seq_printf(seq,
		"summary scan_pages=%u total_reads=%u read_errors=%u euclean_reads=%u "
		"key_magic_pages=%u env_magic_pages=%u secure_magic_pages=%u head_hits=%u "
		"cross_page_hits=%u invalid_tail_hits=%u valid_bbt_hits=%u "
		"unstable_hits=%u exact_key_hits=%u outer_crc_checked=%u "
		"outer_crc_ok=%u unique_candidates=%u dropped_candidates=%u\n",
		scan->scan_pages, scan->total_reads, scan->read_errors,
		scan->euclean_reads, scan->key_magic_pages,
		scan->env_magic_pages, scan->secure_magic_pages, scan->head_hits,
		scan->cross_page_hits, scan->invalid_tail_hits,
		scan->valid_bbt_hits, scan->unstable_hits,
		scan->exact_key_hits, scan->outer_crc_checked,
		scan->outer_crc_ok, scan->unique_candidates,
		scan->dropped_candidates);

	for (i = 0; i < scan->unique_candidates; i++) {
		candidate = &scan->candidates[i];
		seq_printf(seq,
			"candidate=%u bbt_crc=0x%08x occurrences=%u stable=%u "
			"exact_key=%u key_magic=%u env_magic=%u secure_magic=%u "
			"outer_checked=%u outer_ok=%u "
			"first=%u:%u:0x%x last=%u:%u:0x%x timestamps=%u..%u "
			"entries=%u factory=%u runtime=%u out_of_range=%u parts=%u "
			"first_outer_stored=0x%08x first_outer_calc=0x%08x "
			"plausible=%u\n",
			i, candidate->bbt_crc, candidate->occurrences,
			candidate->stable_occurrences,
			candidate->exact_key_occurrences,
			candidate->key_magic_occurrences,
			candidate->env_magic_occurrences,
			candidate->secure_magic_occurrences,
			candidate->outer_crc_checked, candidate->outer_crc_ok,
			candidate->first_block, candidate->first_page,
			candidate->first_offset, candidate->last_block,
			candidate->last_page, candidate->last_offset,
			candidate->min_timestamp, candidate->max_timestamp,
			candidate->entries, candidate->factory,
			candidate->runtime, candidate->out_of_range,
			candidate->part_count, candidate->first_stored_crc,
			candidate->first_calculated_crc,
			candidate->entries <= 256 &&
			!candidate->out_of_range);
		if (candidate->entries > 256 || candidate->out_of_range)
			continue;
		seq_printf(seq, "candidate_entries=%u:", i);
		emitted = 0;
		for (j = 0; j < MAX_BAD_BLK_NUM; j++) {
			value = (u16)candidate->bbt.nand_bbt[j];
			if (!value)
				continue;
			seq_printf(seq, "%s%04x", emitted ? "," : "", value);
			emitted++;
		}
		seq_putc(seq, '\n');
		seq_printf(seq, "candidate_raw=%u:", i);
		for (j = 0; j < sizeof(candidate->bbt); j++)
			seq_printf(seq, "%02x", ((u8 *)&candidate->bbt)[j]);
		seq_putc(seq, '\n');
	}
	return 0;
}

static int g339_service_bbt_open(struct inode *inode, struct file *file)
{
	struct g339_scan_state *scan;
	int error;

	if (atomic_cmpxchg(&g339_scan_open, 0, 1))
		return -EBUSY;
	scan = kzalloc(sizeof(*scan), GFP_KERNEL);
	if (!scan) {
		error = -ENOMEM;
		goto clear_open;
	}
	scan->candidates = vzalloc(G339_MAX_UNIQUE_BBTS *
				    sizeof(*scan->candidates));
	if (!scan->candidates) {
		error = -ENOMEM;
		goto free_scan;
	}
	scan->aml_chip = PDE(inode)->data;
	scan->scan_error = g339_scan_nand(scan);
	error = single_open(file, g339_service_bbt_show, scan);
	if (error)
		goto free_candidates;
	return 0;

free_candidates:
	vfree(scan->candidates);
free_scan:
	kfree(scan);
clear_open:
	atomic_set(&g339_scan_open, 0);
	return error;
}

static int g339_service_bbt_release(struct inode *inode, struct file *file)
{
	struct seq_file *seq = file->private_data;
	struct g339_scan_state *scan = seq->private;

	vfree(scan->candidates);
	kfree(scan);
	atomic_set(&g339_scan_open, 0);
	return single_release(inode, file);
}

static const struct file_operations g339_service_bbt_fops = {
	.owner = THIS_MODULE,
	.open = g339_service_bbt_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = g339_service_bbt_release,
};

int g339_service_bbt_register(struct aml_nand_chip *aml_chip)
{
	if (!proc_create_data("g340_service_bbt", S_IRUGO, NULL,
			      &g339_service_bbt_fops, aml_chip)) {
		printk(KERN_ERR
		       "G340 rescue: failed to create service BBT diagnostic\n");
		return -ENOMEM;
	}
	printk(KERN_INFO
	       "G340 rescue: read-only legacy/tail BBT diagnostic ready\n");
	return 0;
}
