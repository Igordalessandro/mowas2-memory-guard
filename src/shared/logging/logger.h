#pragma once

namespace logger {

bool init();

void log(const char* fmt, ...);

void flush_now();

}
