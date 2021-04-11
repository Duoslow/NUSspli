/***************************************************************************
 * This file is part of NUSspli.                                           *
 * Copyright (c) 2019-2020 Pokes303                                        *
 * Copyright (c) 2020-2021 V10lator <v10lator@myway.de>                    *
 *                                                                         *
 * This program is free software; you can redistribute it and/or modify    *
 * it under the terms of the GNU General Public License as published by    *
 * the Free Software Foundation; either version 2 of the License, or       *
 * (at your option) any later version.                                     *
 *                                                                         *
 * This program is distributed in the hope that it will be useful,         *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 * GNU General Public License for more details.                            *
 *                                                                         *
 * You should have received a copy of the GNU General Public License along *
 * with this program; if not, write to the Free Software Foundation, Inc., *
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.             *
 ***************************************************************************/

#include <wut-fixups.h>

#include <file.h>
#include <ioThread.h>
#include <utils.h>

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <coreinit/memory.h>
#include <coreinit/time.h>

uint8_t readUInt8(const char *file, uint32_t pos)
{
	FILE* fp = fopen(file, "rb");
	fseek(fp, pos, SEEK_SET);
	uint8_t result;
	fread(&result, 1, 1, fp);
	fclose(fp);
	return result;
}

uint16_t readUInt16(const char *file, uint32_t pos)
{
	FILE* fp = fopen(file, "rb");
	fseek(fp, pos, SEEK_SET);
	uint16_t result;
	fread(&result, 2, 1, fp);
	fclose(fp);
	return result;
}

uint32_t readUInt32(const char *file, uint32_t pos)
{
	FILE* fp = fopen(file, "rb");
	fseek(fp, pos, SEEK_SET);
	uint32_t result;
	fread(&result, 4, 1, fp);
	fclose(fp);
	return result;
}

uint64_t readUInt64(const char *file, uint32_t pos)
{
	FILE* fp = fopen(file, "rb");
	fseek(fp, pos, SEEK_SET);
	uint64_t result;
	fread(&result, 8, 1, fp);
	fclose(fp);
	return result;
}

void writeVoidBytes(FILE* fp, uint32_t len)
{
	uint8_t bytes[len];
	OSBlockSet(bytes, 0, len);
	addToIOQueue(bytes, len, 1, fp);
}

void writeCustomBytes(FILE *fp, const char *str)
{
	if(str[0] == '0' && str[1] == 'x')
		str += 2;
	
	size_t size = strlen(str) >> 1;
	uint8_t bytes[size];
	hexToByte(str, bytes);
	addToIOQueue(bytes, size, 1, fp);
}

void writeRandomBytes(FILE* fp, uint32_t len)
{
	uint32_t len32 = len < 9 ? 1 : len >> 2;
	uint32_t bytes[len32];
	for(int i = 0; i < len32; i++)
		bytes[i] = rand();
	addToIOQueue(bytes, len, 1, fp);
}

/*
 * The header is meaned to be human (hex editor) and machine readable. The format is:
 *  - Magic 32 bit value (defined by Nintendo)
 *  - 0x00010203040506070809 As a magic value / metadata detection
 *  - 2 bit spacing
 *  - 64 bit "NUSspli\0" string
 *  - 64 bit padding
 *  - 64 bit version string ending with 0x00
 *  - 64 bit padding
 *  - Max 64 bit file type string ending with 0x00
 
 *  - Padding till 0x000000EE / area reserved for future use
 *  - uint8_t storing the meta version number (currently 1, so 00000001 or 0x01)
 *  - 128 + 32 random bits marking the end of the header usable area
 *  - 256 + 128 + 64 + 32 bit padding (defined by Nintendo / end of header)
 */
void writeHeader(FILE *fp, FileType type)
{
	writeCustomBytes(fp, "0x00010004000102030405060708090000"); // Magic 32 bit value + our magic value + padding
	writeCustomBytes(fp, "0x4E555373706C69"); // "NUSspli"
	writeVoidBytes(fp, 0x9);
	int vl = strlen(NUSSPLI_VERSION);
	addToIOQueue(NUSSPLI_VERSION, vl, 1, fp);
	
	writeVoidBytes(fp, 0x10 - vl);
	char *cb;
	int v;
	switch(type)
	{
		case FILE_TYPE_TIK:
			cb = "0x5469636B6574"; // Ticket
			v = 0xB9;
			break;
		case FILE_TYPE_CERT:
			cb = "0x4365727469666963617465"; // "Certificate"
			v = 0xB4;
			break;
		default:
			cb = "00";
			v = 0xBE;
	}
	
	writeCustomBytes(fp, cb);
	writeVoidBytes(fp, v);
	writeCustomBytes(fp, "0x01"); // TODO: Don't hardcode in here
	writeRandomBytes(fp, 0x14);
	writeVoidBytes(fp, 0x3C);
}

bool fileExists(const char *path)
{
	struct stat fs;
	return stat(path, &fs) == 0;
}

bool dirExists(const char *path)
{
	struct stat ds;
	return stat(path, &ds) == 0 && S_ISDIR(ds.st_mode);
}

void removeDirectory(const char *path)
{
	size_t len = strlen(path);
	char newPath[len + 1024]; // TODO
	strcpy(newPath, path);
	
	if(newPath[len - 1] != '/')
		newPath[len++] = '/';
	
	char *inSentence = newPath + len;
	DIR *dir = opendir(path);
	if(dir != NULL)
	{
		for(struct dirent *entry = readdir(dir); entry != NULL; entry = readdir(dir))
		{
			strcpy(inSentence, entry->d_name);
			if(entry->d_type & DT_DIR)
				removeDirectory(newPath);
			else
				remove(newPath);
		}
		closedir(dir);
		remove(path);
	}
}

void moveDirectory(const char *src, const char *dest)
{
	size_t len = strlen(src);
	char newSrc[len + 1024]; // TODO
	strcpy(newSrc, src);
	
	if(newSrc[len - 1] != '/')
		newSrc[len++] = '/';
	
	char *inSrc = newSrc + len;
	DIR *dir = opendir(src);
	if(dir != NULL)
	{
		mkdir(dest, 0777);
		len = strlen(dest);
		char newDest[len + 1024]; // TODO
		strcpy(newDest, dest);
		
		if(newDest[len - 1] != '/')
			newDest[len++] = '/';
		
		char *inDest = newDest + len;
		
		for(struct dirent *entry = readdir(dir); entry != NULL; entry = readdir(dir))
		{
			strcpy(inSrc, entry->d_name);
			strcpy(inDest, entry->d_name);
			if(entry->d_type & DT_DIR)
				moveDirectory(newSrc, newDest);
			else
				rename(newSrc, newDest);
		}
		closedir(dir);
		remove(src);
	}
}

long getFilesize(FILE *fp)
{
	off_t i = ftello(fp);
	if(fseek(fp, 0, SEEK_END) != 0)
		return -1;
	
	long fileSize = ftello(fp);
	if(fileSize == -1)
		debugPrintf("ftello() failed: %s", strerror(errno));
	
	fseeko(fp, i, SEEK_SET);
	return fileSize;
}
