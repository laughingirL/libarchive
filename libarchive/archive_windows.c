/*-
 * Copyright (c) 2003-2007 Kees Zeelenberg
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * $FreeBSD$
 */

/*
 * A set of compatibility glue for building libarchive on Windows platforms.
 *
 * Originally created as "libarchive-nonposix.c" by Kees Zeelenberg
 * for the GnuWin32 project, trimmed significantly by Tim Kientzle.
 *
 * Much of the original file was unnecessary for libarchive, because
 * many of the features it emulated were not strictly necessary for
 * libarchive.  I hope for this to shrink further as libarchive
 * internals are gradually reworked to sit more naturally on both
 * POSIX and Windows.  Any ideas for this are greatly appreciated.
 *
 * The biggest remaining issue is the dev/ino emulation; libarchive
 * has a couple of public APIs that rely on dev/ino uniquely
 * identifying a file.  This doesn't match well with Windows.  I'm
 * considering alternative APIs.
 */

#ifdef _WIN32

#include <errno.h>
#include <stddef.h>
#include <sys/utime.h>
#include <sys/stat.h>
#include <process.h>
#include <stdlib.h>
#include <windows.h>
#include "archive_platform.h"

/* Make a link to FROM called TO.  */
int link (from, to)
     const char *from;
     const char *to;
{
	int res;

	if (from == NULL || to == NULL) {
		set_errno (EINVAL);
		return -1;
	}

	if (!_access (from, F_OK))
		res = CopyFile (from, to, FALSE);
	else {
		/* from doesn not exist; try to prepend it with the dirname of to */
		char *fullfrompath, *slash, *todir;
		todir = strdup (to);
		if (!todir)
			return -1;
		slash = strrchr(todir, '/');
		if (slash)
			*slash = '\0';
		fullfrompath = malloc (strlen (from) + strlen (todir) + 2);
		if (!fullfrompath)
			return -1;
		strcpy (fullfrompath, todir);
		strcat (fullfrompath, "/");
		strcat (fullfrompath, from);
		if (todir)
			free (todir);
		if (_access (fullfrompath, R_OK))
			return -1;
		res = CopyFile (fullfrompath, to, FALSE);
		if (fullfrompath)
			free (fullfrompath);
	}

	if (res == 0) {
		set_errno (EINVAL);
		return -1;
	}
	return 0;
}

/* Make a symbolic link to FROM called TO.  */
int symlink (from, to)
     const char *from;
     const char *to;
{
	return link (from, to);
}

static int get_dev_ino (HANDLE hFile, dev_t *dev, ino_t *ino)
{
/* dev_t: short (2 bytes);  ino_t: unsigned int (4 bytes) */
#define LODWORD(l) ((DWORD)((DWORDLONG)(l)))
#define HIDWORD(l) ((DWORD)(((DWORDLONG)(l)>>32)&0xFFFFFFFF))
#define MAKEDWORDLONG(a,b) ((DWORDLONG)(((DWORD)(a))|(((DWORDLONG)((DWORD)(b)))<<32)))

#define INOSIZE (8*sizeof(ino_t)) /* 32 */
//#define DEVSIZE (8*sizeof(dev_t)) /* 16 */
#define SEQNUMSIZE (16)

	BY_HANDLE_FILE_INFORMATION FileInformation;
	uint64_t ino64, FileReferenceNumber ;
	ino_t resino;
	dev_t resdev;
	DWORD VolumeSerialNumber;
	
	*ino = 0;
	*dev = 0;
	if (hFile == INVALID_HANDLE_VALUE) /* file cannot be opened */
		return 0;
	ZeroMemory (&FileInformation, sizeof(FileInformation));
	if (!GetFileInformationByHandle (hFile, &FileInformation)) /* cannot obtain FileInformation */
		return 0;
	ino64 = (uint64_t) MAKEDWORDLONG (
		FileInformation.nFileIndexLow, FileInformation.nFileIndexHigh);
	FileReferenceNumber = ino64 & ((~(0ULL)) >> SEQNUMSIZE); /* remove sequence number */
	/* transform 64-bits ino into 32-bits by hashing */
	resino = (ino_t) (
			( (LODWORD(FileReferenceNumber)) ^ ((LODWORD(FileReferenceNumber)) >> INOSIZE) )
//		^
//			( (HIDWORD(FileReferenceNumber)) ^ ((HIDWORD(FileReferenceNumber)) >> INOSIZE) )
		);
	*ino = resino;
	VolumeSerialNumber = FileInformation.dwVolumeSerialNumber;
	//resdev = 	(unsigned short) ( (LOWORD(VolumeSerialNumber)) ^ ((HIWORD(VolumeSerialNumber)) >> DEVSIZE) );
	resdev = (dev_t) VolumeSerialNumber;
	*dev = resdev;
//printf ("get_dev_ino: dev = %d; ino = %u\n", resdev, resino);
	return 0;
}

