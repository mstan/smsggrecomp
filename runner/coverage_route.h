#ifndef SMS_COVERAGE_ROUTE_H
#define SMS_COVERAGE_ROUTE_H
#include <stdbool.h>
#include <stdint.h>
bool coverage_route_load(const char *path, uint32_t rom_crc);
uint64_t coverage_route_last_frame(void);
int coverage_route_frame(uint64_t frame);
bool coverage_route_failed(void);
void coverage_route_close(void);
#endif
