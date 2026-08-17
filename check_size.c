#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/uio.h>

#define u8  uint8_t
#define u16 uint16_t
#define u32 uint32_t
#define MAX_IOCTL_SGE 16

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

/* The whole point of this tool is that the layout matches what the megaraid_sas
   driver expects. Asserting at compile time makes a mismatch a build failure
   rather than a line of output nobody reads - main() used to return 0
   unconditionally, so running it in CI proved nothing. */
_Static_assert(sizeof(struct megasas_iocpacket) == 404,
               "megasas_iocpacket must be 404 bytes (0x194)");
_Static_assert(offsetof(struct megasas_iocpacket, frame) == 20,
               "megasas_iocpacket.frame must be at offset 20");
_Static_assert(offsetof(struct megasas_iocpacket, sgl) == 148,
               "megasas_iocpacket.sgl must be at offset 148");

int main(void) {
    printf("sizeof(megasas_iocpacket) = %zu (expected 0x194 = 404)\n", sizeof(struct megasas_iocpacket));
    printf("sizeof(megasas_pthru_frame) = %zu\n", sizeof(struct megasas_pthru_frame));
    printf("sizeof(iovec) = %zu\n", sizeof(struct iovec));
    printf("offsetof sgl_off = %zu\n", offsetof(struct megasas_iocpacket, sgl_off));
    printf("offsetof frame = %zu\n", offsetof(struct megasas_iocpacket, frame));
    printf("offsetof sgl = %zu\n", offsetof(struct megasas_iocpacket, sgl));
    return 0;
}
