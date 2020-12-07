#include <fs/buffer.h>
#include <fs/vfs.h>
#include <include/errno.h>
#include <include/limits.h>
#include <memory/vmm.h>
#include <system/time.h>
#include <utils/debug.h>
#include <utils/math.h>
#include <utils/string.h>

#include "ext2.h"

static int ext2_recursive_block_action(struct vfs_superblock *sb,
									   int level, uint32_t block,
									   void *arg,
									   int (*action)(struct vfs_superblock *, uint32_t, void *))
{
	assert(level <= 3);
	if (level > 0)
	{
		int ret = -ENOENT;
		uint32_t *block_buf = (uint32_t *)ext2_bread_block(sb, block);
		for (int i = 0, nblocks = sb->s_blocksize / 4; i < nblocks; ++i)
			if ((ret = ext2_recursive_block_action(sb, level - 1, block_buf[i], arg, action)) >= 0)
				break;
		return ret;
	}
	else
		return action(sb, block, arg);
}

static int find_unused_block_number(struct vfs_superblock *sb)
{
	struct ext2_superblock *ext2_sb = EXT2_SB(sb);

	uint32_t number_of_groups = div_ceil(ext2_sb->s_blocks_count, ext2_sb->s_blocks_per_group);
	for (uint32_t group = 0; group < number_of_groups; group += 1)
	{
		struct ext2_group_desc *gdp = ext2_get_group_desc(sb, group);
		unsigned char *block_bitmap = (unsigned char *)ext2_bread_block(sb, gdp->bg_block_bitmap);

		for (uint32_t i = 0; i < sb->s_blocksize; ++i)
			if (block_bitmap[i] != 0xff)
				for (int j = 0; j < 8; ++j)
					if (!(block_bitmap[i] & (1 << j)))
						return group * ext2_sb->s_blocks_per_group + i * 8 + j + ext2_sb->s_first_data_block;
	}
	return -ENOSPC;
}

static int find_unused_inode_number(struct vfs_superblock *sb)
{
	struct ext2_superblock *ext2_sb = EXT2_SB(sb);

	uint32_t number_of_groups = div_ceil(ext2_sb->s_blocks_count, ext2_sb->s_blocks_per_group);
	for (uint32_t group = 0; group < number_of_groups; group += 1)
	{
		struct ext2_group_desc *gdp = ext2_get_group_desc(sb, group);
		unsigned char *inode_bitmap = (unsigned char *)ext2_bread_block(sb, gdp->bg_inode_bitmap);

		for (uint32_t i = 0; i < sb->s_blocksize; ++i)
			if (inode_bitmap[i] != 0xff)
				for (uint8_t j = 0; j < 8; ++j)
					if (!(inode_bitmap[i] & (1 << j)))
						return group * ext2_sb->s_inodes_per_group + i * 8 + j + EXT2_STARTING_INO;
	}
	return -ENOSPC;
}

static int ext2_add_entry(struct vfs_superblock *sb, uint32_t block, void *arg)
{
	struct vfs_dentry *dentry = arg;
	int filename_length = strlen(dentry->d_name);

	char *block_buf = ext2_bread_block(sb, block);

	int size = 0, new_rec_len = 0;
	struct ext2_dir_entry *entry = (struct ext2_dir_entry *)block_buf;
	while (size < sb->s_blocksize && (char *)entry < block_buf + sb->s_blocksize)
	{
		// NOTE: MQ 2020-12-01 some ext2 tools mark entry with zero indicate an unused entry
		if (!entry->ino && (!entry->rec_len || entry->rec_len >= EXT2_DIR_REC_LEN(filename_length)))
		{
			entry->ino = dentry->d_inode->i_ino;
			if (S_ISREG(dentry->d_inode->i_mode))
				entry->file_type = 1;
			else if (S_ISDIR(dentry->d_inode->i_mode))
				entry->file_type = 2;
			else
				assert_not_implemented();
			entry->name_len = filename_length;
			memcpy(entry->name, dentry->d_name, entry->name_len);
			entry->rec_len = max_t(uint16_t, new_rec_len, entry->rec_len);

			ext2_bwrite_block(sb, block, block_buf);
			return 0;
		}
		if (EXT2_DIR_REC_LEN(filename_length) + EXT2_DIR_REC_LEN(entry->name_len) < entry->rec_len)
		{
			new_rec_len = entry->rec_len - EXT2_DIR_REC_LEN(entry->name_len);
			entry->rec_len = EXT2_DIR_REC_LEN(entry->name_len);
			size += entry->rec_len;
			entry = (struct ext2_dir_entry *)((char *)entry + entry->rec_len);
			memset(entry, 0, new_rec_len);
		}
		else
		{
			size += entry->rec_len;
			new_rec_len = sb->s_blocksize - size;
			entry = (struct ext2_dir_entry *)((char *)entry + entry->rec_len);
		}
	}
	return -ENOENT;
}

