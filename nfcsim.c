/*
 * For kernel 3.13
 */

#include <linux/init.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/vmalloc.h>
#include <linux/math64.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/nand.h>
#include <linux/mtd/nand_bch.h>
#include <linux/mtd/partitions.h>
#include <linux/delay.h>
#include <linux/list.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>


#define NFC_FIRST_ID_BYTE  0x98
#define NFC_SECOND_ID_BYTE 0x39
#define NFC_THIRD_ID_BYTE  0xFF /* No byte */
#define NFC_FOURTH_ID_BYTE 0xFF /* No byte */

/* After a command is input, the simulator goes to one of the following states */
#define STATE_CMD_NONE         0x00000000 /* No command state */
#define STATE_CMD_READ0        0x00000001 /* read data from the beginning of page */
#define STATE_CMD_READ1        0x00000002 /* read data from the second half of page */
#define STATE_CMD_READSTART    0x00000003 /* read data second command (large page devices) */
#define STATE_CMD_PAGEPROG     0x00000004 /* start page program */
#define STATE_CMD_READOOB      0x00000005 /* read OOB area */
#define STATE_CMD_ERASE1       0x00000006 /* sector erase first command */
#define STATE_CMD_STATUS       0x00000007 /* read status */
#define STATE_CMD_SEQIN        0x00000009 /* sequential data input */
#define STATE_CMD_READID       0x0000000A /* read ID */
#define STATE_CMD_ERASE2       0x0000000B /* sector erase second command */
#define STATE_CMD_RESET        0x0000000C /* reset */
#define STATE_CMD_RNDOUT       0x0000000D /* random output command */
#define STATE_CMD_RNDOUTSTART  0x0000000E /* random output start command */
#define STATE_CMD_MASK         0x0000000F /* command states mask */


#define PK(fmt, args...)  printk(KERN_INFO "nfc: " fmt, ##args )
#define PKL(fmt, args...) printk(KERN_INFO "nfc: " fmt "\n", ##args )

/* biggest size needed I can think off */
#define NFC_BUF_SIZE  ( 4096 + 1024 )


static uint log            = 0;
module_param(log,            uint, 0400);
MODULE_PARM_DESC(log,            "Perform logging if not zero");


union nfc_mem {
	u_char *byte;    /* for byte access */
	uint16_t *word;  /* for 16-bit word access */
};

struct nfcs_info{
	struct mtd_partition partition;
	struct mtd_info mtd;
	struct nand_chip chip;

	u_char ids[4];

	struct {
		unsigned command;
		u_char status;
		uint row;
		uint column;
		uint count; /* internal counter */
		uint num;   /* number of bytes which must be processed */
		uint off;
	} regs;

	struct {
		uint64_t totsz;
		uint pgsz;
		uint oobsz;
		uint pgszoob;
		uint pgnum;
		uint secsz;
		uint pgsec;
	} geom;

	struct kmem_cache *slab;  /* Slab allocator for nand pages */
	union nfc_mem *pages;
	union nfc_mem buf;
};

struct nfcs_info nfc;


static int check_command(int cmd)
{
	switch (cmd) {
	case NAND_CMD_READ0:
	case NAND_CMD_READ1:
	case NAND_CMD_READSTART:
	case NAND_CMD_PAGEPROG:
	case NAND_CMD_READOOB:
	case NAND_CMD_ERASE1:
	case NAND_CMD_STATUS:
	case NAND_CMD_SEQIN:
	case NAND_CMD_READID:
	case NAND_CMD_ERASE2:
	case NAND_CMD_RESET:
	case NAND_CMD_RNDOUT:
	case NAND_CMD_RNDOUTSTART:
		return 0;

	default:
		return 1;
	}
}


static union nfc_mem * nfc_get_page(uint pg_index)
{
	union nfc_mem *page = &(nfc.pages[pg_index]);

	PKL("get page 0x%x", pg_index);
	if(page->byte)
		return page;

	/* PKL("allocating memory for page 0x%x", pg_index); */
	page->byte = kmem_cache_alloc(nfc.slab, GFP_NOFS);
	if (page->byte == NULL) {
		PKL("prog_page: error allocating memory for page %d", pg_index);
		return NULL;
	}


	memset(page->byte, 0xff, nfc.geom.pgszoob);

	return page;
}

static union nfc_mem* nfc_get_page_noalloc(uint pg_index)
{
	return &(nfc.pages[pg_index]);
}


