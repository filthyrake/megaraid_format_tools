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
That drive's mode page evidently already read 512, since FORMAT UNIT on its own
cannot change the sector size (see the next note); don't count on that.
See "The IMMED bit" below for why status 45 (`MFI_STAT_SCSI_IO_FAILED`) shows up here.

**Note:** `FORMAT UNIT` has no block-size field - the sector size comes from a
preceding `MODE SELECT`. This tool only sends `FORMAT UNIT`, so it relies on the
drive's current mode page. If it leaves the drive at 520, use `mega_modesel` or
`mega_format_immed`, which set the block size first.

### 3. `mega_modesel.c` - MODE SELECT + FORMAT Tool
Uses MODE SELECT to set block size to 512, then FORMAT UNIT to apply. This was needed for Drive 2 (slot 5) where the direct FORMAT UNIT approach didn't work.

**Usage:**
```bash
gcc -o mega_modesel mega_modesel.c
./mega_modesel /dev/sda <target_id>
```

### 4. `check_size.c` - Structure Validation Tool
Validates that the MegaRAID IOCTL structures match the expected sizes (404 bytes for `megasas_iocpacket`).

### 5. `mega_format_immed.c` - FORMAT UNIT with IMMED (background format)
Same idea as `mega_modesel.c` (MODE SELECT to 512, then FORMAT UNIT) but the
FORMAT UNIT is sent with the **IMMED bit set** so it returns immediately and the
drive formats in the background. This makes the reformat reliable on slow,
multi-TB spinning drives, not just fast SSDs (see "The IMMED bit" below).

**Usage:**
```bash
gcc -o mega_format_immed mega_format_immed.c
./mega_format_immed /dev/sda <target_id>
# then poll progress every 60s until it finishes:
./mega_progress /dev/sda <target_id> 60
```

### 6. `mega_progress.c` - FORMAT UNIT progress poller
Sends REQUEST SENSE and reports background format progress. While a format runs
the drive answers with sense key `0x2` / ASC `0x04` / ASCQ `0x04`
("LOGICAL UNIT NOT READY, FORMAT IN PROGRESS") plus a 0-65535 progress value.

Both fixed-format (`0x70`/`0x71`) and descriptor-format (`0x72`/`0x73`) sense
data are decoded, and the progress bytes are only trusted when the drive sets
`SKSV`. Once no format is in progress it issues `READ CAPACITY(10)` and reports
the drive's **actual** block size - completion is not inferred from a sense key
of `0x0`, which equally means "nothing has happened yet".

**Usage:**
```bash
gcc -o mega_progress mega_progress.c

# report once and exit
./mega_progress /dev/sda <target_id>
# FORMAT IN PROGRESS: 42.0% (27524/65536)

# or poll every 60s until the format finishes
./mega_progress /dev/sda <target_id> 60
# ...
# Drive ready: block size 512 bytes, 3907029168 blocks (2.00 TB)
```

Exit status: `0` = ready at 512 bytes, `2` = ready but not at 512 bytes,
`3` = not confirmed complete (still formatting, not ready, or a UNIT ATTENTION
got in the way), `1` = error.

`3` is deliberately **not** success: the decision it gates is whether to power
cycle the drive, and a drive must not lose power mid-format. Script it as
`./mega_progress /dev/sda 3 60 && power_cycle` only if you are happy for `2`
and `3` to both block the power cycle - which is the safe direction.

## The IMMED bit: why a format can "fail", hang, or half-complete

By default `FORMAT UNIT` does **not** return until the entire format finishes.
Sent through the MegaRAID passthrough that causes a subtle, drive-dependent bug:

- On a **fast SSD**, the format finishes in seconds/minutes - sometimes before the
  RAID controller's command timeout - so the tool appears to work (this is the
  origin of the *"status 45 failure but it actually worked"* note above; status
  45 = `MFI_STAT_SCSI_IO_FAILED`).
