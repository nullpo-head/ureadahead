/* ureadahead
 *
 * trace.c - boot tracing
 *
 * Copyright © 2009 Canonical Ltd.
 * Author: Scott James Remnant <scott@netsplit.com>.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif /* HAVE_CONFIG_H */

#define _ATFILE_SOURCE


#include <sys/select.h>
#include <sys/mount.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/param.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <blkid.h>
#define NO_INLINE_FUNCS
#include <ext2fs.h>

#include <linux/fs.h>
#include <linux/fiemap.h>

#include <nih/macros.h>
#include <nih/alloc.h>
#include <nih/string.h>
#include <nih/list.h>
#include <nih/hash.h>
#include <nih/main.h>
#include <nih/logging.h>
#include <nih/error.h>
#include <tracefs.h>

#include "trace.h"
#include "pack.h"
#include "values.h"
#include "file.h"


/**
 * INODE_GROUP_PRELOAD_THRESHOLD:
 *
 * Number of inodes in a group before we preload that inode's blocks.
 **/
#define INODE_GROUP_PRELOAD_THRESHOLD 8

/**
 * Constants for our hash table implementation.
 **/
#define HASH_BITS 16
#define HASH_SIZE (1 << HASH_BITS) // 16384
#define HASH_MASK (HASH_SIZE - 1)

/**
 * FILEMAP_START_MARK:
 *
 * Sentinel value of a file map range array.
 **/
#define FILEMAP_START_MARK	((unsigned long)-1)

/**
 * PAGE_SHIFT:
 *
 * Shift width of a page size (4096)
 **/
#define PAGE_SHIFT	12

/**
 * FS_SYSTEM
 *
 * "fs" subsystem of the tracefs.
 **/
#define FS_SYSTEM "fs"

/**
 * FILEMAP_SYSTEM
 *
 * "filemap" subsystem of the tracefs.
 **/
#define FILEMAP_SYSTEM	"filemap"

/**
 * EVENTS:
 *
 * TraceFS events to enable.
 *
 **/
static const char *EVENTS[][2] = {
	/* required events for trace to work */
	{FS_SYSTEM, "do_sys_open"},
	{FS_SYSTEM, "open_exec"},
	/* optional events follow */
	{FS_SYSTEM, "uselib"},
	{FILEMAP_SYSTEM, "mm_filemap_fault"},
	{FILEMAP_SYSTEM, "mm_filemap_get_pages"},
	{FILEMAP_SYSTEM, "mm_filemap_map_pages"}};

#define NR_REQUIRED_EVENTS 2
#define NR_EVENTS (sizeof (EVENTS) / sizeof (EVENTS[0]))

/* A half-open file range */
struct file_map {
	/* inclusive */
	off_t				start;
	/* exclusive */
	off_t				end;
};

/* Holds file maps of a file */
struct inode_data {
	struct inode_data		*next;
	unsigned long			inode;
	char				*dev_name;
	/* a sorted array of file maps */
	struct file_map			*map;
	int				nr_maps;
	char				*name;
};

/* Holds a list of inode_data for a device */
struct device_data {
	struct device_data		*next;
	char				*name;
	dev_t				id;
	int				nr_inodes;
	/* a sorted array of inodes */
	struct inode_data		*inodes;
};

/* tep types common to filemap events */
struct filemap_tep {
	struct tep_event	 *event;
	struct tep_format_field  *inode;
	struct tep_format_field  *device;
	struct tep_format_field  *index;
	struct tep_format_field  *last_index; /* May be NULL */
};


/* Prototypes for static functions */
static int       read_trace          (const void *parent,
				      const char *path_prefix_filter,
				      const PathPrefixOption *path_prefix,
				      PackFile **files, size_t *num_files,
				      int force_ssd_mode);
static void      remove_untouched_blocks  (const void *parent,
					  struct device_data **device_hash,
					  PackFile *file);
static int       read_trace_cb       (struct tep_event *event, struct tep_record *record,
				      int cpu, void *read_trace_context);
static int       read_path_trace     (struct tep_event *event, struct tep_record *record,
				      const void *parent,
				      const char *path_prefix_filter,
				      const PathPrefixOption *path_prefix,
				      PackFile **files, size_t *num_files,
				      int force_ssd_mode);
static int       read_filemap_trace  (struct tep_event *event,
				      struct tep_record *record,
				      struct filemap_tep *fault,
				      struct filemap_tep *get_pages,
				      struct filemap_tep *map_pages,
				      struct device_data **device_hash);
static void      trace_add_file_map  (struct device_data **device_hash,
				      dev_t dev, unsigned long ino,
				      off_t index, off_t last_index);
static void      fix_path            (char *pathname);
static int       trace_add_path      (const void *parent, const char *pathname,
				      PackFile **files, size_t *num_files,
				      int force_ssd_mode);
static int       ignore_path         (const char *pathname);
static PackFile *trace_file          (const void *parent, dev_t dev,
				      PackFile **files, size_t *num_files,
				      int force_ssd_mode);
static int       trace_add_chunks    (const void *parent,
				      PackFile *file, PackPath *path,
				      int fd, off_t size);
static int       trace_add_extents   (const void *parent,
				      PackFile *file, PackPath *path,
				      int fd, off_t size,
				      off_t offset, off_t length);
static int       trace_add_groups    (const void *parent, PackFile *file);
static int       trace_sort_blocks   (const void *parent, PackFile *file);
static int       trace_sort_paths    (const void *parent, PackFile *file);
static void                  add_map            (struct inode_data *inode,
						 off_t index, off_t last_index);
static int		     cmp_file_map_range (const void *A, const void *B);
static int                   cmp_file_map       (const void *A, const void *B);
static struct inode_data    *add_inode          (struct device_data *dev, unsigned long ino);
static struct inode_data    *find_inode         (struct device_data *dev, unsigned long ino);
static int                   cmp_inodes_range   (const void *A, const void *B);
static int                   cmp_inodes         (const void *A, const void *B);
static struct device_data   *add_device         (struct device_data **device_hash, dev_t dev);
static struct device_data   *find_device        (struct device_data **device_hash, dev_t dev);


static void
sig_interrupt (int signum)
{
}

