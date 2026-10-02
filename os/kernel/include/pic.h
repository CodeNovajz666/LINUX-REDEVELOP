#ifndef PIC_H
#define PIC_H

#include <stdint.h>

void pic_init(void);
void pic_eoi(uint32_t int_no);
void pic_mask_irq(uint32_t irq, int masked);

#endif /* PIC_H */
