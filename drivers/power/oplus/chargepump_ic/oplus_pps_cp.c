// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2018-2022 Oplus. All rights reserved.
 */


#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/version.h>

#ifdef CONFIG_OPLUS_CHARGER_MTK
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/kobject.h>
#include <linux/platform_device.h>
#include <asm/atomic.h>

#include <linux/module.h>
#include <uapi/linux/rtc.h>
#include <linux/rtc.h>
#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0))
#include <mt-plat/charging.h>
#endif
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/proc_fs.h>
extern void mt_power_off(void);
#else
#include <linux/proc_fs.h>
#include <linux/debugfs.h>
#include <linux/gpio.h>
#include <linux/errno.h>
#include <linux/power_supply.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/bitops.h>
#include <linux/mutex.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/regulator/machine.h>
#include <soc/oplus/device_info.h>
#include <soc/oplus/system/boot_mode.h>
#endif

#include "../oplus_vooc.h"
#include "../oplus_gauge.h"
#include "../oplus_charger.h"
#include "../oplus_pps.h"
#include "../oplus_pps_ops_manager.h"

int __attribute__((weak)) oplus_pps_get_authentiate(void)
{
	return 0;
}
int __attribute__((weak)) oplus_sm8350_pps_get_authentiate(void)
{
	return 0;
}
int __attribute__((weak)) oplus_sm8350_read_vbat0_voltage(void)
{
	return 0;
}
int __attribute__((weak)) oplus_sm8350_check_btb_temp(void)
{
	return 0;
}


int oplus_cp_master_get_ucp_flag(void)
{
	return 0;
}
int oplus_cp_slave_get_ucp_flag(void)
{
	return 0;
}

int oplus_cp_slave_b_get_ucp_flag(void)
{
	return 0;
}

int oplus_cp_master_get_vout(void)
{
	return 0;
}

int oplus_cp_slave_get_vout(void)
{
	return 0;
}

int oplus_cp_slave_b_get_vout(void)
{
	return 0;
}

int oplus_cp_master_get_vac(void)
{
	return 0;
}

int oplus_cp_slave_get_vac(void)
{
	return 0;
}

int oplus_cp_slave_b_get_vac(void)
{
	return 0;
}

int oplus_cp_master_get_vbus(void)
{
	return 0;
}

int oplus_cp_slave_get_vbus(void)
{
	return 0;
}

int oplus_cp_master_get_tdie(void)
{
	return 0;
}

int oplus_cp_slave_get_tdie(void)
{
	return 0;
}

int oplus_cp_slave_b_get_tdie(void)
{
	return 0;
}

int oplus_cp_slave_b_get_vbus(void)
{
	return 0;
}

int oplus_cp_master_get_ibus(void)
{
	return 0;
}

int oplus_cp_slave_get_ibus(void)
{
	return 0;
}

int oplus_cp_slave_b_get_ibus(void)
{
	return 0;
}

int oplus_cp_master_cp_enable(int enable)
{
	return 0;
}

int oplus_cp_slave_cp_enable(int enable)
{
	return 0;
}

int oplus_cp_slave_b_cp_enable(int enable)
{
	return 0;
}

bool oplus_cp_master_get_enable(void)
{
	return 0;
}

bool oplus_cp_slave_get_enable(void)
{
	return 0;
}

void oplus_cp_master_pmid2vout_enable(bool enable)
{
}

void oplus_cp_slave_pmid2vout_enable(bool enable)
{
}

bool oplus_cp_slave_b_get_enable(void)
{
	return 0;
}

void oplus_cp_master_cfg_sc(void)
{
}

void oplus_cp_slave_cfg_sc(void)
{
}

void oplus_cp_master_cfg_bypass(void)
{
}

void oplus_cp_slave_cfg_bypass(void)
{
}

int oplus_cp_cfg_mode_init(int mode)
{
	return 0;
}

void oplus_cp_master_hardware_init(void)
{
}

void oplus_cp_slave_hardware_init(void)
{
}

void oplus_cp_hardware_init(void)
{
}

void oplus_cp_master_reset(void)
{
}

void oplus_cp_slave_reset(void)
{
}

void oplus_cp_reset(void)
{
}

void oplus_cp_pmid2vout_enable(bool enable)
{
}

int oplus_cp_master_dump_registers(void)
{
	return 0;
}

int oplus_cp_slave_dump_registers(void)
{
	return 0;
}

extern int op10_read_input_voltage(void);
extern int op10_read_vbat0_voltage(void);
extern int op10_check_btb_temp(void);
extern int oplus_chg_pps_get_max_cur(int vbus_mv);
extern void oplus_op10_set_mcu_pps_mode(bool pps);
extern int oplus_op10_get_mcu_pps_mode(void);

struct oplus_pps_operations oplus_cp_pps_ops = {
	.set_mcu_pps_mode = oplus_op10_set_mcu_pps_mode,
	.get_mcu_pps_mode = oplus_op10_get_mcu_pps_mode,
	.get_vbat0_volt = oplus_sm8350_read_vbat0_voltage,
#ifdef CONFIG_OPLUS_CHARGER_MTK
	.check_btb_temp = op10_check_btb_temp,
#else
	.check_btb_temp = oplus_sm8350_check_btb_temp,
#endif
	.pps_get_authentiate = oplus_sm8350_pps_get_authentiate,
	.pps_pdo_select = oplus_chg_set_pps_config,
	.get_pps_status = oplus_chg_get_pps_status,
	.get_pps_max_cur = oplus_chg_pps_get_max_cur,

	.pps_cp_hardware_init = oplus_cp_hardware_init,
	.pps_cp_reset = oplus_cp_reset,
	.pps_cp_mode_init = oplus_cp_cfg_mode_init,
	.pps_cp_pmid2vout_enable = oplus_cp_pmid2vout_enable,

	.pps_mos_ctrl = oplus_cp_master_cp_enable,
	.pps_get_cp_master_vbus = oplus_cp_master_get_vbus,
	.pps_get_cp_master_ibus = oplus_cp_master_get_ibus,
	.pps_get_ucp_flag = oplus_cp_master_get_ucp_flag,
	.pps_get_cp_master_vac = oplus_cp_master_get_vac,
	.pps_get_cp_master_vout = oplus_cp_master_get_vout,
	.pps_get_cp_master_tdie = oplus_cp_master_get_tdie,

	.pps_get_cp_slave_vbus = oplus_cp_slave_get_vbus,
	.pps_get_cp_slave_ibus = oplus_cp_slave_get_ibus,
	.pps_mos_slave_ctrl = oplus_cp_slave_cp_enable,
	.pps_get_cp_slave_vac = oplus_cp_slave_get_vac,
	.pps_get_cp_slave_vout = oplus_cp_slave_get_vout,
	.pps_get_cp_slave_tdie = oplus_cp_slave_get_tdie,

	.pps_get_cp_slave_b_vbus = oplus_cp_slave_b_get_vbus,
	.pps_get_cp_slave_b_ibus = oplus_cp_slave_b_get_ibus,
	.pps_mos_slave_b_ctrl = oplus_cp_slave_b_cp_enable,
	.pps_get_cp_slave_b_vac = oplus_cp_slave_b_get_vac,
	.pps_get_cp_slave_b_vout = oplus_cp_slave_b_get_vout,
	.pps_get_cp_slave_b_tdie = oplus_cp_slave_b_get_tdie,
};

int oplus_pps_cp_init(void)
{
	return 0;
}

void oplus_pps_cp_deinit(void)
{
}