int
trace (int daemonise,
       int timeout,
       const char *filename_to_replace,
       const char *pack_file,
       const char *path_prefix_filter,
       const PathPrefixOption *path_prefix,
       int use_existing_trace_events,
       int force_ssd_mode)
{
	int                 old_events_enabled[NR_EVENTS] = {};
	int                 old_tracing_enabled = 0;
	int                 old_buffer_size_kb = 0;
	struct sigaction    act;
	struct sigaction    old_sigterm;
	struct sigaction    old_sigint;
	struct timeval      tv;
	nih_local PackFile *files = NULL;
	size_t              num_files = 0;

	if (! use_existing_trace_events) {
		for (int i = 0; i < NR_EVENTS; i++) {
			int ret;
			enum tracefs_enable_state old_state = tracefs_event_is_enabled (NULL, EVENTS[i][0], EVENTS[i][1]);
			old_events_enabled[i] = (old_state == TRACEFS_ALL_ENABLED || old_state == TRACEFS_SOME_ENABLED);
			ret = tracefs_event_enable (NULL, EVENTS[i][0], EVENTS[i][1]);
			if (ret < 0) {
				if (i < NR_REQUIRED_EVENTS) {
					nih_error ("Failed to enable %s", EVENTS[i][1]);
					nih_error_raise_system ();
					return -1;
				}
				nih_debug ("Missing %s tracing: %d", EVENTS[i][1], ret);
			}
		}
	}
	/* cpu 0 to get the size per core, assuming all cpus have the same size */
	if ((old_buffer_size_kb = tracefs_instance_get_buffer_size (NULL, 0)) < 0) {
		nih_error ("Failed to get the buffer size");
		nih_error_raise_system ();
		return -1;
	}
	if (tracefs_instance_set_buffer_size (NULL, 8192, -1) < 0) {
		nih_error ("Failed to set the buffer size");
		nih_error_raise_system ();
		return -1;
	}
	if ((old_tracing_enabled = tracefs_trace_is_on (NULL)) < 0) {
		nih_error ("Failed to get if the trace is on");
		nih_error_raise_system ();
		return -1;
	}
	if (tracefs_trace_on (NULL) < 0) {
		nih_error ("Failed to set the trace on");
		nih_error_raise_system ();
		return -1;
	}

	if (daemonise) {
		pid_t pid;

		pid = fork ();
		if (pid < 0) {
			nih_error_raise_system ();
			return -1;
		} else if (pid > 0) {
			_exit (0);
		}
	}

	/* Sleep until we get signals */
	act.sa_handler = sig_interrupt;
	sigemptyset (&act.sa_mask);
	act.sa_flags = 0;

	sigaction (SIGTERM, &act, &old_sigterm);
	sigaction (SIGINT, &act, &old_sigint);

	if (timeout) {
		tv.tv_sec = timeout;
		tv.tv_usec = 0;

		select (0, NULL, NULL, NULL, &tv);
	} else {
		pause ();
	}

	sigaction (SIGTERM, &old_sigterm, NULL);
	sigaction (SIGINT, &old_sigint, NULL);

	/* Restore previous tracing settings */
	if (old_tracing_enabled == 0 && tracefs_trace_off (NULL) < 0) {
		nih_error_raise_system ();
		return -1;
	}
	if (! use_existing_trace_events) {
		for (int i = 0; i < NR_EVENTS; i++) {
			if (old_events_enabled[i] > 0)
				continue;
			tracefs_event_disable (NULL,
					       EVENTS[i][0], EVENTS[i][1]);
		}
	}

	/* Be nicer */
	if (nice (15))
		;

	/* Read trace log */
	if (read_trace (NULL, path_prefix_filter, path_prefix,
			&files, &num_files, force_ssd_mode) < 0)
		return -1;

	/*
	 * Restore the trace buffer size (which has just been read) and free
	 * a bunch of memory.
	 */
	if (tracefs_instance_set_buffer_size (NULL, old_buffer_size_kb, -1) < 0) {
		nih_error ("Failed to restore the buffer size");
		nih_error_raise_system ();
		return -1;
	}

	/* Write out pack files */
	for (size_t i = 0; i < num_files; i++) {
		nih_local char *filename = NULL;
		if (pack_file) {
			filename = NIH_MUST (nih_strdup (NULL, pack_file));
		} else {
			filename = pack_file_name_for_device (NULL,
							      files[i].dev);
			if (! filename) {
				NihError *err;

				err = nih_error_get ();
				nih_warn ("%s", err->message);
				nih_free (err);

				continue;
			}

			/* If filename_to_replace is not NULL, only write out
			 * the file and skip others.
			 */
			if (filename_to_replace &&
			    strcmp (filename_to_replace, filename)) {
				nih_info ("Skipping %s", filename);
				continue;
			}
		}
		nih_info ("Writing %s", filename);

		/* We only need to apply additional sorting to the
		 * HDD-optimised packs, the SSD ones can read in random
		 * order quite happily.
		 *
		 * Also for HDD, generate the inode group preloading
		 * array.
		 */
		if (files[i].rotational) {
			trace_add_groups (files, &files[i]);

			trace_sort_blocks (files, &files[i]);
			trace_sort_paths (files, &files[i]);
		}

		write_pack (filename, &files[i]);

		if (nih_log_priority < NIH_LOG_MESSAGE)
			pack_dump (&files[i], SORT_OPEN);
	}

	return 0;
}

/* data type for the tracefs_iterate_raw_events callback  */
struct read_trace_context {
	const void		 *parent;

	struct tep_event	 *do_sys_open;
	struct tep_event	 *open_exec;
	struct tep_event	 *uselib;
	struct filemap_tep	 filemap_fault;
	struct filemap_tep	 filemap_get_pages;
	struct filemap_tep	 filemap_map_pages;

	const char		 *path_prefix_filter;
	const PathPrefixOption   *path_prefix;
	PackFile		 **files;
	size_t			 *num_files;
	int			 force_ssd_mode;

	struct device_data       *device_hash[HASH_SIZE];
};

static void
init_filemap_tep (struct tep_handle *tep, const char *event_name,
		  struct filemap_tep *filemap)
{
	filemap->event = tep_find_event_by_name (tep, NULL, event_name);
	if (! filemap->event)
		return;
	filemap->inode = tep_find_field (filemap->event, "i_ino");
	nih_assert (filemap->inode != NULL);
	filemap->device = tep_find_field (filemap->event, "s_dev");
	nih_assert (filemap->device != NULL);
	filemap->index = tep_find_field (filemap->event, "index");
	nih_assert (filemap->index != NULL);
	filemap->last_index = tep_find_field (filemap->event, "last_index");
	/* last_index can be NULL since fault does not have it */
}

static void free_device (struct device_data *dev)
{
	struct inode_data *inode;
	int i;

	for (i = 0; i < dev->nr_inodes; i++) {
		inode = &dev->inodes[i];
		/* inode->map has one meta data element at the start */
		inode->map--;
		printf("taakyas: free inode: %ld, map: %p\n", inode->inode, inode->map);
		free (inode->map);
		free (inode->name);
	}
	/* dev->inodes has one meta data element at the start */
	dev->inodes--;
	free (dev->inodes);
	free (dev->name);
	free (dev);
}

static void free_device_hash (struct device_data **device_hash)
{
	struct device_data *dev;
	int i;

	for (i = 0; i < HASH_SIZE; i++) {
		while (device_hash[i]) {
			dev = device_hash[i];
			device_hash[i] = dev->next;
			free_device (dev);
		}
	}
}

static int
read_trace (const void *parent,
	    const char *path_prefix_filter,
	    const PathPrefixOption *path_prefix,
	    PackFile **files, size_t *num_files, int force_ssd_mode)
{
	int err = 0;
	const char *systems[] = { FS_SYSTEM, FILEMAP_SYSTEM, NULL };
	struct tep_handle *tep;
	struct read_trace_context context;

	nih_assert (path_prefix != NULL);
	nih_assert (files != NULL);
	nih_assert (num_files != NULL);

	tep = tracefs_local_events_system (NULL, systems);
	if (!tep)
		nih_return_system_error (-1);

	context.parent = parent;

	context.do_sys_open = NULL; //tep_find_event_by_name (tep, FS_SYSTEM, "do_sys_open");
	context.open_exec = NULL; //tep_find_event_by_name (tep, FS_SYSTEM, "open_exec");
	context.uselib = NULL; // tep_find_event_by_name (tep, FS_SYSTEM, "uselib");
	init_filemap_tep (tep, "mm_filemap_fault", &context.filemap_fault);
	init_filemap_tep (tep, "mm_filemap_map_pages", &context.filemap_map_pages);
	init_filemap_tep (tep, "mm_filemap_get_pages", &context.filemap_get_pages);

	context.path_prefix_filter = path_prefix_filter;
	context.path_prefix = path_prefix;
	context.files = files;
	context.num_files = num_files;
	context.force_ssd_mode = force_ssd_mode;

	memset (context.device_hash, 0, sizeof(context.device_hash));

	if (tracefs_iterate_raw_events (tep, NULL, NULL, 0, read_trace_cb, &context) < 0) {
		nih_return_system_error (-1);
		err = -1;
		goto out;
	}

	/* Remove blocks no process touched if we have these events */
	if (context.filemap_fault.event != NULL &&
	    context.filemap_map_pages.event != NULL &&
	    context.filemap_get_pages.event != NULL) {
		for (int i = 0; i < *num_files; i++) {
			remove_untouched_blocks (*files, context.device_hash, &(*files)[i]);
		}
	}

out:
	tep_free (tep);
	free_device_hash (context.device_hash);
	return err;
}

