/**
 ******************************************************************************
 * @file    boot_ota.c
 * @brief   第③步：串口固件升级状态机（HEAD -> DATA x N -> END -> 提交）
 *
 * 【为什么必须经过 W25Q】
 *   C8T6 只有 20 KB SRAM，而一版 APP 固件是 22 564 B，无论如何塞不进 RAM。
 *   所以每收到一帧 128 B 就立刻顺序写进外部 Flash，收完再从 W25Q 流式读回
 *   算 CRC32；校验通过之后才允许擦内部 APP 区。这样也顺带拿到一个好处：
 *   传坏包 / 断线 / 中途断电，APP 区一个字节都没动，板子不会变砖。
 *
 * 【为什么帧内绝不 HAL_Delay】
 *   F1 的 USART 没有 FIFO，RXNE 只锁存 1 个字节。115200 下每 86.8 us 就来
 *   一个字节，一旦在收帧途中插入 10 ms 延时，后面的字节全部溢出丢失。
 *   所以只有"还没进帧、在等 SOF"的阶段才允许慢一点（其实也是紧轮询），
 *   从 SOF 开始的每一个字节都用不带任何延时的紧循环去读。
 ******************************************************************************
 */
#include "boot_ota.h"
#include "boot_cfg.h"
#include "boot_uart.h"
#include "boot_flash.h"
#include "boot_crc.h"
#include "boot_jump.h"
#include "w25q.h"
#include "stm32f1xx_hal.h"
#include <string.h>

/* W25Q 暂存区开头的 32 B 镜像头。成员全是 uint32_t，无填充，正好 32 B。
 * 上位机用 struct.pack('<8I', ...) 生成，字节序一律小端。 */
typedef struct
{
    uint32_t magic;          /* OTA_IMAGE_MAGIC = "OTA2" */
    uint32_t length;         /* 固件字节数，不含这 32 B 头 */
    uint32_t crc32;          /* 对固件数据的 CRC32（与 zlib.crc32 一致） */
    uint32_t version;        /* 上位机给的版本号，Boot 只记录不解释 */
    uint32_t reserved[4];
} ota_image_hdr_t;

typedef struct
{
    uint8_t  type;
    uint16_t seq;
    uint16_t len;
    uint8_t  payload[OTA_PAYLOAD_MAX];
} ota_frame_t;

/* ------------------------------------------------------------------
 * CRC16-CCITT-FALSE：poly 0x1021 / init 0xFFFF / 不反射 / 末尾不取反。
 * 只用于单帧校验（128 B 级别），整包完整性由 CRC32 负责。
 * 上位机 tools/ota_send.py 里是逐位相同的算法。
 * ------------------------------------------------------------------ */
