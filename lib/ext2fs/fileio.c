/*
 * fileio.c --- Simple file I/O routines
 * 
 * Copyright (C) 1997 Theodore Ts'o.
 *
 * %Begin-Header%
 * This file may be redistributed under the terms of the GNU Public
 * License.
 * %End-Header%
 */

#include <stdio.h>
#include <string.h>
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <stdlib.h>

#include <linux/ext2_fs.h>

#include "ext2fs.h"

struct ext2_file {
	errcode_t		magic;
	ext2_filsys 		fs;
	ino_t			ino;
	struct ext2_inode	inode;
	int 			flags;
	ext2_off_t		pos;
	blk_t			blockno;
	blk_t			physblock;
	char 			*buf;
};

/*
 * XXX Doesn't handle writing yet
 */
errcode_t ext2fs_file_open(ext2_filsys fs, ino_t ino,
			   int flags, ext2_file_t *ret)
{
	ext2_file_t 	file;
	errcode_t	retval;

	/*
	 * Don't let caller create or open a file for writing if the
	 * filesystem is read-only.
	 */
	if ((flags & (EXT2_FILE_WRITE | EXT2_FILE_CREATE)) &&
	    !(fs->flags & EXT2_FLAG_RW))
		return EXT2_ET_RO_FILSYS;

	file = (ext2_file_t) malloc(sizeof(struct ext2_file));
	if (!file)
		return EXT2_NO_MEMORY;
	
	memset(file, 0, sizeof(struct ext2_file));
	file->magic = EXT2_ET_MAGIC_EXT2_FILE;
	file->fs = fs;
	file->ino = ino;
	file->flags = flags & EXT2_FILE_MASK;

	retval = ext2fs_read_inode(fs, ino, &file->inode);
	if (retval)
		goto fail;
	
	file->buf = malloc(fs->blocksize);
	if (!file->buf) {
		retval = EXT2_NO_MEMORY;
		goto fail;
	}

	*ret = file;
	return 0;
	
fail:
	if (file->buf)
		free(file->buf);
	free(file);
	return retval;
}

/*
 * This function flushes the dirty block buffer out to disk if
 * necessary.
 */
static errcode_t ext2fs_file_flush(ext2_file_t file)
{
	errcode_t	retval;
	
	EXT2_CHECK_MAGIC(file, EXT2_ET_MAGIC_EXT2_FILE);

	if (!(file->flags & EXT2_FILE_BUF_VALID) ||
	    !(file->flags & EXT2_FILE_BUF_DIRTY))
		return 0;

	/*
	 * OK, the physical block hasn't been allocated yet.
	 * Allocate it.
	 */
	if (!file->physblock) {
		retval = ext2fs_bmap(file->fs, file->ino, &file->inode,
				     file->buf, BMAP_ALLOC,
				     file->blockno, &file->physblock);
		if (retval)
			return retval;
	}

	retval = io_channel_write_blk(file->fs->io, file->physblock,
				      1, file->buf);
	if (retval)
		return retval;

	file->flags &= ~EXT2_FILE_BUF_DIRTY;

	return retval;
}

errcode_t ext2fs_file_close(ext2_file_t file)
{
	errcode_t	retval;
	
	EXT2_CHECK_MAGIC(file, EXT2_ET_MAGIC_EXT2_FILE);

	retval = ext2fs_file_flush(file);
	
	if (file->buf)
		free(file->buf);
	free(file);

	return retval;
}


errcode_t ext2fs_file_read(ext2_file_t file, void *buf,
			   int wanted, int *got)
{
	ext2_filsys	fs;
	errcode_t	retval;
	blk_t		b, pb;
	int		start, left, c, count = 0;
	char		*ptr = buf;

	EXT2_CHECK_MAGIC(file, EXT2_ET_MAGIC_EXT2_FILE);
	fs = file->fs;

again:
	if (file->pos >= file->inode.i_size)
		goto done;

	b = file->pos / fs->blocksize;
	if (b != file->blockno) {
		retval = ext2fs_file_flush(file);
		if (retval)
			goto fail;
		file->flags &= ~EXT2_FILE_BUF_VALID;
	}
	file->blockno = b;
	if (!(file->flags & EXT2_FILE_BUF_VALID)) {
		retval = ext2fs_bmap(fs, file->ino, &file->inode,
				     file->buf, 0, b, &pb);
		if (retval)
			goto fail;
		if (pb) {
			file->physblock = pb;
			retval = io_channel_read_blk(fs->io, pb, 1, file->buf);
			if (retval)
				goto fail;
		} else {
			file->physblock = 0;
			memset(file->buf, 0, fs->blocksize);
		}
		
		file->flags |= EXT2_FILE_BUF_VALID;
	}
	start = file->pos % fs->blocksize;
	c = fs->blocksize - start;
	if (c > wanted)
		c = wanted;
	left = file->inode.i_size - file->pos ;
	if (c > left)
		c = left;
	
	memcpy(ptr, file->buf+start, c);
	file->pos += c;
	ptr += c;
	count += c;
	wanted -= c;

	if (wanted > 0)
		goto again;

done:
	if (got)
		*got = count;
	return 0;

fail:
	if (count)
		goto done;
	return retval;
}


errcode_t ext2fs_file_write(ext2_file_t file, void *buf,
			    int nbytes, int *written)
{
	ext2_filsys	fs;
	errcode_t	retval;
	blk_t		b, pb;
	int		start, c, count = 0;
	char		*ptr = buf;

	EXT2_CHECK_MAGIC(file, EXT2_ET_MAGIC_EXT2_FILE);
	fs = file->fs;

	if (!(file->flags & EXT2_FILE_WRITE))
		return EXT2_FILE_RO;

again:
	b = file->pos / fs->blocksize;
	if (b != file->blockno) {
		retval = ext2fs_file_flush(file);
		if (retval)
			goto fail;
		file->flags &= ~EXT2_FILE_BUF_VALID;
	}
	file->blockno = b;
	if (!(file->flags & EXT2_FILE_BUF_VALID)) {
		retval = ext2fs_bmap(fs, file->ino, &file->inode,
				     file->buf, BMAP_ALLOC, b, &pb);
		if (retval)
			goto fail;
		file->physblock = pb;
		
		retval = io_channel_read_blk(fs->io, pb, 1, file->buf);
		if (retval)
			goto fail;
		file->flags |= EXT2_FILE_BUF_VALID;
	}
	start = file->pos % fs->blocksize;
	c = fs->blocksize - start;
	if (c > nbytes)
		c = nbytes;
	
	file->flags |= EXT2_FILE_BUF_DIRTY;
	memcpy(file->buf+start, ptr, c);
	file->pos += c;
	ptr += c;
	count += c;
	nbytes -= c;

	if (nbytes > 0)
		goto again;

done:
	if (written)
		*written = count;
	return 0;

fail:
	if (count)
		goto done;
	return retval;
}


errcode_t ext2fs_file_llseek(ext2_file_t file, ext2_off_t offset,
			     int whence, ext2_off_t *ret_pos)
{
	EXT2_CHECK_MAGIC(file, EXT2_ET_MAGIC_EXT2_FILE);

	if (whence == EXT2_SEEK_SET)
		file->pos = offset;
	else if (whence == EXT2_SEEK_CUR)
		file->pos += offset;
	else if (whence == EXT2_SEEK_END)
		file->pos = file->inode.i_size + offset;
	else
		return EXT2_INVALID_ARGUMENT;

	if (ret_pos)
		*ret_pos = file->pos;

	return 0;
}


