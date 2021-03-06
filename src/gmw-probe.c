/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2015 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gi18n.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>
#include <gudev/gudev.h>
#include <linux/fs.h>
#include <linux/usbdevice_fs.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "gmw-cleanup.h"

#define ONE_BLOCK		0x8000
#define ONE_MB			0x100000
#define ONE_GB			0x40000000

typedef struct {
	guint8			*data_old;
	guint8			*data_random;
	gssize			 bytes_read;
	gssize			 bytes_wrote;
	guint64			 address;
	guint64			 offset;
	gboolean		 valid;
} GmwProbeBlock;

typedef struct {
	guint64			 disk_size;
	int			 fd;
	gchar			*block_dev;
	GPtrArray		*data_save;
	GUdevDevice		*udev_device;
} GmwProbeDevice;

#define GMW_ERROR		1
#define GMW_ERROR_FAILED	0
#define GMW_ERROR_IS_FAKE	1

/**
 * gmw_probe_get_random_data:
 **/
static guint8 *
gmw_probe_get_random_data (guint len)
{
	guint i;
	guint8 *data;

	data = g_new (guint8, len);
	for (i = 0; i < len; i++)
		data[i] = g_random_int_range ('a', 'z');
	return data;
}

/**
 * gmw_probe_block_free:
 **/
static void
gmw_probe_block_free (GmwProbeBlock *item)
{
	g_free (item->data_old);
	g_free (item->data_random);
	g_free (item);
}

/**
 * gmw_probe_device_reset:
 **/
static gboolean
gmw_probe_device_reset (GmwProbeDevice *dev, GError **error)
{
	int fd;
	const gchar *devnode;

	/* just reset device */
	devnode = g_udev_device_get_device_file (dev->udev_device);
	g_debug ("Resetting %s", devnode);
	fd = g_open (devnode, O_WRONLY | O_NONBLOCK);
	if (fd < 0) {
		g_set_error (error,
			     GMW_ERROR,
			     GMW_ERROR_FAILED,
			     "Failed to open %s", devnode);
		return FALSE;
	}
	if (ioctl (fd, USBDEVFS_RESET) != 0) {
		g_set_error (error,
			     GMW_ERROR,
			     GMW_ERROR_FAILED,
			     "Failed to reset device");
		close (fd);
		return FALSE;
	}
	close (fd);
	return TRUE;
}

/**
 * gmw_probe_device_open:
 **/
static gboolean
gmw_probe_device_open (GmwProbeDevice *dev, GError **error)
{
	dev->fd = g_open (dev->block_dev, O_RDWR | O_SYNC);
	if (dev->fd < 0) {
		g_set_error (error,
			     GMW_ERROR,
			     GMW_ERROR_FAILED,
			     "Failed to open %s", dev->block_dev);
		return FALSE;
	}

	/* do not use the OS cache */
	posix_fadvise (dev->fd, 0, 0, POSIX_FADV_DONTNEED |
				      POSIX_FADV_RANDOM |
				      POSIX_FADV_NOREUSE);
	return TRUE;
}

/**
 * gmw_probe_device_free:
 **/
static void
gmw_probe_device_free (GmwProbeDevice *dev)
{
	if (dev->fd > 0)
		close (dev->fd);
	g_free (dev->block_dev);
	g_ptr_array_unref (dev->data_save);
	g_free (dev);
}

/**
 * gmw_probe_device_read:
 **/
static gsize
gmw_probe_device_read (GmwProbeDevice *dev, guint64 addr, guint8 *buf, gssize len)
{
	gsize bytes_read;
	lseek (dev->fd, addr, SEEK_SET);
	bytes_read = read (dev->fd, buf, len);
	g_debug ("read %" G_GSSIZE_FORMAT " @ %liMB", bytes_read, addr / ONE_MB);
	return bytes_read;
}

/**
 * gmw_probe_device_write:
 **/
static gsize
gmw_probe_device_write (GmwProbeDevice *dev, guint64 addr, const guint8 *buf, gssize len)
{
	gsize bytes_written;
	lseek (dev->fd, addr, SEEK_SET);
	bytes_written = write (dev->fd, buf, len);
	g_debug ("wrote %" G_GSSIZE_FORMAT " @ %liMB", bytes_written, addr / ONE_MB);
	return bytes_written;
}

