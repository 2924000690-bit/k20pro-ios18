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
#include <sys/types.h>
#include <sys/hakios.input.h>
#include <dev/sweet_board.h>

/* ---- the board (bsd/dev/hakios.input.c on iOS 16) ---- */

int
hakios_is_raphael(void)
{
	return sweet_board_raphael() ? 1 : 0;
}

/* ---- the SPMI arbiter (bsd/dev/hakios.buttons.c on iOS 16) ----
 *
 * The glue only ever takes this around its own arbiter transaction; with no
 * other SPMI driver in this kernel the lock protects nothing yet, but it must
 * exist so the code stays the same when the buttons driver arrives.
 */
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
 * The iOS 16 port read fw/ out of the small image xnuloader kept in memory.
 * This boot chain has no such image, so the files live on the root volume,
 * where the GPU's already do (docs/GPU_KERNEL.md, scripts/gpu-fw-push.sh):
 *
 *     /usr/local/share/firmware/wifi/<name>
 *
 * The caller wants a buffer it can memcpy from and never gives it back, and it
 * asks for the same file once per segment, so read each name once and keep it.
 * The firmware is a few megabytes and lives as long as the kernel does; that is
 * the same lifetime the small image had.
 */
#define HAKIOS_WIFI_FW_DIR      "/usr/local/share/firmware/wifi/"
#define HAKIOS_WIFI_FW_MAX      (32u << 20)
#define HAKIOS_WIFI_FW_CACHE    64

static LCK_GRP_DECLARE(hakios_fw_lck_grp, "hakios_wifi_fw");
static LCK_MTX_DECLARE(hakios_fw_lock, &hakios_fw_lck_grp);

static struct {
	char            name[64];
	const uint8_t  *data;
	uint64_t        size;
} hakios_fw_cache[HAKIOS_WIFI_FW_CACHE];
static unsigned hakios_fw_cached;

static const uint8_t *
hakios_fw_read(const char *name, uint64_t *size)
{
	char path[160];
	vnode_t vp = NULLVP;
	off_t file_size = 0;
	int error, resid = 0;
	vfs_context_t ctx = vfs_context_kernel();

	if (snprintf(path, sizeof(path), "%s%s", HAKIOS_WIFI_FW_DIR, name) >= (int)sizeof(path)) {
		return NULL;
	}
	if (vnode_lookup(path, 0, &vp, ctx) != 0 || vp == NULLVP) {
		return NULL;
	}
	error = vnode_size(vp, &file_size, ctx);
	if (error != 0 || file_size <= 0 || (uint64_t)file_size > HAKIOS_WIFI_FW_MAX) {
		vnode_put(vp);
		return NULL;
	}
	uint8_t *buffer;
	if (strcmp(name, "0-rmtfs-ram") == 0) {
		/*
		 * The RMTFS pool is not firmware: the glue takes its physical address and hands the
		 * range to the modem, page by page, so it must be physically contiguous and below
		 * 4 GiB (hakios.wifi.c's mpss_storage_prepare() checks both).
		 */
		vm_offset_t address = 0;
		if (kmem_alloc_contig(kernel_map, &address, round_page((vm_size_t)file_size), 0, 0, 0,
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
	if (error != 0 || resid != 0) {
		kfree_data(buffer, (vm_size_t)file_size);
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
		if (strcmp(hakios_fw_cache[i].name, name) == 0) {
			*size = hakios_fw_cache[i].size;
			data = hakios_fw_cache[i].data;
			break;
		}
	}
	if (data == NULL && hakios_fw_cached < HAKIOS_WIFI_FW_CACHE) {
		uint64_t read_size = 0;
		data = hakios_fw_read(name, &read_size);
		if (data != NULL) {
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

/*
 * How often the glue has asked for a firmware file. A boot with wifi=1 runs the
 * first steps before the log link is up, so its lines are not in the captured
 * log; this says from userland that the stack was reached at all.
 *
 *     sysctl kern.sweet_wifi_firmware_lookups
 */
SYSCTL_INT(_kern, OID_AUTO, sweet_wifi_firmware_lookups, CTLFLAG_RD,
    &hakios_wifi_firmware_lookups, 0, "how often the Wi-Fi glue asked for a firmware file");

/*
 * The step the Wi-Fi start is on (bsd/dev/hakios.wifi.c): 0 before it, 1 while it is taking the
 * radio's memory, 6..15 that step, 100 + N if step N failed, 255 when it got through. The start
 * runs before the log link is up, so this is how userland sees where it is.
 */
extern uint32_t hakios_wifi_step_running;

SYSCTL_INT(_kern, OID_AUTO, sweet_wifi_step, CTLFLAG_RD,
    (int *)&hakios_wifi_step_running, 0, "the Wi-Fi start's current step");

/* bsd/dev/hakios.image.c exports this over bsd/dev/memdev.c; memdev.c in 11215
 * has mdevlookup() only. Nothing here can map a disk image's bytes yet. */
int
mdevgetkernelrange(__unused dev_t dev, __unused vm_offset_t *base, __unused uint64_t *size)
{
	return -1;
}

/* ---- en0's Apple80211 side (iokit/Kernel/hakios.wlan.cpp on iOS 16) ---- */

void
HakiosWLANStart(__unused const char *interface, __unused const uint8_t *address)
{
}

void
hakios_virtio_net_init(void)
{
}

/* ---- the loader the iOS 16 port ran under (xnuloader) ----
 *
 * That loader kept a log ring the kernel could write to from any core, and it
 * could take the machine back after a test run (wifi_return=1). The iOS 18 boot
 * chain has neither: every line already goes to printf(), and there is nothing
 * to return to. Returning is therefore a panic -- it is only reached when the
 * run is asked for the loader on purpose.
 */
void
loader_log_putc(__unused char c)
{
}

void
loader_return(void)
{
	panic("hakios: wifi_return=1 asked for the loader, which this boot chain does not have");
}

/* ---- the SNOC's interrupts (osfmk/arm64/gic_v3_irq.c on iOS 16) ----
 *
 * Stage C ports that file. Until then the LinuxKPI layer cannot get an IRQ and
 * says so; the radio's bring-up still runs as far as polling takes it.
 */
kern_return_t
gic_v3_spi_attach(__unused uint32_t intid, __unused bool edge,
    __unused void (*handler)(void *context, uint32_t intid), __unused void *context)
{
	return KERN_FAILURE;
}

void
gic_v3_spi_mask(__unused uint32_t intid)
{
}

void
gic_v3_spi_unmask(__unused uint32_t intid)
{
}

void
gic_v3_spi_detach(__unused uint32_t intid)
{
}

uint64_t
gic_v3_spi_count(__unused uint32_t intid)
{
	return 0;
}
