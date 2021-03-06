/* ----------------------------------------------------------------------- *
 *
 *   Copyright 2003 Lars Munch Christensen - All Rights Reserved
 *   Copyright 1998-2008 H. Peter Anvin - All Rights Reserved
 *   Copyright 2012-2015 Pete Batard
 *
 *   Based on the Linux installer program for SYSLINUX by H. Peter Anvin
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, Inc., 53 Temple Place Ste 330,
 *   Boston MA 02111-1307, USA; either version 2 of the License, or
 *   (at your option) any later version; incorporated herein by reference.
 *
 * ----------------------------------------------------------------------- */

/* Memory leaks detection - define _CRTDBG_MAP_ALLOC as preprocessor macro */
#ifdef _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#endif

#include <windows.h>
#include <windowsx.h>
#include <stdio.h>
#include <malloc.h>
#include <ctype.h>

#include "rufus.h"
#include "drive.h"
#include "resource.h"
#include "localization.h"
#include "msapi_utf8.h"

#include "syslinux.h"
#include "syslxfs.h"
#include "libfat.h"
#include "setadv.h"
#include "ntfssect.h"

unsigned char* syslinux_ldlinux[2] = { NULL, NULL };
DWORD syslinux_ldlinux_len[2];
unsigned char* syslinux_mboot = NULL;
DWORD syslinux_mboot_len;

// Workaround for 4K support
uint32_t SECTOR_SHIFT = 9;
uint32_t SECTOR_SIZE = 512;
uint32_t LIBFAT_SECTOR_SHIFT = 9;
uint32_t LIBFAT_SECTOR_SIZE = 512;
uint32_t LIBFAT_SECTOR_MASK = 511;

/*
 * Wrapper for ReadFile suitable for libfat
 */
int libfat_readfile(intptr_t pp, void *buf, size_t secsize,
		    libfat_sector_t sector)
{
	uint64_t offset = (uint64_t) sector * secsize;
	LONG loword = (LONG) offset;
	LONG hiword = (LONG) (offset >> 32);
	LONG hiwordx = hiword;
	DWORD bytes_read;

	if (SetFilePointer((HANDLE) pp, loword, &hiwordx, FILE_BEGIN) != loword ||
		hiword != hiwordx ||
		!ReadFile((HANDLE) pp, buf, (DWORD)secsize, &bytes_read, NULL) ||
		bytes_read != secsize) {
		uprintf("Cannot read sector %u\n", sector);
		return 0;
	}

	return (int)secsize;
}

/*
 * Extract the ldlinux.sys and ldlinux.bss from resources,
 * then patch and install them
 */