static int ext2_create_entry(struct vfs_superblock *sb, struct vfs_inode *dir, struct vfs_dentry *dentry)
{
	struct ext2_inode *ei = EXT2_INODE(dir);
	// NOTE: MQ 2020-11-19 support nth-level block
	for (int i = 0; i < dir->i_blocks; ++i)
	{
		if (i >= EXT2_INO_UPPER_LEVEL0)
			assert_not_reached();

		int block = ei->i_block[i];
		if (!block)
		{
			block = ext2_create_block(dir->i_sb);
			ei->i_block[i] = block;
			dir->i_blocks += 1;
			dir->i_size += sb->s_blocksize;
			ext2_write_inode(dir);
		}
		if (ext2_add_entry(sb, block, dentry) >= 0)
			return 0;
	}
	return -ENOENT;
}

static int ext2_delete_entry(struct vfs_superblock *sb, uint32_t block, void *arg)
{
	const char *name = arg;
	char *block_buf = ext2_bread_block(sb, block);

	char tmpname[NAME_MAX];
	uint32_t size = 0;
	struct ext2_dir_entry *prev = NULL;
	struct ext2_dir_entry *entry = (struct ext2_dir_entry *)block_buf;
	while (size < sb->s_blocksize)
	{
		memcpy(tmpname, entry->name, entry->name_len);
		tmpname[entry->name_len] = 0;

		if (strcmp(tmpname, name) == 0)
		{
			int ino = entry->ino;
			entry->ino = 0;

			if (prev)
				prev->rec_len += entry->rec_len;

			ext2_bwrite_block(sb, block, block_buf);
			return ino;
		}

		prev = entry;
		size += entry->rec_len;
		entry = (struct ext2_dir_entry *)((char *)entry + entry->rec_len);
	}
	return -ENOENT;
}

static int ext2_find_ino(struct vfs_superblock *sb, uint32_t block, void *arg)
{
	const char *name = arg;
	char *block_buf = ext2_bread_block(sb, block);

	char tmpname[NAME_MAX];
	uint32_t size = 0;
	struct ext2_dir_entry *entry = (struct ext2_dir_entry *)block_buf;
	while (size < sb->s_blocksize)
	{
		memcpy(tmpname, entry->name, entry->name_len);
		tmpname[entry->name_len] = 0;

		if (strcmp(tmpname, name) == 0)
			return entry->ino;

		size = size + entry->rec_len;
		entry = (struct ext2_dir_entry *)((char *)entry + entry->rec_len);
	}
	return -ENOENT;
}

uint32_t ext2_create_block(struct vfs_superblock *sb)
{
	struct ext2_superblock *ext2_sb = EXT2_SB(sb);
	uint32_t block = find_unused_block_number(sb);

	// superblock
	ext2_sb->s_free_blocks_count -= 1;
	sb->s_op->write_super(sb);

	// group
	struct ext2_group_desc *gdp = ext2_get_group_desc(sb, get_group_from_block(ext2_sb, block));
	gdp->bg_free_blocks_count -= 1;
	ext2_write_group_desc(sb, gdp);

	// block bitmap
	char *bitmap_buf = ext2_bread_block(sb, gdp->bg_block_bitmap);
	uint32_t relative_block = get_relative_block_in_group(ext2_sb, block);
	bitmap_buf[relative_block / 8] |= 1 << (relative_block % 8);
	ext2_bwrite_block(sb, gdp->bg_block_bitmap, bitmap_buf);

	// clear block data
	char *data_buf = kcalloc(sb->s_blocksize, sizeof(char));
	ext2_bwrite_block(sb, block, data_buf);

	return block;
}

