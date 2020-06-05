/***************************************************************************
 * This file is part of NUSspli.                                           *
 * Copyright (c) 2019-2020 Pokes303                                        *
 * Copyright (c) 2020 V10lator <v10lator@myway.de>                         *
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
#include <input.h>
#include <renderer.h>
#include <status.h>
#include <usb.h>
#include <menu/filebrowser.h>

#include <coreinit/memdefaultheap.h>

#include <dirent.h>
#include <stdbool.h>
#include <string.h>

void drawFBMenuFrame(char **folders, const int foldersSize, const int pos, const int cursor, const bool onUSB)
{
	startNewFrame();
	textToFrame(6, 0, "Select a folder:");
	
	boxToFrame(1, MAX_LINES - 3);
	
	char toWrite[257];
	strcpy(toWrite, "Press \uE000 to select || \uE001 to return || \uE002 to switch to ");
	strcat(toWrite, onUSB ? "SD" : "USB");
	strcat(toWrite, " || \uE003 to refresh");
	textToFrame(ALIGNED_CENTER, MAX_LINES - 2, toWrite);
	
	strcpy(toWrite, "Searching on => ");
	strcat(toWrite, onUSB ? "USB" : "SD");
	strcat(toWrite, ":/install/");
	textToFrame(ALIGNED_CENTER, MAX_LINES - 1, toWrite);
	
	for(int i = 0; i < 13 && i < foldersSize; i++)
	{
		textToFrame(6, i + 2, folders[i + pos]);
		if(cursor == i)
			textToFrame(1, i + 2, "-->");
	}
	drawFrame();
}

char *fileBrowserMenu()
{
	int foldersSize = 1;
	char *folders[1024];
	folders[0] = MEMAllocFromDefaultHeap(3);
	if(folders[0] == NULL)
		return NULL;
	
	strcpy(folders[0], "./");
	bool usbMounted;
	bool onUSB = false;
	DIR *dir;
	
	int cursor;
	int pos;
	int max = MAX_LINES - 5;
	bool mov;
	
refreshDirList:
	usbMounted = mountUSB();
	if(!usbMounted)
		onUSB = false;
	
	for(int i = 1; i < foldersSize; i++)
		MEMFreeToDefaultHeap(folders[i]);
	foldersSize = 1;
	cursor = pos = 0;
	mov = foldersSize >= max;
	
	dir = opendir(onUSB ? INSTALL_DIR_USB : INSTALL_DIR_SD);
	if(dir != NULL)
	{
		size_t len;
		for(struct dirent *entry = readdir(dir); entry != NULL && foldersSize < 1024; entry = readdir(dir))
			if(entry->d_type & DT_DIR) //Check if it's a directory
			{
				len = strlen(entry->d_name);
				folders[foldersSize] = MEMAllocFromDefaultHeap(len + 2);
				
				strcpy(folders[foldersSize], entry->d_name);
				folders[foldersSize][len++] = '/';
				folders[foldersSize++][len] = '\0';
			}
		closedir(dir);
	}
	
	drawFBMenuFrame(folders, foldersSize, pos, cursor, onUSB);
	
	char *ret = NULL;
	while(AppRunning())
	{
		if(app == 2)
			continue;
		else if(app == 9)
			drawFBMenuFrame(folders, foldersSize, pos, cursor, onUSB);
		
		showFrame();
		
		switch(vpad.trigger)
		{
			case VPAD_BUTTON_A:
				if(dir != NULL)
				{
					size_t len = strlen(onUSB ? INSTALL_DIR_USB : INSTALL_DIR_SD) + strlen(folders[cursor + pos]) + 1;
					ret = MEMAllocFromDefaultHeap(len);
					if(ret != NULL)
					{
						strcpy(ret, onUSB ? INSTALL_DIR_USB : INSTALL_DIR_SD);
						strcat(ret, folders[cursor + pos]);
					ret[len] = '\0';
					}
					goto exitFileBrowserMenu;
				}
				break;
			case VPAD_BUTTON_B:
				goto exitFileBrowserMenu;
			case VPAD_BUTTON_UP:
				if(cursor <= 0)
				{
					if(mov)
					{
						if(pos > 0)
							pos--;
						else
						{
							cursor = max;
							pos = foldersSize % max - 1;
						}
					}
					else
						cursor = foldersSize - 1;
				}
				else
					cursor--;
				
				drawFBMenuFrame(folders, foldersSize, pos, cursor, onUSB);
				break;
			case VPAD_BUTTON_DOWN:
				if(cursor >= max || cursor >= foldersSize - 1)
				{
					if (mov && pos < foldersSize % max - 1)
						pos++;
					else
					{
						cursor = 0;
						pos = 0;
					}
				}
				else
					cursor++;
				
				drawFBMenuFrame(folders, foldersSize, pos, cursor, onUSB);
				break;
			case VPAD_BUTTON_X:
				onUSB = !onUSB;
			case VPAD_BUTTON_Y:
				goto refreshDirList;
		}
	}
	
exitFileBrowserMenu:
	for(int i = 0; i < foldersSize; i++)
		MEMFreeToDefaultHeap(folders[i]);
	return ret;
}