/**
 * gmw_probe_device_data_save:
 **/
static gboolean
gmw_probe_device_data_save (GmwProbeDevice *dev,
			    GCancellable *cancellable,
			    GError **error)
{
	GmwProbeBlock *item;
	guint64 chunk_size;
	guint i;

	/* aim for roughtly the same number of chunks for all device sizes */
	chunk_size = dev->disk_size / 256;
	g_debug ("using chunk size of %" G_GUINT64_FORMAT "MB",
		 chunk_size / ONE_MB);
	for (i = 1; i < 40; i++) {
		item = g_new0 (GmwProbeBlock, 1);
		item->valid = TRUE;
		item->offset = g_random_int_range (1, 0xff);
		item->address = i * chunk_size;
		item->data_old = g_new0 (guint8, ONE_BLOCK);
		if (item->address >= dev->disk_size) {
			gmw_probe_block_free (item);
			break;
		}
		item->data_random = gmw_probe_get_random_data (ONE_BLOCK);
		item->bytes_read = gmw_probe_device_read (dev,
							  item->address +
							  item->offset,
							  item->data_old,
							  ONE_BLOCK);
		g_ptr_array_add (dev->data_save, item);
		if (item->bytes_read != ONE_BLOCK)
			break;
		if (g_cancellable_set_error_if_cancelled (cancellable, error))
			return FALSE;
	}
	return TRUE;
}

/**
 * gmw_probe_device_data_set_dummy:
 **/
static gboolean
gmw_probe_device_data_set_dummy (GmwProbeDevice *dev,
				 GCancellable *cancellable,
				 GError **error)
{
	GmwProbeBlock *item;
	guint i;

	for (i = 0; i < dev->data_save->len; i++) {
		item = g_ptr_array_index (dev->data_save, i);
		item->bytes_wrote = gmw_probe_device_write (dev,
							    item->address +
							    item->offset,
							    item->data_random,
							    ONE_BLOCK);

		if (item->bytes_wrote != ONE_BLOCK)
			break;
		if (g_cancellable_set_error_if_cancelled (cancellable, error))
			return FALSE;
	}

	return TRUE;
}

/**
 * gmw_probe_device_data_verify:
 **/
static gboolean
gmw_probe_device_data_verify (GmwProbeDevice *dev,
			      GCancellable *cancellable,
			      GError **error)
{
	GmwProbeBlock *item;
	guint i;
	guint32 offset;
	_cleanup_free_ guint8 *wbuf2 = NULL;

	wbuf2 = g_new (guint8, ONE_BLOCK + 0xff);
	for (i = 0; i < dev->data_save->len; i++) {
		item = g_ptr_array_index (dev->data_save, i);

		/* use a random offset to confuse drives that are just saving
		 * the address and data in some phantom FAT */
		offset = g_random_int_range (1, 0xff);
		item->bytes_read = gmw_probe_device_read (dev,
							  item->address +
							  item->offset - offset,
							  wbuf2,
							  ONE_BLOCK + offset);
		if (item->bytes_read != ONE_BLOCK + offset) {
			g_set_error (error,
				     GMW_ERROR,
				     GMW_ERROR_FAILED,
				     "Failed to read data");
			return FALSE;
		}
		item->valid = memcmp (item->data_random,
				      wbuf2 + offset,
				      ONE_BLOCK) == 0;
		if (g_cancellable_set_error_if_cancelled (cancellable, error))
			return FALSE;

		/* optimize; we don't need to check any more */
		if (!item->valid)
			break;
	}

	/* if we aborted early, the rest of the drive is junk */
	for (i = i; i < dev->data_save->len; i++) {
		item = g_ptr_array_index (dev->data_save, i);
		item->valid = FALSE;
	}

	return TRUE;
}

/**
 * gmw_probe_device_data_restore:
 **/
