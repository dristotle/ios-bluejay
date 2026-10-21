/*
 * Milestone 5 probe: can we talk to the internal UFS that ABL booted from?
 *
 * Runs with the MMU off (all memory Device, so DMA is coherent with no cache
 * maintenance). Dumps the UFSHCI registers ABL left behind and, if the host
 * is enabled and the link is up, points the transfer request list at our own
 * buffers and issues READ-ONLY SCSI commands on LUN 0. No sectors or persistent device attributes are written. Bluejay
 * additionally uses START STOP UNIT on the device WLUN to resume runtime power.
 *
 * Register layout and descriptor formats: JEDEC UFSHCI (JESD223).
 */
#include <stdint.h>
#include "console.h"
#include "ufs.h"

#define UFS_BASE	PLATFORM_UFS	/* ufshc@1d84000, sweet live FDT */

#define REG_CAP		0x00
#define REG_VER		0x08
#define REG_IS		0x20
#define REG_HCS		0x30
#define REG_HCE		0x34
#define REG_UTRLBA	0x50
#define REG_UTRLBAU	0x54
#define REG_UTRLDBR	0x58
#define REG_UTRLCLR	0x5c
#define REG_UTRLRSR	0x60

#define HCS_DP		(1u << 0)
#define HCS_UTRLRDY	(1u << 1)
#define HCS_UCRDY	(1u << 3)

#define BLOCK		4096u
#if defined(SHIM_BLUEJAY)
#define UPIU_SIZE 1024u
#define PRDT_OFF (2 * UPIU_SIZE)
#define PRDT_MAX 4
#else
#define UPIU_SIZE 512u
#define PRDT_OFF (2 * UPIU_SIZE)
#define PRDT_MAX 4
#endif

static inline uint32_t ufs_rd(uint32_t off) { return *(volatile uint32_t *)(UFS_BASE + off); }
static inline void ufs_wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(UFS_BASE + off) = v; }

/* UTP transfer request descriptor (32 bytes). */
struct utrd {
	uint32_t dw0;		/* [31:28] CT, [26:25] DD, [24] I */
	uint32_t dw1;
	uint32_t dw2;		/* [7:0] OCS */
	uint32_t dw3;
	uint32_t ucdba;		/* command descriptor base, 128-byte aligned */
	uint32_t ucdbau;
	uint32_t resp;		/* [31:16] response UPIU offset, [15:0] length (DWORDs) */
	uint32_t prdt;		/* [31:16] PRDT offset, [15:0] entries (DWORDs/entries) */
};

/* UTP command descriptor: command UPIU, response UPIU, PRDT. */
struct ucd {
	uint8_t cmd[UPIU_SIZE];
	uint8_t resp[UPIU_SIZE];
	struct {
		uint32_t addr_lo, addr_hi, rsvd, dbc;	/* dbc = byte count - 1 */
	} prdt[PRDT_MAX];
};

#if defined(SHIM_BLUEJAY)
/* A safe conventional high-bank allocation, also used by the XNU port. */
#define utrl ((struct utrd *)(uintptr_t)0x880100000UL)
#define ucd (*(struct ucd *)(uintptr_t)0x880101000UL)
#define data ((uint8_t *)(uintptr_t)0x880102000UL)
#else
static struct utrd utrl[32] __attribute__((aligned(1024)));
static struct ucd ucd __attribute__((aligned(128)));
static uint8_t data[2 * BLOCK] __attribute__((aligned(4096)));
#endif

static uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

static void zero(void *p, unsigned n)
{
	volatile uint8_t *b = p;
	for (unsigned i = 0; i < n; i++)
		b[i] = 0;
}

