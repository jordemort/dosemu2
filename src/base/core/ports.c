/*
 * SIDOC_BEGIN_MODULE
 *
 * Description: New port handling code for DOSEMU
 *
 * Maintainers: Alberto Vignani (vignani@mail.tin.it)
 *
 * REMARK
 * This is the code that allows and disallows port access within DOSEMU.
 * The BOCHS port IO code was actually very cleverly done.  So the idea
 * was stolen from there.
 *
 * This port I/O code (previously in portss.c, from Scott Bucholz) is based on
 * a table access instead of a switch statement. This method is much more
 * clean and easy to maintain, while not slower than a switch.
 *
 * Remains of the old code are emerging here and there, they will
 * hopefully be moved back to where they belong, mainly video code.
 * /REMARK
 *
 * SIDOC_END_MODULE
 *
 */

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#ifdef HAVE_SYS_IO_H
#include <sys/io.h>
#endif
#include <sys/types.h>
#include <sys/wait.h>

#include "emu.h"
#include "port.h"
#include "timers.h"
#include "video.h"
#include "vgaemu.h"	/* for video retrace */
#include "bios.h"
#include "serial.h"
#include "bitops.h"
#include "mapping.h"
#include "dosemu_config.h"
#include "sig.h"
#ifdef X86_EMULATOR
#include "cpu-emu.h"
#include "bitops.h"
#endif

_port_handler port_handler[EMU_MAX_IO_DEVICES];
unsigned char port_handle_table[0x10000];
unsigned char port_andmask[0x10000];
unsigned char port_ormask[0x10000];
static unsigned char portfast_map[0x10000/8];
unsigned char emu_io_bitmap[0x10000/8];
static pid_t portserver_pid = 0;

static unsigned char port_handles;	/* number of io_handler's */

int in_crit_section = 0;
static const char *crit_sect_caller;

#define SET_HANDLE(p,h)		port_handle_table[(Bit16u)(p)]=(h)
#define EMU_HANDLER(port)	port_handler[port_handle_table[(Bit16u)(port)]]
enum{TYPE_INB, TYPE_OUTB, TYPE_INW, TYPE_OUTW, TYPE_IND, TYPE_OUTD, TYPE_PCI, TYPE_EXIT};

/* ---------------------------------------------------------------------- */
/* PORT TRACING								  */

#if 0
static long nyb2bin[16] =
{
	0x30303030, 0x31303030, 0x30313030, 0x31313030,
	0x30303130, 0x31303130, 0x30313130, 0x31313130,
	0x30303031, 0x31303031, 0x30313031, 0x31313031,
	0x30303131, 0x31303131, 0x30313131, 0x31313131
};

static char *
 p2bin(unsigned char c)
{
	static char s[16] = "   [00000000]";

	((uint32_t *) s)[1] = nyb2bin[(c >> 4) & 15];
	((uint32_t *) s)[2] = nyb2bin[c & 15];

	return s + 3;
}
#endif

#define PORTLOG_MAXBITS		16
#define PORTLOG_MASK		((1 << PORTLOG_MAXBITS) - 1)
#define SIZE_PORTLOGMAP		(1 << (PORTLOG_MAXBITS -3))
static unsigned long *portlog_map = 0;

void register_port_traceing(ioport_t firstport, ioport_t lastport)
{
  firstport &= PORTLOG_MASK;
  lastport &= PORTLOG_MASK;
  if (lastport < firstport) return;
  init_port_traceing();
  T_printf ("PORT: traceing 0x%x-0x%x\n",firstport,lastport);
  for (; firstport <= lastport; firstport++) {
    set_bit(firstport, portlog_map);
  }
}

void clear_port_traceing(void)
{
  if (!portlog_map) portlog_map = malloc(SIZE_PORTLOGMAP);
  memset(portlog_map, 0, SIZE_PORTLOGMAP);
}

void init_port_traceing(void)
{
  if (portlog_map) return;
  clear_port_traceing();
}

#define TT_printf(p,f,v,m) ({ \
  if (debug_level('T') && (test_bit(p, portlog_map) || debug_level('T') >= 5)) { \
    log_printf(1, "%hx %c %x\n", (unsigned short)p, f, v & m); \
  } \
})

static Bit8u log_port_read(ioport_t port, Bit8u r)
{
  TT_printf(port, '>', r, 0xff);
  return r;
}

static Bit16u log_port_read_w(ioport_t port, Bit16u r)
{
  TT_printf(port, '}', r, 0xffff);
  return r;
}

static Bit32u log_port_read_d(ioport_t port, Bit32u r)
{
  TT_printf(port, ']', r, 0xffffffff);
  return r;
}

static void log_port_write(ioport_t port, Bit8u w)
{
  TT_printf(port, '<', w, 0xff);
}

static void log_port_write_w(ioport_t port, Bit16u w)
{
  TT_printf(port, '{', w, 0xffff);
}

static void log_port_write_d(ioport_t port, Bit32u w)
{
  TT_printf(port, '[', w, 0xffffffff);
}

#define LOG_PORT_READ(port, r) (debug_level('T') ? log_port_read(port, r) : r)
#define LOG_PORT_READ_W(port, r) (debug_level('T') ? log_port_read_w(port, r) : r)
#define LOG_PORT_READ_D(port, r) (debug_level('T') ? log_port_read_d(port, r) : r)

