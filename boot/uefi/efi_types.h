/* boot/uefi/efi_types.h -- hand-written minimal EFI type definitions.
 *
 * Only the subset of the UEFI 2.10 spec we actually call is declared
 * here.  Anything else is deliberately omitted to keep this file
 * auditable at a glance (~250 lines vs. 20 kLOC in gnu-efi).
 *
 * All UEFI functions are called via the Microsoft x64 ABI (RCX, RDX,
 * R8, R9 for the first four args, plus 32-byte shadow space above
 * the return address).  Clang's `--target=x86_64-unknown-windows`
 * emits the right calling convention automatically when we invoke
 * function pointers stored in EFI tables.  We do NOT need the
 * historic __attribute__((ms_abi)) annotation because the whole TU
 * is compiled with the Windows target.
 *
 * Function-pointer table layouts (EFI_BOOT_SERVICES, EFI_SYSTEM_TABLE,
 * EFI_SIMPLE_FILE_SYSTEM_PROTOCOL, EFI_FILE_PROTOCOL) are documented
 * in the UEFI 2.10 spec chapters 4, 7, 12, 13.  Offsets are
 * memory-critical -- do not reorder members.
 */

#ifndef AURALITE_BOOT_UEFI_EFI_TYPES_H
#define AURALITE_BOOT_UEFI_EFI_TYPES_H

#include <stdint.h>
#include <stddef.h>

/* Fundamental EFI types (UEFI spec s.2.3). */
typedef void       *EFI_HANDLE;
typedef void       *EFI_EVENT;
typedef uint64_t    EFI_STATUS;
typedef uint16_t    CHAR16;
typedef uint8_t     BOOLEAN;
typedef uint64_t    UINTN;               /* native-size unsigned on x64 */

#define EFI_SUCCESS               0ULL
#define EFI_LOAD_ERROR            0x8000000000000001ULL
#define EFI_INVALID_PARAMETER     0x8000000000000002ULL
#define EFI_UNSUPPORTED           0x8000000000000003ULL
#define EFI_BUFFER_TOO_SMALL      0x8000000000000005ULL
#define EFI_NOT_FOUND             0x800000000000000EULL

/* Memory types (UEFI spec s.7.2, table 30). */
#define EfiReservedMemoryType          0
#define EfiLoaderCode                  1
#define EfiLoaderData                  2
#define EfiBootServicesCode            3
#define EfiBootServicesData            4
#define EfiRuntimeServicesCode         5
#define EfiRuntimeServicesData         6
#define EfiConventionalMemory          7
#define EfiUnusableMemory              8
#define EfiACPIReclaimMemory           9
#define EfiACPIMemoryNVS              10
#define EfiMemoryMappedIO             11
#define EfiMemoryMappedIOPortSpace    12
#define EfiPalCode                    13
#define EfiPersistentMemory           14

typedef struct {
    uint32_t Type;
    uint32_t Pad;
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* File-open flags (UEFI spec s.13.5.2). */
#define EFI_FILE_MODE_READ       0x0000000000000001ULL

/* AllocatePool memory type used everywhere below. */
#define EfiLoaderData_Pool       EfiLoaderData

/* EFI_TABLE_HEADER (UEFI spec s.4.2). */
typedef struct {
    uint64_t Signature;
    uint32_t Revision;
    uint32_t HeaderSize;
    uint32_t CRC32;
    uint32_t Reserved;
} EFI_TABLE_HEADER;

/* Simple Text Output (UEFI spec s.12.4). Only OutputString is used. */
typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void       *Reset;
    EFI_STATUS (*OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                               CHAR16 *String);
    void       *TestString;
    void       *QueryMode;
    void       *SetMode;
    void       *SetAttribute;
    void       *ClearScreen;
    void       *SetCursorPosition;
    void       *EnableCursor;
    void       *Mode;
};

/* Graphics Output Protocol (UEFI spec s.12.9). */
typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    uint32_t                     RedMask;
    uint32_t                     GreenMask;
    uint32_t                     BlueMask;
    uint32_t                     ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    uint32_t                     Version;
    uint32_t                     HorizontalResolution;
    uint32_t                     VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT    PixelFormat;
    EFI_PIXEL_BITMASK            PixelInformation;
    uint32_t                     PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    uint32_t                              MaxMode;
    uint32_t                              Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN                                 SizeOfInfo;
    uint64_t                              FrameBufferBase;
    UINTN                                 FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL EFI_GRAPHICS_OUTPUT_PROTOCOL;
struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    void                              *QueryMode;
    void                              *SetMode;
    void                              *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
};

/* Simple File System Protocol (UEFI spec s.13.4). */
typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    uint64_t   Revision;
    EFI_STATUS (*OpenVolume)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
                             EFI_FILE_PROTOCOL              **Root);
};

struct EFI_FILE_PROTOCOL {
    uint64_t   Revision;
    EFI_STATUS (*Open)(EFI_FILE_PROTOCOL  *This,
                       EFI_FILE_PROTOCOL **NewHandle,
                       CHAR16             *FileName,
                       uint64_t            OpenMode,
                       uint64_t            Attributes);
    EFI_STATUS (*Close)(EFI_FILE_PROTOCOL *This);
    void      *Delete;
    EFI_STATUS (*Read)(EFI_FILE_PROTOCOL *This,
                       UINTN             *BufferSize,
                       void              *Buffer);
    void      *Write;
    EFI_STATUS (*GetPosition)(EFI_FILE_PROTOCOL *This,
                              uint64_t          *Position);
    EFI_STATUS (*SetPosition)(EFI_FILE_PROTOCOL *This,
                              uint64_t           Position);
    EFI_STATUS (*GetInfo)(EFI_FILE_PROTOCOL *This,
                          void              *InformationType,
                          UINTN             *BufferSize,
                          void              *Buffer);
    void      *SetInfo;
    void      *Flush;
};

