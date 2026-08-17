/*
 * mega_progress.c - poll FORMAT UNIT progress via MegaRAID passthrough.
 *
 * When a drive is formatted with the IMMED bit set (see mega_format_immed.c),
 * FORMAT UNIT returns immediately and the drive formats in the background.
 * This tool sends REQUEST SENSE and reports progress.
 *
 * While a format runs the drive answers REQUEST SENSE with:
 *   sense key 0x2 (NOT READY), ASC 0x04, ASCQ 0x04
 *   ("LOGICAL UNIT NOT READY, FORMAT IN PROGRESS")
 * and a progress indication in the sense-key-specific bytes (16-17), scaled
 * to 65536. When the format is done the drive reports no error (sense key 0x0).
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

int send_cmd(int fd, int bus, int target, u8 *cdb, int cdblen, void *data, int len, int dir) {
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
    return (rc == 0) ? pthru->cmd_status : -1;
}

int main(int argc, char *argv[]) {
    int fd_dev, fd_mega, bus_no = 0, target;
    u8 sense[96];
    u8 req_sense_cdb[6] = {0x03, 0x00, 0x00, 0x00, sizeof(sense), 0x00};

    if (argc < 3) {
        printf("MegaRAID FORMAT UNIT progress poller\n");
        printf("Usage: %s <block_device> <target_id>\n", argv[0]);
        return 1;
    }
    target = atoi(argv[2]);

    fd_dev = open(argv[1], O_RDWR | O_NONBLOCK);
    if (fd_dev < 0) { perror("open dev"); return 1; }
    ioctl(fd_dev, SCSI_IOCTL_GET_BUS_NUMBER, &bus_no);
    close(fd_dev);

    fd_mega = open("/dev/megaraid_sas_ioctl_node", O_RDWR);
    if (fd_mega < 0) { perror("open megaraid"); return 1; }

    memset(sense, 0, sizeof(sense));
    int rc = send_cmd(fd_mega, bus_no, target, req_sense_cdb, 6, sense, sizeof(sense), MFI_FRAME_DIR_READ);
    if (rc != 0) {
        printf("REQUEST SENSE failed (status 0x%02x) - wrong target?\n", rc);
        close(fd_mega);
        return 1;
    }

    u8 sense_key = sense[2] & 0x0F;
    u8 asc = sense[12];
    u8 ascq = sense[13];

    if (sense_key == 0x02 && asc == 0x04 && ascq == 0x04) {
        int prog = (sense[16] << 8) | sense[17];   /* scaled to 65536 */
        printf("FORMAT IN PROGRESS: %.1f%% (%d/65536)\n", prog * 100.0 / 65536.0, prog);
    } else if (sense_key == 0x00) {
        printf("Drive reports NO error -> format complete / ready.\n");
        printf("Verify: smartctl -d megaraid,%d -i /dev/sda | grep -i 'block size'\n", target);
    } else {
        printf("sense key=0x%x ASC=0x%02x ASCQ=0x%02x (not a format-in-progress state)\n",
               sense_key, asc, ascq);
    }

    close(fd_mega);
    return 0;
}
