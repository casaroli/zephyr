/****************************************************************************
 * arch/xtensa/src/esp32s3/esp32s3_psram.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#define DT_DRV_COMPAT raspberrypi_rp2350_qmi_psram

#include <stdio.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pinctrl.h>

#include "hardware/structs/ioqspi.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/sync.h"
#include "hardware/gpio.h"

#include "pico/platform/sections.h"
#include "zephyr/toolchain.h"

#include <zephyr/logging/log.h>
#include <zephyr/multi_heap/shared_multi_heap.h>

LOG_MODULE_REGISTER(foo, CONFIG_LOG_DEFAULT_LEVEL);

struct memc_rp2350_psram_config_fmt {
    uint32_t dtr;
    uint32_t dummy_len;
    uint32_t suffix_len;
    uint32_t prefix_len;
    uint32_t data_width;
    uint32_t dummy_width;
    uint32_t suffix_width;
    uint32_t addr_width;
    uint32_t prefix_width;
    uint32_t suffix;
    uint32_t prefix;
};

struct memc_rp2350_psram_config {
    const struct pinctrl_dev_config *pcfg;
	uint32_t max_frequency;
    uint32_t cooldown;
    uint32_t pagebreak;
    uint32_t select_setup;
    uint32_t select_hold;
    uint32_t max_select;
    uint32_t min_deselect;
    uint32_t rxdelay;
    uint32_t clkdiv;
    struct memc_rp2350_psram_config_fmt read;
    struct memc_rp2350_psram_config_fmt write;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static size_t __ramfunc psram_detect(void) {
    int psram_size = 0;

    uint32_t intr_stash = save_and_disable_interrupts();

    // Try and read the PSRAM ID via direct_csr.
    qmi_hw->direct_csr = 30 << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;

    // Need to poll for the cooldown on the last XIP transfer to expire
    // (via direct-mode BUSY flag) before it is safe to perform the first
    // direct-mode operation
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {
    }

    // Exit out of QMI in case we've inited already
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;

    // Transmit as quad.
    qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS | QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB | 0xf5;

    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {
    }

    (void)qmi_hw->direct_rx;

    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS);

    // Read the id
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    uint8_t kgd = 0;
    uint8_t eid = 0;

    for (size_t i = 0; i < 7; i++)
    {
        if (i == 0) {
            qmi_hw->direct_tx = 0x9f;
        } else {
            qmi_hw->direct_tx = 0xff;
        }

        while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS) == 0) {
        }

        while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {
        }

        if (i == 5) {
            kgd = qmi_hw->direct_rx;
        } else if (i == 6) {
            eid = qmi_hw->direct_rx;
        } else {
            (void)qmi_hw->direct_rx;
        }
    }

    // Disable direct csr.
    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);

    if (kgd == 0x5D) {
        psram_size = 1024 * 1024; // 1 MiB
        uint8_t size_id = eid >> 5;
        if (eid == 0x26 || size_id == 2) {
            psram_size *= 8; // 8 MiB
        } else if (size_id == 0) {
            psram_size *= 2; // 2 MiB
        } else if (size_id == 1) {
            psram_size *= 4; // 4 MiB
        }
    }

    restore_interrupts(intr_stash);
    return psram_size;
}

static size_t __ramfunc psram_init(const struct memc_rp2350_psram_config *dev_cfg) {
    /* Select "default" state at initialization time */
    if (pinctrl_apply_state(dev_cfg->pcfg, PINCTRL_STATE_DEFAULT) < 0) {
        return 0;
    }

    /* Make sure flash is deselected - QMI doesn't appear to have a busy flag(!) */
    while ((ioqspi_hw->io[1].status & IO_QSPI_GPIO_QSPI_SS_STATUS_OUTTOPAD_BITS) != IO_QSPI_GPIO_QSPI_SS_STATUS_OUTTOPAD_BITS) {
        ;
    }

    /* Set low speed timing before we can configure the PSRAM */

    // if (CPU FREQUENCY > dev_cfg->max_frequency) {
        qmi_hw->m[1].timing = 0x40000202;
    // } else {
        // qmi_hw->m[1].timing = 0x40000101;
    // }

    // Force a read through XIP to ensure the timing is applied
    volatile uint32_t *ptr = (volatile uint32_t *)0x14000000;
    (void)*ptr;

    size_t psram_size = psram_detect();

    if (!psram_size) {
        return 0;
    }

    // Enable direct mode, PSRAM CS, clkdiv of 10.
    qmi_hw->direct_csr = 10 << QMI_DIRECT_CSR_CLKDIV_LSB | \
        QMI_DIRECT_CSR_EN_BITS | \
        QMI_DIRECT_CSR_AUTO_CS1N_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {
        ;
    }

    // Enable QPI mode on the PSRAM
    const uint32_t CMD_QPI_EN = 0x35;
    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | CMD_QPI_EN;

    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {
        ;
    }

    #if 0
    // Set PSRAM timing for APS6404:
    // - Max select assumes a sys clock speed >= 240MHz
    // - Min deselect assumes a sys clock speed <= 305MHz
    // - Clkdiv of 2 is OK up to 266MHz.
    qmi_hw->m[1].timing = 1 << QMI_M1_TIMING_COOLDOWN_LSB |
        QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB |
        30 << QMI_M1_TIMING_MAX_SELECT_LSB |
        5 << QMI_M1_TIMING_MIN_DESELECT_LSB |
        3 << QMI_M1_TIMING_RXDELAY_LSB |
        2 << QMI_M1_TIMING_CLKDIV_LSB;
    #elif 0
    // Set PSRAM timing for APS6404:
    // - Max select assumes a sys clock speed >= 120MHz
    // - Min deselect assumes a sys clock speed <= 138MHz
    // - Clkdiv of 1 is OK up to 133MHz.
    qmi_hw->m[1].timing = 1 << QMI_M1_TIMING_COOLDOWN_LSB |
        QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB |
        15 << QMI_M1_TIMING_MAX_SELECT_LSB |
        2 << QMI_M1_TIMING_MIN_DESELECT_LSB |
        2 << QMI_M1_TIMING_RXDELAY_LSB |
        1 << QMI_M1_TIMING_CLKDIV_LSB;
    #else
        qmi_hw->m[1].timing =
            dev_cfg->cooldown << QMI_M1_TIMING_COOLDOWN_LSB |
            dev_cfg->pagebreak << QMI_M1_TIMING_PAGEBREAK_LSB |
            dev_cfg->select_setup << QMI_M1_TIMING_SELECT_SETUP_LSB|
            dev_cfg->select_hold << QMI_M1_TIMING_SELECT_HOLD_LSB |
            dev_cfg->max_select << QMI_M1_TIMING_MAX_SELECT_LSB |
            dev_cfg->min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB |
            dev_cfg->rxdelay << QMI_M1_TIMING_RXDELAY_LSB |
            dev_cfg->clkdiv << QMI_M1_TIMING_CLKDIV_LSB;
    #endif

    // Set PSRAM commands and formats
    qmi_hw->m[1].rfmt =
        dev_cfg->read.dtr << QMI_M1_RFMT_DTR_LSB |
        dev_cfg->read.dummy_len << QMI_M1_RFMT_DUMMY_LEN_LSB |
        dev_cfg->read.suffix_len << QMI_M1_RFMT_SUFFIX_LEN_LSB |
        dev_cfg->read.prefix_len << QMI_M1_RFMT_PREFIX_LEN_LSB |
        dev_cfg->read.data_width << QMI_M1_RFMT_DATA_WIDTH_LSB |
        dev_cfg->read.dummy_width << QMI_M1_RFMT_DUMMY_WIDTH_LSB |
        dev_cfg->read.suffix_width << QMI_M1_RFMT_SUFFIX_WIDTH_LSB |
        dev_cfg->read.addr_width << QMI_M1_RFMT_ADDR_WIDTH_LSB |
        dev_cfg->read.prefix_width << QMI_M1_RFMT_PREFIX_WIDTH_LSB;

    qmi_hw->m[1].rcmd =
        dev_cfg->read.suffix << QMI_M1_RCMD_SUFFIX_LSB |
        dev_cfg->read.prefix << QMI_M1_RCMD_PREFIX_LSB;

    qmi_hw->m[1].wfmt =
        dev_cfg->write.dtr << QMI_M1_WFMT_DTR_LSB |
        dev_cfg->write.dummy_len << QMI_M1_WFMT_DUMMY_LEN_LSB |
        dev_cfg->write.suffix_len << QMI_M1_WFMT_SUFFIX_LEN_LSB |
        dev_cfg->write.prefix_len << QMI_M1_WFMT_PREFIX_LEN_LSB |
        dev_cfg->write.data_width << QMI_M1_WFMT_DATA_WIDTH_LSB |
        dev_cfg->write.dummy_width << QMI_M1_WFMT_DUMMY_WIDTH_LSB |
        dev_cfg->write.suffix_width << QMI_M1_WFMT_SUFFIX_WIDTH_LSB |
        dev_cfg->write.addr_width << QMI_M1_WFMT_ADDR_WIDTH_LSB |
        dev_cfg->write.prefix_width << QMI_M1_WFMT_PREFIX_WIDTH_LSB;

    qmi_hw->m[1].wcmd =
        dev_cfg->write.suffix << QMI_M1_WCMD_SUFFIX_LSB |
        dev_cfg->write.prefix << QMI_M1_WCMD_PREFIX_LSB;

    // Disable direct mode
    qmi_hw->direct_csr = 0;

    // Enable writes to PSRAM
    hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);

    // TODO: Detect PSRAM ID and size
    return psram_size;
}

