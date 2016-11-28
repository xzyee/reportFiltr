/*++
Copyright (c) 1997  Microsoft Corporation

Module Name:

    reportfiltr.h

Abstract:

    This module contains the common private declarations for the keyboard
    packet filter

Environment:

    kernel mode only

--*/

#ifndef REPORTFILTR_H
#define REPORTFILTR_H

#pragma warning(disable:4201)

#include "ntddk.h"
#include "kbdmou.h"
#include <ntddkbd.h>
#include <ntdd8042.h>

#pragma warning(default:4201)

#include <wdf.h>
#include <hidport.h>  // located in $(DDK_INC_PATH)/wdm

#define NTSTRSAFE_LIB
#include <ntstrsafe.h>

#include <initguid.h>
#include <devguid.h>

#include "public.h"

#define KBFILTER_POOL_TAG (ULONG) 'tlfK'

#if DBG

#define TRAP()                      DbgBreakPoint()

#define DebugPrint(_x_) DbgPrint _x_

#else   // DBG

#define TRAP()

#define DebugPrint(_x_)

#endif

#define MIN(_A_,_B_) (((_A_) < (_B_)) ? (_A_) : (_B_))

typedef struct _DEVICE_EXTENSION
{
    WDFDEVICE WdfDevice;

    //
    // Queue for handling requests that come from the rawPdo
    //
    WDFQUEUE rawPdoQueue;

  
    //
    // Cached Keyboard Attributes
    //
	HID_DEVICE_ATTRIBUTES HidAttributes;

} DEVICE_EXTENSION, *PDEVICE_EXTENSION;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_EXTENSION,
                                        FilterGetData)


typedef struct _WORKER_ITEM_CONTEXT {

    WDFREQUEST  Request;
    WDFIOTARGET IoTarget;

} WORKER_ITEM_CONTEXT, *PWORKER_ITEM_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(WORKER_ITEM_CONTEXT, GetWorkItemContext)

//
// Prototypes
//
DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD HidFilter_EvtDeviceAdd;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL HidFilter_EvtIoDeviceControlForRawPdo;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL HidFilter_EvtIoDeviceControlFromRawPdo;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL HidFilter_EvtIoInternalDeviceControl;
EVT_WDF_IO_QUEUE_IO_WRITE HidFilter_EvtIoWrite;
//EVT_WDF_IO_QUEUE_IO_DEFAULT HidFilter_EvtIoDefault;

EVT_WDF_REQUEST_COMPLETION_ROUTINE HidFilterRequestCompletionRoutine;
EVT_WDF_REQUEST_COMPLETION_ROUTINE CompletionRoutine2;


//
// IOCTL Related defintions
//

//
// Used to identify kbfilter bus. This guid is used as the enumeration string
// for the device id.

// {D41E9FEF-D989-4DBE-A9F2-FB5E87EB6EE7}
DEFINE_GUID(GUID_BUS_REPORTKBFILTER,
	0xd41e9fef, 0xd989, 0x4dbe, 0xa9, 0xf2, 0xfb, 0x5e, 0x87, 0xeb, 0x6e, 0xe7);


// {2795BF34-1CD4-4A26-8017-F72568F7869C}
DEFINE_GUID(GUID_DEVINTERFACE_REPORTKBFILTER,
	0x2795bf34, 0x1cd4, 0x4a26, 0x80, 0x17, 0xf7, 0x25, 0x68, 0xf7, 0x86, 0x9c);


#define  REPORTHIDFILTR_DEVICE_ID L"{D41E9FEF-D989-4DBE-A9F2-FB5E87EB6EE7}\\HidFilter\0"


typedef struct _RPDO_DEVICE_DATA
{

    ULONG InstanceNo;

    //
    // Queue of the parent device we will forward requests to
    //
    WDFQUEUE ParentQueue;

} RPDO_DEVICE_DATA, *PRPDO_DEVICE_DATA;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(RPDO_DEVICE_DATA, PdoGetData)


NTSTATUS
KbFiltr_CreateRawPdo(
    WDFDEVICE       Device,
    ULONG           InstanceNo
);



#endif  // REPORTFILTR_H