- On a **slow multi-TB 7200rpm HDD**, the format takes **hours**. The controller's
  command timeout fires long before completion and aborts the command with
  `SCSI_IO_FAILED`. The drive is left **half-formatted and invalid**: a SMART
  self-test returns "Input/output error", and the controller reports `0 KB` /
  `UBad`. (Note: `smartctl` may still print `512 bytes / <full capacity>` - that
  is the *intended* geometry from the mode page, not proof the medium is good.)

Setting the **IMMED bit** in the FORMAT UNIT parameter-list header fixes this: the
drive validates the request, returns immediately, and formats in the background.
You then poll with `mega_progress`. `mega_format_immed.c` sets IMMED; the other
formatters do not. If a no-IMMED attempt left a drive half-formatted, just run
`mega_format_immed` and let it complete - the medium recovers once a format
finishes.

## Step-by-Step Process

### Step 1: Identify the Problem Drive
```bash
# List all drives - look for UGUnsp state and 0 KB size
/opt/MegaRAID/perccli/perccli64 /c0 /eall /sall show

# Confirm 520-byte sectors via smartctl passthrough
smartctl -d megaraid,<target_id> -i /dev/sda
```

### Step 2: Choose a formatter based on the drive

**This choice matters - picking wrong can leave the drive unusable.**

| Drive | Use | Why |
|-------|-----|-----|
| SSD | `mega_format512`, then `mega_modesel` if that fails | Format completes inside the controller's command timeout |
| **Spinning HDD, or anything over ~1 TB** | **`mega_format_immed` + `mega_progress`** | A blocking FORMAT UNIT takes hours, hits the controller timeout, and leaves the medium **half-formatted and invalid** |

Do not run `mega_format512` or `mega_modesel` against a slow multi-TB HDD - see
"The IMMED bit" above for what goes wrong and how to recover.

**HDD path:**
```bash
gcc -o mega_format_immed mega_format_immed.c
gcc -o mega_progress mega_progress.c
./mega_format_immed /dev/sda <target_id>

# poll every 60s until it finishes (hours on a multi-TB drive)
./mega_progress /dev/sda <target_id> 60
```

**SSD path:**
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

**Option C: Full COLD power cycle (chassis with no hot-swap backplane)**
- On some servers the drives are cabled directly (no backplane / SES), so you
  can't hot-reseat, and a *warm* reboot is not enough: after the format the drive
  drops into a low-power state and a warm reboot leaves it powered, so the
  controller just re-reads its stale `UGUnsp` / `0 KB` identity. Power the machine
  fully **off**, wait ~30s so the drive spins down and loses power, then power on.
  The controller then discovers it cleanly as a 512-byte JBOD.

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
| `mega_format512.c` | FORMAT UNIT tool - try this first **on SSDs** (not on slow HDDs) |
| `mega_modesel.c` | MODE SELECT + FORMAT tool - use if format512 fails **on SSDs** |
| `mega_format_immed.c` | MODE SELECT + FORMAT UNIT with IMMED (background format; reliable on slow HDDs) |
| `mega_progress.c` | Poll background FORMAT UNIT progress via REQUEST SENSE |
| `mega_inquiry.c` | INQUIRY test tool to verify passthrough works |
| `check_size.c` | Structure size validation tool |
| `README.md` | This documentation |

## Troubleshooting

### FORMAT command reports failure (status 45) but might have worked
- Always verify with `smartctl -d megaraid,X -i /dev/sda | grep block`, or with
  `./mega_progress /dev/sda <target_id>` which reports the drive's real block size
- Status 45 (MFI_STAT_SCSI_IO_FAILED) doesn't always mean failure
- On a **slow HDD** it usually means the controller timed out a blocking
  `FORMAT UNIT` and the medium is now half-formatted - use `mega_format_immed`
  instead. See "The IMMED bit" above.

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

*Also validated on **Dell PowerEdge T130 with PERC H730** (HBA-mode) reformatting*
*2 TB HGST HMRP2000 7200rpm SAS HDDs. On these slow HDDs the non-IMMED FORMAT UNIT*
*timed out and left the medium half-formatted; `mega_format_immed` + `mega_progress`*
*(this PR) made it reliable, and a full cold power cycle was required to clear the*
*controller's stale cache (contributed by @mwventures).*
