/*
 *	SOCK_PACKET support.
 *	Placed under the GNU LGPL.
 *
 *	First cut at a library of handy support routines. Comments, additions
 *	and bug fixes gratefully received.
 *
 *	(c) 1994 Alan Cox	iiitac@pyr.swan.ac.uk	GW4PTS@GB7SWN
 */
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#ifdef HAVE_LIBBSD
#include <bsd/string.h>
#endif
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <net/if.h>
#include <netinet/in.h>
#include "Linux/if_tun.h"
#include <netinet/if_ether.h>
#ifdef HAVE_NETPACKET_PACKET_H
#include <netpacket/packet.h>
#endif
#include <net/ethernet.h>
#include <assert.h>

#include "emu.h"
#include "utilities.h"
#include "priv.h"
#include "pktdrvr.h"
#include "libpacket.h"

#define TAP_DEVICE  "dosemu_tap%d"

static int tun_alloc(char *dev);
static int pkt_is_registered_type(int type);

static uint8_t local_eth_addr[6] = {0,0,0,0,0,0};
#define DOSNET_FAKED_ETH_ADDRESS   "fbx\x90xx"

static int num_backends;
static struct pkt_ops *ops[VNET_TYPE_MAX];

static int pkt_flags;
static int early_fd;
static int rcv_mode;
static int open_cnt;

/* Should return a unique ID corresponding to this invocation of
   dosemu not clashing with other dosemus. We use a random value and
   hope for the best.
   */

static void GenerateDosnetID(void)
{
	pid_t pid = getpid();
	memcpy(local_eth_addr, DOSNET_FAKED_ETH_ADDRESS, 6);
	assert((local_eth_addr[0] & 3) == 2);
	memcpy(local_eth_addr + 3, &pid, 2);
	local_eth_addr[5] = rand();
}

static struct pkt_ops *find_ops(int id)
{
	int i;
	for (i = 0; i < num_backends; i++) {
		if (ops[i]->id == id)
			return ops[i];
	}
	return NULL;
}

#ifdef HAVE_NETPACKET_PACKET_H
/*
 *	Obtain a file handle on a raw ethernet type. In actual fact
 *	you can also request the dummy types for AX.25 or 802.3 also
 *
 *	-1 indicates an error
 *	0  or higher is a file descriptor which we have set non blocking
 *
 *	WARNING: It is ok to listen to a service the system is using (eg arp)
 *	but don't try and run a user mode stack on the same service or all
 *	hell will break loose - unless you use virtual TCP/IP (dosnet).
 */

static int OpenNetworkLinkEth(const char *name, void (*cbk)(int, int))
{
	PRIV_SAVE_AREA
	int s, proto, ret;
	struct ifreq req;
	struct sockaddr_ll addr;
	unsigned short receive_mode;

	proto = htons(ETH_P_ALL);
	enter_priv_on();
	s = socket(PF_PACKET, SOCK_RAW, proto);
	leave_priv_setting();
	if (s < 0) {
		if (errno == EPERM)
			error("Must be root for direct NIC access\n");
		return -1;
	}

	ret = fcntl(s, F_SETFL, O_NDELAY);
	if (ret == -1) {
		pd_printf("OpenNetwork: fcntl failed '%s'\n", strerror(errno));
		close(s);
		return -1;
	}

	strlcpy(req.ifr_name, name, sizeof(req.ifr_name));
	if (ioctl(s, SIOCGIFINDEX, &req) < 0) {
		close(s);
		return -1;
	}
	addr.sll_family = AF_PACKET;
	addr.sll_protocol = proto;
	addr.sll_ifindex = req.ifr_ifindex;
	if (bind(s, (void *)&addr, sizeof(addr)) < 0) {
		pd_printf("OpenNetwork: could not bind socket: %s\n",
			strerror(errno));
		close(s);
		return -1;
	}

	enter_priv_on();
	ret = ioctl(s, SIOCGIFFLAGS, &req);
	leave_priv_setting();
	if (ret < 0) {
		close(s);
		return -1;
	}

	receive_mode = (req.ifr_flags & IFF_PROMISC) ? 6 :
		((req.ifr_flags & IFF_BROADCAST) ? 3 : 2);

	cbk(s, receive_mode);
	return 0;
}
#endif

static int OpenNetworkLinkTap(const char *name, void (*cbk)(int, int))
{
	char devname[256];
	int pkt_fd;

	strlcpy(devname, name, sizeof(devname));
	pkt_fd = tun_alloc(devname);
	if (pkt_fd < 0)
		return pkt_fd;
	cbk(pkt_fd, 6);
	pd_printf("PKT: Using device %s\n", devname);
	return 0;
}