#define LOG_PORT_WRITE(port, w) do{ if (debug_level('T')) log_port_write(port, w); }while(0)
#define LOG_PORT_WRITE_W(port, w) do{ if (debug_level('T')) log_port_write_w(port, w); }while(0)
#define LOG_PORT_WRITE_D(port, w) do{ if (debug_level('T')) log_port_write_d(port, w); }while(0)

/* ---------------------------------------------------------------------- */
/* SIDOC_BEGIN_REMARK
 *
 * The following port_{in|out}{bwd} functions are the main entry points to
 * the port code. They look into the port_handle_table and call the
 * appropriate code, usually the std_port_ functions, but each device is
 * free to register its own functions which in turn will call std_port or
 * directly access I/O (like video code does), or emulate it - AV
 *
 * SIDOC_END_REMARK
 */
/*
 * SIDOC_BEGIN_FUNCTION port_inb(ioport_t port)
 *
 * Handles/simulates an inb() port IO read
 *
 * SIDOC_END_FUNCTION
 */
Bit8u port_inb(ioport_t port)
{
	Bit8u res;
	res = EMU_HANDLER(port).read_portb(port, EMU_HANDLER(port).arg);
	return LOG_PORT_READ(port, res);
}

/*
 * SIDOC_BEGIN_FUNCTION port_outb(ioport_t port, Bit8u byte)
 *
 * Handles/simulates an outb() port IO write
 *
 * SIDOC_END_FUNCTION
 */
void port_outb(ioport_t port, Bit8u byte)
{
	LOG_PORT_WRITE(port, byte);
	EMU_HANDLER(port).write_portb(port, byte, EMU_HANDLER(port).arg);
}

/*
 * SIDOC_BEGIN_FUNCTION port_inw(ioport_t port)
 *
 * Handles/simulates an inw() port IO read.  Usually this invokes
 * port_inb() twice, but it may be necessary to do full word i/o for
 * some video boards.
 *
 * SIDOC_END_FUNCTION
 */
Bit16u port_inw(ioport_t port)
{
	Bit16u res;

	if (EMU_HANDLER(port).read_portw != NULL) {
		res = EMU_HANDLER(port).read_portw(port, EMU_HANDLER(port).arg);
		return LOG_PORT_READ_W(port, res);
	}
	else {
		res = (Bit16u) port_inb(port) | (((Bit16u) port_inb(port + 1)) << 8);
	}
	return res;
}

/*
 * SIDOC_BEGIN_FUNCTION port_outw(ioport_t port, Bit16u word)
 *
 * Handles/simulates an outw() port IO write
 *
 * SIDOC_END_FUNCTION
 */
void port_outw(ioport_t port, Bit16u word)
{
	if (EMU_HANDLER(port).write_portw != NULL) {
		LOG_PORT_WRITE_W(port, word);
		EMU_HANDLER(port).write_portw(port, word, EMU_HANDLER(port).arg);
	}
	else {
		port_outb(port, word & 0xff);
		port_outb(port+1, (word >> 8) & 0xff);
	}
}

/*
 * SIDOC_BEGIN_FUNCTION port_ind(ioport_t port)
 * SIDOC_BEGIN_FUNCTION port_outd(ioport_t port, Bit32u dword)
 *
 * Handles/simulates an ind()/outd() port IO read/write.
 *
 * SIDOC_END_FUNCTION
 */
Bit32u port_ind(ioport_t port)
{
	Bit32u res;

	if (EMU_HANDLER(port).read_portd != NULL) {
		res = EMU_HANDLER(port).read_portd(port, EMU_HANDLER(port).arg);
	}
	else {
		res = (Bit32u) port_inw(port) | (((Bit32u) port_inw(port + 2)) << 16);
	}
	return LOG_PORT_READ_D(port, res);
}

void port_outd(ioport_t port, Bit32u dword)
{
	LOG_PORT_WRITE_D(port, dword);
	if (EMU_HANDLER(port).write_portd != NULL) {
		EMU_HANDLER(port).write_portd(port, dword, EMU_HANDLER(port).arg);
	}
	else {
		port_outw(port, dword & 0xffff);
		port_outw(port+2, (dword >> 16) & 0xffff);
	}
}


/* ---------------------------------------------------------------------- */
/* the following functions are all static!				  */

static void pna_emsg(ioport_t port, char ch, const char *s)
{
	i_printf("PORT%c: %x not available for %s\n", ch, port, s);
}

static void check_crit_section(ioport_t port, const char *function)
{
	if (in_crit_section) {
		error("Port %#x is not available (%s), \"%s\" failed.\n"
			"Adjust your dosemu.conf\n",
			port, function, crit_sect_caller);
		in_crit_section = 0;
		leavedos(46);
	}
}

static Bit8u port_not_avail_inb(ioport_t port, void *arg)
{
/* it is a fact of (hardware) life that unused locations return all
   (or almost all) the bits at 1; some software can try to detect a
   card basing on this fact and fail if it reads 0x00 - AV

   The joystick code is dependent on 0xff as joystick.c:r1.4
   (2005-04-08) stopped registering port handlers if no joystick
   is initialised - Clarence Dang

   Also used for delays, so add some sleep. - stsp
*/
	if (debug_level('i')) pna_emsg(port,'b',"read");
//	idle(0, 50, 0, "inb");
	return 0xff;
}

