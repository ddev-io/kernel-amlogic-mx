/* Full read-only ECC health scan for the G04 NAND. */

#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/nand.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include <mach/nand.h>

#include "g341_ecc_health.h"

#define G341_MAX_EVENTS              16384
#define G341_NO_PAGE                 0xffff
#define G341_PROGRESS_BLOCKS         16
#define G341_ECC_HISTOGRAM_BINS      64

#define G341_EVENT_FIRST_HARD        0x01
#define G341_EVENT_REPEAT_HARD       0x02
#define G341_EVENT_FIRST_WEAK        0x04
#define G341_EVENT_REPEAT_WEAK       0x08
#define G341_EVENT_DATA_MISMATCH     0x10
#define G341_EVENT_OOB_MISMATCH      0x20

struct g341_read_sample {
	s32 error;
	u32 retlen;
	u32 oobretlen;
	u32 ecc_failed;
	u32 ecc_corrected;
	u8 ecc_max;
	u8 hard;
	u8 corrected;
	u8 weak;
};

struct g341_page_event {
	u16 block;
	u16 page;
	s16 first_error;
	s16 repeat_error;
	u16 flags;
	u16 first_failed;
	u16 repeat_failed;
	u16 first_corrected;
	u16 repeat_corrected;
	u8 first_ecc_max;
	u8 repeat_ecc_max;
};

struct g341_block_health {
	u32 pages;
	u32 clean_pages;
	u32 corrected_pages;
	u32 weak_pages;
	u32 repeated_pages;
	u32 first_hard_pages;
	u32 repeat_hard_pages;
	u32 stable_hard_pages;
	u32 transient_hard_pages;
	u32 repeat_only_hard_pages;
	u32 data_mismatch_pages;
	u32 oob_mismatch_pages;
	u32 first_corrected_events;
	u32 repeat_corrected_events;
	u16 first_hard_page;
	u16 first_weak_page;
	u16 first_mismatch_page;
	u8 first_ecc_max;
	u8 repeat_ecc_max;
};

struct g341_scan_state {
	struct aml_nand_chip *aml_chip;
	struct g341_block_health *blocks;
	struct g341_page_event *events;
	u32 first_histogram[G341_ECC_HISTOGRAM_BINS];
	u32 repeat_histogram[G341_ECC_HISTOGRAM_BINS];
	unsigned int total_blocks;
	unsigned int pages_per_block;
	unsigned int weak_threshold;
	unsigned int pages_read;
	unsigned int repeat_reads;
	unsigned int hard_first_pages;
	unsigned int hard_repeat_pages;
	unsigned int stable_hard_pages;
	unsigned int transient_hard_pages;
	unsigned int repeat_only_hard_pages;
	unsigned int weak_pages;
	unsigned int corrected_pages;
	unsigned int data_mismatch_pages;
	unsigned int oob_mismatch_pages;
	unsigned int hard_blocks;
	unsigned int unstable_blocks;
	unsigned int weak_blocks;
	unsigned int event_count;
	unsigned int dropped_events;
	int scan_error;
};

struct g341_progress_state {
	atomic_t running;
	atomic_t complete;
	u32 current_block;
	u32 total_blocks;
	u32 pages_read;
	u32 repeat_reads;
	u32 hard_first_pages;
	u32 stable_hard_pages;
	u32 weak_pages;
	u32 event_count;
	s32 scan_error;
};

static atomic_t g341_scan_open = ATOMIC_INIT(0);
static struct g341_progress_state g341_progress = {
	.running = ATOMIC_INIT(0),
	.complete = ATOMIC_INIT(0),
};

static int g341_sample_hard(const struct mtd_info *mtd,
			    const struct g341_read_sample *sample)
{
	return ((sample->error != 0) && (sample->error != -EUCLEAN)) ||
		sample->ecc_failed || sample->retlen != mtd->writesize ||
		sample->oobretlen != sizeof(struct env_oobinfo_t);
}

static int g341_read_page(struct g341_scan_state *scan,
			  unsigned int block, unsigned int page,
			  u8 *data, u8 *oob,
			  struct g341_read_sample *sample)
{
	struct aml_nand_chip *aml_chip = scan->aml_chip;
	struct mtd_info *mtd = &aml_chip->mtd;
	struct mtd_ecc_stats before;
	struct mtd_oob_ops ops;
	u64 addr;