BOOL InstallSyslinux(DWORD drive_index, char drive_letter, int fs_type)
{
	HANDLE f_handle = INVALID_HANDLE_VALUE;
	HANDLE d_handle = INVALID_HANDLE_VALUE;
	DWORD bytes_read, bytes_written, err;
	S_NTFSSECT_VOLINFO vol_info;
	LARGE_INTEGER vcn, lba, len;
	S_NTFSSECT_EXTENT extent;
	BOOL r = FALSE;
	FILE* fd;
	size_t length;

	static unsigned char* sectbuf = NULL;
	static char* resource[2][2] = {
		{ MAKEINTRESOURCEA(IDR_SL_LDLINUX_V4_SYS), MAKEINTRESOURCEA(IDR_SL_LDLINUX_V4_BSS) },
		{ MAKEINTRESOURCEA(IDR_SL_LDLINUX_V6_SYS), MAKEINTRESOURCEA(IDR_SL_LDLINUX_V6_BSS) } };
	const char* ldlinux = "ldlinux";
	const char* syslinux = "syslinux";
	const char* ldlinux_ext[3] = { "sys", "bss", "c32" };
	const char* mboot_c32 = "mboot.c32";
	char path[MAX_PATH], tmp[64];
	struct libfat_filesystem *fs;
	libfat_sector_t s, *secp;
	libfat_sector_t *sectors = NULL;
	int ldlinux_sectors;
	uint32_t ldlinux_cluster;
	int i, nsectors;
	int bt = (int)ComboBox_GetItemData(hBootType, ComboBox_GetCurSel(hBootType));
	BOOL use_v5 = (bt == BT_SYSLINUX_V6) || ((bt == BT_ISO) && (SL_MAJOR(iso_report.sl_version) >= 5));

	PrintInfoDebug(0, MSG_234, (bt == BT_ISO)?iso_report.sl_version_str:embedded_sl_version_str[use_v5?1:0]);

	// 4K sector size workaround
	SECTOR_SHIFT = 0;
	SECTOR_SIZE = SelectedDrive.Geometry.BytesPerSector;
	while (SECTOR_SIZE>>=1)
		SECTOR_SHIFT++;
	SECTOR_SIZE = SelectedDrive.Geometry.BytesPerSector;
	LIBFAT_SECTOR_SHIFT = SECTOR_SHIFT;
	LIBFAT_SECTOR_SIZE = SECTOR_SIZE;
	LIBFAT_SECTOR_MASK = SECTOR_SIZE - 1;

	sectbuf = malloc(SECTOR_SIZE);
	if (sectbuf == NULL)
		goto out;
	
	/* Initialize the ADV -- this should be smarter */
	syslinux_reset_adv(syslinux_adv);

	/* Access a copy of the ldlinux.sys & ldlinux.bss resources (downloaded or embedded) */
	if ((syslinux_ldlinux_len[0] != 0) && (syslinux_ldlinux_len[1] != 0)) {
		IGNORE_RETVAL(_chdirU(app_dir));
		for (i=0; i<2; i++) {
			syslinux_ldlinux[i] = (unsigned char*) malloc(syslinux_ldlinux_len[i]);
			if (syslinux_ldlinux[i] == NULL)
				goto out;
			static_sprintf(path, "%s/%s-%s%s/%s.%s", FILES_DIR, syslinux, iso_report.sl_version_str,
				iso_report.sl_version_ext, ldlinux, i==0?"sys":"bss");
			fd = fopen(path, "rb");
			if (fd == NULL) {
				uprintf("Could not open %s\n", path);
				goto out;
			}
			length = fread(syslinux_ldlinux[i], 1, (size_t)syslinux_ldlinux_len[i], fd);
			fclose(fd);
			if (length != (size_t)syslinux_ldlinux_len[i]) {
				uprintf("Could not read %s\n", path);
				goto out;
			}
			uprintf("Using existing './%s'\n", path);
		}
	} else {
		for (i=0; i<2; i++) {
		static_sprintf(tmp, "%s.%s", ldlinux, ldlinux_ext[i]);
		syslinux_ldlinux[i] = GetResource(hMainInstance, resource[use_v5?1:0][i],
			_RT_RCDATA, tmp, &syslinux_ldlinux_len[i], TRUE);
		if (syslinux_ldlinux[i] == NULL)
			goto out;
		}
	}

	/* Create ldlinux.sys file */
	static_sprintf(path, "%C:\\%s.%s", drive_letter, ldlinux, ldlinux_ext[0]);
	f_handle = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
			  FILE_SHARE_READ | FILE_SHARE_WRITE,
			  NULL, CREATE_ALWAYS,
			  FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM |
			  FILE_ATTRIBUTE_HIDDEN, NULL);

	if (f_handle == INVALID_HANDLE_VALUE) {
		uprintf("Unable to create '%s'\n", &path[3]);
		goto out;
	}

	/* Write ldlinux.sys file */
	if (!WriteFile(f_handle, (const char _force *)syslinux_ldlinux[0],
		   syslinux_ldlinux_len[0], &bytes_written, NULL) ||
		bytes_written != syslinux_ldlinux_len[0]) {
		uprintf("Could not write '%s'\n", &path[3]);
		goto out;
	}
	if (!WriteFile(f_handle, syslinux_adv, 2 * ADV_SIZE,
		   &bytes_written, NULL) ||
		bytes_written != 2 * ADV_SIZE) {
		uprintf("Could not write ADV to '%s'\n", &path[3]);
		goto out;
	}

	uprintf("Successfully wrote '%s'\n", &path[3]);
	if (bt != BT_ISO)
		UpdateProgress(OP_DOS, -1.0f);

	/* Now flush the media */
	if (!FlushFileBuffers(f_handle)) {
		uprintf("FlushFileBuffers failed\n");
		goto out;
	}

	/* Reopen the volume (we already have a lock) */
	d_handle = GetLogicalHandle(drive_index, TRUE, FALSE);
	if (d_handle == INVALID_HANDLE_VALUE) {
		uprintf("Could open volume for Syslinux installation\n");
		goto out;
	}

	/* Map the file (is there a better way to do this?) */
	ldlinux_sectors = (syslinux_ldlinux_len[0] + 2 * ADV_SIZE + SECTOR_SIZE - 1) >> SECTOR_SHIFT;
	sectors = (libfat_sector_t*) calloc(ldlinux_sectors, sizeof *sectors);
	if (sectors == NULL)
		goto out;

	switch (fs_type) {
	case FS_NTFS:
		static_sprintf(tmp, "%C:\\", drive_letter);
		vol_info.Handle = d_handle;
		err = NtfsSectGetVolumeInfo(tmp, &vol_info);
		if (err != ERROR_SUCCESS) {
			uprintf("Could not fetch NTFS volume info");
			goto out;
		}
		secp = sectors;
		nsectors = 0;
		for (vcn.QuadPart = 0;
			NtfsSectGetFileVcnExtent(f_handle, &vcn, &extent) == ERROR_SUCCESS;
			vcn = extent.NextVcn) {
				err = NtfsSectLcnToLba(&vol_info, &extent.FirstLcn, &lba);
				if (err != ERROR_SUCCESS) {
					uprintf("Could not translate LDLINUX.SYS LCN to disk LBA");
					goto out;
				}
				lba.QuadPart -= vol_info.PartitionLba.QuadPart;
				len.QuadPart = ((extent.NextVcn.QuadPart -
					extent.FirstVcn.QuadPart) *
					vol_info.SectorsPerCluster);
				while (len.QuadPart-- && nsectors < ldlinux_sectors) {
					*secp++ = lba.QuadPart++;
					nsectors++;
				}
		}
		break;
	case FS_FAT16:
	case FS_FAT32:
	case FS_EXFAT:
		fs = libfat_open(libfat_readfile, (intptr_t) d_handle);
		if (fs == NULL) {
			uprintf("Syslinux FAT access error\n");
			goto out;
		}
		ldlinux_cluster = libfat_searchdir(fs, 0, "LDLINUX SYS", NULL);
		secp = sectors;
		nsectors = 0;
		s = libfat_clustertosector(fs, ldlinux_cluster);
		while (s && nsectors < ldlinux_sectors) {
			*secp++ = s;
			nsectors++;
			s = libfat_nextsector(fs, s);
		}
		libfat_close(fs);
		break;
	default:
		uprintf("Unsupported Syslinux filesystem\n");
		goto out;
	}

	/* Patch ldlinux.sys and the boot sector */
	syslinux_patch(sectors, nsectors, 0, 0, NULL, NULL);

	/* Rewrite the file */
	if (SetFilePointer(f_handle, 0, NULL, FILE_BEGIN) != 0 ||
		!WriteFile(f_handle, syslinux_ldlinux[0], syslinux_ldlinux_len[0],
			   &bytes_written, NULL)
		|| bytes_written != syslinux_ldlinux_len[0]) {
		uprintf("Could not write '%s': %s\n", &path[3], WindowsErrorString());
		goto out;
	}

	/* Close file */
	safe_closehandle(f_handle);

	/* Read existing FAT data into boot sector */
	if (SetFilePointer(d_handle, 0, NULL, FILE_BEGIN) != 0 ||
		!ReadFile(d_handle, sectbuf, SECTOR_SIZE,
			   &bytes_read, NULL)
		|| bytes_read != SECTOR_SIZE) {
		uprintf("Could not read boot record: %s\n", WindowsErrorString());
		goto out;
	}

	/* Make the syslinux boot sector */
	syslinux_make_bootsect(sectbuf, (fs_type == FS_NTFS)?NTFS:VFAT);

	/* Write boot sector back */
	if (SetFilePointer(d_handle, 0, NULL, FILE_BEGIN) != 0 ||
		!WriteFile(d_handle, sectbuf, SECTOR_SIZE,
			   &bytes_written, NULL)
		|| bytes_written != SECTOR_SIZE) {
		uprintf("Could not write Syslinux boot record: %s\n", WindowsErrorString());
		goto out;
	}

	uprintf("Successfully wrote Syslinux boot record\n");

	if (bt == BT_SYSLINUX_V6) {
		IGNORE_RETVAL(_chdirU(app_dir));
		static_sprintf(path, "%s/%s-%s", FILES_DIR, syslinux, embedded_sl_version_str[1]);
		IGNORE_RETVAL(_chdir(path));
		static_sprintf(path, "%C:\\%s.%s", drive_letter, ldlinux, ldlinux_ext[2]);
		fd = fopen(&path[3], "rb");
		if (fd == NULL) {
			uprintf("Caution: No '%s' was provided. The target will be missing a required Syslinux file!\n", &path[3]);
		} else {
			fclose(fd);
			if (CopyFileA(&path[3], path, TRUE)) {
				uprintf("Created '%s' (from '%s/%s-%s/%s')", path, FILES_DIR, syslinux, embedded_sl_version_str[1], &path[3]);
			} else {
				uprintf("Failed to create '%s': %s\n", path, WindowsErrorString());
			}
		}
	} else if (IS_REACTOS(iso_report)) {
		uprintf("Setting up ReactOS...\n");
		syslinux_mboot = GetResource(hMainInstance, MAKEINTRESOURCEA(IDR_SL_MBOOT_C32),
			_RT_RCDATA, "mboot.c32", &syslinux_mboot_len, FALSE);
		if (syslinux_mboot == NULL) {
			goto out;
		}
		/* Create mboot.c32 file */
		static_sprintf(path, "%C:\\%s", drive_letter, mboot_c32);
		f_handle = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
				  FILE_SHARE_READ | FILE_SHARE_WRITE,
				  NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (f_handle == INVALID_HANDLE_VALUE) {
			uprintf("Unable to create '%s'\n", path);
			goto out;
		}
		if (!WriteFile(f_handle, syslinux_mboot, syslinux_mboot_len,
			   &bytes_written, NULL) ||
			bytes_written != syslinux_mboot_len) {
			uprintf("Could not write '%s'\n", path);
			goto out;
		}
		safe_closehandle(f_handle);
		static_sprintf(path, "%C:\\syslinux.cfg", drive_letter);
		fd = fopen(path, "w");
		if (fd == NULL) {
			uprintf("Could not create ReactOS 'syslinux.cfg'\n");
			goto out;
		}
		/* Write the syslinux.cfg for ReactOS */
		fprintf(fd, "DEFAULT ReactOS\nLABEL ReactOS\n  KERNEL %s\n  APPEND %s\n",
			mboot_c32, iso_report.reactos_path);
		fclose(fd);
	}

	if (bt != BT_ISO)
		UpdateProgress(OP_DOS, -1.0f);

	r = TRUE;