static void
remove_untouched_blocks  (const void *parent,
			  struct device_data **device_hash,
			  PackFile *file)
{
	PackBlock *reduced_blocks = NULL;
	size_t num_blocks = 0;

	struct file_map *maps;
	size_t nr_maps = 0;
	size_t filemapidx = 0;
	size_t pathidx = -1;

	/* Iterate both blocks and filemaps in order to get the intersection of them
	 * to drop file ranges that no process read */
	for (int blockidx = 0; blockidx < file->num_blocks; blockidx++) {
		PackBlock *block = &file->blocks[blockidx];
		struct file_map block_range = {
			block->offset >> PAGE_SHIFT,
			(block->offset + block->length) >> PAGE_SHIFT};

		/* Prepare the sorted filemap ranges for the next file */
		if (block->pathidx != pathidx) {
			struct device_data *dev = NULL;
			struct inode_data *inode = NULL;

			pathidx = block->pathidx;
			filemapidx = 0;

			dev = find_device (device_hash, file->dev);
			if (dev)
				inode = find_inode (dev, file->paths[pathidx].ino);

			if (! dev || ! inode) {
				/* A file was opened but not read. We only want dentry. */
				reduced_blocks = NIH_MUST (nih_realloc (reduced_blocks,
					parent, sizeof(PackBlock) * (++num_blocks)));
				reduced_blocks[num_blocks - 1].pathidx = block->pathidx;
				reduced_blocks[num_blocks - 1].offset = 0;
				reduced_blocks[num_blocks - 1].length = 0;
				reduced_blocks[num_blocks - 1].physical = 0;

				nr_maps = 0;
				/* The following loops will skip remaining blocks of this path */
				continue;
			}
			maps = inode->map;
			nr_maps = inode->nr_maps;

			for (int k = 0; k < inode->nr_maps; k++) {
				printf("final map: ino: %lu, i: %d index: %ld, last_index: %ld\n", inode->inode, k, inode->map[k].start, inode->map[k].end);
			}
		}

		/* skip filemaps until we find an overlap with the blocks */
		while (filemapidx < nr_maps &&
			cmp_file_map (&maps[filemapidx], &block_range) < 0)
				filemapidx++;

		/* Add blocks while they overlap with the accessed ranges */
		for (;;) {
			loff_t new_offset, new_end, new_length, new_physical;

			if (filemapidx >= nr_maps)
				break;

			struct file_map *range = &maps[filemapidx];

			if (cmp_file_map (range, &block_range) > 0)
				break;

			new_offset = nih_max (range->start << PAGE_SHIFT, block->offset);
			new_end = nih_min (range->end << PAGE_SHIFT, block->offset + block->length);
			new_length = new_end - new_offset;
			new_physical = block->physical + new_offset - block->offset;

			/* new_length is zero when they touch each other. */
			if (new_length > 0) {
				printf("takayas: from pack [%lu, %lu (len: %lu)] to [%lu, %lu (%lu)] due to [%ld, %ld])\n", block->offset, block->offset + block->length, block->length, new_offset, new_end, new_length, range->start << PAGE_SHIFT, range->end << PAGE_SHIFT);
				reduced_blocks = NIH_MUST (nih_realloc (reduced_blocks,
					parent, sizeof(PackBlock) * (++num_blocks)));
				reduced_blocks[num_blocks - 1].pathidx = block->pathidx;
				reduced_blocks[num_blocks - 1].offset = new_offset;
				reduced_blocks[num_blocks - 1].length = new_length;
				reduced_blocks[num_blocks - 1].physical = new_physical;
			}

			else {
				printf("takayas: skip pack [%lu, %lu (len: %lu)] to [%lu, %lu (%lu)] due to [%ld, %ld])\n", block->offset, block->offset + block->length, block->length, new_offset, new_end, new_length, range->start << PAGE_SHIFT, range->end << PAGE_SHIFT);
			}

			/* Next block still can overlap with this range. Next blockidx loop. */
			if (range->end > block_range.end)
				break;
			/* Otherwise, see if the next filemap is still in this block. */
			filemapidx++;
		}
	}

	if (file->blocks) {
		nih_free (file->blocks);
	}
	file->blocks = reduced_blocks;
	file->num_blocks = num_blocks;
}

static int
read_trace_cb  (struct tep_event *event,
		struct tep_record *record,
		int cpu, void *read_trace_context)
{
	struct read_trace_context *context = read_trace_context;

	if ((event->id == context->do_sys_open->id) ||
	    (event->id == context->open_exec->id) ||
	    (context->uselib && event->id == context->uselib->id))
		return read_path_trace (event, record, context->parent,
					context->path_prefix_filter,
					context->path_prefix,
					context->files, context->num_files,
					context->force_ssd_mode);

	if ((context->filemap_fault.event && event->id == context->filemap_fault.event->id) ||
	    (context->filemap_get_pages.event && event->id == context->filemap_get_pages.event->id) ||
	    (context->filemap_map_pages.event && event->id == context->filemap_map_pages.event->id))
		return read_filemap_trace (event, record,
					   &context->filemap_fault,
					   &context->filemap_get_pages,
					   &context->filemap_map_pages,
					   context->device_hash);

	return 0;
}

static int
read_path_trace  (struct tep_event *event, struct tep_record *record,
		  const void *parent,
		  const char *path_prefix_filter,
		  const PathPrefixOption *path_prefix,
		  PackFile **files, size_t *num_files,
		  int force_ssd_mode)
{
	char *path, *tep_path = NULL;
	int   len;

	tep_path = tep_get_field_raw(NULL, event, "filename", record, &len, 0);
	if (! tep_path) {
		nih_warn ("Field 'filename' not found for event %s", event->name);
		return 0;
	}

	path = strndup(tep_path, len);
	if (! path)
		nih_return_system_error(-1);

	fix_path (path);

	if (path_prefix_filter &&
		strncmp (path, path_prefix_filter,
				strlen (path_prefix_filter))) {
		nih_warn ("Skipping %s due to path prefix filter", path);
		goto out;
	}

	if (path_prefix->st_dev != NODEV && path[0] == '/') {
		struct stat stbuf;
		char *rewritten;
		asprintf (&rewritten,
			  "%s%s", path_prefix->prefix, path);
		if (! lstat (rewritten, &stbuf) &&
			stbuf.st_dev == path_prefix->st_dev) {
				/* If |rewritten| exists on the same device as
				 * path_prefix->st_dev, record the rewritten one
				 * instead of the original path.
				 */
			free (path);
			path = rewritten;
		}
	}
	trace_add_path (parent, path, files, num_files, force_ssd_mode);

out:
	free (path);

	return 0;
}

static void
fix_path (char *pathname)
{
	char *ptr;

	nih_assert (pathname != NULL);

	for (ptr = pathname; *ptr; ptr++) {
		size_t len;

		if (ptr[0] != '/')
			continue;

		len = strcspn (ptr + 1, "/");

		/* // and /./, we shorten the string and repeat the loop
		 * looking at the new /
		 */
		if ((len == 0) || ((len == 1) && ptr[1] == '.')) {
			memmove (ptr, ptr + len + 1, strlen (ptr) - len);
			ptr--;
			continue;
		}

		/* /../, we shorten back to the previous / or the start
		 * of the string and repeat the loop looking at the new /
		 */
		if ((len == 2) && (ptr[1] == '.') && (ptr[2] == '.')) {
			char *root;

			for (root = ptr - 1;
			     (root >= pathname) && (root[0] != '/');
			     root--)
				;
			if (root < pathname)
				root = pathname;

			memmove (root, ptr + len + 1, strlen (ptr) - len);
			ptr = root - 1;
			continue;
		}
	}

	while ((ptr != pathname) && (*(--ptr) == '/'))
		*ptr = '\0';
}


