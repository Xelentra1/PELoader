// PELoader.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include "windows.h"
#include <iostream>
#include <stdio.h>
#include <string.h>
#include <fstream>
#include <iostream>
#include <vector>

using namespace std;

#define DEREF_32( name )*(DWORD *)(name)

// Transformas virtual addresses of PE file to offsets
DWORD RVA2RAW(LPVOID fileImage, DWORD dwRVA)
{
	DWORD dwRawRVAAds(0);
	IMAGE_DOS_HEADER* DOSHeader = PIMAGE_DOS_HEADER(fileImage);
	IMAGE_NT_HEADERS* NtHeader = PIMAGE_NT_HEADERS(DWORD(fileImage) + DOSHeader->e_lfanew);
	PIMAGE_SECTION_HEADER pSections = IMAGE_FIRST_SECTION(NtHeader);

	if (!pSections) {
		return dwRawRVAAds;
	}

	while (pSections->VirtualAddress != 0)
	{
		if (dwRVA >= pSections->VirtualAddress &&
			dwRVA < pSections->VirtualAddress + pSections->SizeOfRawData)
		{
			dwRawRVAAds = (dwRVA - pSections->VirtualAddress) + pSections->PointerToRawData;
			break;
		}
		pSections++;
	}

	return dwRawRVAAds;
}

BYTE* ReadPE(LPCSTR filename, LONGLONG &filelen)
{
	FILE *fileptr;
	BYTE *buffer;

	fileptr = fopen(filename, "rb");  // Open the file in binary mode
	fseek(fileptr, 0, SEEK_END);          // Jump to the end of the file
	filelen = ftell(fileptr);             // Get the current byte offset in the file
	rewind(fileptr);                      // Jump back to the beginning of the file

	buffer = (BYTE *)malloc((filelen + 1) * sizeof(char)); // Enough memory for file + \0
	fread(buffer, filelen, 1, fileptr); // Read in the entire file
	fclose(fileptr); // Close the file

	return buffer;
}

bool fixImportTable(PVOID fileImage, PVOID peImage)
{
	printf("[+] Fixing import table...\n");
	
	IMAGE_DOS_HEADER* DOSHeader = PIMAGE_DOS_HEADER(fileImage);
	IMAGE_NT_HEADERS* NtHeader = PIMAGE_NT_HEADERS(DWORD(fileImage) + DOSHeader->e_lfanew);

	IMAGE_DATA_DIRECTORY* importsDir = &(NtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]);

	if (importsDir == NULL) return false;

	DWORD maxSize = importsDir->Size;
	DWORD impAddr = importsDir->VirtualAddress;

	IMAGE_IMPORT_DESCRIPTOR* lib_desc = NULL;
	DWORD parsedSize = 0;

	for (; parsedSize < maxSize; parsedSize += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
		lib_desc = (IMAGE_IMPORT_DESCRIPTOR*)(impAddr + parsedSize + (ULONG_PTR)peImage);

		if (lib_desc->OriginalFirstThunk == NULL && lib_desc->FirstThunk == NULL) break;
		LPSTR lib_name = (LPSTR)((ULONGLONG)peImage + lib_desc->Name);
		printf("    [+] Import DLL: %s\n", lib_name);

		DWORD call_via = lib_desc->FirstThunk;
		DWORD thunk_addr = lib_desc->OriginalFirstThunk;
		if (thunk_addr == NULL) thunk_addr = lib_desc->FirstThunk;

		DWORD offsetField = 0;
		DWORD offsetThunk = 0;
		while (true)
		{
			IMAGE_THUNK_DATA32* fieldThunk = (IMAGE_THUNK_DATA32*)(DWORD(peImage) + offsetField + call_via);
			IMAGE_THUNK_DATA32* orginThunk = (IMAGE_THUNK_DATA32*)(DWORD(peImage) + offsetThunk + thunk_addr);
			PIMAGE_THUNK_DATA  import_Int = (PIMAGE_THUNK_DATA)(lib_desc->OriginalFirstThunk + DWORD(peImage));

			if (import_Int->u1.Ordinal & 0x80000000) {
				//Find Ordinal Id
				DWORD addr = (DWORD)GetProcAddress(LoadLibraryA(lib_name), (char *)(orginThunk->u1.Ordinal & 0xFFFF));
				printf("        [+] Function %x at %x\n", orginThunk->u1.Ordinal, addr);
				fieldThunk->u1.Function = addr;

			}

			if (fieldThunk->u1.Function == NULL) break;

			if (fieldThunk->u1.Function == orginThunk->u1.Function) {

				PIMAGE_IMPORT_BY_NAME by_name = (PIMAGE_IMPORT_BY_NAME)(DWORD(peImage) + orginThunk->u1.AddressOfData);
				if (orginThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG32) return false;

				LPSTR func_name = (LPSTR)by_name->Name;
				DWORD addr = (DWORD)GetProcAddress(LoadLibraryA(lib_name), func_name);
				printf("        [+] Function %s at %x\n", func_name, addr);
				fieldThunk->u1.Function = addr;
			}
			offsetField += sizeof(IMAGE_THUNK_DATA32);
			offsetThunk += sizeof(IMAGE_THUNK_DATA32);
		}
	}
	return true;
}

typedef struct _BASE_RELOCATION_ENTRY {
	WORD Offset : 12;
	WORD Type : 4;
} BASE_RELOCATION_ENTRY;

#define RELOC_32BIT_FIELD 3

