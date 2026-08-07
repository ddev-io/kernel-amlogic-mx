#ifndef __G347_LOGO_RESTORE_H__
#define __G347_LOGO_RESTORE_H__

struct aml_nand_chip;
struct mtd_info;

int g347_logo_restore_register(struct aml_nand_chip *aml_chip);
int g347_logo_write_gate_take(struct mtd_info *mtd,
			      const unsigned char *data,
			      const unsigned char *oob, int page,
			      int cached, int raw);

#endif