int get_dev_ino_fd (int fd, dev_t *dev, ino_t *ino)
{
	HANDLE hFile;
	hFile = (HANDLE) _get_osfhandle (fd);
	return get_dev_ino (hFile, dev, ino);
}

int get_dev_ino_filename (char *path, dev_t *dev, ino_t *ino)
{
	HANDLE hFile;
	int res;
	if (!path || !*path) /* path = NULL */
		return 0;
	if (_access (path, F_OK)) /* path does not exist */
		return -1;
/* obtain handle to file "name"; FILE_FLAG_BACKUP_SEMANTICS is used to open directories */
	hFile = CreateFile (path, 0, 0, NULL, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS | FILE_ATTRIBUTE_READONLY,
		NULL);
	res = get_dev_ino (hFile, dev, ino);
	CloseHandle (hFile);
	return res;
}

int fstati64 (int fd, struct _stati64 *st)
{
	int res;
	res = _fstati64 (fd, st);
	if (res < 0)
		return -1;
	if (st->st_ino == 0)
		res = get_dev_ino_fd (fd, &st->st_dev, &st->st_ino);
//	printf ("fstat: dev = %u; ino = %u\n", st->st_dev, st->st_ino);
	return res;
}

#define WINTIME(sec, usec)	((Int32x32To64(sec, 10000000) + 116444736000000000) + (usec * 10))
static int
__hutimes(HANDLE handle, const struct __timeval *times)
{
	uint64_t wintm;
	FILETIME fatime, fmtime;

	wintm = WINTIME(times[0].tv_sec, times[0].tv_usec);
	fatime.dwLowDateTime = (DWORD)wintm;
	fatime.dwHighDateTime = wintm >> 32;
	wintm = WINTIME(times[1].tv_sec, times[1].tv_usec);
	fmtime.dwLowDateTime = (DWORD)wintm;
	fmtime.dwHighDateTime = wintm >> 32;
	if (SetFileTime(handle, NULL, &fatime, &fmtime) == 0) {
		errno = EINVAL;
		return (-1);
	}
	return (0);
}

int
futimes(int fd, const struct __timeval *times)
{

	return (__hutimes((HANDLE)_get_osfhandle(fd), times));
}

int
utimes(const char *name, const struct __timeval *times)
{
	int ret;
	HANDLE handle;

	handle = CreateFile(name, GENERIC_READ | GENERIC_WRITE,
	    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	    FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (handle == INVALID_HANDLE_VALUE) {
		errno = EINVAL;
		return (-1);
	}
	ret = __hutimes(handle, times);
	CloseHandle(handle);
	return (ret);
}

__int64
la_lseek(int fd, __int64 offset, int whence)
{
	LARGE_INTEGER distance;
	LARGE_INTEGER newpointer;

	if (fd < 0) {
		errno = EBADF;
		return (-1);
	}
	distance.QuadPart = offset;
	if (!SetFilePointerEx((HANDLE)_get_osfhandle(fd), distance,
	    &newpointer, whence)) {
		errno = _dosmaperr(GetLastError());
		return (-1);
	}
	return (newpointer.QuadPart);
}

/* Windows' mbstowcs is differrent error handling from other unix mbstowcs.
 * That one is using MultiByteToWideChar faunction with MB_PRECOMPOSED and
 * MB_ERR_INVALID_CHARS flags.
 * This implements for only to pass libarchive_test.
 */
size_t
la_mbstowcs(wchar_t *wcstr, const char *mbstr, size_t nwchars)
{

	return (MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS,
	    mbstr, (int)strlen(mbstr), wcstr,
	    (int)nwchars));
}

ssize_t
la_write(int fd, const void *buf, size_t nbytes)
{
	uint32_t bytes_written;

#ifdef _WIN64
	if (nbytes > UINT32_MAX) {
		errno = EINVAL;
		return (-1);
	}
#endif
	if (fd < 0) {
		errno = EBADF;
		return (-1);
	}
	if (!WriteFile((HANDLE)_get_osfhandle(fd), buf, (uint32_t)nbytes,
	    &bytes_written, NULL)) {
		errno = _dosmaperr(GetLastError());
		return (-1);
	}
	return (bytes_written);
}

#endif /* _WIN32 */