static gboolean
gmw_probe_device_data_restore (GmwProbeDevice *dev,
			       GCancellable *cancellable,
			       GError **error)
{
	GmwProbeBlock *item;
	guint i;

	for (i = 0; i < dev->data_save->len; i++) {
		item = g_ptr_array_index (dev->data_save, i);
		if (!item->valid)
			continue;
		item->bytes_wrote = gmw_probe_device_write (dev,
							    item->address +
							    item->offset,
							    item->data_old,
							    ONE_BLOCK);
		if (item->bytes_wrote != ONE_BLOCK)
			break;
		if (g_cancellable_set_error_if_cancelled (cancellable, error))
			return FALSE;
	}

	return TRUE;
}

/**
 * gmw_probe_scan_device:
 **/
static gboolean
gmw_probe_scan_device (GmwProbeDevice *dev, GCancellable *cancellable, GError **error)
{
	GmwProbeBlock *item;
	guint i;

	/* open block device */
	if (!gmw_probe_device_open (dev, error))
		return FALSE;

	/* get reported size */
	if (ioctl (dev->fd, BLKGETSIZE64, &dev->disk_size) != 0) {
		g_set_error (error,
			     GMW_ERROR,
			     GMW_ERROR_FAILED,
			     "Failed to get reported size");
		return FALSE;
	}
	if (dev->disk_size == 0) {
		g_set_error_literal (error,
				     GMW_ERROR,
				     GMW_ERROR_FAILED,
				     "Disk capacity reported as zero");
		return FALSE;
	}
	if (dev->disk_size > 0x4000000000llu) {
		g_set_error (error,
			     GMW_ERROR,
			     GMW_ERROR_FAILED,
			     "Disk capacity reported as invalid: %"
			     G_GUINT64_FORMAT "GB",
			     dev->disk_size / ONE_GB);
		return FALSE;
	}
	g_debug ("Disk reports to be %luMB in size", dev->disk_size / ONE_MB);

	/* save data that's there already */
	if (!gmw_probe_device_data_save (dev, cancellable, error))
		return FALSE;

	/* write 32k or random to every 32Mb */
	if (!gmw_probe_device_data_set_dummy (dev, cancellable, error)) {
		gmw_probe_device_data_restore (dev, cancellable, NULL);
		return FALSE;
	}

	/* sanity check for really broken devices */
	for (i = 0; i < dev->data_save->len; i++) {
		item = g_ptr_array_index (dev->data_save, i);
		if (item->bytes_read != item->bytes_wrote) {
			g_set_error (error,
				     GMW_ERROR,
				     GMW_ERROR_FAILED,
				     "Failed to write len at %liMB",
				     item->address / ONE_MB);
			gmw_probe_device_data_restore (dev, cancellable, NULL);
			return FALSE;
		}
	}

	/* reset device */
	if (!gmw_probe_device_reset (dev, error)) {
		gmw_probe_device_data_restore (dev, cancellable, NULL);
		return FALSE;
	}

	/* wait for block drive to reappear */
	close (dev->fd);
	if (!gmw_probe_device_open (dev, error))
		return FALSE;

	/* read each chunk in again */
	if (!gmw_probe_device_data_verify (dev, cancellable, error)) {
		gmw_probe_device_data_restore (dev, cancellable, NULL);
		return FALSE;
	}

	/* write back original data */
	if (!gmw_probe_device_data_restore (dev, cancellable, error))
		return FALSE;

	/* get results */
	for (i = 0; i < dev->data_save->len; i++) {
		item = g_ptr_array_index (dev->data_save, i);
		if (!item->valid) {
			g_set_error (error,
				     GMW_ERROR,
				     GMW_ERROR_IS_FAKE,
				     "Failed to verify data at %liMB",
				     item->address / ONE_MB);
			return FALSE;
		}
	}
	return TRUE;
}

/**
 * gmw_probe_use_device:
 **/