static int OpenNetworkLinkSock(const char *name, void (*cbk)(int, int))
{
	int pkt_fd, ret;
	struct sockaddr_un saddr_un;

	saddr_un.sun_family = PF_UNIX;
	strlcpy(saddr_un.sun_path, name, sizeof(saddr_un.sun_path));
	pkt_fd = socket(PF_UNIX, SOCK_STREAM, 0);
	if (pkt_fd < 0)
		return pkt_fd;
	ret = connect(pkt_fd, (struct sockaddr *)&saddr_un, sizeof(saddr_un));
	if (ret < 0) {
		close(pkt_fd);
		return ret;
	}
	cbk(pkt_fd, 6);
	pd_printf("PKT: Using socket device %s\n", name);
	return 0;
}

static void set_fd(int fd, int mode)
{
	early_fd = fd;
	rcv_mode = mode;
}

static int Open_sockets(const char *name, int vnet)
{
	struct pkt_ops *o = find_ops(vnet);
	if (!o)
		return -1;
	return o->open(name, set_fd);
}

int OpenNetworkLink(void (*cbk)(int, int))
{
	int ret = -1;
	struct pkt_ops *o = NULL;
#define CB() ((o->flags & PFLG_ASYNC) ? cbk : set_fd)
#define BAD_OPS() (!o || ((o->flags & PFLG_ASYNC) && open_cnt > 1))

	open_cnt++;
	assert(early_fd != 0);
	if (early_fd != -1) {
		cbk(early_fd, rcv_mode);
		return 0;
	}
	/* try non-priv setups like vde */
	switch (config.vnet) {
	case VNET_TYPE_AUTO:
		pkt_set_flags(PKT_FLG_QUIET);
		/* no break, try sock, slirp */
	case VNET_TYPE_SOCK:
		if (config.netsock && config.netsock[0])
			o = find_ops(VNET_TYPE_SOCK);
		if (BAD_OPS())
			ret = -1;
		else
			ret = o->open(config.netsock, CB());
		if (ret < 0) {
			if (config.vnet == VNET_TYPE_AUTO || open_cnt > 1)
				warn("PKT: Cannot open sock\n");
			else
				error("Unable to open sock\n");
		} else {
			if (config.vnet == VNET_TYPE_AUTO)
				config.vnet = VNET_TYPE_SOCK;
			pd_printf("PKT: Using sock networking\n");
			break;
		}
		/* no break, try slirp */
	case VNET_TYPE_SLIRP: {
		if (!pkt_is_registered_type(VNET_TYPE_SLIRP)) {
			if (config.vnet != VNET_TYPE_AUTO)
				error("slirp support is not compiled in\n");
			break;
		}
		o = find_ops(VNET_TYPE_SLIRP);
		if (BAD_OPS())
			ret = -1;
		else
			ret = o->open("slirp", CB());
		if (ret < 0) {
			if (config.vnet == VNET_TYPE_AUTO || open_cnt > 1)
				warn("PKT: Cannot run slirp\n");
			else
				error("Unable to run slirp\n");
		} else {
			if (config.vnet == VNET_TYPE_AUTO)
				config.vnet = VNET_TYPE_SLIRP;
			pd_printf("PKT: Using slirp networking\n");
			break;
		}
		/* no break, try VDE */
	}
	case VNET_TYPE_VDE: {
		const char *pr_dev = config.vdeswitch[0] ? config.vdeswitch : "(auto)";
		if (!pkt_is_registered_type(VNET_TYPE_VDE)) {
			if (config.vnet != VNET_TYPE_AUTO)
				error("vde support is not compiled in\n");
			break;
		}
		o = find_ops(VNET_TYPE_VDE);
		if (BAD_OPS())
			ret = -1;
		else
			ret = o->open(config.vdeswitch, CB());
		if (ret < 0) {
			if (config.vnet == VNET_TYPE_AUTO || open_cnt > 1)
				warn("PKT: Cannot run VDE %s\n", pr_dev);
			else
				error("Unable to run VDE %s\n", pr_dev);
		} else {
			if (config.vnet == VNET_TYPE_AUTO)
				config.vnet = VNET_TYPE_VDE;
			pd_printf("PKT: Using device %s\n", pr_dev);
			break;
		}
		/* no break, try whatever remains */
	}
	}
	if (ret != -1 && o && !(o->flags & PFLG_ASYNC))
		cbk(early_fd, rcv_mode);
	if (ret == -1)
		open_cnt--;
	return ret;
}

/*
 *	Close a file handle to a raw packet type.
 */
static void CloseNetworkLinkEth(int pkt_fd)
{
	close(pkt_fd);
}

void CloseNetworkLink(int pkt_fd)
{
	if (!open_cnt)
		return;
	if (--open_cnt == 0)
		find_ops(config.vnet)->close(pkt_fd);
}