static void port_not_avail_outb(ioport_t port, Bit8u byte, void *arg)
{
	check_crit_section(port, "outb");
	if (debug_level('i')) pna_emsg(port,'b',"write");
}

static Bit16u port_not_avail_inw(ioport_t port, void *arg)
{
	if (debug_level('i')) pna_emsg(port,'w',"read");
//	idle(0, 50, 0, "inw");
	return 0xffff;
}

static void port_not_avail_outw(ioport_t port, Bit16u value, void *arg)
{
	check_crit_section(port, "outw");
	if (debug_level('i')) pna_emsg(port,'w',"write");
}

static Bit32u port_not_avail_ind(ioport_t port, void *arg)
{
	if (debug_level('i')) pna_emsg(port,'d',"read");
//	idle(0, 50, 0, "ind");
	return 0xffffffff;
}

static void port_not_avail_outd(ioport_t port, Bit32u value, void *arg)
{
	check_crit_section(port, "outd");
	if (debug_level('i')) pna_emsg(port,'d',"write");
}


/* ---------------------------------------------------------------------- */
/* default port I/O access
 */

struct portreq
{
        ioport_t port;
        int type;
        unsigned long word;
};

static int port_fd_out[2] = {-1, -1};
static int port_fd_in[2] = {-1, -1};

Bit8u std_port_inb(ioport_t port)
{
        struct portreq pr;

        if (current_iopl == 3 || test_bit(port, emu_io_bitmap)) {
		return port_real_inb(port);
	}
	if (!portserver_pid) {
		error ("std_port_inb(0x%X): port server unavailable\n", port);
		return port_not_avail_inb (port, NULL);
	}
	pr.port = port;
	pr.type = TYPE_INB;
	write(port_fd_out[1], &pr, sizeof(pr));
	read(port_fd_in[0], &pr, sizeof(pr));
	return pr.word;
}

static Bit8u std_port_inb_h(ioport_t port, void *arg)
{
	return std_port_inb(port);
}

void std_port_outb(ioport_t port, Bit8u byte)
{
        struct portreq pr;

        if (current_iopl == 3 || test_bit(port, emu_io_bitmap)) {
		port_real_outb(port, byte);
		return;
        }
	if (!portserver_pid) {
		error ("std_port_outb(0x%X,0x%X): port server unavailable\n",
		       port, byte);
		port_not_avail_outb (port, byte, NULL);
		return;
	}
        pr.word = byte;
        pr.port = port;
        pr.type = TYPE_OUTB;
	write(port_fd_out[1], &pr, sizeof(pr));
	read(port_fd_in[0], &pr, sizeof(pr));
}

static void std_port_outb_h(ioport_t port, Bit8u byte, void *arg)
{
	std_port_outb(port, byte);
}

Bit16u std_port_inw(ioport_t port)
{
        struct portreq pr;

        if (current_iopl == 3 || (test_bit(port, emu_io_bitmap) +
                                  test_bit(port + 1, emu_io_bitmap)
                                  == 2)) {
		return port_real_inw(port);
        }
	if (!portserver_pid) {
		error ("std_port_inw(0x%X): port server unavailable\n", port);
		return port_not_avail_inw (port, NULL);
	}
        pr.port = port;
        pr.type = TYPE_INW;
	write(port_fd_out[1], &pr, sizeof(pr));
	read(port_fd_in[0], &pr, sizeof(pr));
	return pr.word;
}

static Bit16u std_port_inw_h(ioport_t port, void *arg)
{
	return std_port_inw(port);
}

void std_port_outw(ioport_t port, Bit16u word)
{
        struct portreq pr;

        if (current_iopl == 3 || (test_bit(port, emu_io_bitmap) +
                                  test_bit(port + 1, emu_io_bitmap)
                                  == 2)) {
		port_real_outw(port, word);
		return;
        }
	if (!portserver_pid) {
		error ("std_port_outw(0x%X,0x%X): port server unavailable\n",
		       port, word);
		port_not_avail_outw (port, word, NULL);
		return;
	}
        pr.word = word;
        pr.port = port;
        pr.type = TYPE_OUTW;
	write(port_fd_out[1], &pr, sizeof(pr));
	read(port_fd_in[0], &pr, sizeof(pr));
}

static void std_port_outw_h(ioport_t port, Bit16u word, void *arg)
{
	std_port_outw(port, word);
}

Bit32u std_port_ind(ioport_t port)
{
        struct portreq pr;

        if (current_iopl == 3 || (test_bit(port, emu_io_bitmap) +
                                  test_bit(port + 1, emu_io_bitmap) +
                                  test_bit(port + 2, emu_io_bitmap) +
                                  test_bit(port + 3, emu_io_bitmap)
                                  == 4)) {
		return port_real_ind(port);
        }
	if (!portserver_pid) {
		error ("std_port_ind(0x%X): port server unavailable\n", port);
		return port_not_avail_ind (port, NULL);
	}
        pr.port = port;
        pr.type = TYPE_IND;
	write(port_fd_out[1], &pr, sizeof(pr));
	read(port_fd_in[0], &pr, sizeof(pr));
	return pr.word;
}

