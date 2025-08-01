/* Copyright (C) 2012 Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#ifndef MYSQL_SERVICE_LOGGER_INCLUDED
#define MYSQL_SERVICE_LOGGER_INCLUDED

#ifndef MYSQL_ABI_CHECK
#include <stdarg.h>
#endif
#include <my_attribute.h>

/**
  @file
  logger service

  Log file with rotation implementation.

  This service implements logging with possible rotation
  of the log files. Interface intentionally tries to be similar to FILE*
  related functions.

  So that one can open the log with logger_open(), specifying
  the limit on the logfile size and the rotations number.

  Then it's possible to write messages to the log with
  logger_printf or logger_vprintf functions.

  As the size of the logfile grows over the specified limit,
  it is renamed to 'logfile.1'. The former 'logfile.1' becomes
  'logfile.2', etc. The file 'logfile.rotations' is removed.
  That's how the rotation works.

  The rotation can be forced with the logger_rotate() call.

  Finally the log should be closed with logger_close().

@notes:
  Implementation checks the size of the log file before it starts new
  printf into it. So the size of the file gets over the limit when it rotates.

  The access is secured with the mutex, so the log is threadsafe.
*/


#ifdef __cplusplus
extern "C" {
#endif

typedef struct logger_handle_st LOGGER_HANDLE;

extern struct logger_service_st {
  void (*logger_init_mutexes)();
  LOGGER_HANDLE* (*open)(const char *path,
                         unsigned long long size_limit,
                         unsigned int rotations, size_t buffer_size);
  int (*close)(LOGGER_HANDLE *log);
  int (*vprintf)(LOGGER_HANDLE *log, const char *fmt, va_list argptr)
    ATTRIBUTE_FORMAT_FPTR(printf, 2, 0);
  int (*printf)(LOGGER_HANDLE *log, const char *fmt, ...)
    ATTRIBUTE_FORMAT_FPTR(printf, 2, 3);
  int (*write)(LOGGER_HANDLE *log, const void *data, size_t size);
  int (*rotate)(LOGGER_HANDLE *log);
  int (*sync)(LOGGER_HANDLE *log);
  int (*resize_buffer)(LOGGER_HANDLE *log, size_t new_buffer_size);
  int (*set_filesize_limit)(LOGGER_HANDLE *log,
                            unsigned long long new_file_limit);
  int (*set_rotations)(LOGGER_HANDLE *log, unsigned int new_rotations);
} *logger_service;

#ifdef MYSQL_DYNAMIC_PLUGIN

#define logger_init_mutexes logger_service->logger_init_mutexes
#define logger_open(path, size_limit, rotations, buffer_size) \
  (logger_service->open(path, size_limit, rotations, buffer_size))
#define logger_close(log) (logger_service->close(log))
#define logger_rotate(log) (logger_service->rotate(log))
#define logger_vprintf(log, fmt, argptr) (logger_service->\
    vprintf(log, fmt, argptr))
#define logger_printf (*logger_service->printf)
#define logger_write(log, data, size) \
  (logger_service->write(log, data, size))
#define logger_sync(log) (logger_service->sync(log))
#define logger_resize_buffer(log, new_buffer_size) \
  (logger_service->resize_buffer(log, new_buffer_size))
#define logger_set_filesize_limit(log, new_file_limit) \
  (logger_service->set_filesize_limit(log, new_file_limit))
#define logger_set_rotations(log, new_rotations) \
  (logger_service->set_rotations(log, new_rotations))
#else

  void logger_init_mutexes();
  LOGGER_HANDLE *logger_open(const char *path,
                             unsigned long long size_limit,
                             unsigned int rotations, size_t buffer_size);
  int logger_close(LOGGER_HANDLE *log);
  int logger_vprintf(LOGGER_HANDLE *log, const char *fmt, va_list argptr)
    ATTRIBUTE_FORMAT(printf, 2, 0);
  int logger_printf(LOGGER_HANDLE *log, const char *fmt, ...)
    ATTRIBUTE_FORMAT(printf, 2, 3);
  int logger_write(LOGGER_HANDLE *log, const void *data, size_t size);
  int logger_rotate(LOGGER_HANDLE *log); 
  int logger_sync(LOGGER_HANDLE *log); 
  int logger_resize_buffer(LOGGER_HANDLE *log, size_t new_buffer_size); 
  int logger_set_filesize_limit(LOGGER_HANDLE *log,
                                unsigned long long new_file_limit);
  int logger_set_rotations(LOGGER_HANDLE *log, unsigned int new_rotations);
#endif


#ifdef __cplusplus
}
#endif

#endif /*MYSQL_SERVICE_LOGGER_INCLUDED*/