static uint16_t ota_crc16_update(uint16_t crc, const uint8_t *p, uint32_t len)
{
    while (len--)
    {
        crc ^= (uint16_t)((uint16_t)(*p++) << 8);
        for (uint8_t i = 0; i < 8; i++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
    }
    return crc;
}

/* Boot 的 LED 已经在 main() 里配好 PC13，这里只做电平翻转。
 * 等待升级期间保持 2 Hz 慢闪，和"正常启动的 5 Hz 快闪"区分开。 */
static void ota_led_toggle(void)
{
    HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
}

/** 收一帧。
 *  @return  1 = 收到合法帧；-1 = 帧收全了但 CRC/格式不对（上位机可重发）；
 *           0 = 没等到 SOF 或帧内字节超时
 */
static int ota_recv_frame(ota_frame_t *f, uint32_t sof_timeout_ms)
{
    uint8_t  hdr[6];
    uint8_t  tail[2];
    uint32_t start   = HAL_GetTick();
    uint32_t next_blink = 250;
    int      ch;

    /* ① 找帧头 SOF。此处是紧轮询，绝不 HAL_Delay —— 见文件头说明。
     *    在等第一帧的 30 s 里，任何非 SOF 字节（复位噪声、上位机发的 'U'、
     *    APP 打印的提示）都直接丢掉，不会被误当成数据。 */
    for (;;)
    {
        uint32_t el = HAL_GetTick() - start;

        if (el >= sof_timeout_ms)
            return 0;

        if (el >= next_blink)
        {
            ota_led_toggle();
            next_blink += 250;              /* 4 次翻转/秒 = 2 Hz */
        }

        ch = boot_uart_getchar();
        if (ch == OTA_FRAME_SOF)
            break;
    }
    hdr[0] = (uint8_t)OTA_FRAME_SOF;

    /* ② 剩下 5 B 头：TYPE / SEQ(2) / LEN(2)，全部小端 */
    for (uint8_t i = 1; i < 6; i++)
    {
        ch = boot_uart_getchar_timeout(OTA_BYTE_TIMEOUT_MS);
        if (ch < 0)
            return 0;
        hdr[i] = (uint8_t)ch;
    }

    f->type = hdr[1];
    f->seq  = (uint16_t)(hdr[2] | ((uint16_t)hdr[3] << 8));
    f->len  = (uint16_t)(hdr[4] | ((uint16_t)hdr[5] << 8));

    if (f->len > OTA_PAYLOAD_MAX)
        return -1;

    /* ③ PAYLOAD */
    for (uint16_t i = 0; i < f->len; i++)
    {
        ch = boot_uart_getchar_timeout(OTA_BYTE_TIMEOUT_MS);
        if (ch < 0)
            return 0;
        f->payload[i] = (uint8_t)ch;
    }

    /* ④ 帧尾 CRC16（覆盖 hdr[1..5] + PAYLOAD，不含 SOF） */
    for (uint8_t i = 0; i < 2; i++)
    {
        ch = boot_uart_getchar_timeout(OTA_BYTE_TIMEOUT_MS);
        if (ch < 0)
            return 0;
        tail[i] = (uint8_t)ch;
    }

    uint16_t want = (uint16_t)(tail[0] | ((uint16_t)tail[1] << 8));
    uint16_t calc = ota_crc16_update(0xFFFFu, &hdr[1], 5);
    calc = ota_crc16_update(calc, f->payload, f->len);

    return (calc == want) ? 1 : -1;
}

static void ota_reply(uint8_t code)
{
    boot_uart_putc(code);
}

/** 把已经完整落在 W25Q 里的镜像提交到内部 APP 区。
 *  返回 1 = 成功；0 = 任何一步失败（此时 APP 区可能已被擦，需要重新升级）。 */
static int ota_commit(const ota_image_hdr_t *img, boot_param_t *param)
{
    uint8_t  buf[256];
    uint32_t off, n;
    uint32_t crc;

    /* ① 从 W25Q 流式读回固件区算 CRC32。
     *    256 B 一块，RAM 占用固定，不随固件大小增长。 */
    crc = boot_crc32_init();
    for (off = 0; off < img->length; off += sizeof(buf))
    {
        n = ((img->length - off) < sizeof(buf)) ? (img->length - off) : sizeof(buf);
        w25q_read(OTA_STAGING_ADDR + OTA_IMAGE_HDR_SIZE + off, buf, n);
        crc = boot_crc32_update(crc, buf, n);
    }
    crc = boot_crc32_final(crc);

    if (crc != img->crc32)
    {
        boot_log("[OTA] CRC MISMATCH w25q=");
        boot_log_hex(crc);
        boot_log(" head=");
        boot_log_hex(img->crc32);
        boot_log(" -> APP untouched\r\n");
        return 0;                          /* APP 区一个字节都没动，绝对安全 */
    }

    boot_log("[OTA] crc32 ok, erasing APP 47 KB...\r\n");

    /* ② 擦【整个】APP 区，而不是只擦 length 覆盖的页。
     *    顺带把迁址前那版 48 KB 旧固件的残骸清干净 —— 11-14 日志里
     *    scan_end=47352 就是它造成的，升级后会变成真实长度。 */
    if (!boot_flash_erase_range(APP_START_ADDR, APP_MAX_SIZE))
    {
        boot_log("[OTA] erase FAILED\r\n");
        return 0;
    }

    /* ③ 从 W25Q 逐块复制回内部 Flash（boot_flash_write 内部按半字编程） */
    for (off = 0; off < img->length; off += sizeof(buf))
    {
        n = ((img->length - off) < sizeof(buf)) ? (img->length - off) : sizeof(buf);
        w25q_read(OTA_STAGING_ADDR + OTA_IMAGE_HDR_SIZE + off, buf, n);

        if (!boot_flash_write(APP_START_ADDR + off, buf, n))
        {
            boot_log("[OTA] write FAILED @");
            boot_log_u32(off);
            boot_log("\r\n");
            return 0;
        }
    }

    /* ④ 内部 Flash 是内存映射的，直接按地址读回来再算一次 CRC32。
     *    这一步能抓到"HAL_FLASH_Program 返回 OK 但实际没写进去"的情况。 */
    if (boot_crc32((const void *)APP_START_ADDR, img->length) != img->crc32)
    {
        boot_log("[OTA] readback CRC FAILED\r\n");
        return 0;
    }

    /* ⑤ 向量表合法性：栈顶必须落在 20 KB RAM 内，复位向量必须落在 APP 区 */
    if (!boot_app_check(APP_START_ADDR))
    {
        boot_log("[OTA] bad vector table\r\n");
        return 0;
    }

    /* ⑥ 记账。参数页在 0x0800FC00，和 APP 区不重叠。
     *    写失败只警告：固件本身已经好了，账本坏了不影响这次启动。 */
    param->upgrade_req = 0;
    param->app_size    = img->length;
    param->app_crc32   = img->crc32;
    param->boot_try    = 0;
    if (!boot_param_write(param))
        boot_log("[OTA] WARN: param page write failed\r\n");

    boot_log("[OTA] write ");
    boot_log_u32(img->length);
    boot_log(" B, readback crc ok\r\n");
    return 1;
}

int boot_ota_run(boot_param_t *param)
{
    ota_frame_t     f;
    ota_image_hdr_t img;
    uint32_t        got        = 0;       /* 已写入 W25Q 的固件字节数 */
    uint16_t        expect_seq = 0;       /* 期望的下一块 DATA 序号 */
    int             head_ok    = 0;       /* 是否已收到并校验 HEAD */
    int             first      = 1;       /* 是否在等整个会话的第一个字节 */
    int             errors     = 0;       /* 连续坏帧计数 */

    /* W25Q 是本次升级的基础设施：没插好就别开始收数据，
     * 否则收到一半才发现写不进去，白等 30 秒。
     *
     * 顺序很关键：这些日志必须全部在 "waiting for firmware" 之前打完。
     * 上位机一看到那行就会立刻发 HEAD 帧，而串口没有 FIFO；
     * 如果 Boot 此时还在初始化 W25Q / 打日志，整帧会被溢出丢光。 */
    if (!w25q_init())
    {
        boot_log("[OTA] W25Q NOT FOUND -> abort\r\n");
        return 0;
    }
    boot_log("[OTA] W25Q id=");
    boot_log_hex(w25q_jedec_id());
    boot_log("\r\n");

    /* 这是进入收帧循环前的最后一条日志：发完立刻开始紧轮询等 SOF */
    boot_log("waiting for firmware ");
    boot_log_u32(OTA_FIRST_FRAME_MS);
    boot_log(" ms...\r\n");

    for (;;)
    {
        /* 第一帧给 30 s（人要打开脚本），之后每帧 3 s 足够 */
        int r = ota_recv_frame(&f, first ? OTA_FIRST_FRAME_MS : 3000UL);

        if (r == 0 && first)
        {
            boot_log("timeout -> fallback to app\r\n");
            return 0;                      /* 误触发一次：原 APP 完好，照常跳走 */
        }

        if (r <= 0)
        {
            ota_reply(OTA_NAK);            /* 让上位机重发本帧 */
            if (++errors > 3)
            {
                boot_log("[OTA] too many frame errors -> abort\r\n");
                ota_reply(OTA_ABORT);
                return 0;
            }
            continue;
        }

        first  = 0;
        errors = 0;

        /* ---------- HEAD ---------- */
        if (f.type == OTA_TYPE_HEAD)
        {
            if (f.seq != 0 || f.len != OTA_IMAGE_HDR_SIZE)
            {
                ota_reply(OTA_NAK);
                continue;
            }

            memcpy(&img, f.payload, sizeof(img));

            if (img.magic != OTA_IMAGE_MAGIC ||
                img.length < 8u || img.length > APP_MAX_SIZE)
            {
                boot_log("[OTA] bad HEAD (magic/length)\r\n");
                ota_reply(OTA_NAK);
                continue;
            }

            /* 擦暂存区覆盖到的扇区。0x020000 是 4 KB 对齐的，
             * 而且和 0x010000 的图标区隔着 64 KB，绝不会误擦图标。 */
            if (!w25q_erase_range(OTA_STAGING_ADDR, OTA_IMAGE_HDR_SIZE + img.length) ||
                !w25q_write(OTA_STAGING_ADDR, &img, sizeof(img)))
            {
                boot_log("[OTA] W25Q staging FAILED -> abort\r\n");
                ota_reply(OTA_ABORT);
                return 0;
            }

            got        = 0;
            expect_seq = 0;
            head_ok    = 1;

            boot_log("[OTA] HEAD len=");
            boot_log_u32(img.length);
            boot_log(" crc=");
            boot_log_hex(img.crc32);
            boot_log("\r\n");
            ota_reply(OTA_ACK);
            continue;
        }

        /* ---------- DATA ---------- */
        if (f.type == OTA_TYPE_DATA)
        {
            if (!head_ok)
            {
                ota_reply(OTA_NAK);
                continue;
            }

            /* 上位机没收到上一个 ACK 时会重发旧帧：数据已经写过，
             * 直接回 ACK 让它继续，避免 got 被重复累加。 */
            if (expect_seq > 0 && f.seq == (uint16_t)(expect_seq - 1u))
            {
                ota_reply(OTA_ACK);
                continue;
            }

            if (f.seq != expect_seq || (got + f.len) > img.length || f.len == 0)
            {
                ota_reply(OTA_NAK);
                continue;
            }

            if (!w25q_write(OTA_STAGING_ADDR + OTA_IMAGE_HDR_SIZE + got,
                            f.payload, f.len))
            {
                boot_log("[OTA] W25Q write FAILED @");
                boot_log_u32(got);
                boot_log(" -> abort\r\n");
                ota_reply(OTA_ABORT);
                return 0;
            }

            got += f.len;
            expect_seq++;

            /* 每 32 帧（4 KB）报一次进度，日志太密会拖慢串口、也会刷屏 */
            if ((expect_seq % 32u) == 0u)
            {
                boot_log("[OTA] recv ");
                boot_log_u32(got);
                boot_log("/");
                boot_log_u32(img.length);
                boot_log("\r\n");
            }

            ota_reply(OTA_ACK);
            continue;
        }

        /* ---------- END ---------- */
        if (f.type == OTA_TYPE_END)
        {
            if (!head_ok || got != img.length || f.seq != expect_seq)
            {
                boot_log("[OTA] premature END (got ");
                boot_log_u32(got);
                boot_log("/");
                boot_log_u32(img.length);
                boot_log(")\r\n");
                ota_reply(OTA_NAK);
                continue;
            }

            if (!ota_commit(&img, param))
            {
                boot_log("[OTA] commit FAILED -> abort\r\n");
                ota_reply(OTA_ABORT);
                return 0;                  /* 主流程会停在 Boot 等重新升级 */
            }

            boot_log("[OTA] DONE, resetting...\r\n");
            ota_reply(OTA_DONE);
            HAL_Delay(100);                /* 等 DONE 和日志真正发完再复位 */
            NVIC_SystemReset();
            return 1;                      /* 正常走不到这里 */
        }

        /* 未知帧类型 */
        ota_reply(OTA_NAK);
    }
}