static Bit32u std_port_ind_h(ioport_t port, void *arg)
{
	return std_port_ind(port);
}

static int do_port_outd(ioport_t port, Bit32u dword, int pci)
{
        struct portreq pr;

        if (current_iopl == 3 || (test_bit(port, emu_io_bitmap) +
                                  test_bit(port + 1, emu_io_bitmap) +
                                  test_bit(port + 2, emu_io_bitmap) +
                                  test_bit(port + 3, emu_io_bitmap)
                                  == 4)) {
		port_real_outd(port, dword);
		return 0;
        }
	if (!portserver_pid) {
		error ("std_port_outd(0x%X,0x%X): port server unavailable\n",
		       port, dword);
		port_not_avail_outd (port, dword, NULL);
		return 0;
	}
        pr.word = dword;
        pr.port = port;
        pr.type = pci ? TYPE_PCI : TYPE_OUTD;
	write(port_fd_out[1], &pr, sizeof(pr));
	return 1;
}

void std_port_outd(ioport_t port, Bit32u dword)
{
        struct portreq pr;
	if (do_port_outd(port, dword, 0))
		read(port_fd_in[0], &pr, sizeof(pr));
}

static void std_port_outd_h(ioport_t port, Bit32u dword, void *arg)
{
	std_port_outd(port, dword);
}

void pci_port_outd(ioport_t port, Bit32u dword)
{
	do_port_outd(port, dword, 1);
}

/* ---------------------------------------------------------------------- */
/* SIDOC_BEGIN_REMARK
 *
 * optimized versions for rep - basically we avoid changing privileges
 * and iopl on and off lots of times. We are safe letting iopl=3 here
 * since we don't exit from this code until finished.
 * This code is shared between VM86 and DPMI.
 *
 * SIDOC_END_REMARK
 */

int port_rep_inb(ioport_t port, Bit8u *base, int df, Bit32u count)
{
	register int incr = df? -1: 1;
	Bit8u *dest = base;
	int count_ = count;

	if (count==0) return 0;
	i_printf("Doing REP insb(%#x) %d bytes at %p, DF %d\n", port,
		count, base, df);
	while (count--) {
	    *dest = port_inb(port);
	    dest += incr;
	}
	if (debug_level('T')) {
		dest = base;
		while (count_--) {
			(void)LOG_PORT_READ(port, *dest);
			dest += incr;
		}
	}
	return dest-base;
}

int port_rep_outb(ioport_t port, Bit8u *base, int df, Bit32u count)
{
	register int incr = df? -1: 1;
	Bit8u *dest = base;
	int count_ = count;

	if (count==0) return 0;
	i_printf("Doing REP outsb(%#x) %d bytes at %p, DF %d\n", port,
		count, base, df);
	while (count--) {
	    port_outb(port, *dest);
	    dest += incr;
	}
	if (debug_level('T')) {
		dest = base;
		while (count_--) {
			LOG_PORT_WRITE(port, *dest);
			dest += incr;
		}
	}
	return dest-base;
}

int port_rep_inw(ioport_t port, Bit16u *base, int df, Bit32u count)
{
	register int incr = df? -1: 1;
	Bit16u *dest = base;
	int count_ = count;

	if (count==0) return 0;
	i_printf("Doing REP insw(%#x) %d words at %p, DF %d\n", port,
		count, base, df);
	if (EMU_HANDLER(port).read_portw == NULL) {
	  Bit16u res;
	  while (count--) {
	    res = port_inb(port);
	    *dest = ((Bit16u)port_inb(port+1) <<8) | res;
	    dest += incr;
	  }
	}
	else {
	  while (count--) {
	    *dest = port_inw(port);
	    dest += incr;
	  }
	}
	if (debug_level('T')) {
		dest = base;
		while (count_--) {
			(void)LOG_PORT_READ_W(port, *dest);
			dest += incr;
		}
	}
	return (Bit8u *)dest-(Bit8u *)base;
}

int port_rep_outw(ioport_t port, Bit16u *base, int df, Bit32u count)
{
	register int incr = df? -1: 1;
	Bit16u *dest = base;
	int count_ = count;

	if (count==0) return 0;
	i_printf("Doing REP outsw(%#x) %d words at %p, DF %d\n", port,
		count, base, df);
	if (EMU_HANDLER(port).write_portw == NULL) {
	  Bit16u res;
	  while (count--) {
	    res = *dest, dest += incr;
	    port_outb(port, res);
	    port_outb(port+1, res>>8);
	  }
	}
	else {
	  while (count--) {
	    port_outw(port, *dest);
	    dest += incr;
	  }
	}
	if (debug_level('T')) {
		dest = base;
		while (count_--) {
			LOG_PORT_WRITE_W(port, *dest);
			dest += incr;
		}
	}
	return (Bit8u *)dest-(Bit8u *)base;
}

int port_rep_ind(ioport_t port, Bit32u *base, int df, Bit32u count)
{
	register int incr = df? -1: 1;
	Bit32u *dest = base;

	if (count==0) return 0;
	while (count--) {
	  *dest = port_ind(port);
	  (void)LOG_PORT_READ_D(port, *dest);
	  dest += incr;
	}
	return (Bit8u *)dest-(Bit8u *)base;
}

