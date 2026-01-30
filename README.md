# MegaRAID 520-to-512 Byte Sector Format Tools

## Problem Summary

I purchased two refurb'd Samsung PM1643a SSDs (OEM model: SLM5B-M3R8SS) and installed them in a Dell R740 Proxmox server with a PERC H330 controller. The drives were previously used in an enterprise storage array (likely Hitachi/HDS or EMC) and formatted with **520-byte sectors** instead of the standard 512-byte sectors.  This made them pretty unusable without flashing my H330 to IT-mode, which would require unacceptable downtime.

### Drives Fixed
| Slot | Serial | Result |
|------|--------|--------|
| 4 | B4YEK05H | ✅ Fixed - /dev/sdf |
| 5 | B4YEK0GR | ✅ Fixed - /dev/sdg |

### Symptoms
- Drive shows as **"UGUnsp"** (Unconfigured Good Unsupported) in perccli
- **Logical Sector Size: 0 KB** reported by controller
- **Size: 0 KB** - controller can't read capacity
- Drive is NOT exposed to Linux as `/dev/sd*` or `/dev/sg*`
- Samsung DC Toolkit cannot see the drive
- `sg_format` cannot access the drive
- All standard erase/format commands via perccli fail with "Operation not allowed"

### Root Cause
```
smartctl -d megaraid,4 -i /dev/sda

Logical block size:   520 bytes
Physical block size:  4160 bytes
```

The PERC H330 (and most RAID controllers) only support 512-byte or 4096-byte sectors. The 520-byte format includes 8 extra bytes per sector for T10-DIF data integrity protection, used by enterprise storage arrays.

## Solution

I discovered that while the PERC H330 won't expose the drive to Linux, **smartctl can communicate with it** via the MegaRAID passthrough IOCTL. So with the help of Claude Code I reverse-engineered the IOCTL interface from smartctl's source code and created custom tools to send SCSI commands directly to the drive.

### Key Discovery
The MegaRAID driver (`megaraid_sas`) provides a passthrough interface at `/dev/megaraid_sas_ioctl_node` that allows sending SCSI commands to physical drives, even those marked as "unsupported."

## Tools Created

### 1. `mega_inquiry.c` - Drive Identification Tool
Tests SCSI passthrough by sending an INQUIRY command to verify communication with the target drive.

**Usage:**
```bash
gcc -o mega_inquiry mega_inquiry.c
./mega_inquiry /dev/sda <target_id>
```

### 2. `mega_format512.c` - FORMAT UNIT Tool
Sends a FORMAT UNIT SCSI command to reformat the drive to 512-byte sectors.

**Usage:**
```bash
gcc -o mega_format512 mega_format512.c
./mega_format512 /dev/sda <target_id>
```

**Note:** This worked on Drive 1 (slot 4) - it reported "status 45" failure but actually succeeded!

### 3. `mega_modesel.c` - MODE SELECT + FORMAT Tool
Uses MODE SELECT to set block size to 512, then FORMAT UNIT to apply. This was needed for Drive 2 (slot 5) where the direct FORMAT UNIT approach didn't work.

**Usage:**
```bash
gcc -o mega_modesel mega_modesel.c
./mega_modesel /dev/sda <target_id>
```

### 4. `check_size.c` - Structure Validation Tool
Validates that the MegaRAID IOCTL structures match the expected sizes (404 bytes for `megasas_iocpacket`).

## Step-by-Step Process

### Step 1: Identify the Problem Drive
```bash
# List all drives - look for UGUnsp state and 0 KB size
/opt/MegaRAID/perccli/perccli64 /c0 /eall /sall show

# Confirm 520-byte sectors via smartctl passthrough
smartctl -d megaraid,<target_id> -i /dev/sda
```

### Step 2: Try FORMAT UNIT First
```bash
# Compile and run
gcc -o mega_format512 mega_format512.c
./mega_format512 /dev/sda <target_id>

# Check if it worked (even if it reported failure!)
smartctl -d megaraid,<target_id> -i /dev/sda | grep "block size"
```

### Step 3: If FORMAT UNIT Fails, Use MODE SELECT
```bash
# Compile and run
gcc -o mega_modesel mega_modesel.c
./mega_modesel /dev/sda <target_id>

# Check result
smartctl -d megaraid,<target_id> -i /dev/sda | grep "block size"
```

### Step 4: Clear Controller Cache (IMPORTANT!)
The PERC controller caches the "unsupported" state. Even after successful format, perccli may still show UGUnsp. You MUST do one of:

**Option A: Hot-reseat the drive (preferred)**
- Physically unplug and replug the drive
- This forces the controller to re-discover it fresh

**Option B: Reboot the server**
- Only if hot-swap isn't possible

