/*
 * What the Wi-Fi glue (bsd/dev/hakios.*.c) expects from the rest of the iOS 16
 * tree but xnu-11215 does not have yet.
 *
 * The glue came over from the iOS 16.5 tree unchanged, so it still asks for a
 * few symbols its neighbours there provide: the board's name (hakios.input.c),
 * the SPMI arbiter lock (hakios.buttons.c), the firmware files of the small
 * image (hakios.image.c), the SNOC's MSIs (osfmk/arm64/gic_v3_irq.c) and the
 * Apple80211 boundary (iokit/Kernel/hakios.wlan.cpp).
 *
 * Each of these is a stub until the device it belongs to is ported; none of
 * them changes what the glue compiles to. The interrupt ones report failure,
 * which is what the LinuxKPI layer handles when the radio is started without
 * them (Stage C ports the real GICv3 driver).
 */
#include <kern/debug.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <pexpert/arm/protos.h>
#include <vm/pmap.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/vm_types.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <vm/vm_kern.h>
#include <vm/vm_kern_xnu.h>
#include <vm/vm_page.h>
#include <sys/types.h>
#include <sys/hakios.input.h>
#include <dev/sweet_board.h>

extern uint32_t vm_lopages_allocated_cpm_failed;
extern uint32_t vm_lopages_allocated_cpm_success;
extern uint64_t kvtophys(vm_offset_t va);

/* ---- the board (bsd/dev/hakios.input.c on iOS 16) ---- */

#define HAKIOS_RMTFS_POOL       (0x310000u)

static uint8_t *hakios_rmtfs_pool;
extern uint64_t kvtophys(vm_offset_t va);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys, vm_size_t size, unsigned int flags);

/*
 * The RMTFS pool is not firmware: mpss_storage_prepare() hands its physical
 * range to the modem page by page, so it must be physically contiguous and
 * below 4 GiB. On this board xnu's managed DRAM window is 0x140000000 and up
 * (chosen/dram-base), so no page of it can ever satisfy that; the low 4 GiB is
 * the region the firmware and the two DSPs use, which this kernel already maps
 * directly (SMEM at 0x86000000, the modem image at 0x8e000000).
 *
 * The modem's reserved window there runs to 0x9ae00000 and the image ends at
 * 0x97600000, so the pool is taken from the tail of that window, just before
 * its end. wifi_rmtfs_pa= overrides the address if a probe says otherwise.
 */
#define HAKIOS_RMTFS_POOL       (0x310000u)
#define HAKIOS_RMTFS_PA         (0x9a000000ull)

static void
hakios_wifi_reserve_pool(void)
{
	uint64_t physical = HAKIOS_RMTFS_PA;
	vm_size_t size = round_page(HAKIOS_RMTFS_POOL);
	uint32_t override = 0;

	if (hakios_rmtfs_pool != NULL) {
		return;
	}
	if (PE_parse_boot_argn("wifi_rmtfs_pa", &override, sizeof(override)) && override != 0) {
		physical = (uint64_t)override;          /* MB, so the whole low window fits a 32-bit arg */
		physical <<= 20;
	}
	physical = (physical + PAGE_MASK) & ~(uint64_t)PAGE_MASK;
	if (physical == 0 || physical + size > 0x100000000ull) {
		printf("wifi: no RMTFS pool: %#llx+%u is not below 4 GiB\n",
		    physical, (unsigned)size);
		return;
	}
	vm_offset_t mapped = ml_io_map_unmappable((vm_offset_t)physical, size, VM_WIMG_WCOMB);
	if (mapped == 0) {
		printf("wifi: no RMTFS pool: cannot map %#llx+%u\n", physical, (unsigned)size);
		return;
	}
	bzero((void *)mapped, (size_t)size);
	hakios_rmtfs_pool = (uint8_t *)mapped;
	printf("wifi: RMTFS pool: virtual %#lx, physical %#llx..%#llx, %u bytes\n",
	    (unsigned long)mapped, physical, physical + size - PAGE_SIZE, (unsigned)size);
}

