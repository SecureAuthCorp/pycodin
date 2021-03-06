// Code for emulating a drive via high-memory accesses.
//
// Copyright (C) 2009  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU LGPLv3 license.

#include "disk.h" // process_ramdisk_op
#include "util.h" // dprintf
#include "memmap.h" // add_e820
#include "biosvar.h" // GET_GLOBAL
#include "bregs.h" // struct bregs

void
describe_ramdisk(struct drive_s *drive_g)
{
    printf("%s", drive_g->model);
}

void
ramdisk_setup(void)
{
    if (!CONFIG_COREBOOT_FLASH || !CONFIG_FLASH_FLOPPY)
        return;

    // Find image.
    struct cbfs_file *file = cbfs_findprefix("floppyimg/", NULL);
    if (!file)
        return;
    u32 size = cbfs_datasize(file);
    dprintf(3, "Found floppy file %s of size %d\n", cbfs_filename(file), size);
    int ftype = find_floppy_type(size);
    if (ftype < 0) {
        dprintf(3, "No floppy type found for ramdisk size\n");
        return;
    }

    // Allocate ram for image.
    void *pos = memalign_tmphigh(PAGE_SIZE, size);
    if (!pos) {
        dprintf(3, "Not enough memory for ramdisk\n");
        return;
    }
    add_e820((u32)pos, size, E820_RESERVED);

    // Copy image into ram.
    cbfs_copyfile(file, pos, size);

    // Setup driver.
    dprintf(1, "Mapping CBFS floppy %s to addr %p\n", cbfs_filename(file), pos);
    struct drive_s *drive_g = addFloppy((u32)pos, ftype, DTYPE_RAMDISK);
    if (drive_g)
        strtcpy(drive_g->model, cbfs_filename(file), ARRAY_SIZE(drive_g->model));
}

static int
ramdisk_copy(struct disk_op_s *op, int iswrite)
{
    u32 offset = GET_GLOBAL(op->drive_g->cntl_id);
    offset += (u32)op->lba * DISK_SECTOR_SIZE;
    u64 opd = GDT_DATA | GDT_LIMIT(0xfffff) | GDT_BASE((u32)op->buf_fl);
    u64 ramd = GDT_DATA | GDT_LIMIT(0xfffff) | GDT_BASE(offset);

    u64 gdt[6];
    if (iswrite) {
        gdt[2] = opd;
        gdt[3] = ramd;
    } else {
        gdt[2] = ramd;
        gdt[3] = opd;
    }

    // Call int 1587 to copy data.
    struct bregs br;
    memset(&br, 0, sizeof(br));
    br.flags = F_CF|F_IF;
    br.ah = 0x87;
    br.es = GET_SEG(SS);
    br.si = (u32)gdt;
    br.cx = op->count * DISK_SECTOR_SIZE / 2;
    call16_int(0x15, &br);

    if (br.flags & F_CF)
        return DISK_RET_EBADTRACK;
    return DISK_RET_SUCCESS;
}

int
process_ramdisk_op(struct disk_op_s *op)
{
    if (!CONFIG_COREBOOT_FLASH || !CONFIG_FLASH_FLOPPY)
        return 0;

    switch (op->command) {
    case CMD_READ:
        return ramdisk_copy(op, 0);
    case CMD_WRITE:
        return ramdisk_copy(op, 1);
    case CMD_VERIFY:
    case CMD_FORMAT:
    case CMD_RESET:
        return DISK_RET_SUCCESS;
    default:
        op->count = 0;
        return DISK_RET_EPARAM;
    }
}
