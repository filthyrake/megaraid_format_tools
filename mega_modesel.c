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
    printf("%s: cmd_status=0x%02x scsi_status=0x%02x\\n", name, pthru->cmd_status, pthru->scsi_status);
    return pthru->cmd_status;
}

int main(int argc, char *argv[]) {
    int fd_dev, fd_mega, bus_no, target;
    
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
    
    if (argc < 3) { printf("Usage: %s <dev> <target>\\n", argv[0]); return 1; }
    target = atoi(argv[2]);
    
    fd_dev = open(argv[1], O_RDWR | O_NONBLOCK);
    ioctl(fd_dev, SCSI_IOCTL_GET_BUS_NUMBER, &bus_no);
    close(fd_dev);
    
    fd_mega = open("/dev/megaraid_sas_ioctl_node", O_RDWR);
    if (fd_mega < 0) { perror("open"); return 1; }
    
    printf("Target %d bus %d\\n\\n", target, bus_no);
    
    printf("Step 1: MODE SELECT - set block size to 512\\n");
    int rc = send_cmd(fd_mega, bus_no, target, mode_sel_cdb, 6, mode_sel_data, 12, MFI_FRAME_DIR_WRITE, "MODE SELECT");
    
    if (rc == 0) {
        printf("\\nStep 2: FORMAT UNIT - apply new settings\\n");
        send_cmd(fd_mega, bus_no, target, format_cdb, 6, NULL, 0, 0, "FORMAT UNIT");
    }
    
    close(fd_mega);
    return 0;
}