	addr = (u64)block * mtd->erasesize + (u64)page * mtd->writesize;
	memset(sample, 0, sizeof(*sample));
	memset(data, 0xa5, mtd->writesize);
	memset(oob, 0xa5, sizeof(struct env_oobinfo_t));
	memset(&ops, 0, sizeof(ops));
	ops.mode = MTD_OOB_AUTO;
	ops.len = mtd->writesize;
	ops.ooblen = sizeof(struct env_oobinfo_t);
	ops.ooboffs = mtd->ecclayout->oobfree[0].offset;
	ops.datbuf = data;
	ops.oobbuf = oob;
	before = mtd->ecc_stats;
	aml_chip->diag_ecc_page_max = 0;
	sample->error = mtd->read_oob(mtd, addr, &ops);
	sample->retlen = ops.retlen;
	sample->oobretlen = ops.oobretlen;
	sample->ecc_failed = mtd->ecc_stats.failed - before.failed;
	sample->ecc_corrected = mtd->ecc_stats.corrected - before.corrected;
	sample->ecc_max = aml_chip->diag_ecc_page_max;
	sample->hard = g341_sample_hard(mtd, sample);
	sample->corrected = sample->error == -EUCLEAN ||
		sample->ecc_corrected || sample->ecc_max;
	sample->weak = !sample->hard &&
		(sample->error == -EUCLEAN ||
		 sample->ecc_max >= scan->weak_threshold);
	return sample->error;
}

static void g341_record_event(struct g341_scan_state *scan,
			      unsigned int block, unsigned int page,
			      const struct g341_read_sample *first,
			      const struct g341_read_sample *repeat,
			      unsigned int data_mismatch,
			      unsigned int oob_mismatch)
{
	struct g341_page_event *event;

	if (scan->event_count >= G341_MAX_EVENTS) {
		scan->dropped_events++;
		return;
	}
	event = &scan->events[scan->event_count++];
	event->block = block;
	event->page = page;
	event->first_error = first->error;
	event->repeat_error = repeat->error;
	event->first_failed = min_t(u32, first->ecc_failed, 0xffff);
	event->repeat_failed = min_t(u32, repeat->ecc_failed, 0xffff);
	event->first_corrected = min_t(u32, first->ecc_corrected, 0xffff);
	event->repeat_corrected = min_t(u32, repeat->ecc_corrected, 0xffff);
	event->first_ecc_max = first->ecc_max;
	event->repeat_ecc_max = repeat->ecc_max;
	if (first->hard)
		event->flags |= G341_EVENT_FIRST_HARD;
	if (repeat->hard)
		event->flags |= G341_EVENT_REPEAT_HARD;
	if (first->weak)
		event->flags |= G341_EVENT_FIRST_WEAK;
	if (repeat->weak)
		event->flags |= G341_EVENT_REPEAT_WEAK;
	if (data_mismatch)
		event->flags |= G341_EVENT_DATA_MISMATCH;
	if (oob_mismatch)
		event->flags |= G341_EVENT_OOB_MISMATCH;
}

static void g341_init_block(struct g341_block_health *health)
{
	memset(health, 0, sizeof(*health));
	health->first_hard_page = G341_NO_PAGE;
	health->first_weak_page = G341_NO_PAGE;
	health->first_mismatch_page = G341_NO_PAGE;
}