bool fixRelocationTable(LPVOID fileImage, LPVOID peImage, ULONGLONG oldBase) {
	IMAGE_DOS_HEADER* DOSHeader = PIMAGE_DOS_HEADER(fileImage);
	IMAGE_NT_HEADERS* NtHeader = PIMAGE_NT_HEADERS(DWORD(fileImage) + DOSHeader->e_lfanew);
	IMAGE_DATA_DIRECTORY* relocationDirectory = &(NtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC]);
	
	if (relocationDirectory == NULL || (relocationDirectory->Size == 0 && relocationDirectory->VirtualAddress == 0) ) {
		printf("[-] Application has no relocation table\n");
		return false;
	}

	DWORD parsedSize = 0;
	DWORD imageRelocationVA = relocationDirectory->VirtualAddress;
	IMAGE_BASE_RELOCATION* reloc = (IMAGE_BASE_RELOCATION*)(imageRelocationVA + parsedSize + DWORD(peImage));

	while (reloc->VirtualAddress != NULL)
	{
		if (reloc->SizeOfBlock >= sizeof(IMAGE_BASE_RELOCATION))
		{

			reloc = (IMAGE_BASE_RELOCATION*)(imageRelocationVA + parsedSize + DWORD(peImage));

			DWORD entriesNum = (reloc->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(BASE_RELOCATION_ENTRY);
			DWORD page = reloc->VirtualAddress;
			BASE_RELOCATION_ENTRY* entry = (BASE_RELOCATION_ENTRY*)(DWORD(reloc) + sizeof(IMAGE_BASE_RELOCATION));

			for (int i = 0; i < entriesNum; i++)
			{
				DWORD offset = entry->Offset;
				DWORD type = entry->Type;
				DWORD relocationField = page + offset;

				if (entry == NULL || type == 0)
					break;
				if (type != RELOC_32BIT_FIELD) {
					printf("    [!] Not supported relocations format at %d: %d\n", (int)i, (int)type);
					return false;
				}
				if (relocationField >= NtHeader->OptionalHeader.SizeOfImage) {
					printf("    [-] Field is out of boundaries: %lx\n", relocationField);
					return false;
				}

				DWORD* relocateAddr = (DWORD*)(DWORD(peImage) + relocationField);
				printf("        [+] Applying relocation at %x\n", relocateAddr);
				(*relocateAddr) = ((*relocateAddr) - oldBase + (DWORD)peImage);
				entry = (BASE_RELOCATION_ENTRY*)(DWORD(entry) + sizeof(BASE_RELOCATION_ENTRY));
			}
		}
		parsedSize += reloc->SizeOfBlock;
	}
	return (parsedSize != 0);
}

int main(int argc, char **argv)
{
	if (argc != 2)
	{
	    printf("[!] Please specify a PE file you want to load");
	    getchar();
	    return 0;
	}

	// const char* fileName = "C:\\Temp\\msg_box.exe";
	const char* fileName = argv[1];

	LONGLONG fileSize = -1;
	LPVOID *fileImage = (LPVOID*)ReadPE(fileName, fileSize);
	
	printf("[*] selfModule: 0x%08x\n", GetModuleHandle(NULL));

	IMAGE_DOS_HEADER* DOSHeader = PIMAGE_DOS_HEADER(fileImage);
	IMAGE_NT_HEADERS* NtHeader = PIMAGE_NT_HEADERS(DWORD(fileImage) + DOSHeader->e_lfanew);

	LPVOID preferAddr = (LPVOID)NtHeader->OptionalHeader.ImageBase;
	printf("[*] preferAddr: 0x%08x\n", preferAddr);
	LPVOID peImage = NULL;

	peImage = (BYTE *)VirtualAlloc(preferAddr, NtHeader->OptionalHeader.SizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!peImage) {
		peImage = (BYTE *)VirtualAlloc(NULL, NtHeader->OptionalHeader.SizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	}
	printf("[*] peImageAddr: 0x%08x\n", peImage);

	PIMAGE_SECTION_HEADER pSecHeader;

	// copying image headers
	MoveMemory(peImage, fileImage, NtHeader->OptionalHeader.SizeOfHeaders);

	int i;
	for (i = 0, pSecHeader = IMAGE_FIRST_SECTION(NtHeader); i < NtHeader->FileHeader.NumberOfSections; i++, pSecHeader++)
	{
		printf("[+] Mapping Section %s\n", pSecHeader->Name);
		MoveMemory((PBYTE)((DWORD)peImage + pSecHeader->VirtualAddress), (PBYTE)((DWORD)fileImage + pSecHeader->PointerToRawData), pSecHeader->SizeOfRawData);
	}

	if (NtHeader->OptionalHeader.DataDirectory[1].Size == 0)
	{
		printf("[-] No Import Table!\n");
	}
	else
	{
		fixImportTable(fileImage, peImage);
	}

	if (peImage != preferAddr)
	{
		printf("[*] Trying to fix relocations...\n");
		if (fixRelocationTable(fileImage, peImage, (DWORD)preferAddr))
		{
			printf("[+] Relocations are fixed\n");
		}
		else 
		{
			printf("[-] Failed to fix relocations\n");
		}
	}

	printf("Runing PE...\n");
	
	// Not needed? 
	/*DWORD previousProtection = 0;
	if (!VirtualProtect(peImage, NtHeader->OptionalHeader.SizeOfImage, PAGE_EXECUTE_READWRITE, &previousProtection))
	{
		printf("[-] Failed to change permissions of peImage memory region");
	}*/

	DWORD peEntryPoint = (DWORD)peImage + NtHeader->OptionalHeader.AddressOfEntryPoint;
	//((void(*)())peEntryPoint)();

	__asm
	{
	mov eax, [peEntryPoint]
	jmp eax
	}

	system("pause");
	return 0;
}