static int nfc_read_page_hwecc(struct mtd_info *mtd, struct nand_chip *chip, uint8_t *buf, int oob_required, int page)
{
	if (page != nfc.regs.row) {
		PKL("page number from CMD_READ0 is %d, but got %d", nfc.regs.row, page);
		dump_stack();
		mtd->ecc_stats.failed++;
		return 0;
	}

	if (nfc.regs.command != NAND_CMD_READ0) {
		PKL("page number from CMD_READ0 is %d, but got %d", nfc.regs.row, page);
		dump_stack();
		mtd->ecc_stats.failed++;
		return 0;

	}

	if (!oob_required) {
		nfc.regs.num -= nfc.geom.oobsz;
	}

	PKL("read page hwecc, copying 0x%x bytes, oob_required %d", nfc.regs.num, oob_required);

	memcpy(buf, nfc.buf.byte, nfc.regs.num);

	return 0;
}

static int nfc_write_page_hwecc(struct mtd_info *mtd, struct nand_chip *chip, const uint8_t *buf, int oob_required)
{

	if (nfc.regs.command != NAND_CMD_SEQIN) {
		dump_stack();
		return 0;
	}
	if (nfc.regs.count > 0) {
		PKL("write page out of range");
		dump_stack();
		return 0;
	}

	if (!oob_required) {
		nfc.regs.num -= nfc.geom.oobsz;
	}

	memcpy(nfc.buf.byte, buf, nfc.regs.num);
	nfc.regs.count += nfc.regs.num;

	return 0;
}

static void nfc_write_buf(struct mtd_info *mtd, const uint8_t *buf, int len)
{
	PKL("write buf, len 0x%x, count 0x%x, num 0x%x", len, nfc.regs.count, nfc.regs.num);

	if (nfc.regs.command != NAND_CMD_SEQIN) {
		dump_stack();
		return;
	}

	if (nfc.regs.count + len > nfc.regs.num) {
		PKL("write buf out of range");
		dump_stack();
		return;
	}

	memcpy(nfc.buf.byte + nfc.regs.count, buf, len);
	nfc.regs.count += len;

	return;
}


static int nfc_waitfunc(struct mtd_info *mtd, struct nand_chip *this)
{
	PKL("nfc wait");
	return 0;
}

static void nfc_select_chip(struct mtd_info *mtd, int chip)
{
	return;
}

