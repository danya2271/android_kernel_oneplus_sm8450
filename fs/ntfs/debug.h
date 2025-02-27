/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * debug.h - NTFS kernel debug support. Part of the Linux-NTFS project.
 *
 * Copyright (c) 2001-2004 Anton Altaparmakov
 */

#ifndef _LINUX_NTFS_DEBUG_H
#define _LINUX_NTFS_DEBUG_H

#include <linux/fs.h>

#include "runlist.h"

#define ntfs_debug(fmt, ...)						\
do {									\
	if (0)								\
		no_printk(fmt, ##__VA_ARGS__);				\
} while (0)

#define ntfs_debug_dump_runlist(rl)	((void)0)

static inline void ntfs_warning(const struct super_block *sb, const char *fmt, ...)
{
}

static inline void ntfs_error(const struct super_block *sb, const char *fmt, ...)
{
}

#endif /* _LINUX_NTFS_DEBUG_H */
