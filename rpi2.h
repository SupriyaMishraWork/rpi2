#ifndef UART_DRIVER_H
#define UART_DRIVER_H

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/string.h>


// Proc file names
#define PROC_UART_TX     "uart_tx"
#define PROC_UART_RX     "uart_rx"
#define PROC_UART_CONFIG "uart_config"
#define PROC_UART_STATUS "uart_status"
#define PROC_UART_STATS  "uart_stats"

// Base addresses for BCM2711 
#define PERIPHERAL_BASE 0xFE000000UL
#define AUX_BASE        (PERIPHERAL_BASE + 0x215000)
#define GPIO_BASE       (PERIPHERAL_BASE + 0x200000)

// GPIO Function Select values 
#define GPIO_FSEL_INPUT  0x0
#define GPIO_FSEL_OUTPUT 0x1
#define GPIO_FSEL_ALT0   0x4
#define GPIO_FSEL_ALT1   0x5
#define GPIO_FSEL_ALT2   0x6
#define GPIO_FSEL_ALT3   0x7
#define GPIO_FSEL_ALT4   0x3
#define GPIO_FSEL_ALT5   0x2

// GPIO Pull-up/down values 
#define GPIO_PUPDN_NONE  0x0
#define GPIO_PUPDN_UP    0x1
#define GPIO_PUPDN_DOWN  0x2

// Mini UART register structure
struct uart_regs {
    volatile u32 IRQ;           /* 0x00 */
    volatile u32 ENABLES;       /* 0x04 */
    volatile u32 RESERVED0[14]; /* 0x08-0x3C */
    volatile u32 MU_IO;         /* 0x40 */
    volatile u32 MU_IER;        /* 0x44 */
    volatile u32 MU_IIR;        /* 0x48 */
    volatile u32 MU_LCR;        /* 0x4C */
    volatile u32 MU_MCR;        /* 0x50 */
    volatile u32 MU_LSR;        /* 0x54 */
    volatile u32 MU_MSR;        /* 0x58 */
    volatile u32 MU_SCRATCH;    /* 0x5C */
    volatile u32 MU_CNTL;       /* 0x60 */
    volatile u32 MU_STAT;       /* 0x64 */
    volatile u32 MU_BAUD;       /* 0x68 */
};

// GPIO register offsets 
#define GPFSEL1    0x04
#define GPPUD      0x94
#define GPPUDCLK0  0x98
#define GPPUPPDN0  0xE4

// Supported baud rates
#define BAUD_9600    9600
#define BAUD_19200   19200
#define BAUD_38400   38400
#define BAUD_57600   57600
#define BAUD_115200  115200

// Data bits
#define DATA_BITS_7  0x0
#define DATA_BITS_8  0x3

// Driver configuration structure
struct uart_config {
    u32 baudrate;
    u32 data_bits;
    u32 system_clock;
};

// Driver statistics structure
struct uart_stats {
    u64 tx_bytes;
    u64 rx_bytes;
    u64 tx_errors;
    u64 rx_errors;
    u64 fifo_overruns;
};

#endif
