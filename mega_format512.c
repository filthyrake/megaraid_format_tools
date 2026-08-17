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
    return (rc == 0 && pthru->cmd_status == 0) ? 0 : 
           (pthru->cmd_status ? pthru->cmd_status : -1);
}

int main(int argc, char *argv[]) {
    int fd_dev, fd_mega, bus_no = 0, target;
    u8 inq_data[96];
    
    /* FORMAT UNIT short parameter list header (FMTDATA=1, LONGLIST=0).
       byte 0: PROTECTION FIELD USAGE = 0
       byte 1: FOV=0 (use drive defaults), IMMED=0
       bytes 2-3: DEFECT LIST LENGTH = 0 (we supply no defect descriptors)

       This used to be a 12-byte MODE SELECT block descriptor, which the drive
       parsed as "defect list length = 8" followed by two short-block defect
       descriptors - silently adding LBA 0 and LBA 512 to the grown defect list
       on every run. FORMAT UNIT has no block-size field; the sector size comes
       from a preceding MODE SELECT (see mega_modesel.c / mega_format_immed.c). */
    u8 format_param[4] = {0x00, 0x00, 0x00, 0x00};

    u8 inq_cdb[6] = {0x12, 0, 0, 0, 96, 0};
    u8 format_cdb[6] = {0x04, 0x10, 0, 0, 0, 0};
    
    if (argc < 3) {
        printf("MegaRAID Drive Formatter (520->512 byte sectors)\n");
        printf("Usage: %s <block_device> <target_id>\n", argv[0]);
        return 1;
    }
    target = atoi(argv[2]);
    
    fd_dev = open(argv[1], O_RDWR | O_NONBLOCK);
    if (fd_dev < 0) { perror("open"); return 1; }
    if (ioctl(fd_dev, SCSI_IOCTL_GET_BUS_NUMBER, &bus_no) < 0) {
        perror("SCSI_IOCTL_GET_BUS_NUMBER");
        close(fd_dev);
        return 1;
    }
    close(fd_dev);
    
    fd_mega = open("/dev/megaraid_sas_ioctl_node", O_RDWR);
    if (fd_mega < 0) { perror("open megaraid"); return 1; }
    
    printf("Checking target %d on bus %d...\n", target, bus_no);
    memset(inq_data, 0, sizeof(inq_data));
    if (send_cmd(fd_mega, bus_no, target, inq_cdb, 6, inq_data, 96, MFI_FRAME_DIR_READ)) {
        printf("INQUIRY failed - wrong target?\n");
        close(fd_mega);
        return 1;
    }
    printf("Found: %.8s %.16s\n\n", inq_data+8, inq_data+16);
    
    printf("*** FORMATTING TO 512-BYTE SECTORS IN 5 SECONDS ***\n");
    printf("*** ALL DATA WILL BE DESTROYED - Ctrl+C to abort ***\n\n");
    for (int i = 5; i > 0; i--) {
        printf("%d...\n", i);
        sleep(1);
    }
    
    printf("\nSending FORMAT UNIT command...\n");
    int rc = send_cmd(fd_mega, bus_no, target, format_cdb, 6, format_param, sizeof(format_param), MFI_FRAME_DIR_WRITE);
    
    if (rc == 0) {
        printf("\nFORMAT command accepted!\n");
        printf("Format in progress - this will take 30-60 minutes.\n");
        printf("Monitor with: smartctl -d megaraid,%d -a /dev/sda\n", target);
    } else {
        printf("\nFORMAT failed with status: 0x%02x\n", rc);
        if (rc == 0x45)
            printf("(0x45 = MFI_STAT_SCSI_IO_FAILED - on a slow HDD this usually means the\n"
                   " controller timed out a blocking FORMAT UNIT. Use mega_format_immed\n"
                   " instead, which sets the IMMED bit. See README.)\n");
    }
    
    close(fd_mega);
    return rc;
}
