#include "vmm.h"

#define PAGE_DIRECTORY_BASE 0xFFFFF000
#define PAGE_TABLE_BASE 0xFFC00000

#define get_page_directory_index(x) (((x) >> 22) & 0x3ff)
#define get_page_table_entry_index(x) (((x) >> 12) & 0x3ff)
#define get_physical_address(x) (*x & ~0xfff)
#define is_page_enabled(x) (x & 0x1)

void vmm_init_and_map(pdirectory *, virtual_addr, physical_addr);
void pt_entry_add_attrib(pt_entry *, uint32_t);
void pt_entry_set_frame(pt_entry *, uint32_t);
void pd_entry_add_attrib(pd_entry *, uint32_t);
void pd_entry_set_frame(pd_entry *, uint32_t);
void vmm_create_page_table(pdirectory *dir, uint32_t virt, uint32_t flags);
void vmm_paging(pdirectory *, virtual_addr);

extern uint32_t memory_bitmap_size;

pdirectory *_current_dir;

void vmm_flush_tlb_entry(virtual_addr addr)
{

  __asm__ __volatile__("invlpg (%0)" ::"r"(addr)
                       : "memory");
}

/*
  Memory layout of our address space
  +-------------------------+ 0xFFFFFFFF
  | Page table mapping      |
  |_________________________| 0xFFC00000
  |                         |
  |-------------------------| 0xF0000000
  |                         |
  | Device drivers          |
  |-------------------------| 0xE0000000
  |                         |
  |                         |
  | Kernel heap             |
  |                         |
  |_________________________| 0xD0000000
  |                         | 
  | Kernel itself           |
  |_________________________| 0xC0000000
  |                         |
  | User stack              |
  |_________________________| 0xBFC00000
  |                         |
  | User thread stack       |
  |_________________________|
  |                         |
  |                         |
  |                         |
  |                         |
  |                         |
  |_________________________| 0x00400000
  |                         |
  | Identiy mapping         |
  +_________________________+ 0x00000000
*/

void vmm_init()
{
  // initialize page table directory

  physical_addr pa_dir = pmm_alloc_block();
  pdirectory *va_dir = (pdirectory *)(pa_dir + KERNEL_HIGHER_HALF);
  memset(va_dir, 0, sizeof(pdirectory));

  vmm_init_and_map(va_dir, 0xC0000000, 0x00000000);

  // for (int i = 0; i < 256; ++i)
  //   vmm_init_and_map(directory, 0xC0000000 + PMM_FRAME_SIZE * 1024 + i, 0x00100000 + PMM_FRAME_SIZE * 1024 * i);

  // NOTE: MQ 2019-05-08 Using the recursive page directory trick when paging (map last entry to directory)
  va_dir->m_entries[1023] = (pa_dir & 0xFFFFF000) | I86_PTE_PRESENT | I86_PTE_WRITABLE;

  vmm_paging(va_dir, pa_dir);
}

void vmm_init_and_map(pdirectory *va_dir, virtual_addr virtual, physical_addr frame)
{
  physical_addr pa_table = pmm_alloc_block();
  ptable *va_table = (ptable *)(pa_table + KERNEL_HIGHER_HALF);
  memset(va_table, 0, sizeof(ptable));

  virtual_addr ivirtual = virtual;
  physical_addr iframe = frame;

  for (int i = 0; i < 1024; ++i, ivirtual += PMM_FRAME_SIZE, iframe += PMM_FRAME_SIZE)
  {
    pt_entry *entry = &va_table->m_entries[get_page_table_entry_index(ivirtual)];
    *entry = iframe | I86_PTE_PRESENT | I86_PTE_WRITABLE;
    pmm_mark_used_addr(iframe);
  }

  pd_entry *entry = &va_dir->m_entries[get_page_directory_index(virtual)];
  *entry = pa_table | I86_PDE_PRESENT | I86_PDE_WRITABLE;
}