static int g341_scan_nand(struct g341_scan_state *scan)
{
	struct aml_nand_chip *aml_chip = scan->aml_chip;
	struct mtd_info *mtd = &aml_chip->mtd;
	struct g341_block_health *health;
	struct g341_read_sample first, repeat;
	u8 *first_data = NULL, *repeat_data = NULL;
	u8 first_oob[sizeof(struct env_oobinfo_t)];
	u8 repeat_oob[sizeof(struct env_oobinfo_t)];
	unsigned int block, page, repeat_page;
	unsigned int data_mismatch, oob_mismatch;
	int error = 0;

	if (!mtd->writesize || !mtd->erasesize ||
	    mtd->erasesize % mtd->writesize)
		return -EINVAL;
	scan->pages_per_block = mtd->erasesize / mtd->writesize;
	scan->total_blocks = div_u64(mtd->size, mtd->erasesize);
	if (!scan->total_blocks || !scan->pages_per_block)
		return -EINVAL;
	scan->weak_threshold = aml_chip->ecc_cnt_limit > 3 ?
			       aml_chip->ecc_cnt_limit - 3 :
			       aml_chip->ecc_cnt_limit;
	if (!scan->weak_threshold)
		scan->weak_threshold = 1;

	scan->blocks = vzalloc((size_t)scan->total_blocks *
				 sizeof(*scan->blocks));
	scan->events = vzalloc((size_t)G341_MAX_EVENTS *
				 sizeof(*scan->events));
	first_data = vmalloc(mtd->writesize);
	repeat_data = vmalloc(mtd->writesize);
	if (!scan->blocks || !scan->events || !first_data || !repeat_data) {
		error = -ENOMEM;
		goto out;
	}
	for (block = 0; block < scan->total_blocks; block++)
		g341_init_block(&scan->blocks[block]);

	memset(&g341_progress, 0, sizeof(g341_progress));
	atomic_set(&g341_progress.running, 1);
	atomic_set(&g341_progress.complete, 0);
	g341_progress.total_blocks = scan->total_blocks;
	printk(KERN_INFO
	       "G341 scan: starting %u blocks x %u pages, weak ECC threshold %u/%u\n",
	       scan->total_blocks, scan->pages_per_block,
	       scan->weak_threshold, aml_chip->ecc_max);

	for (block = 0; block < scan->total_blocks; block++) {
		health = &scan->blocks[block];
		g341_progress.current_block = block;
		if (!(block % G341_PROGRESS_BLOCKS))
			printk(KERN_INFO
			       "G341 scan: block %u/%u pages=%u hard=%u stable=%u weak=%u events=%u\n",
			       block, scan->total_blocks, scan->pages_read,
			       scan->hard_first_pages, scan->stable_hard_pages,
			       scan->weak_pages, scan->event_count);
		for (page = 0; page < scan->pages_per_block; page++) {
			g341_read_page(scan, block, page, first_data,
					first_oob, &first);
			scan->pages_read++;
			health->pages++;
			scan->first_histogram[min_t(unsigned int,
				first.ecc_max, G341_ECC_HISTOGRAM_BINS - 1)]++;
			health->first_ecc_max = max(health->first_ecc_max,
						    first.ecc_max);
			health->first_corrected_events += first.ecc_corrected;
			if (first.corrected) {
				health->corrected_pages++;
				scan->corrected_pages++;
			} else if (!first.hard) {
				health->clean_pages++;
			}
			if (first.hard) {
				health->first_hard_pages++;
				scan->hard_first_pages++;
				if (health->first_hard_page == G341_NO_PAGE)
					health->first_hard_page = page;
			}
			if (first.weak) {
				health->weak_pages++;
				scan->weak_pages++;
				if (health->first_weak_page == G341_NO_PAGE)
					health->first_weak_page = page;
			}

			repeat_page = first.hard || first.weak;
			if (!repeat_page)
				continue;
			g341_read_page(scan, block, page, repeat_data,
					repeat_oob, &repeat);
			scan->repeat_reads++;
			health->repeated_pages++;
			scan->repeat_histogram[min_t(unsigned int,
				repeat.ecc_max, G341_ECC_HISTOGRAM_BINS - 1)]++;
			health->repeat_ecc_max = max(health->repeat_ecc_max,
						     repeat.ecc_max);
			health->repeat_corrected_events += repeat.ecc_corrected;
			if (repeat.hard) {
				health->repeat_hard_pages++;
				scan->hard_repeat_pages++;
			}
			if (first.hard && repeat.hard) {
				health->stable_hard_pages++;
				scan->stable_hard_pages++;
			} else if (first.hard) {
				health->transient_hard_pages++;
				scan->transient_hard_pages++;
			} else if (repeat.hard) {
				health->repeat_only_hard_pages++;
				scan->repeat_only_hard_pages++;
			}
			data_mismatch = memcmp(first_data, repeat_data,
					       mtd->writesize) != 0;
			oob_mismatch = memcmp(first_oob, repeat_oob,
					      sizeof(first_oob)) != 0;
			if (data_mismatch) {
				health->data_mismatch_pages++;
				scan->data_mismatch_pages++;
			}
			if (oob_mismatch) {
				health->oob_mismatch_pages++;
				scan->oob_mismatch_pages++;
			}
			if ((data_mismatch || oob_mismatch) &&
			    health->first_mismatch_page == G341_NO_PAGE)
				health->first_mismatch_page = page;
			g341_record_event(scan, block, page, &first, &repeat,
					  data_mismatch, oob_mismatch);
		}
		if (health->stable_hard_pages)
			scan->hard_blocks++;
		if (health->transient_hard_pages ||
		    health->repeat_only_hard_pages ||
		    health->data_mismatch_pages ||
		    health->oob_mismatch_pages)
			scan->unstable_blocks++;
		if (health->weak_pages)
			scan->weak_blocks++;
		g341_progress.pages_read = scan->pages_read;
		g341_progress.repeat_reads = scan->repeat_reads;
		g341_progress.hard_first_pages = scan->hard_first_pages;
		g341_progress.stable_hard_pages = scan->stable_hard_pages;
		g341_progress.weak_pages = scan->weak_pages;
		g341_progress.event_count = scan->event_count;
		cond_resched();
	}
	printk(KERN_INFO
	       "G341 scan: complete pages=%u repeats=%u stable_hard=%u hard_blocks=%u unstable_blocks=%u weak_blocks=%u\n",
	       scan->pages_read, scan->repeat_reads, scan->stable_hard_pages,
	       scan->hard_blocks, scan->unstable_blocks, scan->weak_blocks);

out:
	vfree(repeat_data);
	vfree(first_data);
	if (error) {
		vfree(scan->events);
		scan->events = NULL;
		vfree(scan->blocks);
		scan->blocks = NULL;
	}
	g341_progress.scan_error = error;
	atomic_set(&g341_progress.running, 0);
	atomic_set(&g341_progress.complete, !error);
	return error;
}

