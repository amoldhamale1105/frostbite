#ifndef LIB_H
#define LIB_H

#define BASE_NUMERIC_ASCII 48
#define BASE_CAPS_ALPHA_ASCII 65

#include "stdint.h"
#include "stdarg.h"

struct DirEntry {
    uint8_t name[8];
    uint8_t ext[3];
    uint8_t attributes;
    uint8_t reserved;
    uint8_t create_ms;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t attr_index;
    uint16_t m_time;
    uint16_t m_date;
    uint16_t cluster_index;
    uint32_t file_size;
} __attribute__((packed));

#define ENTRY_AVAILABLE 0
#define ENTRY_DELETED 0xe5
#define ATTR_VOLUME_LABEL 0x08
#define ATTR_FILETYPE_DIRECTORY 0x10
#define ATTR_LONG_FILENAME 0x0f

#define MAX_FILENAME_BYTES 8
#define MAX_EXTNAME_BYTES 3

int printf(const char* fmt, ...);
char* itoa(int);
char* uitoa(uint32_t);
char* xtoa(uint64_t);

void memset(void* dst, int value, unsigned int size);
void memcpy(void* dst, void* src, unsigned int size);
void memmove(void* dst, void* src, unsigned int size);
int memcmp(void* src1, void* src2, unsigned int size);

/* System call library functions */

int writeu(char* buf, int buf_size);
void sleep(uint64_t ticks_10ms);
int open_file(char* filename);
int close_file(int fd);
uint32_t get_file_size(int fd);
uint32_t read_file(int fd, void* buffer, uint32_t size);
int fork(void);
void wait(int pid);
int exec(char* prog_file, const char* args[]);
void exit(void);
char getchar(void);
int getpid(void);
int read_root_dir(void* buf);

#endif