### Step 5: Verify Success
```bash
# Check perccli - should show JBOD with correct size
/opt/MegaRAID/perccli/perccli64 /c0/e32/s<slot> show

# Check Linux sees it
lsscsi -g | grep -i samsu
lsblk

# Full verification
fdisk -l /dev/sdX
smartctl -a /dev/sdX
```

## What Worked for Each Drive

### Drive 1 (Slot 4, Serial B4YEK05H)
1. Ran `mega_format512` - reported "status 45" failure
2. Checked smartctl - showed 512 bytes! (command actually worked)
3. SCSI rescan picked it up as /dev/sdf
4. No hot-reseat needed

### Drive 2 (Slot 5, Serial B4YEK0GR)
1. Ran `mega_format512` - reported "status 45" failure
2. Checked smartctl - still 520 bytes (didn't work)
3. Ran `mega_modesel` (MODE SELECT + FORMAT) - reported success
4. Checked smartctl - showed 512 bytes!
5. Controller still showed UGUnsp (stale cache)
6. **Hot-reseated the drive** (unplug/replug)
7. Controller recognized it as JBOD, appeared as /dev/sdg

## Final Results

| Property | Before | After |
|----------|--------|-------|
| State | UGUnsp | JBOD |
| Size | 0 KB | 3.49 TiB |
| Logical Sector | 520 bytes | 512 bytes |
| Physical Sector | 4160 bytes | 4096 bytes |
| Linux Device | (none) | /dev/sdf, /dev/sdg |
| SMART Health | N/A | OK |

## How It Works

### MegaRAID IOCTL Structure
```c
struct megasas_iocpacket {
  u16 host_no;           // SCSI host number (from SCSI_IOCTL_GET_BUS_NUMBER)
  u16 __pad1;
  u32 sgl_off;           // Offset to scatter-gather list in frame
  u32 sge_count;         // Number of SG elements
  u32 sense_off;
  u32 sense_len;
  union {
    u8 raw[128];
    struct megasas_pthru_frame pthru;  // Passthrough frame
  } frame;
  struct iovec sgl[16];  // Scatter-gather list (userspace pointers)
};
```

### Key SCSI Commands Used
- **INQUIRY (0x12)** - Identify drive
- **MODE SENSE (0x1A)** - Read current block descriptor
- **MODE SELECT (0x15)** - Set new block size
- **FORMAT UNIT (0x04)** - Apply new format
- **READ CAPACITY (0x25)** - Verify block size

## Diagnostic Commands

```bash
# Check drive status in perccli
/opt/MegaRAID/perccli/perccli64 /c0/e32/s<slot> show all

# Read drive info via smartctl passthrough
smartctl -d megaraid,<target_id> -a /dev/sda

# List all drives
/opt/MegaRAID/perccli/perccli64 /c0 /eall /sall show
lsscsi -g
```

## Prerequisites

On the Proxmox/Linux host:
```bash
apt-get install build-essential smartmontools sg3-utils lsscsi
```

## Files in This Directory

| File | Description |
|------|-------------|
| `mega_format512.c` | FORMAT UNIT tool - try this first |
| `mega_modesel.c` | MODE SELECT + FORMAT tool - use if format512 fails |
| `mega_inquiry.c` | INQUIRY test tool to verify passthrough works |
| `check_size.c` | Structure size validation tool |
| `README.md` | This documentation |

## Troubleshooting

### FORMAT command reports failure (status 45) but might have worked
- Always verify with `smartctl -d megaraid,X -i /dev/sda | grep block`
- Status 45 (MFI_STAT_SCSI_IO_FAILED) doesn't always mean failure

### smartctl shows 512 bytes but perccli still shows UGUnsp
- The controller has stale cache
- **Hot-reseat the drive** (physically unplug and replug)
- Or reboot the server

### Neither FORMAT method works
- Try running FORMAT multiple times
- Some drives may need specific firmware
- May need to try in a different system with direct HBA access (LSI 9211-8i in IT mode)

### Drive appears but can't be set to JBOD
- Usually means controller cache is stale
- Hot-reseat should fix this

## References

- [smartmontools source - megaraid.h](https://github.com/smartmontools/smartmontools/blob/master/smartmontools/megaraid.h)
- [smartmontools source - os_linux.cpp](https://github.com/smartmontools/smartmontools/blob/master/smartmontools/os_linux.cpp)
- [Level1Techs - Reformat 520 to 512 bytes](https://forum.level1techs.com/t/how-to-reformat-520-byte-drives-to-512-bytes-usually/133021)
- [setblocksize tool](https://github.com/ahouston/setblocksize)

---

*Created: January 2026*
*Successfully used on Samsung PM1643a (OEM: SLM5B-M3R8SS) on Dell R740 with PERC H330*
*Two drives reformatted from 520-byte to 512-byte sectors via MegaRAID passthrough IOCTL*
