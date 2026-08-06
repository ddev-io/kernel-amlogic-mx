#ifndef __G343_BBT_RESTORE_H__
#define __G343_BBT_RESTORE_H__

struct aml_nand_chip;
struct mtd_info;

int g343_bbt_restore_register(struct aml_nand_chip *aml_chip);
int g343_bbt_write_gate_take(struct mtd_info *mtd, const unsigned char *data,
			     const unsigned char *oob, int page,
			     int cached, int raw);

#endif
