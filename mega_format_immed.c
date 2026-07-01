/*
 * mega_format_immed.c - 520->512 reformat via MegaRAID passthrough, IMMED.
 *
 * Like mega_modesel.c (MODE SELECT block size 512, then FORMAT UNIT) but the
 * FORMAT UNIT is sent with the IMMED bit set in the parameter-list header.
 *
 * Why IMMED matters: without it, FORMAT UNIT does not return until the whole
 * format finishes. On a fast SSD that can complete before the controller's
 * command timeout, so the older tools "work" (though they may still report
 * SCSI_IO_FAILED / status 45). On a slow multi-TB 7200rpm SAS HDD the format
 * takes hours, the RAID controller's command timeout fires, and the command is
 * aborted with SCSI_IO_FAILED - leaving the medium HALF-FORMATTED and invalid
 * (SMART self-test returns I/O error; controller reports 0 KB / UBad).
 *
 * With IMMED=1 the drive validates the request, returns immediately, and formats
 * in the BACKGROUND. Poll progress with mega_progress.c. This makes the reformat
 * deterministic across both SSDs and slow HDDs.
 */
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
    int rc = ioctl(fd, MEGASAS_IOC_FIRMWARE, &ioc);
    printf("%-12s cmd_status=0x%02x scsi_status=0x%02x\n", name, pthru->cmd_status, pthru->scsi_status);
    return (rc == 0) ? pthru->cmd_status : -1;
}

int main(int argc, char *argv[]) {
    int fd_dev, fd_mega, bus_no = 0, target;
    u8 inq_data[96];

    /* MODE SELECT(6): header(4) + block descriptor(8); block length 0x000200 = 512 */
    u8 mode_sel_data[12] = {
        0x00, 0x00, 0x00, 0x08,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x02, 0x00
    };
    u8 mode_sel_cdb[6] = {0x15, 0x10, 0x00, 0x00, 12, 0x00};   /* PF=1, SP=0, param len 12 */

    /* FORMAT UNIT: FMTPINFO=00 (no protection info), FMTDATA=1 (byte1 = 0x10) */
    u8 format_cdb[6] = {0x04, 0x10, 0x00, 0x00, 0x00, 0x00};
    /* Parameter list header (short): byte1 IMMED=1 (0x02); defect list length 0 */
    u8 format_param[4] = {0x00, 0x02, 0x00, 0x00};

    u8 inq_cdb[6] = {0x12, 0, 0, 0, 96, 0};

    if (argc < 3) {
        printf("MegaRAID Drive Formatter, IMMED (520->512 byte sectors)\n");
        printf("Usage: %s <block_device> <target_id>\n", argv[0]);
        printf("  <block_device> any drive on the same controller (e.g. /dev/sda);\n");
        printf("                 used only to find the host number, never written to.\n");
        printf("  <target_id>    MegaRAID target id of the drive to format.\n");
        return 1;
    }
    target = atoi(argv[2]);

    fd_dev = open(argv[1], O_RDWR | O_NONBLOCK);
    if (fd_dev < 0) { perror("open dev"); return 1; }
    ioctl(fd_dev, SCSI_IOCTL_GET_BUS_NUMBER, &bus_no);
    close(fd_dev);

    fd_mega = open("/dev/megaraid_sas_ioctl_node", O_RDWR);
    if (fd_mega < 0) { perror("open megaraid"); return 1; }

    printf("Target %d on bus %d\n", target, bus_no);
    memset(inq_data, 0, sizeof(inq_data));
    if (send_cmd(fd_mega, bus_no, target, inq_cdb, 6, inq_data, 96, MFI_FRAME_DIR_READ, "INQUIRY")) {
        printf("INQUIRY failed - wrong target?\n");
        close(fd_mega);
        return 1;
    }
    printf("Found: %.8s %.16s\n\n", inq_data + 8, inq_data + 16);

    printf("*** FORMATTING TO 512-BYTE SECTORS IN 5 SECONDS ***\n");
    printf("*** ALL DATA WILL BE DESTROYED - Ctrl+C to abort ***\n\n");
    for (int i = 5; i > 0; i--) { printf("%d...\n", i); sleep(1); }

    printf("\nStep 1: MODE SELECT - set block size to 512\n");
    int rc = send_cmd(fd_mega, bus_no, target, mode_sel_cdb, 6, mode_sel_data, 12, MFI_FRAME_DIR_WRITE, "MODE SELECT");
    if (rc != 0)
        printf("(MODE SELECT returned 0x%02x; continuing - some drives still format.)\n", rc);

    printf("\nStep 2: FORMAT UNIT with IMMED=1 (returns immediately)\n");
    rc = send_cmd(fd_mega, bus_no, target, format_cdb, 6, format_param, 4, MFI_FRAME_DIR_WRITE, "FORMAT UNIT");

    if (rc == 0) {
        printf("\nAccepted. Drive is now formatting in the BACKGROUND (can take hours\n");
        printf("on a multi-TB HDD). Do NOT power off until it finishes.\n");
        printf("Monitor progress with:\n");
        printf("  ./mega_progress %s %d\n", argv[1], target);
        printf("When done, clear the controller's stale cache (see README) and verify:\n");
        printf("  smartctl -d megaraid,%d -i /dev/sda | grep -i 'block size'\n", target);
    } else {
        printf("\nFORMAT UNIT not accepted (status 0x%02x). Inspect sense:\n", rc);
        printf("  ./mega_progress %s %d\n", argv[1], target);
    }

    close(fd_mega);
    return rc;
}
