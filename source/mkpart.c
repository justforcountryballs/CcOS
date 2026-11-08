/* ccos-mkpart DISK - give the empty rest of the ccos USB stick its own partition
 *
 * A freshly written ccos stick (ccos-usb.img) has one small FAT32 partition
 * with ccos on it, and the rest of the stick is empty. This adds a second
 * partition covering that empty space, so ccos has room for your files and
 * apps. It only ever ADDS a partition in unused space after the last one;
 * it never moves, shrinks or deletes anything. MBR (DOS) partition tables only.
 *
 * Prints the new partition's device name (like /dev/sda2) and exits 0.
 * Exit 2: nothing to do (no table, a GPT disk, no free slot, under 1 GB free).
 *
 * Build: gcc -O2 -static -o ccos-mkpart mkpart.c
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <linux/blkpg.h>

static uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void put32(unsigned char *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: ccos-mkpart /dev/sdX\n"); return 1; }
    int fd = open(argv[1], O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror(argv[1]); return 1; }
    int ss = 512;
    ioctl(fd, BLKSSZGET, &ss);
    if (ss != 512) { fprintf(stderr, "ccos-mkpart: %d-byte sectors not supported\n", ss); return 2; }
    uint64_t bytes = 0;
    if (ioctl(fd, BLKGETSIZE64, &bytes) != 0) { perror("size"); return 1; }
    uint64_t total = bytes / 512;

    unsigned char mbr[512];
    if (pread(fd, mbr, 512, 0) != 512) { perror("read"); return 1; }
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) { fprintf(stderr, "ccos-mkpart: no partition table\n"); return 2; }

    uint64_t end = 0; int slot = -1;
    for (int i = 0; i < 4; i++) {
        unsigned char *e = mbr + 446 + i * 16;
        uint8_t type = e[4];
        uint32_t start = le32(e + 8), size = le32(e + 12);
        if (type == 0xEE) { fprintf(stderr, "ccos-mkpart: GPT disk, leaving it alone\n"); return 2; }
        if (type == 0 || size == 0) { if (slot < 0) slot = i; continue; }
        if ((uint64_t)start + size > end) end = (uint64_t)start + size;
    }
    if (slot < 0) { fprintf(stderr, "ccos-mkpart: no free slot\n"); return 2; }

    uint64_t start = (end + 2047) / 2048 * 2048;          /* on a 1 MB boundary */
    if (start >= total) { fprintf(stderr, "ccos-mkpart: no free space\n"); return 2; }
    uint64_t size = total - start;
    if (size > 0xFFFFFFFFu - 2048) size = 0xFFFFFFFFu - 2048;   /* MBR limit: 2 TB */
    size = size / 2048 * 2048;
    if (size < 2048ULL * 1024) { fprintf(stderr, "ccos-mkpart: under 1 GB free (%llu MB)\n", (unsigned long long)(size / 2048)); return 2; }

    unsigned char *e = mbr + 446 + slot * 16;
    memset(e, 0, 16);
    e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;                /* "use LBA" in the old CHS fields */
    e[4] = 0x83;                                          /* Linux */
    e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
    put32(e + 8, (uint32_t)start); put32(e + 12, (uint32_t)size);
    if (pwrite(fd, mbr, 512, 0) != 512 || fsync(fd) != 0) { perror("write"); return 1; }

    /* tell the kernel about it now (the stick is in use, so it can't re-read the table) */
    struct blkpg_partition bp = { .start = (long long)(start * 512), .length = (long long)(size * 512), .pno = slot + 1 };
    struct blkpg_ioctl_arg a = { .op = BLKPG_ADD_PARTITION, .datalen = sizeof bp, .data = &bp };
    if (ioctl(fd, BLKPG, &a) != 0 && errno != EBUSY) { perror("BLKPG"); return 1; }
    close(fd);

    const char *d = argv[1]; size_t n = strlen(d);
    printf("%s%s%d\n", d, (n && d[n - 1] >= '0' && d[n - 1] <= '9') ? "p" : "", slot + 1);
    return 0;
}
