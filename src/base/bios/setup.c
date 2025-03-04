/*
 * (C) Copyright 1992, ..., 2014 the "DOSEMU-Development-Team".
 *
 * for details see file COPYING in the DOSEMU distribution
 */

/* This is a C implementation of the BIOS memory setup. The int vector
   table and variables at 0040:xxxx are initialized. */

#include "emu.h"
#include "bios.h"
#include "memory.h"
#include "hlt.h"
#include "coopth.h"
#include "lowmem.h"
#include "int.h"
#include "iodev.h"
#include "virq.h"
#include "vint.h"
#include "emm.h"
#include "xms.h"
#include "hma.h"
#include "emudpmi.h"
#include "ipx.h"
#include "serial.h"
#include "joystick.h"
#include "utilities.h"
#include "doshelpers.h"
#include "mhpdbg.h"
#include "plugin_config.h"

static int li_tid;
unsigned int bios_configuration;

static void install_int_10_handler (void)
{
  unsigned int ptr;

  if (!config.mouse.intdrv) return;
  /* grab int10 back from video card for mouse */
  ptr = SEGOFF2LINEAR(BIOSSEG, bios_f000_int10_old);
  m_printf("ptr is at %x; ptr[0] = %x, ptr[1] = %x\n",ptr,READ_WORD(ptr),READ_WORD(ptr+2));
  WRITE_WORD(ptr, IOFF(0x10));
  WRITE_WORD(ptr + 2, ISEG(0x10));
  m_printf("after store, ptr[0] = %x, ptr[1] = %x\n",READ_WORD(ptr),READ_WORD(ptr+2));
  /* Otherwise this isn't safe */
  SETIVEC(0x10, INT10_WATCHER_SEG, INT10_WATCHER_OFF);
}

/*
 * DANG_BEGIN_FUNCTION bios_mem_setup
 *
 * description:
 *  Set up all memory areas as would be present on a typical i86 during
 * the boot phase.
 *
 * DANG_END_FUNCTION
 */
static inline void bios_mem_setup(void)
{
  int day_rollover;
  int b;

  video_mem_setup();
  serial_mem_setup();
  printer_mem_setup();

  WRITE_DWORD(BIOS_TICK_ADDR, get_linux_ticks(0, &day_rollover));
  WRITE_BYTE(TICK_OVERFLOW_ADDR, day_rollover);

  /* show 0 serial ports and 3 parallel ports, maybe a mouse, game card and the
   * configured number of floppy disks
   */
  CONF_NFLOP(bios_configuration, config.fdisks);
  CONF_NSER(bios_configuration, _min(config.num_ser, NUM_COMS));
  CONF_NLPT(bios_configuration, _min(config.num_lpt, NUM_LPTS));
  if (config.mouse.intdrv)
    bios_configuration |= CONF_MOUSE;

  bios_configuration |= CONF_DMA;
  if (joy_exist())
    bios_configuration |= CONF_GAME;

  if (config.mathco)
    bios_configuration |= CONF_MATHCO;

  g_printf("CONFIG: 0x%04x    binary: ", bios_configuration);
  for (b = 15; b >= 0; b--)
    g_printf("%s%s", (bios_configuration & (1 << b)) ? "1" : "0", (b%4) ? "" : " ");
  g_printf("\n");

  WRITE_WORD(BIOS_CONFIGURATION, bios_configuration);
  WRITE_WORD(BIOS_MEMORY_SIZE, config.mem_size);	/* size of memory */
  WRITE_BYTE(BIOS_HARDDISK_COUNT, config.hdisks);
}

static int initialized;
static void dosemu_reset(void);
static void bios_setup(void);

static void late_init_thr(void *arg)
{
  if (initialized)
    return;
  /* if something else is to be added here,
   * add the "late_init" member into dev_list instead */
  virq_setup();
  vint_setup();
  pit_late_init();
  video_late_init();
  mouse_late_init();
  mouse_client_post_init();

  initialized = 1;
}

void post_hook(void)
{
  LWORD(eip)++; // skip hlt
  dosemu_reset();
  bios_setup();

  /* late_init can call int10, so make it a thread */
  coopth_start(li_tid, NULL);
}

