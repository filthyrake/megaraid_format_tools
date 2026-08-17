#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <scsi/sg.h>

#define SCSI_IOCTL_GET_BUS_NUMBER 0x5386

#define u8  uint8_t
#define u16 uint16_t
#define u32 uint32_t

#define MEGASAS_MAGIC          'M'
#define MEGASAS_IOC_FIRMWARE   _IOWR(MEGASAS_MAGIC, 1, struct megasas_iocpacket)
#define MFI_CMD_PD_SCSI_IO     0x04
#define MFI_FRAME_DIR_WRITE    0x0008
#define MFI_FRAME_DIR_READ     0x0010
#define MAX_IOCTL_SGE          16

struct megasas_sge32 { u32 phys_addr; u32 length; } __attribute__((packed));
union megasas_sgl { struct megasas_sge32 sge32[1]; } __attribute__((packed));

struct megasas_pthru_frame {
  u8 cmd; u8 sense_len; u8 cmd_status; u8 scsi_status;
  u8 target_id; u8 lun; u8 cdb_len; u8 sge_count;
  u32 context; u32 pad_0;
  u16 flags; u16 timeout; u32 data_xfer_len;
  u32 sense_buf_phys_addr_lo; u32 sense_buf_phys_addr_hi;
  u8 cdb[16];
  union megasas_sgl sgl;
} __attribute__((packed));

struct megasas_iocpacket {
  u16 host_no; u16 __pad1;
  u32 sgl_off; u32 sge_count; u32 sense_off; u32 sense_len;
  union { u8 raw[128]; struct megasas_pthru_frame pthru; } frame;
  struct iovec sgl[MAX_IOCTL_SGE];
} __attribute__((packed));

/* INQUIRY vendor/product are 24 bytes the drive chooses, printed to a root
   operator's terminal. Escape sequences in there could scroll away or overwrite
   a destructive warning, or forge another drive's identity, so emit printable
   ASCII only. */
static void print_ascii(const u8 *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        putchar((s[i] >= 0x20 && s[i] < 0x7f) ? s[i] : '?');
}

/*
 * Parse a MegaRAID target id.
 *
 * atoi() silently turns "4x", "abc" and "" into 0 and returns no error, and
 * target_id is a u8 so 256 wraps to 0 too - either way a mistyped argument
 * aims a command at target 0 instead of refusing. Returns -1 on anything that
 * is not a clean 0-255.
 */
static int parse_target(const char *s) {
    char *end;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 0 || v > 255)
        return -1;
    return (int)v;
}

int main(int argc, char *argv[]) {
    struct megasas_iocpacket ioc;
    struct megasas_pthru_frame *pthru;
    u8 inq_data[96];
    int fd_dev, fd_mega, bus_no, target;
    
    if (argc < 3) {
        printf("Usage: %s <block_device> <target_id>\n", argv[0]);
        return 1;
    }
    target = parse_target(argv[2]);
    if (target < 0) {
        fprintf(stderr, "Invalid target id '%s' - expected 0-255\n", argv[2]);
        return 1;
    }
    
    fd_dev = open(argv[1], O_RDWR | O_NONBLOCK);
    if (fd_dev < 0) { perror("open block device"); return 1; }
    
    if (ioctl(fd_dev, SCSI_IOCTL_GET_BUS_NUMBER, &bus_no) < 0) {
        perror("get bus number"); close(fd_dev); return 1;
    }
    printf("Bus: %d, Target: %d\n", bus_no, target);
    close(fd_dev);
    
    fd_mega = open("/dev/megaraid_sas_ioctl_node", O_RDWR);
    if (fd_mega < 0) { perror("open megaraid"); return 1; }
    
    memset(&ioc, 0, sizeof(ioc));
    memset(inq_data, 0, sizeof(inq_data));
    pthru = &ioc.frame.pthru;
    
    ioc.host_no = bus_no;
    ioc.sge_count = 1;
    ioc.sgl_off = offsetof(struct megasas_pthru_frame, sgl);
    ioc.sgl[0].iov_base = inq_data;
    ioc.sgl[0].iov_len = 96;
    
    pthru->cmd = MFI_CMD_PD_SCSI_IO;
    pthru->cmd_status = 0xFF;
    pthru->target_id = target;
    pthru->cdb_len = 6;
    pthru->sge_count = 1;
    pthru->flags = MFI_FRAME_DIR_READ;
    pthru->data_xfer_len = 96;
    pthru->sgl.sge32[0].phys_addr = (intptr_t)inq_data;
    pthru->sgl.sge32[0].length = 96;
    pthru->cdb[0] = 0x12;
    pthru->cdb[4] = 96;
    
    int rc = ioctl(fd_mega, MEGASAS_IOC_FIRMWARE, &ioc);
    printf("ioctl=%d errno=%d cmd=0x%02x scsi=0x%02x\n",
           rc, errno, pthru->cmd_status, pthru->scsi_status);
    
    if (rc == 0 && pthru->cmd_status == 0) {
        printf("SUCCESS!\nVendor: ");
        print_ascii(inq_data + 8, 8);
        printf("\nProduct: ");
        print_ascii(inq_data + 16, 16);
        putchar('\n');
    }
    close(fd_mega);
    return 0;
}
