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

/* ---- the board (bsd/dev/hakios.input.c on iOS 16) ---- */

#define HAKIOS_RMTFS_POOL       (0x310000u)

static uint8_t *hakios_rmtfs_pool;
extern uint64_t kvtophys(vm_offset_t va);

/*
 * The device's Wi-Fi/modem DMA path accepts only 32-bit physical addresses.
 * KMA_LOMEM pages are fed through vm_page_grablo() only when the VM low-page
 * reserve is enabled. Set its reserve before allocating; pageout fills the
 * queue from pages below max_valid_low_ppnum, which is the same pool KMA_LOMEM
 * uses throughout the kernel. Doing this here reserves the pool before the
 * Wi-Fi firmware and DSP memory are loaded.
 */
static void
hakios_wifi_reserve_pool(void)
{
	vm_offset_t address = 0;
	vm_size_t size = round_page(HAKIOS_RMTFS_POOL);

	if (hakios_rmtfs_pool != NULL) {
		return;
	}
	if (max_valid_low_ppnum == PPNUM_MAX) {
		max_valid_low_ppnum = (ppnum_t)(max_valid_dma_address >> PAGE_SHIFT);
	}
	if (max_valid_low_ppnum == PPNUM_MAX || max_valid_low_ppnum == 0) {
		max_valid_low_ppnum = (ppnum_t)(0x80000000ull >> PAGE_SHIFT);
	}
	if (vm_lopage_free_limit < (size >> PAGE_SHIFT) + 1024) {
		vm_lopage_free_limit = (uint32_t)(size >> PAGE_SHIFT) + 1024;
	}
	if (max_valid_low_ppnum > (ppnum_t)(0x100000000ull >> PAGE_SHIFT)) {
		max_valid_low_ppnum = (ppnum_t)(0x100000000ull >> PAGE_SHIFT);
	}
	vm_lopage_lowater = (size >> PAGE_SHIFT) / 2;
	vm_lopage_needed = TRUE;
	vm_lopage_refill = TRUE;
	printf("wifi: RMTFS allocator: max_dma=%#llx max_low_ppn=%#x low_needed=%d low_free=%u limit=%u\n",
	    (unsigned long long)max_valid_dma_address, max_valid_low_ppnum, vm_lopage_needed,
	    vm_lopage_free_count, vm_lopage_free_limit);
	if (kmem_alloc_contig(kernel_map, &address, size, 0, max_valid_low_ppnum, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA | KMA_LOMEM, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("wifi: no RMTFS pool: low contiguous allocation failed for %u pages (%#x bytes), low free=%u limit=%u cpm=%u/%u\n",
		    (unsigned)(size >> PAGE_SHIFT), (unsigned)size, vm_lopage_free_count, vm_lopage_free_limit,
		    vm_lopages_allocated_cpm_success, vm_lopages_allocated_cpm_failed);
		return;
	}
	hakios_rmtfs_pool = (uint8_t *)address;
	printf("wifi: RMTFS pool reserved: %u bytes at %#lx (physical %#llx..%#llx)\n", HAKIOS_RMTFS_POOL,
	    (unsigned long)address, (unsigned long long)kvtophys(address),
	    (unsigned long long)kvtophys(address + size - PAGE_SIZE));
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

/* ---- GICv3 SPI hooks (Stage C ports osfmk/arm64/gic_v3_irq.c) ---- */
kern_return_t
gic_v3_spi_attach(__unused uint32_t intid, __unused bool edge,
    __unused void (*handler)(void *context, uint32_t intid), __unused void *context)
{
	return KERN_FAILURE;
}
void gic_v3_spi_mask(__unused uint32_t intid) { }
void gic_v3_spi_unmask(__unused uint32_t intid) { }
void gic_v3_spi_detach(__unused uint32_t intid) { }
uint64_t gic_v3_spi_count(__unused uint32_t intid) { return 0; }
