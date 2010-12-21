/*
    Copyright (C) 2010  Guhl, scotty2

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#define _GNU_SOURCE

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>

#include "wpthis.h"
#include "gopt.h"

/*
c02a0570:       e3530802        cmp     r3, #131072     ; 0x20000
c02a0574:       2a000012        bcs     c02a05c4 <mmc_blk_issue_rq+0x3a0>
c02a0578:       e1a0c00d        mov     ip, sp
c02a057c:       e59f16d4        ldr     r1, [pc, #1748] ; c02a0c58 <mmc_blk_issue_rq+0xa34>
c02a0580:       e3cc3d7f        bic     r3, ip, #8128   ; 0x1fc0
c02a0584:       e59f06d0        ldr     r0, [pc, #1744] ; c02a0c5c <mmc_blk_issue_rq+0xa38>
c02a0588:       e3c3303f        bic     r3, r3, #63     ; 0x3f
c02a058c:       e593200c        ldr     r2, [r3, #12]
c02a0590:       e2823fc7        add     r3, r2, #796    ; 0x31c
c02a0594:       e58d3000        str     r3, [sp]
c02a0598:       e5923228        ldr     r3, [r2, #552]
c02a059c:       e5922224        ldr     r2, [r2, #548]
c02a05a0:       eb05330b        bl      c03ed1d4 <printk>
c02a05a4:       e5952038        ldr     r2, [r5, #56]
c02a05a8:       e59d1040        ldr     r1, [sp, #64]
c02a05ac:       e59f06ac        ldr     r0, [pc, #1708] ; c02a0c60 <mmc_blk_issue_rq+0xa3c>
c02a05b0:       e1a024a2        lsr     r2, r2, #9
c02a05b4:       eb053306        bl      c03ed1d4 <printk>
c02a05b8:       e59f06a4        ldr     r0, [pc, #1700] ; c02a0c64 <mmc_blk_issue_rq+0xa40>
c02a05bc:       e30011be        movw    r1, #446        ; 0x1be
c02a05c0:       ebf64b3e        bl      c00332c0 <__bug>
*/

uint32_t brqFilter[] = {0xe3530802,
			0x2a000012, // this guy is going to be the bcs to PAST the bug() call, we're going to make it a b,
			0xe1a0c00d, // which means it's going to turn from [2a xx xx xx] to [ea xx xx xx]
			0xe59f16d4,
			0xe3cc3d7f,
			0xe59f06d0,
			0xe3c3303f,
			0xe593200c,
			0xe2823fc7,
			0xe58d3000,
			0xe5923228,
			0xe5922224,
			0xeb05330b,
			0xe5952038,
			0xe59d1040,
			0xe59f06ac,
			0xe1a024a2,
			0xeb053306,
			0xe59f06a4,
			0xe30011be,
			0xebf64b3e};

uint32_t brqMasks[] = {0xff000000,
		       0x00000000,
		       0xff000000,
		       0xff000000,
		       0xff000000,
		       0xff000000,
		       0xff000000,
		       0xff000000,
		       0xff000000,
		       0xff000000,
		       0xff000000,
		       0xff000000,
		       0xff000000,
		       0xff000000,
		       0xff000000,
		       0xff000000,
		       0xff000000,
		       0xff000000,
		       0xff000000,
		       0xff000000,
		       0xff000000};

