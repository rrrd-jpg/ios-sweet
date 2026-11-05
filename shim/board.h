#pragma once
#ifdef BOARD_S10E
#define FB_BASE   0xca000000UL   /* s3cfb.bootloaderfb from the s-boot cmdline */
#define FB_W      1080
#define FB_H      2280
#else
#define FB_BASE   0x9c000000UL   /* sweet: reserved-memory cont_splash_region */
#define FB_W      1080
#define FB_H      2400
#endif
#define FB_STRIDE (FB_W * 4)