int port_rep_outd(ioport_t port, Bit32u *base, int df, Bit32u count)
{
	register int incr = df? -1: 1;
	Bit32u *dest = base;

	if (count==0) return 0;
	while (count--) {
	  port_outd(port, *dest);
	  LOG_PORT_WRITE_D(port, *dest);
	  dest += incr;
	}
	return (Bit8u *)dest-(Bit8u *)base;
}

/*
 * SIDOC_BEGIN_FUNCTION special_port_inb,special_port_outb
 *
 * I don't know what to do of this stuff... it was added incrementally to
 * port.c and has mainly to do with video code. This is not the right
 * place for it...
 * Anyway, this implements some HGC stuff for X and the emuretrace
 * port access for 0x3c0/0x3da
 *
 * SIDOC_END_FUNCTION
 */

static int r3da_pending = 0;

void do_r3da_pending (void)
{
  if (r3da_pending) {
    (void)std_port_inb(r3da_pending);
    r3da_pending = 0;
  }
}

static Bit8u special_port_inb(ioport_t port, void *arg)
{
	Bit8u res = 0xff;

        if (current_iopl == 3 || test_bit(port, emu_io_bitmap)) {
		return port_real_inb(port);
	}
	if ((port==0x3ba)||(port==0x3da)) {
		res = Misc_get_input_status_1();
		if (!r3da_pending && (config.emuretrace>1)) {
			r3da_pending = port;
		}
	}
	else
	if (port==0x3db)	/* light pen strobe reset */
		res = 0;
	return res;
}

static void special_port_outb(ioport_t port, Bit8u byte, void *arg)
{
        if (current_iopl == 3 || test_bit(port, emu_io_bitmap)) {
		port_real_outb(port, byte);
		return;
        }
	/* Port writes for enable/disable blinking character mode */
	if (port == 0x03c0) {
		static int last_index = -1;
		static int flip_flop = 1;

		/* SIDOC_BEGIN_REMARK
		 *
		 * This is the core of the new emuretrace algorithm:
		 * If a read of port 0x3da is performed we just set it
		 *  as pending and set ioperm OFF for port 0x3c0
		 * When a write to port 0x3c0 is then trapped, we perform
		 *  any pending read to 0x3da and reset the ioperm for
		 *  0x3c0 in the default ON state.
		 * This way we avoid extra port accesses when the program
		 * is only looking for the sync bits, and we don't miss
		 * the case where the read to 0x3da is used to reset the
		 * index/data flipflop for port 0x3c0. Further accesses to
		 * port 0x3c0 are handled at full speed.
		 *
		 * SIDOC_END_REMARK
		 */
		if (config.vga && (config.emuretrace>1)) {
		    if (r3da_pending) {
			(void)std_port_inb_h(r3da_pending, arg);
			r3da_pending = 0;
			std_port_outb_h(0x3c0, byte, arg);
			return;
		    }
		    goto defout;
		}
		flip_flop = !flip_flop;
		if (flip_flop) {
		/* JES This was last_index = 0x10..... WRONG? */
			vga.attr.data[last_index] = byte;
		}
		else {
			last_index = byte;
		}
		return;
	}

defout:
	std_port_outb_h (port, byte, arg);
}

/* ---------------------------------------------------------------------- */
/*
 * SIDOC_BEGIN_FUNCTION port_init()
 *
 * Resets all the port port_handler information.
 * This must be called before parsing the config file -
 * This must NOT be called again when warm booting!
 * Can't use debug logging, it is called too early.
 *
 * SIDOC_END_FUNCTION
 */
