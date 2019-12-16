#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cpu/hal.h"
#include "cpu/gdt.h"
#include "cpu/idt.h"
#include "cpu/pit.h"
#include "cpu/tss.h"
#include "cpu/exception.h"
#include "system/sysapi.h"
#include "system/time.h"
#include "proc/task.h"
#include "devices/kybrd.h"
#include "devices/mouse.h"
#include "devices/pci.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "memory/malloc.h"
#include "devices/ata.h"
#include "fs/vfs.h"
#include "fs/ext2/ext2.h"
#include "devices/char/memory.h"
#include "system/console.h"
#include "multiboot2.h"

extern thread *current_thread;
extern vfs_file_system_type ext2_fs_type;

void kernel_init()
{
  // setup random's seed
  srand(get_seconds(NULL));

  // FIXME: MQ 2019-11-19 ata_init is not called in pci_scan_buses without enabling -O2
  // pci_scan_buses();
  ata_init();

  vfs_init(&ext2_fs_type, "/dev/hda");
  chrdev_memory_init();

  uint32_t fdrandom = vfs_open("/dev/random");
  char *rand = malloc(10);
  vfs_fread(fdrandom, rand, 10);

  console_setup();
  printf("hello world");

  // register system apis
  syscall_init();

  process_load("window server", "/bin/window_server");

  // idle
  update_thread(current_thread, WAITING);
  schedule();

  for (;;)
    ;
}

int kernel_main(unsigned long addr, unsigned long magic)
{
  if (magic != MULTIBOOT2_BOOTLOADER_MAGIC)
  {
    return -1;
  }

  struct multiboot_tag_basic_meminfo *multiboot_meminfo;
  struct multiboot_tag_mmap *multiboot_mmap;
  struct multiboot_tag_framebuffer *multiboot_framebuffer;

  struct multiboot_tag *tag;
  for (tag = (struct multiboot_tag *)(addr + 8);
       tag->type != MULTIBOOT_TAG_TYPE_END;
       tag = (struct multiboot_tag *)((multiboot_uint8_t *)tag + ((tag->size + 7) & ~7)))
  {
    switch (tag->type)
    {
    case MULTIBOOT_TAG_TYPE_BASIC_MEMINFO:
      multiboot_meminfo = (struct multiboot_tag_basic_meminfo *)tag;
      break;
    case MULTIBOOT_TAG_TYPE_MMAP:
    {
      multiboot_mmap = (struct multiboot_tag_mmap *)tag;
      break;
    }
    case MULTIBOOT_TAG_TYPE_FRAMEBUFFER:
    {
      multiboot_framebuffer = (struct multiboot_tag_framebuffer *)tag;
      break;
    }
    }
  }

  // gdt including kernel, user and tss
  gdt_init();
  install_tss(5, 0x10, 0);

  // register irq and handlers
  idt_init();

  // physical memory and paging
  pmm_init(multiboot_meminfo, multiboot_mmap);
  vmm_init();

  exception_init();

  // timer and keyboard
  pit_init();
  kkybrd_install();
  mouse_init();

  console_init(multiboot_framebuffer);

  // enable interrupts to start irqs (timer, keyboard)
  enable_interrupts();

  task_init(kernel_init);

  for (;;)
    ;

  return 0;
}