int
hakios_is_raphael(void)
{
	static bool prepared;
	int raphael = sweet_board_raphael() ? 1 : 0;

	if (raphael && !prepared) {
		prepared = true;
		hakios_wifi_reserve_pool();
	}
	return raphael;
}

/* ---- the SPMI arbiter (bsd/dev/hakios.buttons.c on iOS 16) ---- */
static LCK_GRP_DECLARE(hakios_spmi_lck_grp, "hakios_spmi");
static LCK_SPIN_DECLARE(hakios_spmi_arbiter_lock, &hakios_spmi_lck_grp);

void
hakios_spmi_lock(void)
{
	lck_spin_lock(&hakios_spmi_arbiter_lock);
}

void
hakios_spmi_unlock(void)
{
	lck_spin_unlock(&hakios_spmi_arbiter_lock);
}

static int hakios_wifi_firmware_lookups;

/* ---- the firmware the glue asks for (bsd/dev/hakios.image.c on iOS 16) ----
 *
 * The iOS 16 port read fw/ out of xnuloader's small image. This boot chain has
 * no such image, so files live on the root volume under
 * /usr/local/share/firmware/wifi/. Cache each file: the glue asks for the same
 * name once per segment and keeps the returned pointer for the life of the DSP.
 */
#define HAKIOS_WIFI_FW_DIR      "/usr/local/share/firmware/wifi/"
#define HAKIOS_WIFI_FW_MAX      (32u << 20)
#define HAKIOS_WIFI_FW_CACHE    64

static LCK_GRP_DECLARE(hakios_fw_lck_grp, "hakios_wifi_fw");
static LCK_MTX_DECLARE(hakios_fw_lock, &hakios_fw_lck_grp);

static struct {
	char name[64];
	const uint8_t *data;
	uint64_t size;
} hakios_fw_cache[HAKIOS_WIFI_FW_CACHE];
static unsigned hakios_fw_cached;

