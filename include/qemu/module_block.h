/* AUTOMATICALLY GENERATED, DO NOT MODIFY */
/*
 * QEMU Block Module Infrastructure
 *
 * Copyright Red Hat, Inc. 2015
 *
 * Authors:
 *  Marc Mari       <markmb@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#ifndef QEMU_MODULE_BLOCK_H
#define QEMU_MODULE_BLOCK_H

#include "qemu-common.h"

static const struct {
    const char *format_name;
    const char *protocol_name;
    const char *library_name;
    bool has_probe;
    bool has_probe_device;
} block_driver_modules[] = {
	{
	.library_name = "curl",
	.format_name = "http",
	.protocol_name = "http",
	},
	{
	.library_name = "curl",
	.format_name = "https",
	.protocol_name = "https",
	},
	{
	.library_name = "curl",
	.format_name = "ftp",
	.protocol_name = "ftp",
	},
	{
	.library_name = "curl",
	.format_name = "ftps",
	.protocol_name = "ftps",
	},
	{
	.library_name = "curl",
	.format_name = "tftp",
	.protocol_name = "tftp",
	},
	{
	.library_name = "dmg",
	.format_name = "dmg",
	.has_probe = true,
	},
};

#endif
