/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2016-2020, The Linux Foundation. All rights reserved.
 */

#ifndef _CAM_REQ_MGR_DEBUG_H_
#define _CAM_REQ_MGR_DEBUG_H_

#include <linux/debugfs.h>
#include "cam_req_mgr_core.h"
#include "cam_debug_util.h"
#ifdef CONFIG_DEBUG
int cam_req_mgr_debug_register(struct cam_req_mgr_core_device *core_dev);
int cam_req_mgr_debug_unregister(void);

/* cam_req_mgr_debug_delay_detect()
 * @brief    : increment debug_fs varaible by 1 whenever delay occurred.
 */
void cam_req_mgr_debug_delay_detect(void);
#else
static inline int cam_req_mgr_debug_register(struct cam_req_mgr_core_device *core_dev) {return 0;}
static inline int cam_req_mgr_debug_unregister(void) {return 0;}

/* cam_req_mgr_debug_delay_detect()
 * @brief    : increment debug_fs varaible by 1 whenever delay occurred.
 */
static inline void cam_req_mgr_debug_delay_detect(void) {}
#endif
#endif