#if defined(SHIM_BLUEJAY)
/* UIC operations alter only transient host/link state, never storage. */
static uint32_t uic(uint32_t cmd, uint32_t arg1, uint32_t *value)
{
	uint64_t freq;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
	if (!(ufs_rd(REG_HCS) & HCS_UCRDY))
		return 0xdead0001;
	ufs_wr(REG_IS, 0xffffffffu);
	ufs_wr(0x94, arg1);
	ufs_wr(0x98, 0);
	ufs_wr(0x9c, 0);
	__asm__ volatile("dsb sy" ::: "memory");
	ufs_wr(0x90, cmd);
	uint64_t start = ticks();
	while (!(ufs_rd(REG_IS) & 0x400)) {
		if (ticks() - start > freq)
			return 0xdead0002;
	}
	*value = ufs_rd(0x9c);
	uint32_t rc = ufs_rd(0x98) & 0xff;
	ufs_wr(REG_IS, ufs_rd(REG_IS));
	return rc;
}
#endif

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/*
 * Issue one SCSI command (read direction or no data) in slot 0.
 * Returns 0 on success, otherwise a code describing the failure stage.
 */
static uint32_t scsi(uint8_t lun, const uint8_t *cdb, unsigned cdb_len, void *buf, uint32_t len,
    uint32_t *ocs_out, uint32_t *status_out)
{

	/* Only START STOP UNIT(active) on the device WLUN may change runtime state. */
	if (!cdb_len || (cdb[0] == 0x1b &&
	    (lun != 0xd0 || cdb_len != 6 || cdb[1] || cdb[2] || cdb[3] || cdb[4] != 0x10 || cdb[5])) ||
	    (cdb[0] != 0x1b && lun != 0 && !(cdb[0] == 3 && lun == 0xd0)))
		return 5;
	uintptr_t list = (uintptr_t)utrl;
	if (cdb_len > 16 || len > PRDT_MAX * BLOCK ||
	    (cdb[0] != 0 && cdb[0] != 3 && cdb[0] != 0x12 && cdb[0] != 0x1b && cdb[0] != 0x25 && cdb[0] != 0x28) ||
	    ufs_rd(REG_UTRLBA) != (uint32_t)list ||
	    ufs_rd(REG_UTRLBAU) != (uint32_t)(list >> 32) ||
	    !(ufs_rd(REG_UTRLRSR) & 1) || ufs_rd(REG_UTRLDBR))
		return 5;
#if defined(SHIM_BLUEJAY)
	for (uintptr_t a = (uintptr_t)utrl; a < (uintptr_t)utrl + 0x8000; a += 64)
		__asm__ volatile("dc ivac, %0" : : "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
#endif
	zero(&utrl[0], sizeof(utrl[0]));
	zero(&ucd, sizeof(ucd));

	/* Command UPIU: header (12 bytes), expected length, CDB. */
	ucd.cmd[0] = 0x01;			/* transaction type: COMMAND */
	ucd.cmd[1] = len ? 0x40 : 0x00;		/* flags: R (data in) */
	ucd.cmd[2] = lun;
	ucd.cmd[3] = 0; /* Samsung task tag equals slot 0 */
	ucd.cmd[12] = len >> 24;
	ucd.cmd[13] = len >> 16;
	ucd.cmd[14] = len >> 8;
	ucd.cmd[15] = len;
	for (unsigned i = 0; i < cdb_len; i++)
		ucd.cmd[16 + i] = cdb[i];

	uintptr_t ucd_pa = (uintptr_t)&ucd;
	utrl[0].dw0 = (1u << 28) | ((len ? 2u : 0u) << 25) | (1u << 24);	/* UFS storage, dev->host */
	utrl[0].dw2 = 0x0f;			/* OCS: invalid until the host updates it */
	utrl[0].ucdba = (uint32_t)ucd_pa;
	utrl[0].ucdbau = (uint32_t)(ucd_pa >> 32);
#if defined(SHIM_BLUEJAY)
	/* GS101 uses byte units for response and PRDT fields. */
	utrl[0].resp = UPIU_SIZE << 16 | UPIU_SIZE;
#else
	utrl[0].resp = (UPIU_SIZE / 4) << 16 | (UPIU_SIZE / 4);
#endif
	if (len) {
		uint32_t n = 0;
		for (uint32_t done = 0; done < len; done += BLOCK, n++) {
			uintptr_t pa = (uintptr_t)buf + done;
			uint32_t size = len - done < BLOCK ? len - done : BLOCK;
			ucd.prdt[n].addr_lo = (uint32_t)pa;
			ucd.prdt[n].addr_hi = (uint32_t)(pa >> 32);
			ucd.prdt[n].dbc = size - 1;
		}
#if defined(SHIM_BLUEJAY)
		utrl[0].prdt = PRDT_OFF << 16 | (n * 16);
#else
		utrl[0].prdt = (PRDT_OFF / 4) << 16 | n;
#endif
	}
#if defined(SHIM_BLUEJAY)
	ufs_wr(0x1140, ufs_rd(0x1140) | 1); /* slot 0 command nexus */
	ufs_wr(REG_IS, 0xffffffffu);
#endif
	__asm__ volatile("dsb sy" ::: "memory");

	ufs_wr(REG_UTRLDBR, 1);
	uint64_t start = ticks();
	while (ufs_rd(REG_UTRLDBR) & 1) {
		uint64_t freq;
		__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
		if (ticks() - start > freq) {	/* 1 s */
			con_hex2("UFS WAIT", "dbr=", ufs_rd(REG_UTRLDBR), "ocs=", utrl[0].dw2);
			con_hex2("UFS ERR", "is=", ufs_rd(REG_IS), "resp=", ucd.resp[0]);
			ufs_wr(REG_UTRLCLR, 1u);	/* Samsung clear slot 0 */
			return 1;			/* timeout */
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	*ocs_out = utrl[0].dw2 & 0xff;
	*status_out = ucd.resp[7];		/* SCSI status in the response UPIU */
	if (*ocs_out || *status_out) {
		con_hex2("UFS RESP", "hdr=", be32(ucd.resp), "rsp=", be32(ucd.resp + 4));
		con_hex2("UFS SENSE", "len/key=", (ucd.resp[32] << 24) | (ucd.resp[33] << 16) | ucd.resp[36],
		    "asc/q=", (ucd.resp[46] << 8) | ucd.resp[47]);
		con_hex2("UFS IS", "is=", ufs_rd(REG_IS), "vs=", ufs_rd(0x1138));
		con_hex2("UFS LINK", "pa=", ufs_rd(0x38), "dl=", ufs_rd(0x3c));
	}
	if (*ocs_out)
		return 2;
	if (ucd.resp[0] != 0x21)		/* RESPONSE UPIU */
		return 3;
	return *status_out ? 4 : 0;
}

static uint32_t read_blocks(uint32_t lba, uint32_t count, void *buf)
{
	uint8_t cdb[10] = { 0x28, 0, lba >> 24, lba >> 16, lba >> 8, lba, 0, count >> 8, count, 0 };
	uint32_t ocs, st;
	return scsi(0, cdb, sizeof(cdb), buf, count * BLOCK, &ocs, &st);
}

static uint64_t le64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

/* Name the filesystem whose superblock starts at block lba (f2fs/ext4). */
static const char *fs_type(uint32_t lba)
{
	if (read_blocks(lba, 1, data))
		return "read error";
	const uint8_t *sb = data + 1024;
	if (sb[0] == 0x10 && sb[1] == 0x20 && sb[2] == 0xf5 && sb[3] == 0xf2)
		return "f2fs";
	if (sb[56] == 0x53 && sb[57] == 0xef)
		return "ext4";
	return "other";
}

/* One line per GPT entry of at least 64 MiB: name, first LBA, size in MiB (4K blocks). */
static void list_partitions(uint32_t entry_lba, uint32_t nent)
{
	#if defined(SHIM_BLUEJAY)
	uint8_t *ents = (uint8_t *)(uintptr_t)0x880104000UL;
#else
	static uint8_t ents[128 * 128] __attribute__((aligned(4096)));
#endif
	uint32_t blocks = (nent * 128 + BLOCK - 1) / BLOCK;
	if (read_blocks(entry_lba, blocks, ents)) {
		con_str("UFS PART", "read error");
		return;
	}
	for (uint32_t i = 0; i < nent; i++) {
		const uint8_t *e = ents + i * 128;
		uint64_t first = le64(e + 32), last = le64(e + 40);
		if (!first)
			continue;
		char name[17];
		for (int c = 0; c < 16; c++) {
			uint8_t ch = e[56 + 2 * c];
			name[c] = (ch >= 32 && ch < 127) ? ch : (ch ? '?' : 0);
		}
		name[16] = 0;
		if (((last - first + 1) >> 8) < 64)	/* keep the screen to the big ones */
			continue;
		con_hex2(name, "lba=", (uint32_t)first, "MiB=", (uint32_t)((last - first + 1) >> 8));
		if (!__builtin_strcmp(name, "userdata") || !__builtin_strcmp(name, "cust") ||
		    !__builtin_strcmp(name, "cache"))
			con_str(" fs", fs_type((uint32_t)first));
	}
}

void test_ufs(void)
{
	con_str("UFS", "milestone 5 probe (read-only)");
	uint32_t cap = ufs_rd(REG_CAP), hcs = ufs_rd(REG_HCS), hce = ufs_rd(REG_HCE);
	con_hex2("UFS", "ver=", ufs_rd(REG_VER), "cap=", cap);
	con_hex2("UFS", "hce=", hce, "hcs=", hcs);
	con_hex2("UFS", "is=", ufs_rd(REG_IS), "rsr=", ufs_rd(REG_UTRLRSR));
	con_hex2("UFS", "utrlba=", ufs_rd(REG_UTRLBA), "hi=", ufs_rd(REG_UTRLBAU));
	con_hex2("UFS", "dbr=", ufs_rd(REG_UTRLDBR), "nutrs=", (cap & 0x1f) + 1);

	if (!(hce & 1) || !(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY)) {
		con_str("UFS", "host not enabled/ready: stop");
		return;
	}
	if (ufs_rd(REG_UTRLDBR)) {
		con_str("UFS", "requests outstanding: stop");
		return;
	}

#if defined(SHIM_BLUEJAY)
	con_hex2("UFS CLK", "stop=", ufs_rd(0x11b0), "misc=", ufs_rd(0x11b4));
	con_hex2("UFS DMA", "bufhi=", (uint32_t)((uintptr_t)utrl >> 32), "buflo=", (uint32_t)(uintptr_t)utrl);
	/* exynos_ufs_ungate_clks and disable_auto_ctrl_hcc from Linux. */
	ufs_wr(0x11b0, ufs_rd(0x11b0) & ~0x1fu);
	ufs_wr(0x11b4, ufs_rd(0x11b4) & ~0xf0u);
	ufs_wr(0x24, 0); /* no asynchronous interrupt handler owns the UFS */
	zero(utrl, 32 * sizeof(struct utrd));
	/* HCS ready does not tell us whether ABL parked the link in Hibern8. */
	con_hex2("UFS H8", "ahit=", ufs_rd(0x18), "vs=", ufs_rd(0x1138));
	ufs_wr(0x18, 0); /* stop automatic reentry while we own the controller */
	uint32_t value = 0, uic_rc = uic(1, 0x1571u << 16, &value);
	con_hex2("UFS PWR", "rc=", uic_rc, "mode=", value);
	uic_rc = uic(1, 0x4020u << 16, &value);
	con_hex2("UFS PORT", "rc=", uic_rc, "state=", value);
	uic_rc = uic(0x18, 0, &value);
	con_hex2("UFS EXIT", "rc=", uic_rc, "hcs=", ufs_rd(REG_HCS));
#endif
	/* Take over the transfer request list: stop, point at ours, run. */
	ufs_wr(REG_UTRLRSR, 0);
	uintptr_t list = (uintptr_t)utrl;
	ufs_wr(REG_UTRLBA, (uint32_t)list);
	ufs_wr(REG_UTRLBAU, (uint32_t)(list >> 32));
	ufs_wr(REG_UTRLRSR, 1);
	con_hex2("UFS", "ours=", (uint32_t)list, "rsr=", ufs_rd(REG_UTRLRSR));
	/*
	 * Never ring a doorbell unless the host really uses our list: otherwise
	 * it would execute whatever ABL left in its own slot 0 (maybe a write).
	 */
	if (ufs_rd(REG_UTRLBA) != (uint32_t)list || ufs_rd(REG_UTRLBAU) != (uint32_t)(list >> 32) ||
	    !(ufs_rd(REG_UTRLRSR) & 1)) {
		con_str("UFS", "list base not taken: stop");
		return;
	}

#if defined(SHIM_BLUEJAY)
	/* Same Samsung vendor setup as the supplied working BluejayUfs.c. */
	ufs_wr(0x1100, 12);
	ufs_wr(0x1104, 12);
	ufs_wr(0x1160, 0x0a);
	ufs_wr(0x116c, (1u << 31) | (3u << 27) | 3);
#endif
	uint32_t ocs = 0, st = 0, rc;
#if defined(SHIM_BLUEJAY)
	/* ABL leaves the device in a stopped state: sense 02/04/02. */
	static const uint8_t rsdev[6] = { 3, 0, 0, 0, 96, 0 };
	rc = scsi(0xd0, rsdev, sizeof(rsdev), data, 96, &ocs, &st);
	con_hex2("UFS D SENSE", "rc=", rc, "asc/q=", data[12] << 8 | data[13]);
	static const uint8_t active[6] = { 0x1b, 0, 0, 0, 0x10, 0 };
	rc = scsi(0xd0, active, sizeof(active), 0, 0, &ocs, &st);
	con_hex2("UFS ACTIVE", "rc=", rc, "ocs/st=", ocs << 8 | st);
#endif
	static const uint8_t tur[6] = { 0x00 };
	/* A first command may report a unit attention; retry a few times. */
	for (int i = 0; i < 3; i++) {
		rc = scsi(0, tur, sizeof(tur), 0, 0, &ocs, &st);
		con_hex2("UFS TUR", "rc=", rc, "ocs/st=", ocs << 8 | st);
		if (!rc)
			break;
	}

	static const uint8_t sense[6] = { 3, 0, 0, 0, 96, 0 };
	rc = scsi(0, sense, sizeof(sense), data, 96, &ocs, &st);
	con_hex2("UFS RSENSE", "rc=", rc, "ocs/st=", ocs << 8 | st);
	con_hex2("SCSI SENSE", "key=", data[2], "asc/q=", data[12] << 8 | data[13]);

	static const uint8_t cap10[10] = { 0x25 };	/* READ CAPACITY(10) */
	rc = scsi(0, cap10, sizeof(cap10), data, 8, &ocs, &st);
	con_hex2("UFS CAP", "rc=", rc, "ocs/st=", ocs << 8 | st);
	if (!rc)
		con_hex2("UFS LUN0", "lastlba=", be32(data), "blk=", be32(data + 4));

	/* READ(10) LBA 0, 2 blocks: protective MBR and the GPT header. */
	static const uint8_t rd10[10] = { 0x28, 0, 0, 0, 0, 0, 0, 0, 2, 0 };
	rc = scsi(0, rd10, sizeof(rd10), data, 2 * BLOCK, &ocs, &st);
	con_hex2("UFS READ", "rc=", rc, "ocs/st=", ocs << 8 | st);
	if (rc)
		return;
	con_hex2("UFS MBR", "sig=", data[510] | data[511] << 8, "type=", data[450]);
	const uint8_t *gpt = data + BLOCK;
	char sig[9];
	for (int i = 0; i < 8; i++)
		sig[i] = (gpt[i] >= 32 && gpt[i] < 127) ? gpt[i] : '.';
	sig[8] = 0;
	con_str("UFS GPT", sig);
	uint32_t nent = gpt[80] | gpt[81] << 8 | gpt[82] << 16, esz = gpt[84] | gpt[85] << 8;
	uint32_t elba = gpt[72] | gpt[73] << 8;
	con_hex2("UFS GPT", "entries=", nent, "lba=", elba);
	if (esz != 128 || nent > 128)
		return;
	list_partitions(elba, nent);
}