static int
trace_add_path (const void *parent,
		const char *pathname,
		PackFile ** files,
		size_t *    num_files,
		int         force_ssd_mode)
{
	static NihHash *path_hash = NULL;
	struct stat     statbuf;
	int             fd;
	PackFile *      file;
	PackPath *      path;
	static NihHash *inode_hash = NULL;
	nih_local char *inode_key = NULL;

	nih_assert (pathname != NULL);
	nih_assert (files != NULL);
	nih_assert (num_files != NULL);

	/* We can't really deal with relative paths since we don't know
	 * the working directory that they were opened from.
	 */
	if (pathname[0] != '/') {
		nih_warn ("%s: %s", pathname, _("Ignored relative path"));
		return 0;
	}

	/* Certain paths aren't worth caching, because they're virtual or
	 * temporary filesystems and would waste pack space.
	 */
	if (ignore_path (pathname))
		return 0;

	/* Ignore paths that won't fit in the pack; we could use PATH_MAX,
	 * but with 1000 files that'd be 4M just for the
	 * pack.
	 */
	if (strlen (pathname) > PACK_PATH_MAX) {
		nih_warn ("%s: %s", pathname, _("Ignored far too long path"));
		return 0;
	}

	/* Use a hash table of paths to eliminate duplicate path names from
	 * the table since that would waste pack space (and fds).
	 */
	if (! path_hash)
		path_hash = NIH_MUST (nih_hash_string_new (NULL, 2500));

	if (nih_hash_lookup (path_hash, pathname)) {
		return 0;
	} else {
		NihListEntry *entry;

		entry = NIH_MUST (nih_list_entry_new (path_hash));
		entry->str = NIH_MUST (nih_strdup (entry, pathname));

		nih_hash_add (path_hash, &entry->entry);
	}

	/* Make sure that we have an ordinary file
	 * This avoids us opening a fifo or socket or symlink.
	 */
	if ((lstat (pathname, &statbuf) < 0)
	    || (S_ISLNK (statbuf.st_mode))
	    || (! S_ISREG (statbuf.st_mode)))
		return 0;

	/* Open and stat again to get the genuine details, in case it
	 * changes under us.
	 */
	fd = open (pathname, O_RDONLY | O_NOATIME);
	if (fd < 0) {
		nih_warn ("%s: %s: %s", pathname,
			  _("File vanished or error reading"),
			  strerror (errno));
		return -1;
	}

	if (fstat (fd, &statbuf) < 0) {
		nih_warn ("%s: %s: %s", pathname,
			  _("Error retrieving file stat"),
			  strerror (errno));
		close (fd);
		return -1;
	}

	/* Double-check that it's really still a file */
	if (! S_ISREG (statbuf.st_mode)) {
		close (fd);
		return 0;
	}

	/* Some people think it's clever to split their filesystem across
	 * multiple devices, so we need to generate a different pack file
	 * for each device.
	 *
	 * Lookup file based on the dev_t, potentially creating a new
	 * pack file in the array.
	 */
	file = trace_file (parent, statbuf.st_dev, files, num_files, force_ssd_mode);

	/* Grow the PackPath array and fill in the details for the new
	 * path.
	 */
	file->paths = NIH_MUST (nih_realloc (file->paths, *files,
					     (sizeof (PackPath)
					      * (file->num_paths + 1))));

	path = &file->paths[file->num_paths++];
	memset (path, 0, sizeof (PackPath));

	path->group = -1;
	path->ino = statbuf.st_ino;

	strncpy (path->path, pathname, PACK_PATH_MAX);
	path->path[PACK_PATH_MAX] = '\0';

	/* The paths array contains each unique path opened, but these
	 * might be symbolic or hard links to the same underlying files
	 * and we don't want to read the same block more than once.
	 *
	 * Use a hash table of dev_t/ino_t pairs to make sure we only
	 * read the blocks of an actual file the first time.
	 */
	if (! inode_hash)
		inode_hash = NIH_MUST (nih_hash_string_new (NULL, 2500));

	inode_key = NIH_MUST (nih_sprintf (NULL, "%llu:%llu",
					   (unsigned long long)statbuf.st_dev,
					   (unsigned long long)statbuf.st_ino));

	if (nih_hash_lookup (inode_hash, inode_key)) {
		close (fd);
		return 0;
	} else {
		NihListEntry *entry;

		entry = NIH_MUST (nih_list_entry_new (inode_hash));
		entry->str = inode_key;
		nih_ref (entry->str, entry);

		nih_hash_add (inode_hash, &entry->entry);
	}

	/* There's also no point reading zero byte files, since they
	 * won't have any blocks (and we can't mmap zero bytes anyway).
	 */
	if (! statbuf.st_size) {
		close (fd);
		return 0;
	}

	/* Now read the in-memory chunks of this file and add those to
	 * the pack file too.
	 */
	trace_add_chunks (*files, file, path, fd, statbuf.st_size);
	close (fd);

	return 0;
}

static int
ignore_path (const char *pathname)
{
	nih_assert (pathname != NULL);

	if (! strncmp (pathname, "/proc/", 6))
		return TRUE;
	if (! strncmp (pathname, "/sys/", 5))
		return TRUE;
	if (! strncmp (pathname, "/dev/", 5))
		return TRUE;
	if (! strncmp (pathname, "/tmp/", 5))
		return TRUE;
	if (! strncmp (pathname, "/run/", 5))
		return TRUE;
	if (! strncmp (pathname, "/var/run/", 9))
		return TRUE;
	if (! strncmp (pathname, "/var/log/", 9))
		return TRUE;
	if (! strncmp (pathname, "/var/lock/", 10))
		return TRUE;

	return FALSE;
}


static PackFile *
trace_file (const void *parent,
	    dev_t       dev,
	    PackFile ** files,
	    size_t *    num_files,
	    int         force_ssd_mode)
{
	nih_local char *filename = NULL;
	int             rotational;
	PackFile *      file;

	nih_assert (files != NULL);
	nih_assert (num_files != NULL);

	/* Return any existing file structure for this device */
	for (size_t i = 0; i < *num_files; i++)
		if ((*files)[i].dev == dev)
			return &(*files)[i];

	if (force_ssd_mode) {
		rotational = FALSE;
	} else {
		/* Query sysfs to see whether this disk is rotational; this
		 * obviously won't work for virtual devices and the like, so
		 * default to TRUE for now.
		 */
		filename = NIH_MUST (nih_sprintf (NULL, "/sys/dev/block/%d:%d/queue/rotational",
						major (dev), minor (dev)));
		if (access (filename, R_OK) < 0) {
			/* For devices managed by the scsi stack, the minor device number has to be
			 * masked to find the queue/rotational file.
			 */
			nih_free (filename);
			filename = NIH_MUST (nih_sprintf (NULL, "/sys/dev/block/%d:%d/queue/rotational",
							major (dev), minor (dev) & 0xffff0));
		}

		if (get_value (AT_FDCWD, filename, &rotational) < 0) {
			NihError *err;

			err = nih_error_get ();
			nih_warn (_("Unable to obtain rotationalness for device %u:%u: %s"),
				major (dev), minor (dev), err->message);
			nih_free (err);

			rotational = TRUE;
		}
	}

	/* Grow the PackFile array and fill in the details for the new
	 * file.
	 */
	printf("realloc; *files: %p, num_files: %p\n", *files, num_files);
	printf("realloc; *files: %p, *num_files: %d\n", *files, *num_files);
	*files = NIH_MUST (nih_realloc (*files, parent,
					(sizeof (PackFile) * (*num_files + 1))));

	file = &(*files)[(*num_files)++];
	memset (file, 0, sizeof (PackFile));

	file->dev = dev;
	file->rotational = rotational;
	file->num_paths = 0;
	file->paths = NULL;
	file->num_blocks = 0;
	file->blocks = NULL;

	return file;
}


