#ifndef __ST_IMAGE_H__
#define __ST_IMAGE_H__


#include <stdint.h>

/* 片内图片描述符。
 *
 * 注意: 6 张天气图标已经搬到外部 W25Q(布局见 app/icons_flash.h),
 * 不再以 st_image_t 数组的形式编进固件 —— 那是 27 648 B, 差不多占了
 * APP 分区的一大半。现在图标由 draw_icon_flash() 按行从 W25Q 流式读取,
 * RAM 只占一行 96 B。本结构体保留给仍然放在片内的图片(如开机 logo)。 */
typedef struct
{
    const uint16_t width;
    const uint16_t height;
    const uint8_t *data;
} st_image_t;


#endif /* __ST_IMAGE_H__ */