static const uint8_t *
hakios_fw_read(const char *name, uint64_t *size)
{
	if (strcmp(name, "0-rmtfs-ram") == 0) {
		if (hakios_rmtfs_pool == NULL) {
			hakios_wifi_reserve_pool();
		}
		if (hakios_rmtfs_pool != NULL) {
			*size = HAKIOS_RMTFS_POOL;
			return hakios_rmtfs_pool;
		}
	}
	char path[160];
	vnode_t vp = NULLVP;
	off_t file_size = 0;
	int error, resid = 0;
	vfs_context_t ctx = vfs_context_kernel();

	if (snprintf(path, sizeof(path), "%s%s", HAKIOS_WIFI_FW_DIR, name) >= (int)sizeof(path) ||
	    vnode_lookup(path, 0, &vp, ctx) != 0 || vp == NULLVP) {
		return NULL;
	}
	error = vnode_size(vp, &file_size, ctx);
	if (error || file_size <= 0 || (uint64_t)file_size > HAKIOS_WIFI_FW_MAX) {
		vnode_put(vp);
		return NULL;
	}
	uint8_t *buffer;
	if (strcmp(name, "0-rmtfs-ram") == 0) {
		vm_offset_t address = 0;
		vm_size_t rounded = round_page((vm_size_t)file_size);
		ppnum_t ceiling = (ppnum_t)((0x100000000ull - rounded) >> PAGE_SHIFT);
		if (kmem_alloc_contig(kernel_map, &address, rounded, 0, ceiling, 0,
		    KMA_KOBJECT | KMA_ZERO | KMA_DATA | KMA_LOMEM, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			vnode_put(vp);
			return NULL;
		}
		buffer = (uint8_t *)address;
	} else {
		buffer = kalloc_data((vm_size_t)file_size, Z_WAITOK | Z_ZERO);
	}
	if (buffer == NULL) {
		vnode_put(vp);
		return NULL;
	}
	error = vn_rdwr(UIO_READ, vp, (caddr_t)buffer, (int)file_size, 0, UIO_SYSSPACE,
	    IO_NODELOCKED | IO_UNIT, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
	vnode_put(vp);
	if (error || resid) {
		if (strcmp(name, "0-rmtfs-ram") != 0) {
			kfree_data(buffer, (vm_size_t)file_size);
		}
		return NULL;
	}
	*size = (uint64_t)file_size;
	printf("wifi: firmware %s: %lld bytes from %s\n", name, (long long)file_size, path);
	return buffer;
}

const uint8_t *
hakios_image_file(const char *dir, const char *name, uint64_t *size, __unused int *disk_out)
{
	const uint8_t *data = NULL;
	hakios_wifi_firmware_lookups++;
	if (dir == NULL || name == NULL || size == NULL || strcmp(dir, "fw") != 0 ||
	    strlen(name) >= sizeof(hakios_fw_cache[0].name)) {
		return NULL;
	}
	lck_mtx_lock(&hakios_fw_lock);
	for (unsigned i = 0; i < hakios_fw_cached; i++) {
		if (!strcmp(hakios_fw_cache[i].name, name)) {
			*size = hakios_fw_cache[i].size;
			data = hakios_fw_cache[i].data;
			break;
		}
	}
	if (data == NULL && hakios_fw_cached < HAKIOS_WIFI_FW_CACHE) {
		uint64_t read_size = 0;
		data = hakios_fw_read(name, &read_size);
		if (data) {
			strlcpy(hakios_fw_cache[hakios_fw_cached].name, name,
			    sizeof(hakios_fw_cache[0].name));
			hakios_fw_cache[hakios_fw_cached].data = data;
			hakios_fw_cache[hakios_fw_cached].size = read_size;
			hakios_fw_cached++;
			*size = read_size;
		}
	}
	lck_mtx_unlock(&hakios_fw_lock);
	return data;
}

/* The start runs before the USB log link is up, so userland needs a readout. */
extern uint32_t hakios_wifi_step_running;

SYSCTL_INT(_kern, OID_AUTO, sweet_wifi_firmware_lookups, CTLFLAG_RD,
    &hakios_wifi_firmware_lookups, 0, "Wi-Fi firmware lookups");
SYSCTL_INT(_kern, OID_AUTO, sweet_wifi_step, CTLFLAG_RD,
    (int *)&hakios_wifi_step_running, 0, "Wi-Fi start's current step");

/* ---- loader compatibility ---- */
void
loader_log_putc(__unused char c)
{
}

void
loader_return(void)
{
	panic("wifi_return=1: this boot chain has no loader");
}

/* ---- Apple80211 boundary ---- */
void
HakiosWLANStart(__unused const char *interface, __unused const uint8_t *address)
{
}

void
hakios_virtio_net_init(void)
{
}

/* ---- GICv3 shared peripheral interrupts (SPIs) ----
 *
 * pe_init_fiq() already maps the distributor, enables Group 1 and takes the
 * timer and IPIs. What it does not do is give a driver an SPI: sleh_irq()'s
 * Group 1 branch calls sweet_gic_spi_dispatch() (0294), and this table is what
 * turns an INTID into a handler. The distributor is mapped here from the same
 * ADT node pe_fiq uses, /arm-io/gic, whose reg is <gicd off, size, gicr off,
 * size> relative to the SoC base.
 */
#define GICD_IGROUPR(n)         (0x0080u + 4u * (n))
#define GICD_ISENABLER(n)       (0x0100u + 4u * (n))
#define GICD_ICENABLER(n)       (0x0180u + 4u * (n))
#define GICD_ICPENDR(n)         (0x0280u + 4u * (n))
#define GICD_IPRIORITYR(n)      (0x0400u + (n))
#define GICD_ICFGR(n)           (0x0c00u + 4u * (n))
#define GICD_IROUTER(n)         (0x6000u + 8u * (n))
#define GICD_SPI_PRIORITY       0xa0u

static volatile uint32_t *hakios_gicd;

struct hakios_spi {
	void (*handler)(void *context, uint32_t intid);
	void *context;
};

static struct hakios_spi hakios_spis[1024];

static bool
hakios_gicd_map(void)
{
	DTEntry entry;
	const void *prop;
	unsigned prop_size;
	uint64_t offset, size;

	if (hakios_gicd != NULL) {
		return true;
	}
	if (SecureDTLookupEntry(NULL, "/arm-io/gic", &entry) != kSuccess ||
	    SecureDTGetProperty(entry, "reg", &prop, &prop_size) != kSuccess ||
	    prop_size < 2 * sizeof(uint64_t)) {
		printf("wifi: no GIC distributor in the ADT\n");
		return false;
	}
	offset = ((const uint64_t *)prop)[0];
	size = ((const uint64_t *)prop)[1];
	vm_offset_t mapped = ml_io_map_unmappable((vm_offset_t)(pe_arm_get_soc_base_phys() + offset),
	    (vm_size_t)size, VM_WIMG_IO);
	if (mapped == 0) {
		printf("wifi: cannot map the GIC distributor at %#llx+%#llx\n", offset, size);
		return false;
	}
	hakios_gicd = (volatile uint32_t *)mapped;
	printf("wifi: GIC distributor at %#llx, %#llx bytes\n", offset, size);
	return true;
}

static void
hakios_spi_write(unsigned reg, uint32_t value)
{
	hakios_gicd[reg / 4] = value;
}

static uint32_t
hakios_spi_read(unsigned reg)
{
	return hakios_gicd[reg / 4];
}

kern_return_t
gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context)
{
	if (intid < 32 || intid > 1019 || handler == NULL || !hakios_gicd_map()) {
		return KERN_FAILURE;
	}
	hakios_spis[intid].handler = handler;
	hakios_spis[intid].context = context;

	unsigned word = intid / 32, bit = 1u << (intid % 32);
	/* Group 1 NS: TrustZone owns Group 0 here. */
	hakios_spi_write(GICD_IGROUPR(word), hakios_spi_read(GICD_IGROUPR(word)) | bit);
	hakios_gicd[GICD_IPRIORITYR(intid) / 4] = GICD_SPI_PRIORITY;
	/* ICFGR has 2 bits per interrupt: 0b10 is edge-triggered, 0b00 level. */
	unsigned cfg_off = GICD_ICFGR(intid) / 4;
	uint32_t cfg = hakios_gicd[cfg_off];
	unsigned shift = 2 * (intid % 16) + 1;
	cfg = edge ? (cfg | (1u << shift)) : (cfg & ~(1u << shift));
	hakios_gicd[cfg_off] = cfg;
	/* Route to the processor that attached it (affinity 0 in these MPIDRs). */
	hakios_gicd[GICD_IROUTER(intid) / 4] = 0;
	hakios_gicd[GICD_IROUTER(intid) / 4 + 1] = (uint32_t)(__builtin_arm_rsr64("MPIDR_EL1") & 0xff00ffffffull);
	hakios_spi_write(GICD_ICPENDR(word), bit);
	hakios_spi_write(GICD_ISENABLER(word), bit);
	printf("wifi: SPI %u attached (%s), enabled, routed to this processor\n", intid, edge ? "edge" : "level");
	return KERN_SUCCESS;
}

void
gic_v3_spi_mask(uint32_t intid)
{
	if (intid >= 32 && intid <= 1019 && hakios_gicd != NULL) {
		hakios_spi_write(GICD_ICENABLER(intid / 32), 1u << (intid % 32));
	}
}

void
gic_v3_spi_unmask(uint32_t intid)
{
	if (intid >= 32 && intid <= 1019 && hakios_gicd != NULL) {
		hakios_spi_write(GICD_ISENABLER(intid / 32), 1u << (intid % 32));
	}
}

void
gic_v3_spi_detach(uint32_t intid)
{
	if (intid >= 32 && intid <= 1019) {
		hakios_spis[intid].handler = NULL;
		hakios_spis[intid].context = NULL;
		gic_v3_spi_mask(intid);
	}
}

uint64_t
gic_v3_spi_count(__unused uint32_t intid)
{
	return 0;
}

/* Called from sleh_irq() for every Group 1 interrupt that is not the timer. */
bool
sweet_gic_spi_dispatch(uint32_t intid)
{
	if (intid < 32 || intid > 1019 || hakios_spis[intid].handler == NULL) {
		return false;
	}
	hakios_spis[intid].handler(hakios_spis[intid].context, intid);
	return true;
}