static int
trace_add_chunks (const void *parent,
		  PackFile *  file,
		  PackPath *  path,
		  int         fd,
		  off_t       size)
{
	static int               page_size = -1;
	void *                   buf;
	off_t                    num_pages;
	nih_local unsigned char *vec = NULL;

	nih_assert (file != NULL);
	nih_assert (path != NULL);
	nih_assert (fd >= 0);
	nih_assert (size > 0);

	if (page_size < 0)
		page_size = sysconf (_SC_PAGESIZE);

	/* Map the file into memory */
	buf = mmap (NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	if (buf == MAP_FAILED) {
		nih_warn ("%s: %s: %s", path->path,
			  _("Error mapping into memory"),
			  strerror (errno));
		return -1;
	}

	/* Grab the core memory map of the file */
	num_pages = (size - 1) / page_size + 1;
	vec = NIH_MUST (nih_alloc (NULL, num_pages));
	memset (vec, 0, num_pages);

	if (mincore (buf, size, vec) < 0) {
		nih_warn ("%s: %s: %s", path->path,
			  _("Error retrieving page cache info"),
			  strerror (errno));
		munmap (buf, size);
		return -1;
	}

	/* Clean up */
	if (munmap (buf, size) < 0) {
		nih_warn ("%s: %s: %s", path->path,
			  _("Error unmapping from memory"),
			  strerror (errno));
		return -1;
	}


	/* Now we can figure out which contiguous bits of the file are
	 * in core memory.
	 */
	for (off_t i = 0; i < num_pages; i++) {
		off_t offset;
		off_t length;

		if (! vec[i])
			continue;

		offset = i * page_size;
		length = page_size;

		while (((i + 1) < num_pages) && vec[i + 1]) {
			length += page_size;
			i++;
		}

		/* The rotational crowd need this split down further into
		 * on-disk extents, the non-rotational folks can just use
		 * the chunks data.
		 */
		if (file->rotational) {
			trace_add_extents (parent, file, path, fd, size,
					   offset, length);
		} else {
			PackBlock *block;

			file->blocks = NIH_MUST (nih_realloc (file->blocks, parent,
							      (sizeof (PackBlock)
							       * (file->num_blocks + 1))));

			block = &file->blocks[file->num_blocks++];
			memset (block, 0, sizeof (PackBlock));

			block->pathidx = file->num_paths - 1;
			block->offset = offset;
			block->length = length;
			block->physical = -1;
		}
	}

	return 0;
}

struct fiemap *
get_fiemap (const void *parent,
	    int         fd,
	    off_t       offset,
	    off_t       length)
{
	struct fiemap *fiemap;

	nih_assert (fd >= 0);

	fiemap = NIH_MUST (nih_new (parent, struct fiemap));
	memset (fiemap, 0, sizeof (struct fiemap));

	fiemap->fm_start = offset;
	fiemap->fm_length = length;
	fiemap->fm_flags = 0;

	do {
		/* Query the current number of extents */
		fiemap->fm_mapped_extents = 0;
		fiemap->fm_extent_count = 0;

		if (ioctl (fd, FS_IOC_FIEMAP, fiemap) < 0) {
			nih_error_raise_system ();
			nih_free (fiemap);
			return NULL;
		}

		/* Always allow room for one extra over what we were told,
		 * so we know if they changed under us.
		 */
		fiemap = NIH_MUST (nih_realloc (fiemap, parent,
						(sizeof (struct fiemap)
						 + (sizeof (struct fiemap_extent)
						    * (fiemap->fm_mapped_extents + 1)))));
		fiemap->fm_extent_count = fiemap->fm_mapped_extents + 1;
		fiemap->fm_mapped_extents = 0;

		memset (fiemap->fm_extents, 0, (sizeof (struct fiemap_extent)
						* fiemap->fm_extent_count));

		if (ioctl (fd, FS_IOC_FIEMAP, fiemap) < 0) {
			nih_error_raise_system ();
			nih_free (fiemap);
			return NULL;
		}
	} while (fiemap->fm_mapped_extents
		 && (fiemap->fm_mapped_extents >= fiemap->fm_extent_count));

	return fiemap;
}

static int
trace_add_extents (const void *parent,
		   PackFile *  file,
		   PackPath *  path,
		   int         fd,
		   off_t       size,
		   off_t       offset,
		   off_t       length)
{
	nih_local struct fiemap *fiemap = NULL;

	nih_assert (file != NULL);
	nih_assert (path != NULL);
	nih_assert (fd >= 0);
	nih_assert (size > 0);

	/* Get the extents map for this chunk, then iterate the extents
	 * and put those in the pack instead of the chunks.
	 */
	fiemap = get_fiemap (NULL, fd, offset, length);
	if (! fiemap) {
		NihError *err;

		err = nih_error_get ();
		nih_warn ("%s: %s: %s", path->path,
			  _("Error retrieving chunk extents"),
			  err->message);
		nih_free (err);

		return -1;
	}

	for (__u32 j = 0; j < fiemap->fm_mapped_extents; j++) {
		PackBlock *block;
		off_t      start;
		off_t      end;

		if (fiemap->fm_extents[j].fe_flags & FIEMAP_EXTENT_UNKNOWN)
			continue;

		/* Work out the intersection of the chunk and extent */
		start = nih_max (fiemap->fm_start,
				 fiemap->fm_extents[j].fe_logical);
		end = nih_min ((fiemap->fm_start + fiemap->fm_length),
			       (fiemap->fm_extents[j].fe_logical
				+ fiemap->fm_extents[j].fe_length));

		/* Grow the blocks array to add the extent */
		file->blocks = NIH_MUST (nih_realloc (file->blocks, parent,
						      (sizeof (PackBlock)
						       * (file->num_blocks + 1))));

		block = &file->blocks[file->num_blocks++];
		memset (block, 0, sizeof (PackBlock));

		block->pathidx = file->num_paths - 1;
		block->offset = start;
		block->length = end - start;
		block->physical = (fiemap->fm_extents[j].fe_physical
				   + (start - fiemap->fm_extents[j].fe_logical));
	}

	return 0;
}

static int
trace_add_groups (const void *parent,
		  PackFile *  file)
{
	const char *devname;
	ext2_filsys fs = NULL;

	nih_assert (file != NULL);

	devname = blkid_devno_to_devname (file->dev);
	if (devname
	    && (! ext2fs_open (devname, 0, 0, 0, unix_io_manager, &fs))) {
		nih_assert (fs != NULL);
		size_t            num_groups = 0;
		nih_local size_t *num_inodes = NULL;
		size_t            mean = 0;
		size_t            hits = 0;

		nih_assert (fs != NULL);

		/* Calculate the number of inode groups on this filesystem */
		num_groups = ((fs->super->s_blocks_count - 1)
			      / fs->super->s_blocks_per_group) + 1;

		/* Fill in the pack path's group member, and count the
		 * number of inodes in each group.
		 */
		num_inodes = NIH_MUST (nih_alloc (NULL, (sizeof (size_t)
							 * num_groups)));
		memset (num_inodes, 0, sizeof (size_t) * num_groups);

		for (size_t i = 0; i < file->num_paths; i++) {
			file->paths[i].group = ext2fs_group_of_ino (fs, file->paths[i].ino);
			num_inodes[file->paths[i].group]++;
		}

		/* Iterate the groups and add any group that exceeds the
		 * inode preload threshold.
		 */
		for (size_t i = 0; i < num_groups; i++) {
			mean += num_inodes[i];
			if (num_inodes[i] > INODE_GROUP_PRELOAD_THRESHOLD) {
				file->groups = NIH_MUST (nih_realloc (file->groups, parent,
								      (sizeof (int)
								       * (file->num_groups + 1))));
				file->groups[file->num_groups++] = i;
				hits++;
			}
		}

		mean /= num_groups;

		nih_debug ("%zu inode groups, mean %zu inodes per group, %zu hits",
			   num_groups, mean, hits);

		ext2fs_close (fs);
	}

	return 0;
}


static int
block_compar (const void *a,
	      const void *b)
{
	const PackBlock *block_a = a;
	const PackBlock *block_b = b;

	nih_assert (block_a != NULL);
	nih_assert (block_b != NULL);

	if (block_a->physical < block_b->physical) {
		return -1;
	} else if (block_a->physical > block_b->physical) {
		return 1;
	} else {
		return 0;
	}
}

static int
trace_sort_blocks (const void *parent,
		   PackFile *  file)
{
	nih_assert (file != NULL);

	/* Sort the blocks array by physical location, since these are
	 * read in a separate pass to opening files, there's no reason
	 * to consider which path each block is in - and thus resulting
	 * in a linear disk read.
	 */
	qsort (file->blocks, file->num_blocks, sizeof (PackBlock),
	       block_compar);

	return 0;
}

static int
path_compar (const void *a,
	     const void *b)
{
	const PackPath * const *path_a = a;
	const PackPath * const *path_b = b;

	nih_assert (path_a != NULL);
	nih_assert (path_b != NULL);

	if ((*path_a)->group < (*path_b)->group) {
		return -1;
	} else if ((*path_a)->group > (*path_b)->group) {
		return 1;
	} else if ((*path_a)->ino < (*path_b)->ino) {
		return -1;
	} else if ((*path_b)->ino > (*path_b)->ino) {
		return 1;
	} else {
		return strcmp ((*path_a)->path, (*path_b)->path);
	}
}

static int
trace_sort_paths (const void *parent,
		  PackFile *  file)
{
	nih_local PackPath **paths = NULL;
	nih_local size_t *   new_idx = NULL;
	PackPath *           new_paths;

	nih_assert (file != NULL);

	/* Sort the paths array by ext2fs inode group, ino_t then path.
	 *
	 * Mucking around with things like the physical locations of
	 * first on-disk blocks of the dentry and stuff didn't work out
	 * so well, sorting by path was better, but this seems the best.
	 * (it looks good on blktrace too)
	 */
	paths = NIH_MUST (nih_alloc (NULL, (sizeof (PackPath *)
					    * file->num_paths)));

	for (size_t i = 0; i < file->num_paths; i++)
		paths[i] = &file->paths[i];

	qsort (paths, file->num_paths, sizeof (PackPath *),
	       path_compar);

	/* Calculate the new indexes of each path element in the old
	 * array, and then update the block array's path indexes to
	 * match.
	 */
	new_idx = NIH_MUST (nih_alloc (NULL,
				       (sizeof (size_t) * file->num_paths)));
	for (size_t i = 0; i < file->num_paths; i++)
		new_idx[paths[i] - file->paths] = i;

	for (size_t i = 0; i < file->num_blocks; i++)
		file->blocks[i].pathidx = new_idx[file->blocks[i].pathidx];

	/* Finally generate a new paths array with the new order and
	 * attach it to the file.
	 */
	new_paths = NIH_MUST (nih_alloc (parent,
					 (sizeof (PackPath) * file->num_paths)));
	for (size_t i = 0; i < file->num_paths; i++)
		memcpy (&new_paths[new_idx[i]], &file->paths[i],
			sizeof (PackPath));

	nih_unref (file->paths, parent);
	file->paths = new_paths;

	return 0;
}

/* This gets called for every flemap event in the ring buffer in order */
static int read_filemap_trace  (struct tep_event *event,
				struct tep_record *record,
				struct filemap_tep *fault,
				struct filemap_tep *get_pages,
				struct filemap_tep *map_pages,
				struct device_data **device_hash)
{
	unsigned long long ino;
	unsigned long long device;
	unsigned long long index;
	unsigned long long last_index;
	int major, minor;

	struct filemap_tep *tep;

	if (event->id == fault->event->id)
		tep = fault;
	else if (event->id == get_pages->event->id)
		tep = get_pages;
	else if (event->id == map_pages->event->id)
		tep = map_pages;
	else
		return 1;

	if (tep_read_number_field (tep->inode, record->data, &ino) < 0)
		return 1;

	if (tep_read_number_field (tep->device, record->data, &device) < 0)
		return 1;

	if (tep_read_number_field (tep->index, record->data, &index) < 0)
		return 1;

	if (tep->last_index) {
		if (tep_read_number_field (tep->last_index, record->data, &last_index) < 0)
			return 1;
	} else {
		last_index = index;
	}

	/* The major bit shift is 20 in the trace. */
	major = device >> 20;
	minor = device & 0xff;

	printf("map: ino: %llu, device: %llu, index: %llu, last_index: %llu\n", ino, device, index, last_index);

	trace_add_file_map (device_hash, makedev (major, minor), ino, index, last_index);

	return 0;
}

/*
 * The mm_filemap_add_to_page_cache event was read and gives the device, inode number,
 * and page index. Note the offset into the file that this page is for is found by:
 *  offset = index * page_size
 */
static void trace_add_file_map (struct device_data **device_hash,
				dev_t dev_id, unsigned long ino,
				off_t index, off_t last_index)
{
	struct device_data *dev;
	struct inode_data *inode;
	struct file_map *map, *lower_bound, *upper_bound;
	struct file_map key;
	int idx;

	dev = find_device (device_hash, dev_id);
	if (! dev)
		dev = add_device (device_hash, dev_id);

	inode = find_inode (dev, ino);
	if (! inode)
		inode = add_inode (dev, ino);

	key.start = index;
	key.end = last_index + 1; /* make it a half-open interval */

	/*
	 * The cmp_file_map will match not only if it finds a mapping that the
	 * index is in, but also if the index is at the end of a mapping or
	 * the begging of one. In the latter case, it will return the mapping
	 * that touchs the index.
	 */
	map = bsearch (&key, inode->map, inode->nr_maps, sizeof(key), cmp_file_map);
	if (! map) {
		printf("takayas: couldn't find map for [%ld, %ld)\n", key.start, key.end);
		/* A new index that also does not touch a mapping. */
		add_map (inode, index, last_index);
		return;
	}

	if (map->start <= key.start && map->end >= key.end)
		/* Nothing to do, it is already accounted for */
		return;

	/* find the lower/upper bound of matching ranges. The length usually is 1 or
	 * 2 , so linear search is efficient enough. */
	lower_bound = map;
	upper_bound = map;
	while (lower_bound - 1 >= inode->map && cmp_file_map (lower_bound - 1, &key) == 0)
		lower_bound--;
	map = upper_bound;
	while (upper_bound + 1 < inode->map + inode->nr_maps && cmp_file_map (upper_bound + 1, &key) == 0)
		upper_bound++;

	/* extend the first map to the new range */
	lower_bound->start = nih_min (lower_bound->start, key.start);
	lower_bound->end = nih_max (upper_bound->end, key.end);

	/* If there's only one overlapping, then we are done */
	if (lower_bound == upper_bound)
		return;

	/* remove the merged maps */
	idx = upper_bound - inode->map;
	/* If the upper_bound map was not at the end, then adjust the inode map array */
	if (idx < inode->nr_maps - 1)
		memmove (lower_bound + 1, upper_bound + 1, sizeof(*map) * (inode->nr_maps - idx - 1));
	inode->nr_maps -= upper_bound - lower_bound;
}

/* Returns a match if A is within or touches B. */
static int cmp_file_map (const void *A, const void *B)
{
	const struct file_map *a = A;
	const struct file_map *b = B;

	if (a->end < b->start)
		return -1;

	return b->end < a->start;
}

/*
 * Insert this new index into the inode array.
 * Note, the inode array has a meta data element before it that has
 * FILEMAP_START_MARK as it's "start" element. This is to help the
 * cmp_file_map_range() function to know if the new index is between
 * two other indexes, as it returns the mapping after the index when
 * the index is before it. To do so, it needs to check the element before the
 * element being tested. In order to test the first element (without knowing
 * that it is on the first element), it needs to look before that element.
 * The FILEMAP_START_MARK element will be the element before the first one.
 */
static void add_map (struct inode_data *inode, off_t index, off_t last_index)
{
	struct file_map *map;
	struct file_map key;
	int idx;

	/* Handle the first two trivial cases */
	switch (inode->nr_maps) {
	case 0:
		/* Allocate 2: 1 for this element an 1 for the FILEMAP_START_MARK */
		map = malloc (sizeof(*inode->map) * 2);
		nih_assert (map != NULL);

		/* Add a buffer element at the beginning for cmp_file_map_range() */
		map->start = FILEMAP_START_MARK;
		/* The inode->map will skip over that element */
		map++;
		inode->map = map;
		break;
	case 1:
		/* The allocated array starts one element before the inode->map */
		map = inode->map - 1;
		/* Allocate three. 2 for the elements and one for the FILEMAP_START_MARK */
		map = realloc (map, sizeof(*inode->map) * 3);
		nih_assert (map != NULL);

		inode->map = map + 1;

		/* If the current element is greater than the new one, then move it */
		if (inode->map[0].start > index) {
			inode->map[1] = inode->map[0];
			map = &inode->map[0];
		} else
			map = &inode->map[1];
		break;
	default:
		key.start = index;
		/* convert it from an inclusive interval to a half-open interval */
		key.end = last_index + 1;

		/*
		 * The cmp_file_map_range() will return the map that is after
		 * the index (or NULL if the index is greater than all existing
		 * maps).
		 */
		map = bsearch (&key, inode->map, inode->nr_maps, sizeof(*map),
			       cmp_file_map_range);
		/*
		 * Find the index into the array that this new map index will
		 * be inserted.
		 */
		if (map)
			idx = map - inode->map;
		else
			idx = inode->nr_maps;
		/* Set map to the start of the allocation */
		map = inode->map - 1;
		map = realloc (map, sizeof(*map) * (inode->nr_maps + 2));
		nih_assert (map != NULL);
		map++;
		inode->map = map;

		/* If the new index is not at the end, make room for it */
		if (idx < inode->nr_maps)
			memmove (&map[idx + 1], &map[idx],
				 sizeof(*map) * (inode->nr_maps - idx));
		map = &map[idx];
	}
	map->start = index;
	/* convert it from an inclusive interval to a half-open interval */
	map->end = last_index + 1;
	inode->nr_maps++;
}

/*
 * Range is called when the offset does not touch any of the
 * existing mappings.
 *
 * Returns NULL, if A is bigger than all the other elements.
 * Otherwise, returns the element just after A.
 */
static int cmp_file_map_range (const void *A, const void *B)
{
	const struct file_map *a = A;
	const struct file_map *b2 = B;
	const struct file_map *b1 = b2 - 1;

	if (a->end < b2->start) {
		/* Check if a is between b1 and b2 */
		if (b1->start == FILEMAP_START_MARK || a->start > b1->end)
			return 0;
		else
			return -1;
	}

	/*
	 * This is only called when a search failed,
	 * so a should never be within b.
	 * If we are here, then a > b.
	 */
	return 1;
}

/*
 * add_inode() works the same as add_map() above. Where it creates an
 * array that has a meta element at the start to use for searching
 * for the location between to other elements.
 */
static struct inode_data *add_inode (struct device_data *dev, unsigned long ino)
{
	struct inode_data *inode;
	struct inode_data key;
	int index;

	switch (dev->nr_inodes) {
	case 0:
		/* Add a marker to the beginning of the array for the range compare */
		inode = malloc (sizeof(key) * 2);
		nih_assert (inode != NULL);
		inode->inode = FILEMAP_START_MARK;
		inode++;
		dev->inodes = inode;
		break;
	case 1:
		inode = dev->inodes - 1;
		inode = realloc (inode, sizeof(key) * 3);
		nih_assert (inode != NULL);
		dev->inodes = inode + 1;

		/* If the current element is greater than the new one, then move it */
		if (dev->inodes[0].inode > ino) {
			dev->inodes[1] = dev->inodes[0];
			inode = &dev->inodes[0];
		} else
			inode = &dev->inodes[1];
		break;
	default:
		key.inode = ino;

		/*
		 * Returns the inode after the current one, or NULL
		 * if it's the first one.
		 */
		inode = bsearch (&key, dev->inodes, dev->nr_inodes, sizeof(key),
				 cmp_inodes_range);
		if (inode)
			index = inode - dev->inodes;
		else
			index = dev->nr_inodes;

		/* Set to the start of the allocated array */
		inode = dev->inodes - 1;
		inode = realloc (inode, sizeof(key) * (dev->nr_inodes + 2));
		nih_assert (inode != NULL);
		inode++;
		dev->inodes = inode;

		/* Make room for the new inode if it's not at the end of the array */
		if (index < dev->nr_inodes)
			memmove (&inode[index + 1], &inode[index],
				 sizeof(key) * (dev->nr_inodes - index));
		inode = &inode[index];
	}
	memset (inode, 0, sizeof(*inode));
	inode->inode = ino;
	dev->nr_inodes++;
	return inode;
}

/*
 * Compare to cause bsearch to:
 *
 * Return NULL, if A is bigger than all the other elements.
 * Otherwise, return the element just after A.
 */
static int cmp_inodes_range (const void *A, const void *B)
{
	const struct inode_data *a = A;
	const struct inode_data *b2 = B;
	const struct inode_data *b1 = b2 - 1;

	if (a->inode < b2->inode) {
		/* if a is between b1 and b2, then it's a match */
		if (b1->inode == FILEMAP_START_MARK || a->inode > b1->inode)
			return 0;
		else
			return -1;
	}

	/*
	 * This is only called when a search failed,
	 * so a->inode should never equal b->node.
	 * If we are here, then a > b.
	 */
	return 1;
}

static struct inode_data *find_inode (struct device_data *dev, unsigned long ino)
{
	struct inode_data key;

	key.inode = ino;

	/* Returns a map that just touches the offset */
	return bsearch (&key, dev->inodes, dev->nr_inodes, sizeof(key), cmp_inodes);
}

static int cmp_inodes (const void *A, const void *B)
{
	const struct inode_data *a = A;
	const struct inode_data *b = B;

	if (a->inode < b->inode)
		return -1;

	return a->inode > b->inode;
}

static struct device_data *add_device (struct device_data **device_hash,
				       dev_t id)
{
	struct device_data *dev;
	int key = id & HASH_MASK;

	dev = calloc (1, sizeof(*dev));
	nih_assert (dev != NULL);

	dev->id = id;
	dev->next = device_hash[key];
	device_hash[key] = dev;

	return dev;
}

static struct device_data *find_device (struct device_data **device_hash,
					dev_t id)
{
	struct device_data *dev;
	int key = id & HASH_MASK;

	for (dev = device_hash[key]; dev; dev = dev->next) {
		if (dev->id == id)
			break;
	}

	return dev;
}

#include <assert.h>

void test_trace_add_file_map() {
	struct device_data *device_hash[HASH_SIZE] = {0};

	struct {
		dev_t dev_id;
		unsigned long ino;
		off_t index;
		off_t last_index;
		// Expected outcome after the call
		struct file_map expected_maps[30][2];
		int expected_num_maps;
	} test_cases[] = {
		{
			makedev(8, 0), 12345, 0, 0, {{0, 1}}, 1  // New inode, single map
		},
		{
			makedev(8, 0),
			12345,
			2,
			3,
			{{0, 1}, {2, 4}},
			2  // Same inode, non-overlapping neighbor new map
		},
		{
			makedev(8, 1), 67890, 0, 0, {{0, 1}}, 1  // New device and inode
		},
		{
			makedev(8, 0), 12345, 1, 1, {{0, 4}}, 1  // Fill the gap, merge into a
								 // single map
		},
		{
			makedev(8, 0), 12345, 4, 5, {{0, 6}}, 1  // Neighbor touching the end.
								 // merge
		},
		{
			makedev(8, 0),
			12345,
			8,
			10,
			{{0, 6}, {8, 11}},
			2  // New non-overlapping map
		},
		{
			makedev(8, 0), 12345, 7, 7, {{0, 6}, {7, 11}}, 2  // neighbor touching
									  // the begin
		},
		{
			makedev(8, 0), 12345, 1, 3, {{0, 6}, {7, 11}}, 2  // already covered.
									  // no change
		},
		{
			makedev(8, 0), 12345, 7, 10, {{0, 6}, {7, 11}}, 2  // add exact same
									   // range.
		},
		{
			makedev(8, 0), 12345, 2, 8, {{0, 11}}, 1  // Overlap in the middle,
								  // merge
		},
		{
			makedev(8, 0),
			12345,
			20,
			30,
			{
				{0, 11},
				{20, 31},
			},
			2  // New non-overlapping map
		},
		{
			makedev(8, 0),
			12345,
			50,
			60,
			{{0, 11}, {20, 31}, {50, 61}},
			3  // New non-overlapping map
		},
		{
			makedev(8, 0),
			12345,
			70,
			80,
			{{0, 11}, {20, 31}, {50, 61}, {70, 81}},
			4  // New non-overlapping map
		},
		{
			makedev(8, 0),
			12345,
			90,
			100,
			{{0, 11}, {20, 31}, {50, 61}, {70, 81}, {90, 101}},
			5  // New non-overlapping map
		},
		{
			makedev(8, 0),
			12345,
			25,
			69,
			{{0, 11}, {20, 81}, {90, 101}},
			3  // merge multiple
		},
	};

	// Iterate and test with assertions
	for (size_t i = 0; i < sizeof(test_cases) / sizeof(test_cases[0]); i++) {
		printf("Test case %zu:\n", i + 1);

		trace_add_file_map(device_hash, test_cases[i].dev_id, test_cases[i].ino,
				test_cases[i].index, test_cases[i].last_index);

		// Find the device and inode in the hash
		struct device_data *dev = find_device(device_hash, test_cases[i].dev_id);
		assert(dev != NULL);
		struct inode_data *inode = find_inode(dev, test_cases[i].ino);
		assert(inode != NULL);

		printf("inode: %ld ", inode->inode);
		for (int j = 0; j < inode->nr_maps; j++) {
			printf("[%ld, %ld), ", inode->map[j].start, inode->map[j].end);
		}
		printf("\n");

		// Assert the number of maps and their ranges
		assert(inode->nr_maps == test_cases[i].expected_num_maps);
		for (int j = 0; j < inode->nr_maps; j++) {
			assert(inode->map[j].start == test_cases[i].expected_maps[j]->start);
			assert(inode->map[j].end == test_cases[i].expected_maps[j]->end);
		}
	}

	free_device_hash(device_hash);
}

void test_remove_untouched_blocks() {
	struct device_data *device_hash[HASH_SIZE] = {0};
	nih_local PackFile *file = nih_new(NULL, PackFile);
	file->dev = makedev(8, 0);  // Sample device
	file->rotational = 0;
	file->num_paths = 2;  // Two paths/files in this PackFile

	// Allocate memory for paths and blocks (you'll need to fill these with actual
	// data later)
	file->paths = nih_alloc(file, sizeof(PackPath) * file->num_paths);
	file->paths[0].ino = 1;
	file->paths[1].ino = 2;
	file->num_blocks = 6;
	file->blocks = nih_alloc(
			file, sizeof(PackBlock) *
			file->num_blocks);  // Assuming an initial 5 blocks for testing

	// cover the first block
	trace_add_file_map(device_hash, file->dev, 1, 13,
			18);
			      // part of the second block
	trace_add_file_map(device_hash, file->dev, 1, 22,
			23);
			      // cover 3rd and 4th blocks
	trace_add_file_map(device_hash, file->dev, 1, 32,
			45);
			      // cover part of the 5th block
	trace_add_file_map(device_hash, file->dev, 1, 52,
			53);
			      // still part of the 5th block
	trace_add_file_map(device_hash, file->dev, 1, 56,
			57);
			      // touch the begin of 6th block
	trace_add_file_map(device_hash, file->dev, 1, 62,
			62);
			      // touch the end of 6th block
	trace_add_file_map(device_hash, file->dev, 1, 69,
			69);

	puts("takayas after add file map");

	// first block. will be covered
	file->blocks[0].pathidx = 0;
	file->blocks[0].offset = 13 << PAGE_SHIFT;
	file->blocks[0].length = 5 << PAGE_SHIFT;

	file->blocks[1].pathidx = 0;
	file->blocks[1].offset = 20 << PAGE_SHIFT;
	file->blocks[1].length = 5 << PAGE_SHIFT;

	file->blocks[2].pathidx = 0;
	file->blocks[2].offset = 33 << PAGE_SHIFT;
	file->blocks[2].length = 5 << PAGE_SHIFT;

	file->blocks[3].pathidx = 0;
	file->blocks[3].offset = 43 << PAGE_SHIFT;
	file->blocks[3].length = 5 << PAGE_SHIFT;

	file->blocks[4].pathidx = 0;
	file->blocks[4].offset = 53 << PAGE_SHIFT;
	file->blocks[4].length = 5 << PAGE_SHIFT;

	file->blocks[5].pathidx = 0;
	file->blocks[5].offset = 63 << PAGE_SHIFT;
	file->blocks[5].length = 5 << PAGE_SHIFT;

	printf("before file: %d\n", file->num_blocks);

	// Call the function under test
	remove_untouched_blocks(file, device_hash, file);

	PackBlock expected_blocks[] = {
		{0, 13 << PAGE_SHIFT, 5 << PAGE_SHIFT},
		{0, 22 << PAGE_SHIFT, 2 << PAGE_SHIFT},
		{0, 33 << PAGE_SHIFT, 5 << PAGE_SHIFT},
		{0, 43 << PAGE_SHIFT, 3 << PAGE_SHIFT},
		{0, 53 << PAGE_SHIFT, 1 << PAGE_SHIFT},
		{0, 56 << PAGE_SHIFT, 2 << PAGE_SHIFT},
	};
	int expected_num_blocks =
		sizeof(expected_blocks) / sizeof(expected_blocks[0]);

	printf("file: %ld\n", file->num_blocks);
	// Assertions to validate the outcome
	nih_assert(file->num_blocks == expected_num_blocks);
	for (int i = 0; i < expected_num_blocks; i++) {
		assert(file->blocks[i].pathidx == expected_blocks[i].pathidx);
		assert(file->blocks[i].offset == expected_blocks[i].offset);
		assert(file->blocks[i].length == expected_blocks[i].length);
	}

	// Cleanup
	free_device_hash(device_hash);
}

