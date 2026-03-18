#pragma once

#include <stdbool.h>
#include <stdint.h>




bool nvme_init(void);
void nvme_poll_completions(void);
bool nvme_read(uint64_t sector, uint32_t count, void *buf);
bool nvme_write(uint64_t sector, uint32_t count, void *buf);