out:
	safe_free(sectbuf);
	safe_free(syslinux_ldlinux[0]);
	safe_free(syslinux_ldlinux[1]);
	safe_free(sectors);
	safe_closehandle(d_handle);
	safe_closehandle(f_handle);
	return r;
}

uint16_t GetSyslinuxVersion(char* buf, size_t buf_size, char** ext)
{
	size_t i, j, k;
	char *p;
	uint16_t version;
	const char LINUX[] = { 'L', 'I', 'N', 'U', 'X', ' ' };
	static char* nullstr = "";
	char unauthorized[] = {'<', '>', ':', '|', '*', '?', '\\', '/'};

	*ext = nullstr;
	if (buf_size < 256)
		return 0;

	// Start at 64 to avoid the short incomplete version at the beginning of ldlinux.sys
	for (i=64; i<buf_size-64; i++) {
		if (memcmp(&buf[i], LINUX, sizeof(LINUX)) == 0) {
			// Check for ISO or SYS prefix
			if (!( ((buf[i-3] == 'I') && (buf[i-2] == 'S') && (buf[i-1] == 'O'))
			    || ((buf[i-3] == 'S') && (buf[i-2] == 'Y') && (buf[i-1] == 'S')) ))
			  continue;
			i += sizeof(LINUX);
			version = (((uint8_t)strtoul(&buf[i], &p, 10))<<8) + (uint8_t)strtoul(&p[1], &p, 10);
			if (version == 0)
				continue;
			p[safe_strlen(p)] = 0;
			// Ensure that our extra version string starts with a slash
			*p = '/';
			// Remove the x.yz- duplicate if present
			for (j=0; (buf[i+j] == p[1+j]) && (buf[i+j] != ' '); j++);
			if (p[j+1] == '-')
				j++;
			if (j >= 4) {
				p[j] = '/';
				p = &p[j];
			}
			for (j=safe_strlen(p)-1; j>0; j--) {
				// Arch Linux affixes a star for their version - who knows what else is out there...
				if ((p[j] == ' ') || (p[j] == '*'))
					p[j] = 0;
				else
					break;
			}
			// Sanitize the string
			for (j=1; j<safe_strlen(p); j++) {
				// Some people are bound to have invalid chars in their date strings
				for (k=0; k<sizeof(unauthorized); k++) {
					if (p[j] == unauthorized[k])
						p[j] = '_';
				}
			}
			// If all we have is a slash, return the empty string for the extra version
			*ext = (p[1] == 0)?nullstr:p;
			return version;
		}
	}
	return 0;
}