static void nfc_cmdfunc(struct mtd_info *mtd, unsigned command,	int column, int page_addr)
{
	int i;
	unsigned pre_command = nfc.regs.command;

	if (check_command(command)) {
		PKL("unknow command %x", command);
		return;
	}

	nfc.regs.command = command;

	PKL("processing command 0x%x", command);

	if(command == NAND_CMD_READID) {
		/* 4 id bytes */
		nfc.regs.num = 4;
		nfc.regs.row = 0;
		nfc.regs.off = 0;
		nfc.regs.count = 0;
		nfc.regs.column = 0;

		/* copy id to the chip buffer */
		memcpy(nfc.buf.byte, nfc.ids, nfc.regs.num);
	}
	else if (command == NAND_CMD_RESET){
		PKL("CMD RESET");

		nfc.regs.num = 0;
		nfc.regs.row = 0;
		nfc.regs.off = 0;
		nfc.regs.count = 0;
		nfc.regs.column = 0;
	}
	else if (command == NAND_CMD_READOOB) {
		union nfc_mem *pg;

		PKL("CMD READOOB");

		nfc.regs.off = nfc.geom.pgsz;
		nfc.regs.row = page_addr;
		nfc.regs.num = nfc.geom.pgszoob - nfc.regs.off - nfc.regs.column;
		nfc.regs.count = 0;
		nfc.regs.column = 0;

		/* copy oob data from page to buf */
		pg = nfc_get_page(nfc.regs.row);


		memcpy(nfc.buf.byte, pg->byte + nfc.regs.off, nfc.regs.num);
	}
	else if (command == NAND_CMD_READ0) {
		union nfc_mem * pg;
		PKL("CMD READ0 col 0x%x row 0x%x", column, page_addr);

		nfc.regs.off = 0;
		nfc.regs.num = nfc.geom.pgszoob - nfc.regs.column - nfc.regs.off;
		nfc.regs.row = page_addr;
		nfc.regs.count = 0;
		nfc.regs.column = column;

		pg = nfc_get_page(nfc.regs.row);

		memcpy(nfc.buf.byte, pg->byte + nfc.regs.off + nfc.regs.column, nfc.regs.num);
	}
	else if (command == NAND_CMD_ERASE1) {
		PKL("CMD ERASE1");

		nfc.regs.num = 0;
		nfc.regs.row = 0;
		nfc.regs.off = 0;
		nfc.regs.count = 0;
		nfc.regs.column = 0;

		if (nfc.regs.column != 0) {
			PKL("erase expecting column is 0");
		}
		if (nfc.regs.row % nfc.geom.pgsec != 0) {
			PKL("erase expecting row at the block boundary");
			nfc.regs.row -= (nfc.regs.row % nfc.geom.pgsec);
		}

		PKL("erase 0x%x pages from page 0x%x ", nfc.geom.pgsec, nfc.regs.row);
		/*
		 * start to erase the block, we just free the memory if allocated
		 * page is allocated at read, memory will be really erased by setting mem to 0xff
		 */
		for (i = 0; i < nfc.geom.pgsec; i++ ) {
			union nfc_mem *pg;
			uint pgnum = nfc.regs.row + i;

			pg = nfc_get_page_noalloc(pgnum);
			if (pg->byte != NULL) {
				kmem_cache_free(nfc.slab, pg->byte);
				pg->byte = NULL;
			}
		}
	}
	else if (command == NAND_CMD_ERASE2) {
		PKL("CMD ERASE2");

		if (pre_command != NAND_CMD_ERASE1) {
			PKL("expecting the previous command is ERASE1");
			dump_stack();
			return;
		}

		nfc.regs.num = 0;
		nfc.regs.row = 0;
		nfc.regs.off = 0;
		nfc.regs.count = 0;
		nfc.regs.column = 0;

		PKL("Handling ERASE2, nothing to do");
	}
	else if (command == NAND_CMD_SEQIN) {
		PKL("CMD SEQIN");

		nfc.regs.num = nfc.geom.pgszoob - nfc.regs.column;
		nfc.regs.row = page_addr;
		nfc.regs.off = 0;
		nfc.regs.count = 0;
		nfc.regs.column = column;
	}
	else if (command == NAND_CMD_PAGEPROG) {
		union nfc_mem *pg;

		PKL("CMD PAGEPROG");
		if (pre_command != NAND_CMD_SEQIN) {
			PKL("expecting the previous command is SEQIN");
			dump_stack();
			return;
		}

		/* page to be programed is passed in in command SEQIN */
		pg = nfc_get_page(nfc.regs.row);
		memcpy(pg->byte + nfc.regs.column, nfc.buf.byte, nfc.regs.num);
	}
	else if (command == NAND_CMD_STATUS) {
		PKL("CMD STATUS");

		nfc.regs.num = 1;
		nfc.regs.row = 0;
		nfc.regs.off = 0;
		nfc.regs.count = 0;
		nfc.regs.column = 0;

		nfc.buf.byte[0] = nfc.regs.status;
	}
	else {
		PKL("cmdfunc: not implemented command %x", command);
		dump_stack();
	}

	return;
}

static u16 nfc_read_word(struct mtd_info *mtd)
{
	struct nand_chip *chip = &nfc.chip;

	return chip->read_byte(mtd) | (chip->read_byte(mtd) << 8);
}

static uint8_t nfc_read_byte(struct mtd_info *mtd)
{
	uint8_t outb = 0;

	if (nfc.regs.count >= nfc.regs.num) {
		PKL("read byte at index %d, out of range %d", nfc.regs.count, nfc.regs.num);
		outb = 0xff;
		return outb;
	}

	outb = nfc.buf.byte[nfc.regs.count];
	nfc.regs.count ++;

	PKL("read byte, value 0x%x", outb);

	return outb;
}

static void nfc_read_buf(struct mtd_info *mtd, uint8_t *buf, int len)
{
	PKL("read buf, len 0x%x", len);

	if(nfc.regs.count + len > nfc.regs.num) {
		PKL("read out of the boundary: %d, %d", nfc.regs.count, nfc.regs.num);
		return;
	}

	memcpy(buf, nfc.buf.byte + nfc.regs.count, len);
	nfc.regs.count += len;

	return;
}



