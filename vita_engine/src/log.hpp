#pragma once

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace fsn::log {

inline SceUID g_log_fd = -1;

inline void init(const std::string& path) {
  sceIoMkdir("ux0:data", 0777);
  sceIoMkdir("ux0:data/FateReVita", 0777);
  g_log_fd = sceIoOpen(path.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
}

inline void close() {
  if (g_log_fd >= 0) {
    sceIoClose(g_log_fd);
    g_log_fd = -1;
  }
}

inline void info(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  int n = std::vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  buf[n++] = '\n';
  buf[n] = '\0';
  sceClibPrintf("%s", buf);
  if (g_log_fd >= 0) {
    sceIoWrite(g_log_fd, buf, n);
  }
}

}  // namespace fsn::log