int port_init(void)
{
	int i;

	/* set unused elements to appropriate values */
	for (i=0; i < EMU_MAX_IO_DEVICES; i++) {
	  port_handler[i].read_portb   = NULL;
	  port_handler[i].write_portb  = NULL;
	  port_handler[i].read_portw   = NULL;
	  port_handler[i].write_portw  = NULL;
	  port_handler[i].read_portd   = NULL;
	  port_handler[i].write_portd  = NULL;
	}

  /* handle 0 maps to the unmapped IO device handler.  Basically any
     ports which don't map to any other device get mapped to this
     handler which does absolutely nothing.
   */
	port_handler[NO_HANDLE].read_portb = port_not_avail_inb;
	port_handler[NO_HANDLE].write_portb = port_not_avail_outb;
	port_handler[NO_HANDLE].read_portw = port_not_avail_inw;
	port_handler[NO_HANDLE].write_portw = port_not_avail_outw;
	port_handler[NO_HANDLE].read_portd = port_not_avail_ind;
	port_handler[NO_HANDLE].write_portd = port_not_avail_outd;
	port_handler[NO_HANDLE].handler_name = "unknown port";

  /* the STD handles will be in use by many devices, and their fd
     will always be -1
   */
	port_handler[HANDLE_STD_IO].read_portb = std_port_inb_h;
	port_handler[HANDLE_STD_IO].write_portb = std_port_outb_h;
	port_handler[HANDLE_STD_IO].read_portw = std_port_inw_h;
	port_handler[HANDLE_STD_IO].write_portw = std_port_outw_h;
	port_handler[HANDLE_STD_IO].read_portd = std_port_ind_h;
	port_handler[HANDLE_STD_IO].write_portd = std_port_outd_h;
	port_handler[HANDLE_STD_IO].handler_name = "std port io";

	port_handler[HANDLE_STD_RD].read_portb = std_port_inb_h;
	port_handler[HANDLE_STD_RD].write_portb = port_not_avail_outb;
	port_handler[HANDLE_STD_RD].read_portw = std_port_inw_h;
	port_handler[HANDLE_STD_RD].write_portw = port_not_avail_outw;
	port_handler[HANDLE_STD_RD].read_portd = std_port_ind_h;
	port_handler[HANDLE_STD_RD].write_portd = port_not_avail_outd;
	port_handler[HANDLE_STD_RD].handler_name = "std port read";

	port_handler[HANDLE_STD_WR].read_portb = port_not_avail_inb;
	port_handler[HANDLE_STD_WR].write_portb = std_port_outb_h;
	port_handler[HANDLE_STD_WR].read_portw = port_not_avail_inw;
	port_handler[HANDLE_STD_WR].write_portw = std_port_outw_h;
	port_handler[HANDLE_STD_WR].read_portd = port_not_avail_ind;
	port_handler[HANDLE_STD_WR].write_portd = std_port_outd_h;
	port_handler[HANDLE_STD_WR].handler_name = "std port write";
#if 0
	port_handler[HANDLE_VID_IO].read_portb = video_port_in;
	port_handler[HANDLE_VID_IO].write_portb = video_port_out;
	port_handler[HANDLE_VID_IO].read_portw = NULL;
	port_handler[HANDLE_VID_IO].write_portw = NULL;
	port_handler[HANDLE_VID_IO].read_portd = NULL;
	port_handler[HANDLE_VID_IO].write_portd = NULL;
	port_handler[HANDLE_VID_IO].handler_name = "video port io";
#else
	port_handler[HANDLE_VID_IO].read_portb = std_port_inb_h;
	port_handler[HANDLE_VID_IO].write_portb = std_port_outb_h;
	port_handler[HANDLE_VID_IO].read_portw = std_port_inw_h;
	port_handler[HANDLE_VID_IO].write_portw = std_port_outw_h;
	port_handler[HANDLE_VID_IO].read_portd = std_port_ind_h;
	port_handler[HANDLE_VID_IO].write_portd = std_port_outd_h;
	port_handler[HANDLE_VID_IO].handler_name = "std port io";
#endif

	port_handler[HANDLE_SPECIAL].read_portb = special_port_inb;
	port_handler[HANDLE_SPECIAL].write_portb = special_port_outb;
	port_handler[HANDLE_SPECIAL].read_portw = NULL;
	port_handler[HANDLE_SPECIAL].write_portw = NULL;
	port_handler[HANDLE_SPECIAL].read_portd = NULL;
	port_handler[HANDLE_SPECIAL].write_portd = NULL;
	port_handler[HANDLE_SPECIAL].handler_name = "extra stuff";

	port_handles = STD_HANDLES;

	memset (port_handle_table, NO_HANDLE, sizeof(port_handle_table));
	memset (port_andmask, 0xff, sizeof(port_andmask));
	memset (port_ormask, 0, sizeof(port_ormask));

	return port_handles;	/* unused but useful */
}

static void portserver_exit(void *arg)
{
	error("port server terminated, exiting\n");
	leavedos(1);
}