static int __init nfc_init_module(void)
{
	int retval = -ENOMEM;
	int i;
	struct nand_chip *chip = &nfc.chip;
	struct mtd_info *mtd = &nfc.mtd;

	PKL("module init");

	nfc.partition.name = "nfc simulation partition 1";
	nfc.partition.offset = 0;
	nfc.partition.size = MTDPART_SIZ_FULL;

	nfc.ids[0] = NFC_FIRST_ID_BYTE;
	nfc.ids[1] = NFC_SECOND_ID_BYTE;
	nfc.ids[2] = NFC_THIRD_ID_BYTE;
	nfc.ids[3] = NFC_FOURTH_ID_BYTE;

	/* simulator is always ready and writable */
	nfc.regs.status = NAND_STATUS_WP | NAND_STATUS_READY;

	mtd->priv = chip;
	mtd->owner = THIS_MODULE;

	chip->ecc.mode = NAND_ECC_HW;
	/* What is the proper value? Need to be set? */
	chip->ecc.size = 512;
	chip->ecc.strength = 1;

	chip->ecc.read_page = nfc_read_page_hwecc;
	chip->ecc.write_page = nfc_write_page_hwecc;
	chip->waitfunc = nfc_waitfunc;
	chip->select_chip = nfc_select_chip;
	chip->cmdfunc = nfc_cmdfunc;
	chip->read_word = nfc_read_word;
	chip->read_byte = nfc_read_byte;
	chip->read_buf = nfc_read_buf;
	chip->write_buf = nfc_write_buf;
	/* needed for nand scan  */
	nfc.buf.byte = kmalloc(NFC_BUF_SIZE, GFP_KERNEL);
	if (!nfc.buf.byte) {
		PKL("unable to allocate chip buf\n");
		retval = -ENOMEM;
		goto exit;
	}

	/*
	 * scan nand identity, fill the meta data like page/oob size information
	 * based on the info of device id
	 */
	retval = nand_scan_ident(mtd, 1, NULL);
	if (retval) {
		PKL("cannot scan NFC simulator device identity");
		if (retval > 0)
			retval = -ENXIO;
		goto exit;
	}

	nfc.geom.totsz = mtd->size;
	nfc.geom.pgsz = mtd->writesize;
	nfc.geom.oobsz = mtd->oobsize;
	nfc.geom.pgszoob = nfc.geom.pgsz + nfc.geom.oobsz;
	nfc.geom.pgnum = div_u64(nfc.geom.totsz, nfc.geom.pgsz);
	nfc.geom.secsz = mtd->erasesize;
	nfc.geom.pgsec = nfc.geom.secsz / nfc.geom.pgsz;

	nfc.pages = vmalloc(nfc.geom.pgnum * sizeof(union nfc_mem));
	if (!nfc.pages) {
		PKL("unable to allocate page array\n");
		retval = -ENOMEM;
		goto exit;
	}
	for (i = 0; i < nfc.geom.pgnum; i++) {
		nfc.pages[i].byte = NULL;
	}

	nfc.slab = kmem_cache_create("nfcsim", nfc.geom.pgszoob, 0, 0, NULL);
	if (!nfc.slab) {
		PKL("unable to allocate page kmem_cache\n");
		retval = -ENOMEM;
		goto exit;
	}



	retval = nand_scan_tail(mtd);
	if (retval) {
		PKL("cannot scan NFC simulator device tail");
		if (retval > 0)
			retval = -ENXIO;
		goto exit;
	}

	PKL("bloc size is %d, pg num in block %d", nfc.geom.secsz, nfc.geom.pgsec);

	retval = mtd_device_register(mtd, &(nfc.partition), 1);
exit:
	return retval;
}
module_init(nfc_init_module);


static void __exit nfc_cleanup_module(void)
{
	int i;
	struct mtd_info *mtd = &nfc.mtd;

	if (nfc.pages) {
		for (i = 0; i < nfc.geom.pgnum; i++) {
			if (nfc.pages[i].byte)
				kmem_cache_free(nfc.slab,
						nfc.pages[i].byte);
		}
		kmem_cache_destroy(nfc.slab);
		vfree(nfc.pages);
	}
	if (nfc.buf.byte) {
		kfree(nfc.buf.byte);
	}
	mtd_device_unregister(mtd);
	PKL("module exit");
}
module_exit(nfc_cleanup_module);

MODULE_LICENSE ("GPL");
MODULE_AUTHOR ("Xun Gu");
MODULE_DESCRIPTION ("The NAND flash controller simulator");