/*
 *	Handy support routines.
 */

/*
 *	NET2 or NET3 - work for both.
 *      (NET3 is valid for all kernels > 1.3.38)
 */
#define NET3

#ifdef HAVE_NETPACKET_PACKET_H
/*
 *	Obtain the hardware address of an interface.
 *	addr should be a buffer of 8 bytes or more.
 *
 *	Return:
 *	0	Success, buffer holds data.
 *	-1	Error.
 */
static int GetDeviceHardwareAddressEth(unsigned char *addr)
{
	int s;
	struct ifreq req;
	int err;

	s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s == -1) {
		return -1;
	}

	strlcpy(req.ifr_name, config.ethdev, sizeof(req.ifr_name));

	err = ioctl(s, SIOCGIFHWADDR, &req);
	close(s);
	if (err == -1)
		return err;
#ifdef NET3
	memcpy(addr, req.ifr_hwaddr.sa_data,8);
#else
	memcpy(addr, req.ifr_hwaddr, 8);
#endif

	return 0;
}
#endif

void pkt_get_fake_mac(unsigned char *addr)
{
	memcpy(addr, local_eth_addr, 6);
}

static int GetDeviceHardwareAddressTap(unsigned char *addr)
{
	/* This routine is totally local; doesn't make
	   request to actual device. */
	pkt_get_fake_mac(addr);
	return 0;
}

int GetDeviceHardwareAddress(unsigned char *addr)
{
	int i;
	int ret = find_ops(config.vnet)->get_hw_addr(addr);
	pd_printf("Assigned Ethernet Address = ");
	for (i=0; i < 6; i++)
		pd_printf("%02x:", local_eth_addr[i] & 0xff);
	pd_printf("\n");
	return ret;
}

#ifdef HAVE_NETPACKET_PACKET_H
/*
 *	Obtain the maximum packet size on an interface.
 *
 *	Return:
 *	>0	Return is the mtu of the interface
 *	-1	Error.
 */
static int GetDeviceMTUEth(void)
{
	int s;
	struct ifreq req;
	int err;

	s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s == -1) {
		return -1;
	}

	strlcpy(req.ifr_name, config.ethdev, sizeof(req.ifr_name));

	err = ioctl(s, SIOCGIFMTU, &req);
	close(s);
	if (err < 0)
		return -1;
	return req.ifr_mtu;
}
#endif

static int GetDeviceMTUTap(void)
{
	return 1500;
}

int GetDeviceMTU(void)
{
	return find_ops(config.vnet)->get_MTU();
}

int tun_alloc(char *dev)
{
      PRIV_SAVE_AREA
      struct ifreq ifr;
      int fd, err;

      enter_priv_on();
      fd = open("/dev/net/tun", O_RDWR | O_CLOEXEC);
      leave_priv_setting();
      if (fd < 0)
         return -1;

      memset(&ifr, 0, sizeof(ifr));

      /* Flags: IFF_TUN   - TUN device (no Ethernet headers)
       *        IFF_TAP   - TAP device
       *
       *        IFF_NO_PI - Do not provide packet information
       */
      ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
      if (*dev) {
        err = snprintf(ifr.ifr_name, IFNAMSIZ, "%s", dev);
        if (err >= IFNAMSIZ) {
          close(fd);
          return -1;
        }
      }

      enter_priv_on();
      err = ioctl(fd, TUNSETIFF, (void *) &ifr);
      leave_priv_setting();
      if (err < 0) {
         close(fd);
         return err;
      }
      strcpy(dev, ifr.ifr_name);

      return fd;
}

static ssize_t pkt_read_eth(int pkt_fd, void *buf, size_t count)
{
    struct timeval tv;
    fd_set readset;

    tv.tv_sec = 0;				/* set a (small) timeout */
    tv.tv_usec = 0;

    /* anything ready? */
    FD_ZERO(&readset);
    FD_SET(pkt_fd, &readset);
    /* anything ready? */
    if (select(pkt_fd + 1, &readset, NULL, NULL, &tv) <= 0)
        return 0;

    if(!FD_ISSET(pkt_fd, &readset))
        return 0;

    return read(pkt_fd, buf, count);
}

