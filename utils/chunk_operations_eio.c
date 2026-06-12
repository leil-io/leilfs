#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

typedef ssize_t (*pread_t)(int, void *, size_t, off_t);
typedef ssize_t (*pwrite_t)(int, const void *, size_t, off_t);
typedef ssize_t (*pwritev_t)(int, const struct iovec *, int, off_t);
typedef int (*close_t)(int);
typedef int (*fsync_t)(int);

#define FILENAME_BUFSIZE 1024
#define COMMAND_BUFSIZE FILENAME_BUFSIZE

// only files with this substring in their name ale influenced by this library
#define FILENAME_TRIGGER "/chunk_"

// offset in a file used in some scenarios
#define FAR_OFFSET_THRESHOLD 102400

int EIO_replies = 0;

// for files which match FILENAME_TRIGGER, this library will cause the following functions to fail:
// * pread always fails with EIO if file name contains "pread_EIO"
// * pwrite always fails with EIO if file name contains "pwrite_EIO"
// * close always fails with EIO if file name contains "close_EIO"
// * fsync always fails with EIO if file name contains "fsync_EIO"
// * pread fails with EIO if offset>FAR_OFFSET_THRESHOLD and file name contains "pread_far_EIO"
// * pwrite fails with EIO if offset>FAR_OFFSET_THRESHOLD and file name contains "pwrite_far_EIO"
// * pread fails with EIO if the first operation and takes 2s more if file name contains
// "pread_slow_and_one_eio_trigger"
// * pwrite fails with EIO if the first operation and takes 2s more if file name contains
// "pwrite_slow_and_one_eio_trigger"
// * pread takes 10ms + 1us per 250B if file name contains "pread_only_slow"
// * pwrite takes 10ms + 1us per 250B if file name contains "pwrite_only_slow"

// returns -1 on failure and sets errno (via readlink call)
ssize_t read_filename(int fd, char *buf, int bufsize) {
	char fdpath[COMMAND_BUFSIZE] = {0};

	sprintf(fdpath, "/proc/self/fd/%d", fd);
	memset(buf, 0, bufsize);
	return readlink(fdpath, buf, bufsize);
}

static int err_on_operation(int fd, const char* opname, size_t offset, size_t size) {
	char filename[FILENAME_BUFSIZE] = {0};
	char always_eio_trigger[COMMAND_BUFSIZE] = {0};
	char far_eio_trigger[COMMAND_BUFSIZE] = {0};
	char slow_and_one_eio_trigger[COMMAND_BUFSIZE] = {0};
	char only_slow[COMMAND_BUFSIZE] = {0};

	ssize_t result = read_filename(fd, filename, FILENAME_BUFSIZE);
	if (result == -1) {
		// cannot read filename, so we assume this file doesn't satisfy the EIO pattern
		return 0;
	}
	if (!strstr(filename, FILENAME_TRIGGER)) {
		return 0;
	}

	// prepare substrings of the filename which trigger errors in various scenarios
	sprintf(always_eio_trigger, "%s_EIO", opname);
	sprintf(far_eio_trigger, "%s_far_EIO", opname);
	sprintf(slow_and_one_eio_trigger, "%s_slow_and_one_EIO", opname);
	sprintf(only_slow, "%s_only_slow", opname);

	// TODO: remove fixed pattern for SMRs after the basic support
	if (strstr(filename, always_eio_trigger) || strstr(filename, "sauna_nullb0")) {
		return EIO;
	} else if (strstr(filename, far_eio_trigger) &&
	           (offset > FAR_OFFSET_THRESHOLD || size > FAR_OFFSET_THRESHOLD - offset)) {
		return EIO;
	} else if (strstr(filename, slow_and_one_eio_trigger)) {
		// sleep for a while to simulate a slow operation
		sleep(2);  // 2 seconds
		if (EIO_replies == 0) {
			EIO_replies++;
			return EIO;
		}
		return 0;
	} else if (strstr(filename, only_slow)) {
		// sleep for a while to simulate a slow operation on hdd of 250MB/s peak throughput
		int kBaseLatency_us = 10 * 1000;  // 10ms
		int bytesPerUs = 250;  // 250B per 1us
		usleep(kBaseLatency_us + size / bytesPerUs);
		return 0;
	} else {
		return 0;
	}
}

// define functions overridden by this library

ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
	int err;
	static pread_t _pread = NULL;

	err = err_on_operation(fd, "pread", offset, count);
	if (err) {
		errno = err;
		return -1;
	}
	if (!_pread) {
		_pread = (pread_t)dlsym(RTLD_NEXT, "pread");
	}
	return _pread(fd, buf, count, offset);
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
	int err;
	static pwrite_t _pwrite = NULL;

	err = err_on_operation(fd, "pwrite", offset, count);
	if (err) {
		errno = err;
		return -1;
	}
	if (!_pwrite) {
		_pwrite = (pwrite_t)dlsym(RTLD_NEXT, "pwrite");
	}
	return _pwrite(fd, buf, count, offset);
}

ssize_t pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset) {
	size_t total = 0;
	for (int i = 0; i < iovcnt; ++i) total += iov[i].iov_len;

	int err = err_on_operation(fd, "pwrite", offset, total);
	if (err) {
		errno = err;
		return -1;
	}

	static pwritev_t _pwritev = NULL;
	if (!_pwritev) { _pwritev = (pwritev_t)dlsym(RTLD_NEXT, "pwritev"); }
	return _pwritev(fd, iov, iovcnt, offset);
}

int close(int fd) {
	int err;
	static close_t _close = NULL;

	err = err_on_operation(fd, "close", 0, 0);
	if (err) {
		errno = err;
		return -1;
	}
	if (!_close) {
		_close = (close_t)dlsym(RTLD_NEXT, "close");
	}
	return _close(fd);
}

int fsync(int fd) {
	int err;
	static fsync_t _fsync = NULL;

	err = err_on_operation(fd, "fsync", 0, 0);
	if (err) {
		errno = err;
		return -1;
	}
	if (!_fsync) {
		_fsync = (fsync_t)dlsym(RTLD_NEXT, "fsync");
	}
	return _fsync(fd);
}
