#include "vmm.h"

#define PAGE_DIRECTORY_INDEX(x) (((x) >> 22) & 0x3ff)
#define PAGE_TABLE_INDEX(x) (((x) >> 12) & 0x3ff)
#define PAGE_GET_PHYSICAL_ADDRESS(x) (*x & ~0xfff)

void vmm_init_and_map(pdirectory *, virtual_addr, physical_addr);
void pt_entry_add_attrib(pt_entry *, uint32_t);
void pt_entry_set_frame(pt_entry *, uint32_t);
void pd_entry_add_attrib(pd_entry *, uint32_t);
void pd_entry_set_frame(pd_entry *, uint32_t);
void vmm_create_page_table(pdirectory *dir, uint32_t virt, uint32_t flags);
void vmm_paging(pdirectory *);

pdirectory *_current_dir;

void vmm_init()
{
  // initialize page table directory
  size_t directory_frames = div_ceil(sizeof(pdirectory), PMM_FRAME_SIZE);
  pdirectory *directory = (pdirectory *)pmm_alloc_blocks(directory_frames);
  memset(directory, 0, sizeof(pdirectory));

  vmm_init_and_map(directory, 0x00000000, 0x00000000);
  vmm_init_and_map(directory, 0xC0000000, 0x00100000);

  vmm_paging(directory);
}

void vmm_init_and_map(pdirectory *directory, virtual_addr virtual, physical_addr frame)
{
  int ptable_frame_count = div_ceil(sizeof(ptable), PMM_FRAME_SIZE);

  ptable *table = (ptable *)pmm_alloc_blocks(ptable_frame_count);
  memset(table, 0, sizeof(table));

  virtual_addr ivirtual = virtual;
  physical_addr iframe = frame;
  for (int i = 0; i < 1024; ++i, ivirtual += 4096, iframe += 4096)
  {
    pt_entry *entry = &table->m_entries[PAGE_TABLE_INDEX(ivirtual)];
    pt_entry_add_attrib(entry, I86_PTE_PRESENT | I86_PTE_WRITABLE | I86_PTE_USER);
    pt_entry_set_frame(entry, iframe);
  }

  pd_entry *entry = &directory->m_entries[PAGE_DIRECTORY_INDEX(virtual)];
  pd_entry_add_attrib(entry, I86_PDE_PRESENT | I86_PDE_WRITABLE | I86_PDE_USER);
  pd_entry_set_frame(entry, table);
}

void vmm_paging(pdirectory *directory)
{
  _current_dir = directory;
  __asm__ __volatile__("mov %0, %%cr3         \n"
                       "mov %%cr0, %%ecx      \n"
                       "or $0x80000000, %%ecx \n"
                       "mov %%ecx, %%cr0      \n" ::"r"(&directory->m_entries));
}

void pt_entry_add_attrib(pt_entry *e, uint32_t attr)
{
  *e |= attr;
}

void pt_entry_set_frame(pt_entry *e, uint32_t addr)
{
  *e = (*e & ~I86_PTE_FRAME) | addr;
}

void pd_entry_add_attrib(pd_entry *e, uint32_t attr)
{
  *e |= attr;
}

void pd_entry_set_frame(pd_entry *e, uint32_t addr)
{
  *e = (*e & ~I86_PDE_FRAME) | addr;
}

pdirectory *vmm_get_directory()
{
  return _current_dir;
}

void vmm_map_phyiscal_address(pdirectory *dir, uint32_t virt, uint32_t phys, uint32_t flags)
{
  if (!dir->m_entries[PAGE_DIRECTORY_INDEX(virt)])
    vmm_create_page_table(dir, virt, flags);

  ptable *pt = dir->m_entries[PAGE_DIRECTORY_INDEX(virt)] & ~0xfff;
  pt_entry *entry = &pt->m_entries[PAGE_TABLE_INDEX(virt)];
  pt_entry_set_frame(entry, phys);
  pt_entry_add_attrib(entry, flags);
}

void vmm_create_page_table(pdirectory *dir, uint32_t virt, uint32_t flags)
{
  if (dir->m_entries[PAGE_DIRECTORY_INDEX(virt)])
    return;

  int ptable_frame_count = div_ceil(sizeof(ptable), PMM_FRAME_SIZE);

  ptable *table = (ptable *)pmm_alloc_blocks(ptable_frame_count);
  memset(table, 0, sizeof(table));

  pd_entry *pd = &dir->m_entries[PAGE_DIRECTORY_INDEX(virt)];
  pd_entry_set_frame(pd, table);
  pd_entry_add_attrib(pd, flags);
}