static ssize_t pkt_read_sock(int pkt_fd, void *buf, size_t count)
{
    struct timeval tv;
    fd_set readset;
    uint32_t tmpbuf;
    uint32_t len;
    int ret;

    tv.tv_sec = 0;				/* set a (small) timeout */
    tv.tv_usec = 0;

    /* anything ready? */
    FD_ZERO(&readset);
    FD_SET(pkt_fd, &readset);
    /* anything ready? */
    if (select(pkt_fd + 1, &readset, NULL, NULL, &tv) <= 0)
        return 0;

    if(!FD_ISSET(pkt_fd, &readset))
        return 0;

    ret = read(pkt_fd, &tmpbuf, sizeof(tmpbuf));
    if (ret < 4)
        return 0;
    len = ntohl(tmpbuf);
    if (len > count) {
        error("PKT: buffer too small, %zi need %i\n", count, len);
        len = count;
    }
    ret = read(pkt_fd, buf, len);
    if (ret != len)
        error("PKT: expected %i byes but got %i\n", len, ret);
    return ret;
}

ssize_t pkt_read(int fd, void *buf, size_t count)
{
    return find_ops(config.vnet)->pkt_read(fd, buf, count);
}

static ssize_t pkt_write_eth(int pkt_fd, const void *buf, size_t count)
{
    return write(pkt_fd, buf, count);
}

static ssize_t pkt_write_sock(int pkt_fd, const void *buf, size_t count)
{
    uint32_t len = htonl(count);
    write(pkt_fd, &len, sizeof(len));
    return write(pkt_fd, buf, count);
}

ssize_t pkt_write(int fd, const void *buf, size_t count)
{
    return find_ops(config.vnet)->pkt_write(fd, buf, count);
}

int pkt_register_backend(struct pkt_ops *o)
{
    int idx = num_backends++;
    assert(idx < ARRAY_SIZE(ops));
    ops[idx] = o;
    return idx;
}

#ifdef HAVE_NETPACKET_PACKET_H
static struct pkt_ops eth_ops = {
	.id = VNET_TYPE_ETH,
	.open = OpenNetworkLinkEth,
	.close = CloseNetworkLinkEth,
	.get_hw_addr = GetDeviceHardwareAddressEth,
	.get_MTU = GetDeviceMTUEth,
	.pkt_read = pkt_read_eth,
	.pkt_write = pkt_write_eth,
};
#endif

static struct pkt_ops sock_ops = {
	.id = VNET_TYPE_SOCK,
	.open = OpenNetworkLinkSock,
	.close = CloseNetworkLinkEth,
	.get_hw_addr = GetDeviceHardwareAddressTap,
	.get_MTU = GetDeviceMTUTap,
	.pkt_read = pkt_read_sock,
	.pkt_write = pkt_write_sock,
};

static struct pkt_ops tap_ops = {
	.id = VNET_TYPE_TAP,
	.open = OpenNetworkLinkTap,
	.close = CloseNetworkLinkEth,
	.get_hw_addr = GetDeviceHardwareAddressTap,
	.get_MTU = GetDeviceMTUTap,
	.pkt_read = pkt_read_eth,
	.pkt_write = pkt_write_eth,
};

void LibpacketInit(void)
{
	int ret;

	GenerateDosnetID();

#ifdef HAVE_NETPACKET_PACKET_H
	pkt_register_backend(&eth_ops);
#endif
	pkt_register_backend(&tap_ops);
	pkt_register_backend(&sock_ops);

#ifdef USE_DL_PLUGINS
#ifdef USE_VDE
	load_plugin("vde");
#endif
#ifdef USE_SLIRP
	load_plugin("slirp");
#endif
#endif
	early_fd = -1;

	/* Open sockets only for priv configs */
	switch (config.vnet) {
	case VNET_TYPE_ETH:
		pd_printf("PKT: Using ETH device %s\n", config.ethdev);
		ret = Open_sockets(config.ethdev, VNET_TYPE_ETH);
		if (ret < 0)
			error("PKT: Cannot open %s: %s\n", config.ethdev, strerror(errno));
		else
			pd_printf("PKT: eth backend enabled, dev=%s\n", config.ethdev);
		break;
	case VNET_TYPE_TAP: {
		char devname[256];
		if (!config.tapdev || !config.tapdev[0]) {
			pd_printf("PKT: Using dynamic TAP device\n");
			strcpy(devname, TAP_DEVICE);
		} else {
			pd_printf("PKT: trying to bind to TAP device %s\n", config.tapdev);
			strlcpy(devname, config.tapdev, sizeof(devname));
		}
		ret = Open_sockets(devname, VNET_TYPE_TAP);
		if (ret < 0)
			error("PKT: Cannot open %s: %s\n", devname, strerror(errno));
		else
			pd_printf("PKT: tap backend enabled, dev=%s\n", devname);
		break;
	}
	}
}

void pkt_set_flags(int flags)
{
	pkt_flags |= flags;
}

void pkt_clear_flags(int flags)
{
	pkt_flags &= ~flags;
}

int pkt_get_flags(void)
{
	return pkt_flags;
}

static int pkt_is_registered_type(int type)
{
	return !!find_ops(type);
}
