/*
 * Copyright (C) 2014 Loci Controls Inc.
 *               2017 HAW Hamburg
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @ingroup     cpu_cc2538
 * @ingroup     drivers_periph_uart
 * @{
 *
 * @file
 * @brief       Low-level UART driver implementation
 *
 * @author      Ian Martin <ian@locicontrols.com>
 * @author      Sebastian Meiling <s@mlng.net>
 * @}
 */

#include <stddef.h>

#include "board.h"
#include "cpu.h"
#include "periph/uart.h"
#include "periph_conf.h"

#undef BIT
#define BIT(n) ( 1 << (n) )

enum {
    FIFO_LEVEL_1_8TH = 0,
    FIFO_LEVEL_2_8TH = 1,
    FIFO_LEVEL_4_8TH = 2,
    FIFO_LEVEL_6_8TH = 3,
    FIFO_LEVEL_7_8TH = 4,
};

/* Valid word lengths for the LCRHbits.WLEN bit field: */
enum {
    WLEN_5_BITS = 0,
    WLEN_6_BITS = 1,
    WLEN_7_BITS = 2,
    WLEN_8_BITS = 3,
};

/* Bit field definitions for the UART Line Control Register: */
#define FEN   BIT( 4) /**< Enable FIFOs */

/* Bit masks for the UART Masked Interrupt Status (MIS) Register: */
#define OEMIS BIT(10) /**< UART overrun error masked status */
#define BEMIS BIT( 9) /**< UART break error masked status */
#define FEMIS BIT( 7) /**< UART framing error masked status */
#define RTMIS BIT( 6) /**< UART RX time-out masked status */
#define RXMIS BIT( 4) /**< UART RX masked interrupt status */

#define UART_CTL_HSE_VALUE    0
#define DIVFRAC_NUM_BITS      6
#define DIVFRAC_MASK          ( (1 << DIVFRAC_NUM_BITS) - 1 )

/** @brief Indicates if there are bytes available in the UART0 receive FIFO */
#define uart0_rx_avail() ( UART0->cc2538_uart_fr.FRbits.RXFE == 0 )

/** @brief Indicates if there are bytes available in the UART1 receive FIFO */
#define uart1_rx_avail() ( UART1->cc2538_uart_fr.FRbits.RXFE == 0 )

/** @brief Read one byte from the UART0 receive FIFO */
#define uart0_read()     ( UART0->DR )

/** @brief Read one byte from the UART1 receive FIFO */
#define uart1_read()     ( UART1->DR )

/*---------------------------------------------------------------------------*/

/**
 * @brief Allocate memory to store the callback functions.
 */
static uart_isr_ctx_t uart_config[UART_NUMOF];

cc2538_uart_t * const UART0 = (cc2538_uart_t *)0x4000c000;
cc2538_uart_t * const UART1 = (cc2538_uart_t *)0x4000d000;

/*---------------------------------------------------------------------------*/
static void reset(cc2538_uart_t *u)
{
    /* Make sure the UART is disabled before trying to configure it */
    u->cc2538_uart_ctl.CTLbits.UARTEN = 0;

    u->cc2538_uart_ctl.CTLbits.RXE = 1;
    u->cc2538_uart_ctl.CTLbits.TXE = 1;
    u->cc2538_uart_ctl.CTLbits.HSE = UART_CTL_HSE_VALUE;

    /* Clear error status */
    u->cc2538_uart_dr.ECR = 0xFF;

    /* Flush FIFOs by clearing LCHR.FEN */
    u->cc2538_uart_lcrh.LCRH &= ~FEN;

    /* Restore LCHR configuration */
    u->cc2538_uart_lcrh.LCRH |= FEN;

    /* UART Enable */
    u->cc2538_uart_ctl.CTLbits.UARTEN = 1;
}
/*---------------------------------------------------------------------------*/

#if UART_0_EN
void UART_0_ISR(void)
{
    uint_fast16_t mis;

    /* Latch the Masked Interrupt Status and clear any active flags */
    mis = UART_0_DEV->cc2538_uart_mis.MIS;
    UART_0_DEV->ICR = mis;

    while (UART_0_DEV->cc2538_uart_fr.FRbits.RXFE == 0) {
        uart_config[0].rx_cb(uart_config[0].arg, UART_0_DEV->DR);
    }

    if (mis & (OEMIS | BEMIS | FEMIS)) {
        /* ISR triggered due to some error condition */
        reset(UART_0_DEV);
    }

    cortexm_isr_end();
}
#endif /* UART_0_EN */

#if UART_1_EN
void UART_1_ISR(void)
{
    uint_fast16_t mis;

    /* Latch the Masked Interrupt Status and clear any active flags */
    mis = UART_1_DEV->cc2538_uart_mis.MIS;
    UART_1_DEV->ICR = mis;

    while (UART_1_DEV->cc2538_uart_fr.FRbits.RXFE == 0) {
        uart_config[1].rx_cb(uart_config[1].arg, UART_1_DEV->DR);
    }

    if (mis & (OEMIS | BEMIS | FEMIS)) {
        /* ISR triggered due to some error condition */
        reset(UART_1_DEV);
    }

    cortexm_isr_end();
}
#endif /* UART_1_EN */

static int init_base(uart_t uart, uint32_t baudrate);