struct elfHeader
{
    uint8_t  ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct sectionHeader
{
    uint32_t name;
    uint32_t type;
    uint32_t flags;
    uint32_t addr;
    uint32_t offset;
    uint32_t size;
    uint32_t link;
    uint32_t info;
    uint32_t addralign;
    uint32_t entsize;
};

struct listEnt
{
    char *string;
    struct listEnt *current;
    struct listEnt *next;
};

void freeStrings(struct listEnt **root);
int addString(char *string, struct listEnt **root);
void *fuzzyInstSearch(uint32_t *needle, uint32_t *haystack, uint32_t *masks, uint32_t needleLength, uint32_t haystackLength);
extern long init_module(void *umod, unsigned long len, const char *uargs);

#define MAX_FUNC_LEN 0xa6c
#define INFILE "/dev/block/mmcblk0p7"
#define OUTFILE "/dev/block/mmcblk0p7"

#define MOD_RET_OK       ENOSYS
#define MOD_RET_FAILINIT ENOTEMPTY
#define MOD_RET_FAILWP   ELOOP
#define MOD_RET_FAIL     ENOMSG
#define MOD_RET_NONEED   EXFULL

#define VERSION_A	0
#define VERSION_B	02

int debug = 0;

int main(int argc, const char **argv)
{
    struct elfHeader *header;
    struct sectionHeader *section;
    struct sectionHeader *modInfoSection;
    uint8_t *stringTable;
    unsigned int ent;
    void *modInfo = 0;
    size_t modSize;
    struct listEnt *stringRoot = 0;
    struct listEnt *current;
    void *curString;
    int neededBuffer = 0;
    void *buffer;
    void *tmpBuffer;
//    FILE *output;
    struct utsname kernInfo;
    FILE *kallsyms;
    char tempString[256];
    uint32_t brqAddress = 0;
    uint32_t mapBase;
    uint32_t pageSize;
    void *kernel;
    uint32_t *filterAddress;
    int kernelFD;
    FILE *fdin, *fdout;
    char ch;
    char *backupFile;
    time_t ourTime;

    int cid = 0, secu_flag = 0, sim_unlock = 0, verify = 0, help = 0, disable_wp = 0;
    const char* s_secu_flag;
    const char* s_cid;
    
    if(argc > 1)
    {
	void *options= gopt_sort( & argc, argv, gopt_start(
		gopt_option('h', 0, gopt_shorts('h', '?'), gopt_longs("help", "HELP")),
		gopt_option('v', 0, gopt_shorts('v'), gopt_longs("version")),
		gopt_option('s', GOPT_ARG, gopt_shorts('s'), gopt_longs("secu_flag")),
		gopt_option('c', GOPT_ARG, gopt_shorts('c'), gopt_longs("cid")),
		gopt_option('S', 0, gopt_shorts('S'), gopt_longs("sim_unlock")),
		gopt_option('f', 0, gopt_shorts('f'), gopt_longs("free_all")),
		gopt_option('w', 0, gopt_shorts('w'), gopt_longs("disable_wp")),
		gopt_option('d', 0, gopt_shorts('d'), gopt_longs("debug"))));
	
	
	if(gopt(options, 'v'))
	{
	    //if any of the version options was specified
	    printf("gfree version: %d.%d\n", VERSION_A, VERSION_B);
	    exit(0);
	}
	
	if(gopt_arg(options, 's', &s_secu_flag))
	{
	    // if -s or --secu_flag was specified, check s_secu_flag
	    if(!strcmp(s_secu_flag, "on"))
	    {
		secu_flag = 1;
		printf("--secu_flag on set\n");
	    }
	    else if(!strcmp(s_secu_flag, "off"))
	    {
		secu_flag = 2;
		printf("--secu_flag off set\n");
	    }
	}
	
	if(gopt_arg(options, 'c', &s_cid))
	{
	    // if -c or --cid was specified, check s_cid
	    size_t size;
	    size = strlen(s_cid);
	    if(size != 8)
	    {
		printf("Error: CID must be a 8 character string. Length of specified string: %d\n", (int)size);
		exit(1);
	    } 
	    else 
	    {
		cid = 1;
		printf("--cid set. CID will be changed to: %s\n", s_cid);
	    }
	}
	
	if(gopt(options, 'S'))
	{
	    //if any of the sim_unlock options was specified
	    sim_unlock = 1;
	    printf("--sim_unlock. SIMLOCK will be removed\n");
	}
	
	if(gopt(options, 'V'))
	{
	    //if any of the sim_unlock options was specified
	    verify = 1;
	    printf("--verify. CID, secu_flag, SIMLOCK will be verified\n");
	}
	
	if(gopt(options, 'f'))
	{
	    secu_flag = 2;
	    printf("--secu_flag off set\n");
	    cid = 1;
	    s_cid = "11111111";
	    printf("--cid set. CID will be changed to: %s\n", s_cid);
	    sim_unlock = 1;
	    printf("--sim_unlock. SIMLOCK will be removed\n");
	}

	if(gopt(options, 'w'))
	    disable_wp = 1;

	if(gopt(options, 'd'0)
	    debug = 1;
	
	if(gopt(options, 'h'))
	    help = 1;
    } 
    else 
	help = 1;
    
    if(help)
    {
	//if any of the help options was specified
	printf("gfree usage:\n");
	printf("gfree [-h|-?|--help] [-v|--version] [-s|--secu_flag on|off]\n");
	printf("\t-h | -? | --help: display this message\n");
	printf("\t-v | --version: display program version\n");
	printf("\t-s | --secu_flag on|off: turn secu_flag on or off\n");
	printf("\t-c | --cid <CID>: set the CID to the 8-char long CID\n");
	printf("\t-S | --sim_unlock: remove the SIMLOCK\n");
	printf("\t-w | --disable_wp: disable write protect on eMMC and remove kernel filter only\n");
	printf("\n");
	printf("\t-f | --free_all: same as --secu_flag off --sim_unlock --cid 11111111\n");
	exit(0);
    }
    
    if(!cid && !secu_flag && !sim_unlock)
    {
	printf("no valid option specified, see gfree -h\n" );
	exit(0);
    }
    
    ourTime = time(0);
    
    header = (struct elfHeader *)wpthis_ko;

    setvbuf(stdout, 0, _IONBF, 0);
    setvbuf(stderr, 0, _IONBF, 0);

    printf("Section header entry size: %d\n", header->shentsize);
    printf("Number of section headers: %d\n", header->shnum);
    printf("Total section header table size: %d\n", header->shentsize * header->shnum);
    printf("Section header file offset: 0x%.8x (%d)\n", header->shoff, header->shoff);
    printf("Section index for section name string table: %d\n", header->shstrndx);

    // setup string table
    stringTable = (uint8_t *)
	((struct sectionHeader *)((uint32_t)wpthis_ko + header->shoff + (header->shentsize * header->shstrndx)))->offset + (uint32_t)wpthis_ko;

    printf("String table offset: 0x%.8x (%d)\n", (uint32_t)stringTable - (uint32_t)wpthis_ko, (uint32_t)stringTable - (uint32_t)wpthis_ko);

    // scan through section header entries until we find .modinfo
    printf("Searching for .modinfo section...\n");
    for(ent = 0; ent < header->shnum; ent++)
    {
	section = (struct sectionHeader *)((uint32_t)wpthis_ko + header->shoff + (header->shentsize * ent));
	if(!strcmp((char*)&stringTable[section->name], ".modinfo"))
	{
	    printf(" - Section[%d]: %s\n", ent, &stringTable[section->name]);
	    printf(" -- offset: 0x%.8x (%d)\n", section->offset, section->offset);
	    printf(" -- size: 0x%.8x (%d)\n", section->size, section->size);
	    modInfo = (void *)((uint32_t)wpthis_ko + section->offset);
	    modSize = section->size;
	    modInfoSection = section;
	    break;
	}
	else
	{
	    if(debug)
		printf(" - Section[%d]: %s\n", ent, &stringTable[section->name]);
	}
    }

    if(!modInfo)
    {
	fprintf(stderr, "Failed to find .modinfo section in ko\n");
	return 1;
    }

    // pick the aligned strings out of .modinfo
    neededBuffer = 0;
    curString = modInfo;
    while((uint32_t)curString < ((uint32_t)modInfo + modSize))
    {
	if(strlen(curString))
	{
	    if(!addString(curString, &stringRoot))
	    {
		fprintf(stderr, "Failed to add string to linked list (srsly?)\n");
		return 1;
	    }
	    curString += strlen(curString);
	}
	else
	    curString++;
    }

    // get kernel release information
    if(uname(&kernInfo))
    {
        fprintf(stderr, "Failed getting info from uname()\n");
        return 1;
    }

    printf("Kernel release: %s\n", kernInfo.release);

    // 2 pass setup of aligned strings
    neededBuffer = 0;
    current = stringRoot;
    while(current)
    {
	if(strstr(current->string, "vermagic="))
	{
	    free(current->string);
	    current->string = malloc(snprintf(0, 0, "vermagic=%s preempt mod_unload ARMv7 ", kernInfo.release) + 1);
	    if(!current->string)
	    {
		fprintf(stderr, "Failed to allocate memory for vermagic string... lol.\n");
		return 1;
	    }
	    sprintf(current->string, "vermagic=%s preempt mod_unload ARMv7 ", kernInfo.release);
	}
	neededBuffer += strlen(current->string) + 1;
	neededBuffer += (neededBuffer % 4) ? 4 - (neededBuffer % 4) : 0;
	current = current->next;
    }
    printf("New .modinfo section size: %d\n", neededBuffer);

    buffer = malloc(neededBuffer);
    if(!buffer)
    {
	fprintf(stderr, "Failed to allocate buffer for aligned strings\n");
	return 1;
    }

    neededBuffer = 0;
    current = stringRoot;
    tmpBuffer = buffer;
    while(current)
    {
	neededBuffer += strlen(current->string) + 1;
	strcpy(tmpBuffer, current->string);
	tmpBuffer += strlen(current->string) + 1;
	tmpBuffer += (neededBuffer % 4) ? 4 - (neededBuffer % 4) : 0;
	neededBuffer += (neededBuffer % 4) ? 4 - (neededBuffer % 4) : 0;
	current = current->next;
    }
    freeStrings(&stringRoot);

    /*
    if(!(output = fopen("modinfo.bin", "wb")))
    {
	fprintf(stderr, "Failed to open modinfo.bin\n");
	return 1;
    }
    if(fwrite(modInfo, modSize, 1, output) != 1)
    {
	fprintf(stderr, "Failed writing modinfo.bin\n");
	return 1;
    }
    fclose(output);
    if(!(output = fopen("modinfo-new.bin", "wb")))
    {
	fprintf(stderr, "Failed to open modinfo-new.bin\n");
	return 1;
    }
    if(fwrite(buffer, neededBuffer, 1, output) != 1)
    {
	fprintf(stderr, "Failed writing modinfo-new.bin\n");
	return 1;
    }
    fclose(output);
    */

    // copy elf, attach new modinfo section, fix section header.
    tmpBuffer = malloc(sizeof(wpthis_ko) + neededBuffer);
    if(!tmpBuffer)
    {
	fprintf(stderr, "Failed to allocate new ELF image\n");
	return 1;
    }
    memcpy(tmpBuffer, wpthis_ko, sizeof(wpthis_ko));
    memcpy(tmpBuffer + sizeof(wpthis_ko), buffer, neededBuffer);
    modInfoSection = (struct sectionHeader *)(((uint32_t)modInfoSection - (uint32_t)wpthis_ko) + (uint32_t)tmpBuffer);
    modInfoSection->offset = sizeof(wpthis_ko);
    modInfoSection->size = neededBuffer;
    free(buffer);

    /*
    if(!(output = fopen("wpthis-new.ko", "wb")))
    {
	fprintf(stderr, "Failed to open wpthis-new.ko\n");
	return 1;
    }
    if(fwrite(tmpBuffer, sizeof(wpthis_ko) + neededBuffer, 1, output) != 1)
    {
	fprintf(stderr, "Failed writing wpthis-new.ko\n");
	return 1;
    }
    fclose(output);
    */

    // load the module. ENOSYS means ok.
    printf("Attempting to power cycle eMMC... ");
    if(!init_module(tmpBuffer, sizeof(wpthis_ko) + neededBuffer, ""))
    {
	printf("Failed.\n");
	fprintf(stderr, "Module successfully loaded and stayed resident... This is *not* right.\n");
	return 1;
    }

    switch(errno)
    {
	case MOD_RET_OK:
	    printf("OK.\n");
	    printf("Write protect was successfully disabled.\n");
	    break;

	case MOD_RET_FAILINIT:
	    printf("Failed.\n");
	    fprintf(stderr, "Module failed init, check dmesg.\n");
	    return 1;

	case MOD_RET_FAILWP:
	    printf("Failed. (Not fatal)\n");
	    fprintf(stderr, "Module tried to power cycle eMMC, but could not verify write-protect status.\n");
	    break;

	case MOD_RET_FAIL:
	    printf("Failed.\n");
	    fprintf(stderr, "Module failed to power cycle eMMC.\n");
	    break;

	case MOD_RET_NONEED:
	    printf("OK.\n");
	    printf("Module did not power-cycle eMMC, write-protect already off.\n");
	    break;

	default:
	    printf("Failed.\n");
	    fprintf(stderr, "Module returned an unknown code (%d).\n", strerror(errno));
	    return 1;
    }

    free(tmpBuffer);
    
    if(!(kallsyms = fopen("/proc/kallsyms", "rb")))
    {
	fprintf(stderr, "Failed to open /proc/kallsyms\n");
	return 1;
    }
    buffer = malloc(1024);
    if(!buffer)
    {
	fprintf(stderr, "Failed to allocate 1024 bytes. You've got bigger problems than this error.\n");
	return 1;
    }
    printf("Searching for mmc_blk_issue_rq symbol...\n");
    while(fgets(tempString, 256, kallsyms))
    {
	char *address;
	char *type;
	char *name;
	char *module;

	address = strtok(tempString, "\n");
	address = strtok(tempString, " ");
	type = strtok(0, " ");
	name = strtok(0, "\t");
	module = strtok(0, " ");

	if(!strcmp("mmc_blk_issue_rq", name))
	{
	    printf(" - Address: %s, type: %s, name: %s, module: %s\n", address, type, name, module ? module : "N/A");
	    brqAddress = strtoul(address, 0, 16);
	}
    }

    pageSize = getpagesize();
    mapBase = brqAddress - (brqAddress % pageSize);
    printf("Kernel map base: 0x%.8x\n", mapBase);
    if((kernelFD = open("/dev/kmem", O_RDWR)) < 0)
    {
	fprintf(stderr, "Failed to open /dev/kmem: %s\n", strerror(errno));
	return 1;
    }
    kernel = mmap(0, pageSize * 2, PROT_READ | PROT_WRITE, MAP_SHARED, kernelFD, mapBase);
    if(kernel == MAP_FAILED)
    {
	fprintf(stderr, "Failed to mmap kernel memory: %s\n", strerror(errno));
	return 1;
    }
    printf("Kernel memory mapped to 0x%.8x\n", (uint32_t)kernel);

    printf("Searching for brq filter...\n");
    filterAddress = (uint32_t *)fuzzyInstSearch(brqFilter, kernel, brqMasks, sizeof(brqFilter), pageSize * 2);

    if(filterAddress)
    {
	printf(" - Address: 0x%.8x + 0x%x\n", brqAddress, (uint32_t)filterAddress - (uint32_t)kernel + mapBase - brqAddress);

	if(((filterAddress[1] & 0xFF000000) >> 24) != 0x2a)
	{
	    printf(" - ***WARNING***: Found fuzzy match for brq filter, but conditional branch isn't. (0x%.8x)\n", filterAddress[1]);
	}
	else
	{
	    printf(" - 0x%.8x -> 0x%.8x\n", filterAddress[1], filterAddress[1] = 0xea000000 | (filterAddress[1] & 0x00ffffff));
	}
    }
    else
    {
	printf(" - ***WARNING***: Did not find brq filter.\n");
    }

    munmap(kernel, pageSize * 2);

    if(disable_wp)
	return 0;

    // Guhl's part7 patch/backup code
    printf("Patching and backing up partition 7...\n");
    fdin = fopen(INFILE, "rb");
    if (fdin == NULL){
		printf("Error opening input file.\n");
		return -1;
    }

    backupFile = malloc(snprintf(0, 0, "/sdcard/part7backup-%lu.bin", ourTime) + 1);
    if(!backupFile) {
		fprintf(stderr, "Failed to allocate memory for backup file name.. lol\n");
		return 1;
    }
    sprintf(backupFile, "/sdcard/part7backup-%lu.bin", ourTime);
    
    fdout = fopen(backupFile, "wb");
    if (fdout == NULL){
		printf("Error opening copy file.\n");
		return -1;
    }
    
    //  create a copy of the partition
	while(!feof(fdin)) {
		ch = fgetc(fdin);
		if(ferror(fdin)) {
		  printf("Error reading input file.\n");
		  exit(1);
		}
		if(!feof(fdin)) fputc(ch, fdout);
		if(ferror(fdout)) {
		  printf("Error writing copy file.\n");
		  exit(1);
		}
	}
	if(fclose(fdin)==EOF) {
		printf("Error closing input file.\n");
		exit(1);
	}

	if(fclose(fdout)==EOF) {
		printf("Error closing copy file.\n");
		exit(1);
	}

//  copy back and patch
	long j;

	fdin = fopen(backupFile, "rb");
	if (fdin == NULL){
		printf("Error opening copy file.\n");
		return -1;
	}

	fdout = fopen(OUTFILE, "wb");
	if (fdout == NULL){
		printf("Error opening output file.\n");
		return -1;
	}

	j = 0;

	while(!feof(fdin)) {
		ch = fgetc(fdin);
		if(ferror(fdin)) {
		  printf("Error reading copy file.\n");
		  exit(1);
		}
		// secu_flag
		if (j==0xa00 && secu_flag!=0) {
			if (secu_flag==1){
				fprintf( stdout, "patching secu_flag: 1\n");
				ch = 0x01;
			} else {
				fprintf( stdout, "patching secu_flag: 0\n");
				ch = 0x00;
			}
		}
		// CID
		if ((j>=0x200 && j<=0x207)&& (cid!=0)) {
			ch = s_cid[j-0x200];
		}
		// SIM LOCK
		if (sim_unlock!=0){
			if ((j>0x80003 && j<0x807fc) || (j>=0x80800 && j<=0x82fff)){
				ch = 0x00;
			} else if (j==0x80000) {
				ch = 0x78;
			} else if (j==0x80001) {
				ch = 0x56;
			} else if (j==0x80002) {
				ch = 0xF3;
			} else if (j==0x80003) {
				ch = 0xC9;
			} else if (j==0x807fc) {
				ch = 0x49;
			} else if (j==0x807fd) {
				ch = 0x53;
			} else if (j==0x807fe) {
				ch = 0xF4;
			} else if (j==0x807ff) {
				ch = 0x7D;
			}
		}
		if(!feof(fdin)) fputc(ch, fdout);
		if(ferror(fdout)) {
		  printf("Error writing output file.\n");
		  exit(1);
		}
		j++;
	}
	if(fclose(fdin)==EOF) {
		printf("Error closing copy file.\n");
		exit(1);
	}

	if(fclose(fdout)==EOF) {
		printf("Error closing output file.\n");
		exit(1);
	}

    printf("Done.\n");

    return 0;
}

int addString(char *string, struct listEnt **root)
{
    struct listEnt *current;
    struct listEnt *last;

    current = *root;

    if(!current)
    {
	current = malloc(sizeof(struct listEnt));
	if(!current)
	    return 0;

	*root = current;
	last = current;
    }
    else
    {
	current = current->current;

	current->next = malloc(sizeof(struct listEnt));
	if(!current->next)
	    return 0;

	last = current;
	current = current->next;
    }

    current->next = 0;
    current->string = malloc(strlen(string) + 1);
    current->current = 0;
    if(!current->string)
    {
	last->next = 0;
	free(current);
	if(current == *root)
	    *root = 0;
	return 1;
    }
    strcpy(current->string, string);
    (*root)->current = current;
    return 1;
}

void freeStrings(struct listEnt **root)
{
    struct listEnt *current; 
    struct listEnt *last;

    if(!*root)
	return;

    current = *root;

    while(current)
    {
	last = current;
	free(current->string);
	current = current->next;
	free(last);
    }
    *root = 0;
}

void *fuzzyInstSearch(uint32_t *needle, uint32_t *haystack, uint32_t *masks, uint32_t needleLength, uint32_t haystackLength)
{
    uint32_t *currentHaystackPtr;
    uint32_t *currentNeedlePtr;
    uint32_t *currentMaskPtr;

    currentHaystackPtr = haystack;
    currentNeedlePtr = needle;
    currentMaskPtr = masks;

    while(((uint32_t)currentHaystackPtr < ((uint32_t)haystack + haystackLength - needleLength)) &&
	((uint32_t)currentNeedlePtr < ((uint32_t)needle + needleLength)))
    {
	if(debug)
	    printf("h: 0x%.8x, n: 0x%.8x, *h: 0x%.8x, *n: 0x%.8x, ", currentHaystackPtr, currentNeedlePtr, *currentHaystackPtr, *currentNeedlePtr);

	if((*currentHaystackPtr & *currentMaskPtr) != (*currentNeedlePtr & *currentMaskPtr))
	{
	    if(debug)
		printf("Fail\n");

	    currentNeedlePtr = needle;
	    currentMaskPtr = masks;
	    currentHaystackPtr++;
	    continue;
	}

	if(debug)
	    printf("Match\n");

	currentNeedlePtr++;
	currentMaskPtr++;
	currentHaystackPtr++;
    }

    if((uint32_t)currentNeedlePtr == ((uint32_t)needle + needleLength))
    {
	// found.
	return (void *)((uint32_t)currentHaystackPtr - needleLength);
    }
    return 0;
}