#ifdef CONFIG_SHARED_MULTI_HEAP
static struct shared_multi_heap_region smh_psram = {
	.addr = DT_REG_ADDR(DT_NODELABEL(psram)),
	.size = DT_REG_SIZE(DT_NODELABEL(psram)),
	.attr = SMH_REG_ATTR_EXTERNAL,
};
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rp23xx_psramconfig
 *
 * Description:
 *   Initialize any extra heap.
 *
 ****************************************************************************/

 static int memc_rp23xx_psram_init(const struct device *dev) {
  // psram_init(CONFIG_RP23XX_PSRAM_CS1_GPIO);
  //

  	const struct memc_rp2350_psram_config *dev_cfg = dev->config;

      LOG_INF("foo");
      LOG_ERR("device name: %s", dev->name);
      LOG_ERR("max frequency: %u", dev_cfg->max_frequency);
  size_t size;
//   printf("Initializing PSRAM...");
  size = psram_init(dev_cfg);


  printf("intialized psram of size %llu\n", (uintmax_t)size);

#ifdef CONFIG_SHARED_MULTI_HEAP
    int ret = shared_multi_heap_pool_init();
    if (ret < 0) {
        return ret;
    }
    ret = shared_multi_heap_add(&smh_psram, NULL);
    if (ret < 0) {
        return ret;
    }
#endif
  return 0;
}