static void bios_setup(void)
{
  int i;

  /* initially, no HMA */
  set_a20(0);

  /* init trapped interrupts called via jump */
  for (i = 0; i < 256; i++) {
    if (config.vga) {
      uint16_t seg, off;
      unsigned int addr;

      seg = int_bios_area[i] >> 16;
      off = int_bios_area[i] & 0xffff;
      v_printf("int0x%x was 0x%04x:0x%04x\n", i, seg, off);
      addr = SEGOFF2LINEAR(seg, off);
      if (addr >= VBIOS_START && addr < VBIOS_START + VBIOS_SIZE) {
	v_printf("Setting int0x%x to 0x%04x:0x%04x\n", i, seg, off);
	SETIVEC(i, seg, off);
	continue;
      }
    }

    switch (i) {
    case 0x60 ... 0x67:
    case 0x79 ... 0xff:
      /* interrupts >= 0xc0 are NULL unless defined by DOSEMU */
      SETIVEC(i, 0, 0);
      break;
    case 0x68 ... 0x6f:
      /* 0x68-0x6f are usually set to iret */
      SETIVEC(i, IRET_SEG, IRET_OFF);
      break;
    case 0x70 ... 0x78:
      SETIVEC(i, BIOSSEG, EOI2_OFF);
      break;
    case 0 ... 7:
    case 0x10 ... 0x5f:
      SETIVEC(i, BIOSSEG, INT_OFF(i));
      break;
    case 8 ... 0x0f:
      SETIVEC(i, BIOSSEG, EOI_OFF);
      break;
    }
  }

  SETIVEC(DOS_HELPER_INT, BIOSSEG, INT_OFF(DOS_HELPER_INT));
  SETIVEC(0xe7, BIOSSEG, INT_OFF(0xe7));
  SETIVEC(0x09, INT09_SEG, INT09_OFF);
  SETIVEC(0x08, INT08_SEG, INT08_OFF);
  /* 0x30 and 0x31 are not vectors, they are the 5-byte long jump.
   * While 0x30 is overwritten entirely, only one byte is overwritten
   * in 0x31. We need to zero it out so that it at least does not
   * point into random bios location. */
  SETIVEC(0x31, 0, 0);
  SETIVEC(0x70, INT70_SEG, INT70_OFF);
  SETIVEC(0x71, INT71_SEG, INT71_OFF);
  SETIVEC(0x1e, INT1E_SEG, INT1E_OFF);
  SETIVEC(0x41, INT41_SEG, INT41_OFF);
  SETIVEC(0x46, INT46_SEG, INT46_OFF);
  SETIVEC(0x75, INT75_SEG, INT75_OFF);

  if (config.ems_size)
    SETIVEC(0x67, BIOSSEG, INT_OFF(0x67));
  if (config.pktdrv)
    SETIVEC(0x60, PKTDRV_SEG, PKTDRV_OFF);
  if (config.ipxsup)
    SETIVEC(0x7a, BIOSSEG, INT_OFF(0x7a));
  if (config.mouse.intdrv)
    SETIVEC(0x74, BIOSSEG, Mouse_ROUTINE_OFF);

  /* set up PIC */
  port_outb(0x20, 0x10);   // ICW1
  port_outb(0x21, 8);      // ICW2, set irq to 8
  port_outb(0x21, 1 << 2); // ICW3m, slave on irq2
  port_outb(0xa0, 0x10);   // ICW1
  port_outb(0xa1, 0x70);   // ICW2, set irq to 0x70
  port_outb(0xa1, 2);      // ICW3s, master uses irq2
  /* mask out SB irqs or Blood game crashes */
  if (config.sound)
    port_outb(0x21, (1 << config.sb_irq) | port_inb(0x21));

  /* Install new handler for video-interrupt into bios_f000_int10ptr,
   * for video initialization at f800:4200
   * If config_vbios_seg=0xe000 -> e000:3, else c000:3
   * Next will be the call to int0xe6,al=8 which starts video BIOS init
   */
  install_int_10_handler();

  bios_mem_setup();		/* setup values in BIOS area */
}

static void dosemu_reset(void)
{
  initialized = 0;
  dos_post_boot_reset();
  iodev_reset();		/* reset all i/o devices          */
  commands_plugin_inte6_reset();
  lowmem_reset();		/* release memory used by helper utilities */
#ifdef USE_MHPDBG
  mhp_debug(DBG_BOOT, 0, 0);
#endif
}

void bios_setup_init(void)
{
  li_tid = coopth_create("late_init", late_init_thr);
}
