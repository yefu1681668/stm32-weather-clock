#ifndef __DELAY_H__
#define __DELAY_H__


#include <stdint.h>


void delay_init(void);
void delay_us(uint32_t us);
void delay_ms(uint32_t ms);


#endif /* __DELAY_H__ */