static int g341_ecc_progress_show(struct seq_file *seq, void *unused)
{
	(void)unused;
	seq_printf(seq,
		"g341_progress running=%d complete=%d current_block=%u "
		"total_blocks=%u pages_read=%u repeat_reads=%u "
		"hard_first_pages=%u stable_hard_pages=%u weak_pages=%u "
		"events=%u scan_error=%d no_write=1\n",
		atomic_read(&g341_progress.running),
		atomic_read(&g341_progress.complete),
		g341_progress.current_block, g341_progress.total_blocks,
		g341_progress.pages_read, g341_progress.repeat_reads,
		g341_progress.hard_first_pages,
		g341_progress.stable_hard_pages, g341_progress.weak_pages,
		g341_progress.event_count, g341_progress.scan_error);
	return 0;
}

static int g341_ecc_progress_open(struct inode *inode, struct file *file)
{
	return single_open(file, g341_ecc_progress_show, NULL);
}

static const struct file_operations g341_ecc_progress_fops = {
	.owner = THIS_MODULE,
	.open = g341_ecc_progress_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int g341_ecc_health_show(struct seq_file *seq, void *unused)
{
	struct g341_scan_state *scan = seq->private;
	struct g341_block_health *health;
	struct g341_page_event *event;
	unsigned int i;

	(void)unused;
	seq_printf(seq,
		"g341=read_only scan=all_virtual_pages repeat=hard_or_weak "
		"no_block_status=1 no_block_isbad=1 no_key_init=1 no_secure_init=1 "
		"no_write=1 proposed_factory_entry=none\n");
	seq_printf(seq,
		"geometry blocks=%u pages_per_block=%u writesize=0x%x erasesize=0x%x "
		"bch_max=%u bch_limit=%u weak_threshold=%u boot_reserved_blocks=0,1\n",
		scan->total_blocks, scan->pages_per_block,
		scan->aml_chip->mtd.writesize, scan->aml_chip->mtd.erasesize,
		scan->aml_chip->ecc_max, scan->aml_chip->ecc_cnt_limit,
		scan->weak_threshold);
	seq_printf(seq,
		"summary scan_error=%d pages_read=%u repeat_reads=%u corrected_pages=%u "
		"weak_pages=%u hard_first_pages=%u hard_repeat_pages=%u "
		"stable_hard_pages=%u transient_hard_pages=%u repeat_only_hard_pages=%u "
		"data_mismatch_pages=%u oob_mismatch_pages=%u hard_blocks=%u "
		"unstable_blocks=%u weak_blocks=%u events=%u dropped_events=%u\n",
		scan->scan_error, scan->pages_read, scan->repeat_reads,
		scan->corrected_pages, scan->weak_pages,
		scan->hard_first_pages, scan->hard_repeat_pages,
		scan->stable_hard_pages, scan->transient_hard_pages,
		scan->repeat_only_hard_pages, scan->data_mismatch_pages,
		scan->oob_mismatch_pages, scan->hard_blocks,
		scan->unstable_blocks, scan->weak_blocks,
		scan->event_count, scan->dropped_events);
	seq_puts(seq, "first_ecc_histogram=");
	for (i = 0; i < G341_ECC_HISTOGRAM_BINS; i++)
		if (scan->first_histogram[i])
			seq_printf(seq, "%s%u:%u", i ? "," : "", i,
				   scan->first_histogram[i]);
	seq_putc(seq, '\n');
	seq_puts(seq, "repeat_ecc_histogram=");
	for (i = 0; i < G341_ECC_HISTOGRAM_BINS; i++)
		if (scan->repeat_histogram[i])
			seq_printf(seq, "%s%u:%u", i ? "," : "", i,
				   scan->repeat_histogram[i]);
	seq_putc(seq, '\n');

	for (i = 0; i < scan->total_blocks; i++) {
		health = &scan->blocks[i];
		if (!health->stable_hard_pages && !health->transient_hard_pages &&
		    !health->repeat_only_hard_pages && !health->weak_pages &&
		    !health->data_mismatch_pages && !health->oob_mismatch_pages)
			continue;
		seq_printf(seq,
			"block=%u virtual_addr=0x%llx pages=%u clean=%u corrected=%u weak=%u "
			"repeated=%u first_hard=%u repeat_hard=%u stable_hard=%u "
			"transient_hard=%u repeat_only_hard=%u data_mismatch=%u "
			"oob_mismatch=%u max_ecc=%u repeat_max_ecc=%u "
			"first_hard_page=%u first_weak_page=%u first_mismatch_page=%u "
			"proposed_runtime_entry=%u boot_reserved=%u\n",
			i, (unsigned long long)i * scan->aml_chip->mtd.erasesize,
			health->pages, health->clean_pages, health->corrected_pages,
			health->weak_pages, health->repeated_pages,
			health->first_hard_pages, health->repeat_hard_pages,
			health->stable_hard_pages, health->transient_hard_pages,
			health->repeat_only_hard_pages, health->data_mismatch_pages,
			health->oob_mismatch_pages, health->first_ecc_max,
			health->repeat_ecc_max, health->first_hard_page,
			health->first_weak_page, health->first_mismatch_page,
			health->stable_hard_pages && i > 1, i <= 1);
	}
	for (i = 0; i < scan->event_count; i++) {
		event = &scan->events[i];
		seq_printf(seq,
			"event=%u block=%u page=%u flags=0x%x first_error=%d "
			"repeat_error=%d first_failed=%u repeat_failed=%u "
			"first_corrected=%u repeat_corrected=%u first_ecc_max=%u "
			"repeat_ecc_max=%u\n",
			i, event->block, event->page, event->flags,
			event->first_error, event->repeat_error,
			event->first_failed, event->repeat_failed,
			event->first_corrected, event->repeat_corrected,
			event->first_ecc_max, event->repeat_ecc_max);
	}
	return 0;
}

static int g341_ecc_health_open(struct inode *inode, struct file *file)
{
	struct g341_scan_state *scan;
	int error;

	if (atomic_cmpxchg(&g341_scan_open, 0, 1))
		return -EBUSY;
	scan = kzalloc(sizeof(*scan), GFP_KERNEL);
	if (!scan) {
		error = -ENOMEM;
		goto clear_open;
	}
	scan->aml_chip = PDE(inode)->data;
	scan->scan_error = g341_scan_nand(scan);
	if (scan->scan_error) {
		error = scan->scan_error;
		goto free_scan;
	}
	error = single_open(file, g341_ecc_health_show, scan);
	if (error)
		goto free_arrays;
	return 0;

free_arrays:
	vfree(scan->events);
	vfree(scan->blocks);
free_scan:
	kfree(scan);
clear_open:
	atomic_set(&g341_scan_open, 0);
	return error;
}

static int g341_ecc_health_release(struct inode *inode, struct file *file)
{
	struct seq_file *seq = file->private_data;
	struct g341_scan_state *scan = seq->private;

	vfree(scan->events);
	vfree(scan->blocks);
	kfree(scan);
	atomic_set(&g341_scan_open, 0);
	return single_release(inode, file);
}

static const struct file_operations g341_ecc_health_fops = {
	.owner = THIS_MODULE,
	.open = g341_ecc_health_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = g341_ecc_health_release,
};

int g341_ecc_health_register(struct aml_nand_chip *aml_chip)
{
	if (!proc_create_data("g341_ecc_health", S_IRUGO, NULL,
			      &g341_ecc_health_fops, aml_chip)) {
		printk(KERN_ERR
		       "G341 rescue: failed to create ECC health diagnostic\n");
		return -ENOMEM;
	}
	if (!proc_create_data("g341_ecc_progress", S_IRUGO, NULL,
			      &g341_ecc_progress_fops, NULL)) {
		printk(KERN_ERR
		       "G341 rescue: failed to create ECC progress diagnostic\n");
		return -ENOMEM;
	}
	printk(KERN_INFO
	       "G341 rescue: full read-only ECC health diagnostics ready\n");
	return 0;
}
