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

int send_cmd(int fd, int bus, int target, u8 *cdb, int cdblen, void *data, int len, int dir, const char *name) {
    struct megasas_iocpacket ioc;
    struct megasas_pthru_frame *pthru = &ioc.frame.pthru;
    memset(&ioc, 0, sizeof(ioc));
    ioc.host_no = bus;
    if (len > 0) {
        ioc.sge_count = 1;
        ioc.sgl_off = offsetof(struct megasas_pthru_frame, sgl);
        ioc.sgl[0].iov_base = data;
        ioc.sgl[0].iov_len = len;
        pthru->sge_count = 1;
        pthru->data_xfer_len = len;
        pthru->sgl.sge32[0].phys_addr = (intptr_t)data;
        pthru->sgl.sge32[0].length = len;
    }
    pthru->cmd = MFI_CMD_PD_SCSI_IO;
    pthru->cmd_status = 0xFF;
    pthru->target_id = target;
    pthru->cdb_len = cdblen;
    pthru->flags = dir;
    pthru->timeout = 0;
    memcpy(pthru->cdb, cdb, cdblen);
    if (ioctl(fd, MEGASAS_IOC_FIRMWARE, &ioc) < 0) {
        printf("%s: ioctl failed: %s\n", name, strerror(errno));
        return -1;
    }
    /* Only cmd_status is copied back by the driver (a single-byte
       copy_to_user of frame.hdr.cmd_status); scsi_status stays whatever our
       own memset left, so printing it would show a fabricated 0x00. */
    printf("%-12s cmd_status=0x%02x\n", name, pthru->cmd_status);
    return pthru->cmd_status;
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

/* INQUIRY vendor/product are 24 bytes the drive chooses, printed to a root
   operator's terminal. Escape sequences in there could scroll away or overwrite
   a destructive warning, or forge another drive's identity, so emit printable
   ASCII only. */
static void print_ascii(const u8 *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        putchar((s[i] >= 0x20 && s[i] < 0x7f) ? s[i] : '?');
}

int main(int argc, char *argv[]) {
    int fd_dev, fd_mega, bus_no = 0, target;
    u8 inq_data[96];
    u8 inq_cdb[6] = {0x12, 0, 0, 0, 96, 0};

    /* MODE SELECT(6) parameter: header(4) + block descriptor(8) = 12 bytes
       Setting block size to 512 (0x000200) */
    u8 mode_sel_data[12] = {
        0x00,                   /* Mode data length (ignored for MODE SELECT) */
        0x00,                   /* Medium type */
        0x00,                   /* Device-specific parameter */
        0x08,                   /* Block descriptor length = 8 */
        0x00, 0x00, 0x00, 0x00, /* Number of blocks (0 = use drive default) */
        0x00,                   /* Reserved */
        0x00, 0x02, 0x00        /* Block length = 512 bytes */
    };
    
    /* MODE SELECT(6) CDB: PF=1 (page format), SP=0 */
    u8 mode_sel_cdb[6] = {0x15, 0x10, 0x00, 0x00, 12, 0x00};
    
    /* FORMAT UNIT CDB - no data, use mode page settings */
    u8 format_cdb[6] = {0x04, 0x00, 0x00, 0x00, 0x00, 0x00};
    
    if (argc < 3) {
        printf("MegaRAID MODE SELECT + FORMAT UNIT (520->512 byte sectors)\n");
        printf("Usage: %s <block_device> <target_id>\n", argv[0]);
        printf("  <block_device> any drive on the same controller (e.g. /dev/sda);\n");
        printf("                 used only to find the host number, never written to.\n");
        printf("  <target_id>    MegaRAID target id of the drive to format.\n");
        printf("\n");
        printf("Sends a BLOCKING FORMAT UNIT. Safe on SSDs; on a slow or multi-TB\n");
        printf("HDD use mega_format_immed instead - see README, \"The IMMED bit\".\n");
        return 1;
    }
    target = parse_target(argv[2]);
    if (target < 0) {
        fprintf(stderr, "Invalid target id '%s' - expected 0-255\n", argv[2]);
        return 1;
    }

    /* Line-buffer stdout so the warning and countdown below reach the terminal
       as they happen rather than at exit when stdout is a pipe (tee, script). */
    setvbuf(stdout, NULL, _IOLBF, 0);

    fd_dev = open(argv[1], O_RDWR | O_NONBLOCK);
    if (fd_dev < 0) { perror("open dev"); return 1; }
    if (ioctl(fd_dev, SCSI_IOCTL_GET_BUS_NUMBER, &bus_no) < 0) {
        perror("SCSI_IOCTL_GET_BUS_NUMBER");
        close(fd_dev);
        return 1;
    }
    close(fd_dev);
    
    fd_mega = open("/dev/megaraid_sas_ioctl_node", O_RDWR);
    if (fd_mega < 0) { perror("open"); return 1; }
    
    printf("Target %d bus %d\n\n", target, bus_no);

    /* Confirm which drive this actually is before destroying it. This tool used
       to go straight from argv to FORMAT UNIT with no identity check and no
       abort window, so a mistyped target formatted a different drive silently. */
    memset(inq_data, 0, sizeof(inq_data));
    if (send_cmd(fd_mega, bus_no, target, inq_cdb, 6, inq_data, 96, MFI_FRAME_DIR_READ, "INQUIRY")) {
        printf("INQUIRY failed - wrong target?\n");
        close(fd_mega);
        return 1;
    }
    printf("Found: ");
    print_ascii(inq_data + 8, 8);
    putchar(' ');
    print_ascii(inq_data + 16, 16);
    printf("\n\n");

    printf("Step 1: MODE SELECT - set block size to 512\n");
    int rc = send_cmd(fd_mega, bus_no, target, mode_sel_cdb, 6, mode_sel_data, 12, MFI_FRAME_DIR_WRITE, "MODE SELECT");

    if (rc != 0) {
        printf("\nMODE SELECT failed (status 0x%02x) - FORMAT UNIT not sent.\n", rc);
        close(fd_mega);
        /* Used to return 0 here, reporting success for a drive never touched. */
        return 1;
    }

    printf("\n*** FORMATTING TO 512-BYTE SECTORS IN 5 SECONDS ***\n");
    printf("*** ALL DATA WILL BE DESTROYED - Ctrl+C to abort ***\n\n");
    for (int i = 5; i > 0; i--) { printf("%d...\n", i); sleep(1); }

    printf("\nStep 2: FORMAT UNIT - apply new settings\n");
    rc = send_cmd(fd_mega, bus_no, target, format_cdb, 6, NULL, 0, 0, "FORMAT UNIT");

    /* Exit status must survive truncation mod 256: returning a raw -1 would
       surface as 255, and a raw SCSI status could collide with mega_progress's
       exit codes (2 = wrong block size). Report success or failure only. */
    close(fd_mega);
    return (rc == 0) ? 0 : 1;
}