/* port server: this function runs in a separate process from the main
   DOSEMU. This enables the main DOSEMU to drop root privileges. The
   server can do that as well: by setting iopl(3).
   Maybe this server should wrap DOSEMU rather than be forked from
   it.
*/
static void port_server(void)
{
	sigset_t set;
        struct portreq pr;
	_port_handler *ph, *ph1, *ph2, *ph3;
	signal(SIGINT, SIG_DFL);
	signal(SIGPIPE, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGPIPE);
	sigaddset(&set, SIGHUP);
	sigaddset(&set, SIGTERM);
	sigprocmask(SIG_UNBLOCK, &set, NULL);
        priv_iopl(3);
	priv_drop();
        close(port_fd_in[0]);
        close(port_fd_out[1]);
        g_printf("server started\n");
        for (;;) {
                read(port_fd_out[0], &pr, sizeof(pr));
                if (pr.type >= TYPE_EXIT)
                        _exit(0);
		ph = &EMU_HANDLER(pr.port);
		ph1 = &EMU_HANDLER(pr.port + 1);
		ph2 = &EMU_HANDLER(pr.port + 2);
		ph3 = &EMU_HANDLER(pr.port + 3);
		if (pr.type == TYPE_PCI) {
			/* get addr and data i/o access as close to each other
			   as possible, both to minimize possible races, and
			   for speed */
			struct portreq pr2;
			read(port_fd_out[0], &pr2, sizeof(pr2));
			ph->write_portd(pr.port, pr.word, ph->arg);
			pr = pr2;
		}
                switch (pr.type) {
                case TYPE_INB:
                        pr.word = ph->read_portb(pr.port, ph->arg);
                        break;
                case TYPE_OUTB:
                        ph->write_portb(pr.port, pr.word, ph->arg);
                        break;
                case TYPE_INW:
                        if (ph->read_portb == ph1->read_portb) {
                            pr.word = ph->read_portw(pr.port, ph->arg);
                        } else {
                            i_printf("PORT: splitting inw(0x%x)\n", pr.port);
                            pr.word = ph->read_portb(pr.port, ph->arg) |
                                     (ph1->read_portb(pr.port + 1, ph->arg) << 8);
                        }
                        break;
                case TYPE_OUTW:
                        if (ph->write_portb == ph1->write_portb) {
                            ph->write_portw(pr.port, pr.word, ph->arg);
                        } else {
                            i_printf("PORT: splitting outw(0x%x)\n", pr.port);
                            ph->write_portb(pr.port, pr.word, ph->arg);
                            ph1->write_portb(pr.port + 1, pr.word >> 8, ph->arg);
                        }
                        break;
                case TYPE_IND:
                        if (ph->read_portb == ph1->read_portb &&
                            ph->read_portb == ph2->read_portb &&
                            ph->read_portb == ph3->read_portb
                        ) {
                            pr.word = ph->read_portd(pr.port, ph->arg);
                        } else {
                            i_printf("PORT: splitting ind(0x%x)\n", pr.port);
                            pr.word = ph->read_portb(pr.port, ph->arg) |
                                     (ph1->read_portb(pr.port + 1, ph->arg) << 8) |
                                     (ph2->read_portb(pr.port + 2, ph->arg) << 16) |
                                     (ph3->read_portb(pr.port + 3, ph->arg) << 24);
                        }
                        break;
                case TYPE_OUTD:
                        if (ph->write_portb == ph1->write_portb &&
                            ph->write_portb == ph2->write_portb &&
                            ph->write_portb == ph3->write_portb
                        ) {
                            ph->write_portd(pr.port, pr.word, ph->arg);
                        } else {
                            i_printf("PORT: splitting outd(0x%x)\n", pr.port);
                            ph->write_portb(pr.port, pr.word, ph->arg);
                            ph1->write_portb(pr.port + 1, pr.word >> 8, ph->arg);
                            ph2->write_portb(pr.port + 2, pr.word >> 16, ph->arg);
                            ph3->write_portb(pr.port + 3, pr.word >> 24, ph->arg);
                        }
                        break;
                }
                write(port_fd_in[1], &pr, sizeof(pr));
        }
}

/*
 * SIDOC_BEGIN_FUNCTION extra_port_init()
 *
 * Catch all the special cases previously defined in ports.c
 * mainly video stuff that should be moved away from here
 * This must be called at the end of initialization phase
 *
 * NOTE: the order in which these inits are done could be significant!
 *   I tried to keep it the same it was in ports.c but this code surely
 *   can still have bugs
 *
 * SIDOC_END_FUNCTION
 */
int extra_port_init(void)
{
  int i;
/*
 * DANG_FIXTHIS This stuff should be moved to video code!!
 */
	if (portlog_map) {
	    /* switch off ioperm for $_ports that are traced and not forced fast */
	    for (i = 0; i < sizeof(port_handle_table); i++) {
		if (test_bit(i, portfast_map)) clear_bit(i, portlog_map);
		if (test_bit(i, portlog_map) &&
		    port_handle_table[i] >= HANDLE_STD_IO &&
		    port_handle_table[i] <= HANDLE_STD_WR) {
			set_ioperm(i, 1, 0);
			i_printf ("PORT: switched off ioperm for traced port 0x%x\n", i);
		}
	    }
	}

        if (can_do_root_stuff) {
                for (i = 0; i < sizeof(port_handle_table); i++) {
                        if (config.pci || config.pci_video ||
			    config.speaker == SPKR_NATIVE || (
			    port_handle_table[i] >= HANDLE_STD_IO &&
                            port_handle_table[i] <= HANDLE_STD_WR)) {
                                /* fork the privileged port server */
                                g_printf("starting port server\n");
                                pipe(port_fd_out);
                                pipe(port_fd_in);
                                portserver_pid = fork();
                                if (portserver_pid == 0) {
                                        setsid();
                                        port_server();
                                        _exit(0);	// never come here
                                }
                                close(port_fd_in[1]);
                                close(port_fd_out[0]);
                                sigchld_register_handler(portserver_pid,
                                    portserver_exit, NULL);
                                break;
                        }
                }
        }

 	return 0;
}

void port_exit(void)
{
	int stat;
	struct portreq pr;
	if (!portserver_pid) return;
	sigchld_enable_handler(portserver_pid, 0);
	pr.type = TYPE_EXIT;
	write(port_fd_out[1], &pr, sizeof(pr));
	waitpid(portserver_pid, &stat, 0);
	portserver_pid = 0;
}

void release_ports (void)
{
	memset (port_handle_table, NO_HANDLE, sizeof(port_handle_table));
	memset (port_andmask, 0xff, sizeof(port_andmask));
	memset (port_ormask, 0, sizeof(port_ormask));

}

/* ---------------------------------------------------------------------- */
/*
 * SIDOC_BEGIN_FUNCTION port_register_handler
 *
 * Assigns a handle in the port table to a range of ports with or
 * without a device, and registers the ports
 *
 * SIDOC_END_FUNCTION
 */
