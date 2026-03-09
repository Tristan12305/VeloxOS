
//no stack trace yet
#pragma once 

__attribute__((noreturn)) void earlyPanic(void);
__attribute__((noreturn)) void panic(const char* msg);