PINCTRL_DT_INST_DEFINE(0);

struct memc_rp2350_psram_config memc_rp2350_config = {
    .pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(0),
	.max_frequency = DT_INST_PROP(0, max_frequency),
	.cooldown = DT_INST_PROP(0, cooldown),
	.pagebreak = DT_INST_PROP(0, pagebreak),
	.select_setup = DT_INST_PROP(0, select_setup),
	.select_hold = DT_INST_PROP(0, select_hold),
	.max_select = DT_INST_PROP(0, max_select),
	.min_deselect = DT_INST_PROP(0, min_deselect),
	.rxdelay = DT_INST_PROP(0, rxdelay),
	.clkdiv = DT_INST_PROP(0, clkdiv),
    .read = {
        .dtr = DT_PROP(DT_CHILD(DT_DRV_INST(0), read), dtr),
        .dummy_len = DT_PROP(DT_CHILD(DT_DRV_INST(0), read), dummy_len),
        .suffix_len = DT_PROP(DT_CHILD(DT_DRV_INST(0), read), suffix_len),
        .prefix_len = DT_PROP(DT_CHILD(DT_DRV_INST(0), read), prefix_len),
        .data_width = DT_PROP(DT_CHILD(DT_DRV_INST(0), read), data_width),
        .dummy_width = DT_PROP(DT_CHILD(DT_DRV_INST(0), read), dummy_width),
        .suffix_width = DT_PROP(DT_CHILD(DT_DRV_INST(0), read), suffix_width),
        .addr_width = DT_PROP(DT_CHILD(DT_DRV_INST(0), read), addr_width),
        .prefix_width = DT_PROP(DT_CHILD(DT_DRV_INST(0), read), prefix_width),
        .prefix = DT_PROP(DT_CHILD(DT_DRV_INST(0), read), prefix),
        .suffix = DT_PROP(DT_CHILD(DT_DRV_INST(0), read), suffix),
    },
    .write = {
        .dtr = DT_PROP(DT_CHILD(DT_DRV_INST(0), write), dtr),
        .dummy_len = DT_PROP(DT_CHILD(DT_DRV_INST(0), write), dummy_len),
        .suffix_len = DT_PROP(DT_CHILD(DT_DRV_INST(0), write), suffix_len),
        .prefix_len = DT_PROP(DT_CHILD(DT_DRV_INST(0), write), prefix_len),
        .data_width = DT_PROP(DT_CHILD(DT_DRV_INST(0), write), data_width),
        .dummy_width = DT_PROP(DT_CHILD(DT_DRV_INST(0), write), dummy_width),
        .suffix_width = DT_PROP(DT_CHILD(DT_DRV_INST(0), write), suffix_width),
        .addr_width = DT_PROP(DT_CHILD(DT_DRV_INST(0), write), addr_width),
        .prefix_width = DT_PROP(DT_CHILD(DT_DRV_INST(0), write), prefix_width),
        .prefix = DT_PROP(DT_CHILD(DT_DRV_INST(0), write), prefix),
        .suffix = DT_PROP(DT_CHILD(DT_DRV_INST(0), write), suffix),
    },
};


DEVICE_DT_INST_DEFINE(0, &memc_rp23xx_psram_init, NULL, NULL,
		      &memc_rp2350_config, POST_KERNEL,
		      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, NULL);