int port_register_handler(emu_iodev_t device, int flags)
{
    int handle, i;

    /* first find existing handle for function or create new one */
    for (handle=0; handle < port_handles; handle++) {
	if (!strcmp(port_handler[handle].handler_name, device.handler_name))
	      break;
    }

    if (handle >= port_handles) {
	/* no existing handle found, create new one */
	if (port_handles >= EMU_MAX_IO_DEVICES) {
		error("PORT: too many IO devices, increase EMU_MAX_IO_DEVICES\n");
		leavedos(77);
	}

	port_handles++;
	port_handler[handle] = device;
	/*
	 * for byte and double, a NULL function means that the port
	 * access is not available, while for word means that it will
	 * be translated into 2 byte accesses
	 */
	if (!device.read_portb)
	    port_handler[handle].read_portb = port_not_avail_inb;
	if (!device.write_portb)
	    port_handler[handle].write_portb = port_not_avail_outb;
    }

  /* change table to reflect new handler id for that address */
    for (i = device.start_addr; i <= device.end_addr; i++) {
	if (port_handle_table[i] != 0) {
		error("PORT: conflicting devices: %s & %s for port %#x\n",
		      port_handler[handle].handler_name,
		      EMU_HANDLER(i).handler_name, i);
		config.exitearly = 1;
		return 4;
	}
	port_handle_table[i] = handle;
	if (flags & PORT_FORCE_FAST) /* force fast, no tracing allowed */
		set_bit(i, portfast_map);
    }

    i_printf("PORT: registered \"%s\" handle 0x%02x [0x%04x-0x%04x]\n",
	port_handler[handle].handler_name, handle, device.start_addr,
	device.end_addr);

    if (flags & PORT_FAST) {
	i_printf("PORT: trying to give fast access to ports [0x%04x-0x%04x]\n",
		 device.start_addr, device.end_addr);
	if (set_ioperm(device.start_addr, device.end_addr-device.start_addr+1, 1) == -1) {
	  i_printf("PORT: fast failed: using perm/iopl for ports [0x%04x-0x%04x]\n",
		   device.start_addr, device.end_addr);
	}
    }
    return 0;
}


/*
 * SIDOC_BEGIN_FUNCTION port_allow_io
 *
 *
 * SIDOC_END_FUNCTION
 */
Boolean port_allow_io(ioport_t start, Bit16u size, int permission, Bit8u ormask,
	Bit8u andmask, int portspeed)
{
	static emu_iodev_t io_device;
	int usemasks = 0;
	unsigned int flags = 0;

        if (!can_do_root_stuff) {
		warn("Direct port I/O in dosemu.conf requires root privs and -s\n");
                return FALSE;
	}

	i_printf("PORT: allow_io for port 0x%04x:%d perm=%x or=%x and=%x\n",
		 start, size, permission, ormask, andmask);

	if ((ormask != 0) || (andmask != 0xff)) {
		if (size > 1)
			i_printf("PORT: andmask & ormask not supported for multiple ports\n");
		else
			usemasks = 1;
	}

	if (permission == IO_RDWR)
		io_device.handler_name = "std port io";
	else if (permission == IO_READ)
		io_device.handler_name = "std port read";
	else
		io_device.handler_name = "std port write";

	io_device.start_addr   = start;
	io_device.end_addr     = start + size - 1;

	if (usemasks) {
		port_andmask[start] = andmask;
		port_ormask[start] = ormask;
	}
	if (portspeed >= 0) {
		flags |= PORT_FAST;
		if (portspeed > 0)
			flags |= PORT_FORCE_FAST;
	}
	port_register_handler(io_device, flags);
	return TRUE;
}

/*
 * SIDOC_BEGIN_FUNCTION set_ioperm
 *
 * wrapper for the ioperm() syscall, returns -1 if not successful.
 *
 * SIDOC_END_FUNCTION
 */
int
set_ioperm(int start, int size, int flag)
{
#ifdef HAVE_SYS_IO_H
	PRIV_SAVE_AREA
	int tmp;

	if ((!can_do_root_stuff && flag == 1))
	    return -1;		/* don't bother */

	/* While possibly not the best behavior I figure we ought to,
	   turn the privilege on here instead of in every caller.
	   If we want a privileged version of this function we can
	   call ioperm.
	 */
	enter_priv_on();
	tmp = ioperm(start, size, flag);
	leave_priv_setting();

	if (tmp==0) {
	    int i;
	    for (i=start; i<(start+size); i++) {
		if (flag) {
		    set_bit(i, emu_io_bitmap);
		} else {
		    clear_bit(i, emu_io_bitmap);
		}
	    }
	}
	i_printf ("nPORT: set_ioperm [%x:%d:%d] returns %d\n",start,size,flag,tmp);
	return tmp;
#else
	return -1;
#endif
}

void port_enter_critical_section(const char *caller)
{
	if (in_crit_section) {
	    error("Critical section conflict for %s and %s\n",
		    crit_sect_caller, caller);
	    in_crit_section = 0;
	    leavedos(49);
	}
	in_crit_section++;
	crit_sect_caller = caller;
}

void port_leave_critical_section(void)
{
	if (!in_crit_section) {
	    error("leave_critical_section without enter\n");
	    leavedos(49);
	}
	in_crit_section--;
}

/* ====================================================================== */