/* Loaded Image Protocol (UEFI spec s.9.1). */
typedef struct {
    uint32_t                                   Revision;
    EFI_HANDLE                                 ParentHandle;
    void                                      *SystemTable;
    EFI_HANDLE                                 DeviceHandle;
    void                                      *FilePath;
    void                                      *Reserved;
    uint32_t                                   LoadOptionsSize;
    void                                      *LoadOptions;
    void                                      *ImageBase;
    uint64_t                                   ImageSize;
    uint32_t                                   ImageCodeType;
    uint32_t                                   ImageDataType;
    void                                      *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* EFI_FILE_INFO (UEFI spec s.13.5.16).  We only read the Size field. */
typedef struct {
    uint64_t   Size;
    uint64_t   FileSize;
    uint64_t   PhysicalSize;
    uint8_t    CreateTime[16];
    uint8_t    LastAccessTime[16];
    uint8_t    ModificationTime[16];
    uint64_t   Attribute;
    CHAR16     FileName[1];               /* variable length */
} EFI_FILE_INFO;

/* Boot Services (UEFI spec s.7).  Function-pointer offsets MUST match
 * the spec's Table 6 exactly, or we call the wrong routine.  We list
 * every entry (as void* if unused) so offset arithmetic stays right. */
typedef struct EFI_BOOT_SERVICES EFI_BOOT_SERVICES;
struct EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER Hdr;

    /* Task priority services */
    void *RaiseTPL;
    void *RestoreTPL;

    /* Memory services */
    EFI_STATUS (*AllocatePages)(uint32_t Type, uint32_t MemoryType,
                                UINTN Pages, uint64_t *Memory);
    EFI_STATUS (*FreePages)(uint64_t Memory, UINTN Pages);
    EFI_STATUS (*GetMemoryMap)(UINTN *MemoryMapSize,
                               EFI_MEMORY_DESCRIPTOR *MemoryMap,
                               UINTN *MapKey,
                               UINTN *DescriptorSize,
                               uint32_t *DescriptorVersion);
    EFI_STATUS (*AllocatePool)(uint32_t PoolType, UINTN Size,
                               void **Buffer);
    EFI_STATUS (*FreePool)(void *Buffer);

    /* Event & timer services */
    void *CreateEvent;
    void *SetTimer;
    void *WaitForEvent;
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;

    /* Protocol handler services */
    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_STATUS (*HandleProtocol)(EFI_HANDLE Handle, void *Protocol,
                                 void **Interface);
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;

    /* Image services */
    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    EFI_STATUS (*ExitBootServices)(EFI_HANDLE ImageHandle, UINTN MapKey);

    /* Misc services */
    void *GetNextMonotonicCount;
    void *Stall;
    void *SetWatchdogTimer;

    /* DriverSupport services */
    void *ConnectController;
    void *DisconnectController;

    /* Open and close protocol services */
    EFI_STATUS (*OpenProtocol)(EFI_HANDLE Handle, void *Protocol,
                               void **Interface, EFI_HANDLE AgentHandle,
                               EFI_HANDLE ControllerHandle, uint32_t Attributes);
    void *CloseProtocol;
    void *OpenProtocolInformation;

    /* Library services */
    void *ProtocolsPerHandle;
    void *LocateHandleBuffer;
    EFI_STATUS (*LocateProtocol)(void *Protocol, void *Registration,
                                 void **Interface);
    void *InstallMultipleProtocolInterfaces;
    void *UninstallMultipleProtocolInterfaces;

    /* 32-bit CRC services */
    void *CalculateCrc32;

    /* Misc services */
    void *CopyMem;
    void *SetMem;
    void *CreateEventEx;
};

/* System Table (UEFI spec s.4.3). */
typedef struct {
    EFI_TABLE_HEADER                 Hdr;
    CHAR16                          *FirmwareVendor;
    uint32_t                         FirmwareRevision;
    EFI_HANDLE                       ConsoleInHandle;
    void                            *ConIn;
    EFI_HANDLE                       ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE                       StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    void                            *RuntimeServices;
    EFI_BOOT_SERVICES               *BootServices;
    UINTN                            NumberOfTableEntries;
    void                            *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* EFI_CONFIGURATION_TABLE (UEFI spec s.4.6): one entry per firmware-vendor
 * table (ACPI RSDP, SMBIOS, ...), reached via
 * EFI_SYSTEM_TABLE.ConfigurationTable[0..NumberOfTableEntries-1].  A GUID
 * is a plain 16-byte value here (not the ms_abi-callable pointer form used
 * for protocol lookups), so it is compared byte-for-byte. */
typedef struct {
    uint8_t VendorGuid[16];
    void   *VendorTable;
} EFI_CONFIGURATION_TABLE;

/* OpenProtocol attributes (UEFI spec s.7.3.9). */
#define EFI_OPEN_PROTOCOL_GET_PROTOCOL   0x00000002

/* AllocatePages Type argument. */
#define AllocateAnyPages         0
#define AllocateMaxAddress       1
#define AllocateAddress          2

#endif /* AURALITE_BOOT_UEFI_EFI_TYPES_H */
