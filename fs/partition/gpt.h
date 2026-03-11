#pragma once
#include <stddef.h>
#include <stdint.h>


//simple GPT partition driver.
//reads LBA1 and LBA2 (gpt header and partition entry array.)
//retrievs information 

void read_lba();
