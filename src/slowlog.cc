/*****************************************************************************
*  Copyright 2011 Sergey Shekyan
*
*  Licensed under the Apache License, Version 2.0 (the "License");
*  you may not use this file except in compliance with the License.
*  You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
* *****************************************************************************/

/*****
 * Author: Sergey Shekyan sshekyan@qualys.com
 *
 * Slow HTTP attack  vulnerability test tool
 *  http://code.google.com/p/slowhttptest/
 *****/


#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctime>
#include <string.h>
#include <execinfo.h>

#include "slowlog.h"

namespace {
static FILE* log_file = NULL;
static FILE* csv_file = NULL;
int current_log_level;

void dispose_of_log() {
  if (log_file && log_file != stdout) {
    fclose(log_file);
  }
  if(csv_file) {
    fclose(csv_file);
  }
}


void print_call_stack() {
  static void* buf[64];
  const int depth = backtrace(buf, sizeof(buf)/sizeof(buf[0]));
  backtrace_symbols_fd(buf, depth, fileno(stdout));
  if (stdout != log_file) {
    backtrace_symbols_fd(buf, depth, fileno(log_file));
  }
}
}

namespace slowhttptest {
void slowlog_init(int debug_level, const char* file_name, bool need_csv) {
  log_file = file_name == NULL ? stdout : fopen(file_name, "w");
  if(!log_file) {
    printf("Unable to open log file %s for writing: %s", file_name,
           strerror(errno));
  }
  if(need_csv) {
    time_t rawtime;
    struct tm * timeinfo;
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    char csv_file_name[32] = {0};
    strftime(csv_file_name, 22, "slow_%H%M%Y%m%d.csv", timeinfo);
    csv_file = fopen(csv_file_name , "w");
    if(!csv_file) {
      printf("Unable to open csv file %s for writing: %s\n",
             csv_file_name,
             strerror(errno));
    } else {
      fprintf(csv_file, "Pending,Connected,Closed,Error\n");
    }
  }
  atexit(&dispose_of_log);
  current_log_level = debug_level;
}

void check(bool f, const char* message) {
  if (!f) {
    fprintf(log_file, "%s\n", message);
    fflush(log_file);
    print_call_stack();
    exit(1);
  }   
}

void log_fatal(const char* format, ...) {
  const time_t  now = time(NULL);
  char ctimebuf[32];
  const char* buf = ctime_r(&now, ctimebuf);

  fprintf(log_file, "%-.24s FATAL:", buf);

  va_list va;
  va_start(va, format);
  vfprintf(log_file, format, va);
  va_end(va);
  fflush(log_file);
  print_call_stack();
  exit(1);
}

void dump_csv(const char* format, ...) {
  va_list va;
  va_start(va, format);
  vfprintf(csv_file, format, va);
  fflush(csv_file);
  va_end(va);
}

void slowlog(int lvl, const char* format, ...) {
  if(lvl <= current_log_level) {
    const time_t now = time(NULL);
    char ctimebuf[32];
    const char* buf = ctime_r(&now, ctimebuf);

    fprintf(log_file, "%-.24s:", buf);

    va_list va;
    va_start(va, format);
    vfprintf(log_file, format, va);
    va_end(va);
  }
}

}  // namespace slowhttptest