void vmm_paging(pdirectory *va_dir, physical_addr pa_dir)
{
  _current_dir = va_dir;

  __asm__ __volatile__("mov %0, %%cr3           \n"
                       "mov %%cr4, %%ecx        \n"
                       "and $~0x00000010, %%ecx \n"
                       "mov %%ecx, %%cr4        \n"
                       "mov %%cr0, %%ecx        \n"
                       "or $0x80000000, %%ecx   \n"
                       "mov %%ecx, %%cr0        \n" ::"r"(pa_dir));
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

pdirectory *create_address_space(pdirectory *current)
{
  pdirectory *dir = pmm_alloc_block();

  if (!dir)
    return NULL;
  memset(dir, 0, sizeof(pdirectory));
  memcpy(dir->m_entries[768], current->m_entries[768], 256 * sizeof(pd_entry));
  return dir;
}

pdirectory *vmm_get_directory()
{
  return _current_dir;
}

/*
  NOTE: MQ 2019-05-08
  cr3 -> 0xsomewhere (physical address): | 0 | 1 | ... | 1023 | (1023th pd's value is 0xsomewhere)
  When translating a virtual address vAddr:
    + de is ath page directory index (vAddr >> 22)
    + te is bth page table index (vAddr >> 12 & 0x1111111111)
  If pd[de] is not present, getting 4KB free from pmm_alloc_block() and assigning it at page directory entry

  mmus' formula: pd = cr3(); pt = *(pd+4*PDX); page = *(pt+4*PTX)
  The issue is we cannot directly access *(pd+4*PDX) and access/modify a page table entry because everything(kernel) is now a virtual address
  To overcome the egg-chicken's issue, we set 1023pde=cr3 

  For example:
  0xFFC00000 + de * 0x1000 is mapped to pd[de] (physical address). As you know virtual <-> physical address is 4KB aligned
  0xFFC00000 + de * 0x1000 + te * 0x4 is mapped to pd[de] + te * 0x4 (this is what mmu will us to translate vAddr)
  0xFFC00000 + de * 0x1000 + te * 0x4 = xxx <-> *(pt+4*ptx) = xxx
*/
void vmm_map_phyiscal_address(pdirectory *va_dir, uint32_t virt, uint32_t phys, uint32_t flags)
{
  if (!is_page_enabled(va_dir->m_entries[get_page_directory_index(virt)]))
    vmm_create_page_table(va_dir, virt, flags);

  uint32_t *table = PAGE_TABLE_BASE + get_page_directory_index(virt) * PMM_FRAME_SIZE;
  uint32_t tindex = get_page_table_entry_index(virt);

  table[tindex] = phys | flags;
}

void vmm_create_page_table(pdirectory *va_dir, uint32_t virt, uint32_t flags)
{
  if (is_page_enabled(va_dir->m_entries[get_page_directory_index(virt)]))
    return;

  physical_addr pa_table = pmm_alloc_block();

  va_dir->m_entries[get_page_directory_index(virt)] = pa_table | flags;
  vmm_flush_tlb_entry(virt);

  memset(PAGE_TABLE_BASE + get_page_directory_index(virt) * PMM_FRAME_SIZE, 0, sizeof(ptable));
}

uint32_t kernel_stack_index = 0;
void *create_kernel_stack(int32_t blocks)
{
#define KERNEL_STACK_ALLOC_TOP 0xC0000000

  physical_addr paddr;
  virtual_addr vaddr;

  paddr = pmm_alloc_blocks(blocks);

  if (!paddr)
    return 0;

  kernel_stack_index += blocks;
  vaddr = KERNEL_STACK_ALLOC_TOP + kernel_stack_index * PMM_FRAME_SIZE;

  for (int i = 0; i < blocks; ++i)
    vmm_map_phyiscal_address(vmm_get_directory(),
                             vaddr + i * PMM_FRAME_SIZE,
                             paddr + i * PMM_FRAME_SIZE, I86_PTE_PRESENT | I86_PTE_WRITABLE);

  return vaddr + blocks * PMM_FRAME_SIZE - 4;
}