static struct vfs_inode *ext2_create_inode(struct vfs_inode *dir, struct vfs_dentry *dentry, mode_t mode)
{
	struct vfs_superblock *sb = dir->i_sb;
	struct ext2_superblock *ext2_sb = EXT2_SB(sb);
	uint32_t ino = find_unused_inode_number(sb);
	struct ext2_group_desc *gdp = ext2_get_group_desc(sb, get_group_from_inode(ext2_sb, ino));

	// superblock
	ext2_sb->s_free_inodes_count -= 1;
	sb->s_op->write_super(sb);

	// group descriptor
	gdp->bg_free_inodes_count -= 1;
	if (S_ISDIR(mode))
		gdp->bg_used_dirs_count += 1;
	ext2_write_group_desc(sb, gdp);

	// inode bitmap
	char *inode_bitmap_buf = ext2_bread_block(sb, gdp->bg_inode_bitmap);
	uint32_t relative_inode = get_relative_inode_in_group(ext2_sb, ino);
	inode_bitmap_buf[relative_inode / 8] |= 1 << (relative_inode % 8);
	ext2_bwrite_block(sb, gdp->bg_inode_bitmap, inode_bitmap_buf);

	// inode table
	struct ext2_inode *ei_new = kcalloc(1, sizeof(struct ext2_inode));
	ei_new->i_links_count = 1;
	struct vfs_inode *inode = sb->s_op->alloc_inode(sb);
	inode->i_ino = ino;
	inode->i_mode = mode;
	inode->i_size = 0;
	inode->i_fs_info = ei_new;
	inode->i_sb = sb;
	inode->i_atime.tv_sec = get_seconds(NULL);
	inode->i_ctime.tv_sec = get_seconds(NULL);
	inode->i_mtime.tv_sec = get_seconds(NULL);
	inode->i_flags = 0;
	inode->i_blocks = 0;
	// NOTE: MQ 2020-11-18 When creating inode, it is safe to assume that it is linked to dir entry?
	inode->i_nlink = 1;

	if (S_ISREG(mode))
	{
		inode->i_op = &ext2_file_inode_operations;
		inode->i_fop = &ext2_file_operations;
	}
	else if (S_ISDIR(mode))
	{
		inode->i_op = &ext2_dir_inode_operations;
		inode->i_fop = &ext2_dir_operations;

		struct ext2_inode *ei = EXT2_INODE(inode);
		uint32_t block = ext2_create_block(inode->i_sb);
		ei->i_block[0] = block;
		inode->i_blocks += 1;
		inode->i_size += sb->s_blocksize;
		ext2_write_inode(inode);

		char *block_buf = ext2_bread_block(inode->i_sb, block);

		struct ext2_dir_entry *c_entry = (struct ext2_dir_entry *)block_buf;
		c_entry->ino = inode->i_ino;
		memcpy(c_entry->name, ".", 1);
		c_entry->name_len = 1;
		c_entry->rec_len = EXT2_DIR_REC_LEN(1);
		c_entry->file_type = 2;

		struct ext2_dir_entry *p_entry = (struct ext2_dir_entry *)(block_buf + c_entry->rec_len);
		p_entry->ino = dir->i_ino;
		memcpy(p_entry->name, "..", 2);
		p_entry->name_len = 2;
		p_entry->rec_len = sb->s_blocksize - c_entry->rec_len;
		p_entry->file_type = 2;

		ext2_bwrite_block(inode->i_sb, block, block_buf);
	}
	else
		assert_not_reached();

	sb->s_op->write_inode(inode);
	dentry->d_inode = inode;

	if (ext2_create_entry(sb, dir, dentry) >= 0)
		return inode;
	return NULL;
}

static struct vfs_inode *ext2_lookup_inode(struct vfs_inode *dir, struct vfs_dentry *dentry)
{
	struct ext2_inode *ei = EXT2_INODE(dir);
	struct vfs_superblock *sb = dir->i_sb;

