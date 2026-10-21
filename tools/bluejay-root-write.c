/* Bounded recovery writer for the existing Bluejay userdata partition.
 * Accept Android sparse v1 RAW/FILL only. No device/path selection is exposed.
 * --validate parses the identical stream without opening any block device.
 */
#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <linux/fs.h>

#define BLOCK 4096u
#define IMAGE_BYTES (16ull << 30)
#define USERDATA_BYTES 60619546624ull
#define USERDATA_START512 18321200ull
#define USERDATA_SECTORS512 118397552ull
#define RAW 0xcac1
#define FILL 0xcac2
#define MAX_RAW (8u << 20)

static unsigned char buffer[MAX_RAW];
static bool validating;
static bool checking;
static int target = -1;
static dev_t userdata_rdev;

static void fail(const char *reason)
{
    fprintf(stderr, "BLUEJAY_ROOT_REFUSED: %s (errno=%d)\n", reason, errno);
    exit(1);
}

static uint16_t le16(const unsigned char *p)
{
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t be32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static void exact_read(void *data, size_t size)
{
    unsigned char *p = data;
    while (size) {
        ssize_t n = read(STDIN_FILENO, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) fail("truncated sparse stream");
        p += n;
        size -= (size_t)n;
    }
}

static uint64_t sysfs_number(const char *path)
{
    uint64_t n;
    FILE *f = fopen(path, "r");
    if (!f || fscanf(f, "%" SCNu64, &n) != 1) fail("cannot verify sysfs partition");
    fclose(f);
    return n;
}

static dev_t check_device(const char *path, const char *blockname)
{
    struct stat st;
    char sysfile[128];
    unsigned int ma, mi;
    snprintf(sysfile, sizeof(sysfile), "/sys/class/block/%s/dev", blockname);
    FILE *f = fopen(sysfile, "r");
    if (!f || fscanf(f, "%u:%u", &ma, &mi) != 2)
        fail("cannot verify sysfs device number");
    fclose(f);
    dev_t expected = makedev(ma, mi);
    if (stat(path, &st) || !S_ISBLK(st.st_mode) || st.st_rdev != expected)
        fail("unexpected userdata/protected partition mapping");
    return expected;
}

static void open_userdata(void)
{
    char resolved[256];
    ssize_t len = readlink("/dev/block/by-name/userdata", resolved, sizeof(resolved) - 1);
    if (len < 0) fail("missing userdata by-name link");
    resolved[len] = '\0';
    char *canonical = realpath("/dev/block/by-name/userdata", NULL);
    if (!canonical || strcmp(canonical, "/dev/block/sda27"))
        fail("userdata is not sda27");
    free(canonical);
    userdata_rdev = check_device("/dev/block/by-name/userdata", "sda27");
    dev_t esp_rdev = check_device("/dev/block/by-name/esp", "sda28");
    dev_t win_rdev = check_device("/dev/block/by-name/win", "sda29");
    if (userdata_rdev == esp_rdev || userdata_rdev == win_rdev || esp_rdev == win_rdev)
        fail("protected devices alias userdata");
    if (sysfs_number("/sys/class/block/sda27/start") != USERDATA_START512 ||
        sysfs_number("/sys/class/block/sda27/size") != USERDATA_SECTORS512 ||
        sysfs_number("/sys/class/block/sda28/start") != 136720384ull ||
        sysfs_number("/sys/class/block/sda29/start") != 137695232ull)
        fail("partition table differs from verified Bluejay layout");
    FILE *mounts = fopen("/proc/self/mountinfo", "r");
    if (!mounts) fail("cannot check mounts");
    char line[4096], dev[64], userdata_number[64];
    snprintf(userdata_number, sizeof(userdata_number), "%u:%u",
             major(userdata_rdev), minor(userdata_rdev));
    while (fgets(line, sizeof(line), mounts)) {
        if (sscanf(line, "%*u %*u %63s", dev) != 1) fail("invalid mountinfo");
        if (!strcmp(dev, userdata_number)) fail("userdata is mounted");
    }
    fclose(mounts);
    target = open("/dev/block/sda27", (checking ? O_RDONLY : O_RDWR) | O_CLOEXEC | O_EXCL);
    if (target < 0) fail("cannot exclusively open userdata");
    struct stat st;
    uint64_t size = 0;
    if (fstat(target, &st) || st.st_rdev != userdata_rdev ||
        ioctl(target, BLKGETSIZE64, &size) || size != USERDATA_BYTES ||
        IMAGE_BYTES + (8ull << 20) > size)
        fail("unexpected opened userdata size/device");
    fprintf(stderr, "BLUEJAY_ROOT_TARGET sda27 first=%llu bytes=%llu image=%llu\n",
            (unsigned long long)USERDATA_START512,
            (unsigned long long)size, (unsigned long long)IMAGE_BYTES);
}

static void write_bounded(uint64_t offset, const void *data, size_t size)
{
    /* Final independent guard immediately before every pwrite. */
    if (offset > IMAGE_BYTES || size > IMAGE_BYTES - offset ||
        offset > USERDATA_BYTES || size > USERDATA_BYTES - offset ||
        offset % BLOCK || size % BLOCK)
        fail("write exceeds image/userdata bounds");
    if (validating) return;
    const unsigned char *p = data;
    while (size) {
        ssize_t n = pwrite(target, p, size, (off_t)offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) fail("userdata write failed");
        p += n;
        offset += (uint64_t)n;
        size -= (size_t)n;
    }
}

static void check_hfs_header(const unsigned char *data, size_t size)
{
    if (size < BLOCK || memcmp(data + 1024, "HX\0\5", 4) ||
        be32(data + 1024 + 40) != BLOCK ||
        be32(data + 1024 + 44) != IMAGE_BYTES / BLOCK ||
        !(be32(data + 1024 + 4) & (1u << 13)))
        fail("first block is not the expected journaled 16GiB HFSX root");
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--check-device")) {
        checking = true;
        open_userdata();
        close(target);
        puts("BLUEJAY_ROOT_DEVICE_VALID");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--validate")) validating = true;
    else if (argc != 1) fail("only --validate/--check-device are accepted; no target argument");
    unsigned char header[28];
    exact_read(header, sizeof(header));
    uint32_t chunks = le32(header + 20);
    if (le32(header) != 0xed26ff3a || le16(header + 4) != 1 ||
        le16(header + 6) != 0 || le16(header + 8) != 28 ||
        le16(header + 10) != 12 || le32(header + 12) != BLOCK ||
        le32(header + 16) != IMAGE_BYTES / BLOCK || !chunks ||
        chunks > IMAGE_BYTES / BLOCK || le32(header + 24) != 0)
        fail("unsupported sparse header/size");
    uint64_t offset = 0, next_report = 1ull << 30;
    for (uint32_t i = 0; i < chunks; i++) {
        unsigned char chunk[12];
        exact_read(chunk, sizeof(chunk));
        uint16_t type = le16(chunk);
        uint64_t size = (uint64_t)le32(chunk + 4) * BLOCK;
        uint32_t encoded = le32(chunk + 8);
        if (le16(chunk + 2) || !size || offset > IMAGE_BYTES ||
            size > IMAGE_BYTES - offset) fail("chunk exceeds image bounds");
        if (type == RAW) {
            if (size > MAX_RAW || encoded != size + 12) fail("invalid RAW chunk");
            exact_read(buffer, (size_t)size);
            if (!i) {
                check_hfs_header(buffer, (size_t)size);
                if (!validating) open_userdata();
            }
            write_bounded(offset, buffer, (size_t)size);
        } else if (type == FILL) {
            if (!i || encoded != 16) fail("invalid first/FILL chunk");
            unsigned char value[4];
            exact_read(value, sizeof(value));
            /* Host encoder uses FILL solely for zeroed image ranges. */
            if (le32(value)) fail("only zero FILL is accepted");
            memset(buffer, 0, sizeof(buffer));
            uint64_t left = size, position = offset;
            while (left) {
                size_t n = left < sizeof(buffer) ? (size_t)left : sizeof(buffer);
                write_bounded(position, buffer, n);
                position += n;
                left -= n;
            }
        } else {
            fail("DONT_CARE/CRC/unknown chunks are not accepted");
        }
        offset += size;
        if (offset >= next_report) {
            fprintf(stderr, "BLUEJAY_ROOT_PROGRESS %llu/%llu\n",
                    (unsigned long long)offset, (unsigned long long)IMAGE_BYTES);
            next_report = offset + (1ull << 30);
        }
    }
    if (offset != IMAGE_BYTES) fail("sparse stream does not cover entire image");
    unsigned char trailing;
    ssize_t end;
    do { end = read(STDIN_FILENO, &trailing, 1); } while (end < 0 && errno == EINTR);
    if (end != 0) fail("trailing data/read failure");
    if (!validating && fsync(target)) fail("userdata fsync failed");
    if (target >= 0) close(target);
    puts(validating ? "BLUEJAY_ROOT_STREAM_VALID" : "BLUEJAY_ROOT_INSTALL_DONE");
    return 0;
}