int uart_init(uart_t uart, uint32_t baudrate, uart_rx_cb_t rx_cb, void *arg)
{
    /* initialize basic functionality */
    int res = init_base(uart, baudrate);
    if (res != UART_OK) {
        return res;
    }

    /* register callbacks */
    uart_config[uart].rx_cb = rx_cb;
    uart_config[uart].arg = arg;

    /* configure interrupts and enable RX interrupt */
    switch (uart) {
#if UART_0_EN
        case UART_0:
            NVIC_SetPriority(UART0_IRQn, UART_IRQ_PRIO);
            NVIC_EnableIRQ(UART0_IRQn);
            break;
#endif
#if UART_1_EN
        case UART_1:
            NVIC_SetPriority(UART1_IRQn, UART_IRQ_PRIO);
            NVIC_EnableIRQ(UART1_IRQn);
            break;
#endif
        default:
            return UART_NODEV;
    }

    return UART_OK;
}

static int init_base(uart_t uart, uint32_t baudrate)
{
    cc2538_uart_t *u = NULL;

    switch (uart) {
#if UART_0_EN
        case UART_0:
            u = UART_0_DEV;
            gpio_init_af(UART_0_RX_PIN, UART0_RXD, GPIO_IN);
            gpio_init_af(UART_0_TX_PIN, UART0_TXD, GPIO_OUT);
            break;
#endif
#if UART_1_EN
        case UART_1:
            u = UART_1_DEV;
            gpio_init_af(UART_1_RX_PIN, UART1_RXD, GPIO_IN);
            gpio_init_af(UART_1_TX_PIN, UART1_TXD, GPIO_OUT);
            break;
#endif

        default:
            (void)u;
            return UART_NODEV;
    }

#if UART_0_EN || UART_1_EN
    /* Enable clock for the UART while Running, in Sleep and Deep Sleep */
    unsigned int uart_num = ( (uintptr_t)u - (uintptr_t)UART0 ) / 0x1000;
    SYS_CTRL_RCGCUART |= (1 << uart_num);
    SYS_CTRL_SCGCUART |= (1 << uart_num);
    SYS_CTRL_DCGCUART |= (1 << uart_num);

    /* Make sure the UART is disabled before trying to configure it */
    u->cc2538_uart_ctl.CTL = 0;

    /* Run on SYS_DIV */
    u->CC = 0;

    /* On the CC2538, hardware flow control is supported only on UART1 */
    if (u == UART1) {
#ifdef UART_1_RTS_PIN
        gpio_init_af(UART_1_RTS_PIN, UART1_RTS, GPIO_OUT);
        u->cc2538_uart_ctl.CTLbits.RTSEN = 1;
#endif

#ifdef UART_1_CTS_PIN
        gpio_init_af(UART_1_CTS_PIN, UART1_CTS, GPIO_IN);
        u->cc2538_uart_ctl.CTLbits.CTSEN = 1;
#endif
    }

    /* Enable clock for the UART while Running, in Sleep and Deep Sleep */
    uart_num = ( (uintptr_t)u - (uintptr_t)UART0 ) / 0x1000;
    SYS_CTRL_RCGCUART |= (1 << uart_num);
    SYS_CTRL_SCGCUART |= (1 << uart_num);
    SYS_CTRL_DCGCUART |= (1 << uart_num);

    /*
     * UART Interrupt Masks:
     * Acknowledge RX and RX Timeout
     * Acknowledge Framing, Overrun and Break Errors
     */
    u->cc2538_uart_im.IM = 0;
    u->cc2538_uart_im.IMbits.RXIM = 1; /**< UART receive interrupt mask */
    u->cc2538_uart_im.IMbits.RTIM = 1; /**< UART receive time-out interrupt mask */
    u->cc2538_uart_im.IMbits.OEIM = 1; /**< UART overrun error interrupt mask */
    u->cc2538_uart_im.IMbits.BEIM = 1; /**< UART break error interrupt mask */
    u->cc2538_uart_im.IMbits.FEIM = 1; /**< UART framing error interrupt mask */

    /* Set FIFO interrupt levels: */
    u->cc2538_uart_ifls.IFLSbits.RXIFLSEL = FIFO_LEVEL_4_8TH; /**< MCU default */
    u->cc2538_uart_ifls.IFLSbits.TXIFLSEL = FIFO_LEVEL_4_8TH; /**< MCU default */

    u->cc2538_uart_ctl.CTLbits.RXE = 1;
    u->cc2538_uart_ctl.CTLbits.TXE = 1;
    u->cc2538_uart_ctl.CTLbits.HSE = UART_CTL_HSE_VALUE;

    /* Set the divisor for the baud rate generator */
    uint32_t divisor = sys_clock_freq();
    divisor <<= UART_CTL_HSE_VALUE + 2;
    divisor += baudrate / 2; /**< Avoid a rounding error */
    divisor /= baudrate;
    u->IBRD = divisor >> DIVFRAC_NUM_BITS;
    u->FBRD = divisor & DIVFRAC_MASK;

    /* Configure line control for 8-bit, no parity, 1 stop bit and enable  */
    u->cc2538_uart_lcrh.LCRH = (WLEN_8_BITS << 5) | FEN;

    /* UART Enable */
    u->cc2538_uart_ctl.CTLbits.UARTEN = 1;

    return UART_OK;
#endif /* UART_0_EN || UART_1_EN */
}

void uart_write(uart_t uart, const uint8_t *data, size_t len)
{
    cc2538_uart_t *u;

    switch (uart) {
#if UART_0_EN
        case UART_0:
            u = UART_0_DEV;
            break;
#endif
#if UART_1_EN
        case UART_1:
            u = UART_1_DEV;
            break;
#endif
        default:
            return;
    }

    /* Block if the TX FIFO is full */
    for (size_t i = 0; i < len; i++) {
        while (u->cc2538_uart_fr.FRbits.TXFF) {}
        u->DR = data[i];
    }
}

void uart_poweron(uart_t uart)
{
    (void) uart;

}

void uart_poweroff(uart_t uart)
{
    (void) uart;
}