	for (int i = 0, ino = 0; i < ei->i_blocks; ++i)
	{
		if (!ei->i_block[i])
			continue;

		if ((i < EXT2_INO_UPPER_LEVEL0 && (ino = ext2_recursive_block_action(sb, 0, ei->i_block[i], dentry->d_name, ext2_find_ino)) > 0) ||
			((EXT2_INO_UPPER_LEVEL0 <= i && i < EXT2_INO_UPPER_LEVEL1) && (ino = ext2_recursive_block_action(sb, 1, ei->i_block[12], dentry->d_name, ext2_find_ino)) > 0) ||
			((EXT2_INO_UPPER_LEVEL1 <= i && i < EXT2_INO_UPPER_LEVEL2) && (ino = ext2_recursive_block_action(sb, 2, ei->i_block[13], dentry->d_name, ext2_find_ino)) > 0) ||
			((EXT2_INO_UPPER_LEVEL2 <= i && i < EXT2_INO_UPPER_LEVEL3) && (ino = ext2_recursive_block_action(sb, 3, ei->i_block[14], dentry->d_name, ext2_find_ino)) > 0))
		{
			struct vfs_inode *inode = dir->i_sb->s_op->alloc_inode(dir->i_sb);
			inode->i_ino = ino;
			ext2_read_inode(inode);
			return inode;
		}
	}
	return NULL;
}

static int ext2_mknod(struct vfs_inode *dir, struct vfs_dentry *dentry, int mode, dev_t dev)
{
	struct vfs_inode *inode = ext2_lookup_inode(dir, dentry);
	if (inode == NULL)
		inode = ext2_create_inode(dir, dentry, mode);
	inode->i_rdev = dev;
	init_special_inode(inode, mode, dev);
	ext2_write_inode(inode);

	dentry->d_inode = inode;
	return 0;
}

static int ext2_unlink(struct vfs_inode *dir, struct vfs_dentry *dentry)
{
	struct ext2_inode *ei = EXT2_INODE(dir);
	struct vfs_superblock *sb = dir->i_sb;

	for (int i = 0, ino = 0; i < ei->i_blocks; ++i)
	{
		if (!ei->i_block[i])
			continue;

		if ((i < EXT2_INO_UPPER_LEVEL0 && (ino = ext2_recursive_block_action(sb, 0, ei->i_block[i], dentry->d_name, ext2_delete_entry)) > 0) ||
			((EXT2_INO_UPPER_LEVEL0 <= i && i < EXT2_INO_UPPER_LEVEL1) && (ino = ext2_recursive_block_action(sb, 1, ei->i_block[12], dentry->d_name, ext2_delete_entry)) > 0) ||
			((EXT2_INO_UPPER_LEVEL1 <= i && i < EXT2_INO_UPPER_LEVEL2) && (ino = ext2_recursive_block_action(sb, 2, ei->i_block[13], dentry->d_name, ext2_delete_entry)) > 0) ||
			((EXT2_INO_UPPER_LEVEL2 <= i && i < EXT2_INO_UPPER_LEVEL3) && (ino = ext2_recursive_block_action(sb, 3, ei->i_block[14], dentry->d_name, ext2_delete_entry)) > 0))
		{
			struct vfs_inode *inode = dir->i_sb->s_op->alloc_inode(dir->i_sb);
			inode->i_ino = ino;
			ext2_read_inode(inode);

			inode->i_nlink -= 1;
			ext2_write_inode(inode);
			// TODO: If i_nlink == 0, should we delete ext2 inode?
			break;
		}
	}
	return 0;
}

static int ext2_rename(struct vfs_inode *old_dir, struct vfs_dentry *old_dentry,
					   struct vfs_inode *new_dir, struct vfs_dentry *new_dentry)
{
	new_dentry->d_inode = old_dentry->d_inode;
	return ext2_create_entry(new_dir->i_sb, new_dir, new_dentry);
}

static void ext2_truncate_inode(struct vfs_inode *i)
{
}

struct vfs_inode_operations ext2_file_inode_operations = {
	.truncate = ext2_truncate_inode,
};

struct vfs_inode_operations ext2_dir_inode_operations = {
	.create = ext2_create_inode,
	.lookup = ext2_lookup_inode,
	.mknod = ext2_mknod,
	.rename = ext2_rename,
	.unlink = ext2_unlink,

};

struct vfs_inode_operations ext2_special_inode_operations = {};
