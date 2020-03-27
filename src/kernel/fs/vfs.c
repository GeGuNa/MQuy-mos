#include <include/errno.h>
#include <kernel/utils/string.h>
#include <kernel/memory/vmm.h>
#include <kernel/proc/task.h>
#include "vfs.h"
#include "dev.h"
#include "ext2/ext2.h"
#include "tmpfs/tmpfs.h"

static vfs_file_system_type *file_systems;
struct list_head vfsmntlist;

extern process *current_process;

vfs_file_system_type **find_filesystem(const char *name)
{
  vfs_file_system_type **p;
  for (p = &file_systems; *p; p = &(*p)->next)
  {
    if (strcmp((*p)->name, name) == 0)
      break;
  }
  return p;
}

int register_filesystem(vfs_file_system_type *fs)
{
  vfs_file_system_type **p = find_filesystem(fs->name);

  if (*p)
    return -EBUSY;
  else
    *p = fs;

  return 0;
}

int unregister_filesystem(vfs_file_system_type *fs)
{
  vfs_file_system_type **p;
  for (p = &file_systems; *p; p = &(*p)->next)
    if (strcmp((*p)->name, fs->name) == 0)
    {
      *p = (*p)->next;
      return 0;
    }
  return -EINVAL;
}

int find_unused_fd_slot()
{
  for (int i = 0; i < 256; ++i)
    if (!current_process->files->fd[i])
      return i;

  return -EINVAL;
}

vfs_inode *init_inode()
{
  vfs_inode *i = kmalloc(sizeof(vfs_inode));
  i->i_blocks = 0;
  i->i_size = 0;
  sema_init(&i->i_sem, 1);

  return i;
}

void init_special_inode(vfs_inode *inode, umode_t mode, dev_t dev)
{
  inode->i_mode = mode;
  if (S_ISCHR(mode))
  {
    inode->i_fop = &def_chr_fops;
    inode->i_rdev = dev;
  }
}

vfs_mount *lookup_mnt(vfs_dentry *d)
{
  vfs_mount *iter;
  list_for_each_entry(iter, &vfsmntlist, sibling)
  {
    if (iter->mnt_mountpoint == d)
      return iter;
  }

  return NULL;
}

vfs_mount *do_mount(const char *fstype, int flags, const char *path)
{
  char *dir, *name;
  strlsplat(path, strliof(path, "/"), &dir, &name);

  vfs_file_system_type *fs = *find_filesystem(fstype);
  vfs_mount *mnt = fs->mount(fs, fstype, name);
  nameidata *nd = path_walk(dir);

  list_add_tail(&mnt->mnt_mountpoint->d_sibling, &nd->dentry->d_subdirs);
  list_add_tail(&mnt->sibling, &vfsmntlist);

  return mnt;
}

void init_rootfs(vfs_file_system_type *fs_type, char *dev_name)
{
  vfs_mount *mnt = fs_type->mount(fs_type, dev_name, "/");
  list_add_tail(&mnt->sibling, &vfsmntlist);

  current_process->fs->d_root = mnt->mnt_root;
  current_process->fs->mnt_root = mnt;
}

// NOTE: MQ 2019-07-24
// we use device mounted name as identifier https://en.wikibooks.org/wiki/Guide_to_Unix/Explanations/Filesystems_and_Swap#Disk_Partitioning
void vfs_init(vfs_file_system_type *fs, char *dev_name)
{
  INIT_LIST_HEAD(&vfsmntlist);

  init_ext2_fs();
  init_rootfs(fs, dev_name);

  init_tmpfs();
  chrdev_init();
}