static gboolean
gmw_probe_use_device (GUdevClient *udev_client,
		      const gchar *block_dev,
		      GCancellable *cancellable,
		      GError **error)
{
	_cleanup_object_unref_ GUdevDevice *udev_device = NULL;
	GmwProbeDevice *dev;

	/* create worker object */
	dev = g_new0 (GmwProbeDevice, 1);
	dev->block_dev = g_strdup (block_dev);
	dev->data_save = g_ptr_array_new_with_free_func ((GDestroyNotify) gmw_probe_block_free);

	/* find udev device */
	udev_device = g_udev_client_query_by_device_file (udev_client, block_dev);
	if (udev_device == NULL) {
		g_set_error (error,
			     GMW_ERROR,
			     GMW_ERROR_FAILED,
			     "Failed to find %s", block_dev);
		gmw_probe_device_free (dev);
		return FALSE;
	}
	dev->udev_device = g_udev_device_get_parent_with_subsystem (udev_device,
								    "usb",
								    "usb_device");
	if (dev->udev_device == NULL) {
		g_set_error_literal (error,
				     GMW_ERROR,
				     GMW_ERROR_FAILED,
				     "Not a USB device");
		gmw_probe_device_free (dev);
		return FALSE;
	}

	/* actually do the scanning now */
	if (!gmw_probe_scan_device (dev, cancellable, error)) {
		gmw_probe_device_free (dev);
		return FALSE;
	}

	/* success */
	gmw_probe_device_free (dev);
	return TRUE;
}

/**
 * gmw_probe_is_block_device_valid:
 **/
static gboolean
gmw_probe_is_block_device_valid (const gchar *block_device)
{
	guint i;

	/* dev prefix */
	if (!g_str_has_prefix (block_device, "/dev/"))
		return FALSE;

	/* has no partition number */
	for (i = 5; block_device[i] != '\0'; i++) {
		if (g_ascii_isdigit (block_device[i]))
			return FALSE;
	}
	return TRUE;
}


/**
 * gmw_probe_is_block_device_mounted:
 **/
static gboolean
gmw_probe_is_block_device_mounted (const gchar *block_device)
{
	_cleanup_free_ gchar *data = NULL;
	if (!g_file_get_contents ("/etc/mtab", &data, NULL, NULL))
		return FALSE;
	return g_strrstr (data, block_device) != NULL;
}

/**
 * main:
 **/
int
main (int argc, char **argv)
{
	GOptionContext *context;
	const gchar *subsystems[] = { "usb", NULL };
	gboolean verbose = FALSE;
	int status = EXIT_SUCCESS;
	_cleanup_object_unref_ GUdevClient *udev_client = NULL;
	_cleanup_error_free_ GError *error = NULL;
	_cleanup_object_unref_ GCancellable *cancellable = NULL;

	const GOptionEntry options[] = {
		{ "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose,
			/* TRANSLATORS: command line option */
			_("Show extra debugging information"), NULL },
		{ NULL}
	};

	/* make this predictable */
	g_random_set_seed (0xdead);

	/* TRANSLATORS: A program to copy the LiveUSB image onto USB hardware */
	context = g_option_context_new (NULL);
	g_option_context_add_main_entries (context, options, NULL);
	if (!g_option_context_parse (context, &argc, &argv, &error)) {
		status = EXIT_FAILURE;
		g_print ("Failed to parse command line: %s\n", error->message);
		goto out;
	}

	if (verbose)
		g_setenv ("G_MESSAGES_DEBUG", "all", TRUE);

	/* valid arguments */
	if (argc != 2 || !gmw_probe_is_block_device_valid (argv[1])) {
		status = EXIT_FAILURE;
		g_print ("Block device required as argument\n");
		goto out;
	}

	/* already mounted */
	if (gmw_probe_is_block_device_mounted (argv[1])) {
		status = EXIT_FAILURE;
		g_print ("Partition mounted from block device\n");
		goto out;
	}

	/* probe device */
	cancellable = g_cancellable_new ();
	udev_client = g_udev_client_new (subsystems);
	if (!gmw_probe_use_device (udev_client, argv[1], cancellable, &error)) {
		status = EXIT_FAILURE;
		if (g_error_matches (error, GMW_ERROR, GMW_ERROR_IS_FAKE)) {
			g_print ("Device is FAKE: %s\n", error->message);
		} else {
			g_print ("Failed to scan device: %s\n", error->message);
		}
		goto out;
	}
	g_print ("Device is GOOD\n");
out:
	g_option_context_free (context);
	return status;
}
