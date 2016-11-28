/*--

Copyright (c) Microsoft Corporation.  All rights reserved.

    THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY
    KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A PARTICULAR
    PURPOSE.


Module Name:

    reportfiltr.c

Abstract: This is an upper device filter driver sample for PS/2 keyboard. This
        driver layers in between the KbdClass driver and i8042prt driver and
        hooks the callback routine that moves keyboard inputs from the port
        driver to class driver. With this filter, you can remove or insert
        additional keys into the stream. This sample also creates a raw
        PDO and registers an interface so that application can talk to
        the filter driver directly without going thru the PS/2 devicestack.
        The reason for providing this additional interface is because the keyboard
        device is an exclusive secure device and it's not possible to open the
        device from usermode and send custom ioctls.

        If you want to filter keyboard inputs from all the keyboards (ps2, usb)
        plugged into the system then you can install this driver as a class filter
        and make it sit below the kbdclass filter driver by adding the service
        name of this filter driver before the kbdclass filter in the registry at
        " HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\
        {4D36E96B-E325-11CE-BFC1-08002BE10318}\UpperFilters"


Environment:

    Kernel mode only.

--*/

#include "reportfiltr.h"
#include <usb.h>

#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (PAGE, HidFilter_EvtDeviceAdd)
#pragma alloc_text (PAGE, HidFilter_EvtIoInternalDeviceControl)
#endif

ULONG InstanceNo = 0;

NTSTATUS
DriverEntry(
    IN PDRIVER_OBJECT  DriverObject,
    IN PUNICODE_STRING RegistryPath
    )
/*++

Routine Description:

    Installable driver initialization entry point.
    This entry point is called directly by the I/O system.

Arguments:

    DriverObject - pointer to the driver object

    RegistryPath - pointer to a unicode string representing the path,
                   to driver-specific key in the registry.

Return Value:

    STATUS_SUCCESS if successful,
    STATUS_UNSUCCESSFUL otherwise.

--*/
{
    WDF_DRIVER_CONFIG               config;
    NTSTATUS                        status;

    DebugPrint(("HidClass Filter Driver Sample - Driver Framework Edition.\n"));
    DebugPrint(("Built %s %s\n", __DATE__, __TIME__));

    //
    // Initiialize driver config to control the attributes that
    // are global to the driver. Note that framework by default
    // provides a driver unload routine. If you create any resources
    // in the DriverEntry and want to be cleaned in driver unload,
    // you can override that by manually setting the EvtDriverUnload in the
    // config structure. In general xxx_CONFIG_INIT macros are provided to
    // initialize most commonly used members.
    //

    WDF_DRIVER_CONFIG_INIT(
        &config,
        HidFilter_EvtDeviceAdd
    );

    //
    // Create a framework driver object to represent our driver.
    //
    status = WdfDriverCreate(DriverObject,
                            RegistryPath,
                            WDF_NO_OBJECT_ATTRIBUTES,
                            &config,
                            WDF_NO_HANDLE); // hDriver optional
    if (!NT_SUCCESS(status)) {
        DebugPrint(("WdfDriverCreate failed with status 0x%x\n", status));
    }

    return status;
}

NTSTATUS
HidFilter_EvtDeviceAdd(
    IN WDFDRIVER        Driver,
    IN PWDFDEVICE_INIT  DeviceInit
    )
/*++
Routine Description:

    EvtDeviceAdd is called by the framework in response to AddDevice
    call from the PnP manager. Here you can query the device properties
    using WdfFdoInitWdmGetPhysicalDevice/IoGetDeviceProperty and based
    on that, decide to create a filter device object and attach to the
    function stack.

    If you are not interested in filtering this particular instance of the
    device, you can just return STATUS_SUCCESS without creating a framework
    device.

Arguments:

    Driver - Handle to a framework driver object created in DriverEntry

    DeviceInit - Pointer to a framework-allocated WDFDEVICE_INIT structure.

Return Value:

    NTSTATUS

--*/
{
    WDF_OBJECT_ATTRIBUTES   deviceAttributes;
    NTSTATUS                status;
    WDFDEVICE               hDevice;
    WDFQUEUE                hQueue;
    PDEVICE_EXTENSION       filterExt;
    WDF_IO_QUEUE_CONFIG     ioQueueConfig;

    UNREFERENCED_PARAMETER(Driver);

    PAGED_CODE();

    DebugPrint(("Enter FilterEvtDeviceAdd \n"));

    //
    // Tell the framework that you are filter driver. Framework
    // takes care of inherting all the device flags & characterstics
    // from the lower device you are attaching to.
    //
    WdfFdoInitSetFilter(DeviceInit);

    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_UNKNOWN);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_EXTENSION);

    //
    // Create a framework device object.  This call will in turn create
    // a WDM deviceobject, attach to the lower stack and set the
    // appropriate flags and attributes.
    //
    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &hDevice);
    if (!NT_SUCCESS(status)) {
        DebugPrint(("WdfDeviceCreate failed with status code 0x%x\n", status));
        return status;
    }

    filterExt = FilterGetData(hDevice);
	filterExt->WdfDevice = hDevice; //没有用到

	//------------------------------------------------
	// 创建缺省的queue，用于直通？
	//------------------------------------------------
    //
    // Configure the default queue to be Parallel. Do not use sequential queue
    // if this driver is going to be filtering PS2 ports because it can lead to
    // deadlock. The PS2 port driver sends a request to the top of the stack when it
    // receives an ioctl request and waits for it to be completed. If you use a
    // a sequential queue, this request will be stuck in the queue because of the 
    // outstanding ioctl request sent earlier to the port driver.
    //
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&ioQueueConfig,
		WdfIoQueueDispatchParallel);

    //
    // Framework by default creates non-power managed queues for
    // filter drivers.
    //

    ioQueueConfig.EvtIoWrite = HidFilter_EvtIoWrite;
	ioQueueConfig.EvtIoInternalDeviceControl = HidFilter_EvtIoInternalDeviceControl;
	//ioQueueConfig.PowerManaged = WdfTrue;
	//ioQueueConfig.EvtIoDefault = HidFilter_EvtIoDefault;

	deviceAttributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfIoQueueCreate(hDevice,
                            &ioQueueConfig,
							&deviceAttributes, //意味着queue没有context!
                            WDF_NO_HANDLE // pointer to default queue
                            );
    if (!NT_SUCCESS(status)) {
        DebugPrint( ("WdfIoQueueCreate failed 0x%x\n", status));
        return status;
    }

	//------------------------------------------------
	// 创建另一个queue，用于处理pdo转发过来的
	//------------------------------------------------
    //
    // Create a new queue to handle IOCTLs that will be forwarded to us from
    // the rawPDO. 
    //
    WDF_IO_QUEUE_CONFIG_INIT(&ioQueueConfig,
                             WdfIoQueueDispatchParallel);

    //
    // Framework by default creates non-power managed queues for
    // filter drivers.
    //
    ioQueueConfig.EvtIoDeviceControl = HidFilter_EvtIoDeviceControlFromRawPdo;
	ioQueueConfig.PowerManaged = WdfFalse;

    status = WdfIoQueueCreate(hDevice,
                            &ioQueueConfig,
                            WDF_NO_OBJECT_ATTRIBUTES, //意味着queue没有context!
                            &hQueue //输出，需要保存
                            );
    if (!NT_SUCCESS(status)) {
        DebugPrint( ("WdfIoQueueCreate failed 0x%x\n", status));
        return status;
    }

    filterExt->rawPdoQueue = hQueue;

	//------------------------------------------------
	// 创建pdo
	//------------------------------------------------

    //
    // Create a RAW pdo so we can provide a sideband communication with
    // the application. Please note that not filter drivers desire to
    // produce such a communication and not all of them are contrained
    // by other filter above which prevent communication thru the device
    // interface exposed by the main stack. So use this only if absolutely
    // needed. Also look at the toaster filter driver sample for an alternate
    // approach to providing sideband communication.
    //
    //status = KbFiltr_CreateRawPdo(hDevice, ++InstanceNo);

    return status;
}

VOID
HidFilter_EvtIoDeviceControlFromRawPdo(
    IN WDFQUEUE      Queue,
    IN WDFREQUEST    Request,
    IN size_t        OutputBufferLength,
    IN size_t        InputBufferLength,
    IN ULONG         IoControlCode
    )
/*++

Routine Description:

    This routine is the dispatch routine for device control requests.

Arguments:

    Queue - Handle to the framework queue object that is associated
            with the I/O request.
    Request - Handle to a framework request object.

    OutputBufferLength - length of the request's output buffer,
                        if an output buffer is available.
    InputBufferLength - length of the request's input buffer,
                        if an input buffer is available.

    IoControlCode - the driver-defined or system-defined I/O control code
                    (IOCTL) that is associated with the request.

Return Value:

   VOID

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    WDFDEVICE hDevice;
    WDFMEMORY outputMemory;
    PDEVICE_EXTENSION devExt;
    size_t bytesTransferred = 0;

    UNREFERENCED_PARAMETER(InputBufferLength);

    DebugPrint(("Entered HidFilter_EvtIoDeviceControlFromRawPdo\n"));

    hDevice = WdfIoQueueGetDevice(Queue);
    devExt = FilterGetData(hDevice);

    //
    // Process the ioctl and complete it when you are done.
    //

    switch (IoControlCode) {
    case IOCTL_KBFILTR_GET_KEYBOARD_ATTRIBUTES:
        
        //
        // Buffer is too small, fail the request
        //
        if (OutputBufferLength < sizeof(HID_DEVICE_ATTRIBUTES)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        status = WdfRequestRetrieveOutputMemory(Request, &outputMemory);
        
        if (!NT_SUCCESS(status)) {
            DebugPrint(("WdfRequestRetrieveOutputMemory failed %x\n", status));
            break;
        }
        
        status = WdfMemoryCopyFromBuffer(outputMemory,
                                    0,
                                    &devExt->HidAttributes,
                                    sizeof(HID_DEVICE_ATTRIBUTES));

        if (!NT_SUCCESS(status)) {
            DebugPrint(("WdfMemoryCopyFromBuffer failed %x\n", status));
            break;
        }

        bytesTransferred = sizeof(HID_DEVICE_ATTRIBUTES);
        
        break;    
    default:
        status = STATUS_NOT_IMPLEMENTED;
        break;
    }
    
    WdfRequestCompleteWithInformation(Request, status, bytesTransferred);

    return;
}

VOID
HidFilter_EvtIoInternalDeviceControl(
    IN WDFQUEUE      Queue,
    IN WDFREQUEST    Request,
    IN size_t        OutputBufferLength,
    IN size_t        InputBufferLength,
    IN ULONG         IoControlCode
    )
/*++

Routine Description:

    This routine is the dispatch routine for internal device control requests.
    There are two specific control codes that are of interest:

    IOCTL_HID_GET_DEVICE_ATTRIBUTES:
        xxx.

   

Arguments:

    Queue - Handle to the framework queue object that is associated
            with the I/O request.
    Request - Handle to a framework request object.

    OutputBufferLength - length of the request's output buffer,
                        if an output buffer is available.
    InputBufferLength - length of the request's input buffer,
                        if an input buffer is available.

    IoControlCode - the driver-defined or system-defined I/O control code
                    (IOCTL) that is associated with the request.

Return Value:

   VOID

--*/
{
    PDEVICE_EXTENSION               devExt;
    NTSTATUS                        status = STATUS_SUCCESS;
    
    WDFDEVICE                       hDevice;
    BOOLEAN                         forwardWithCompletionRoutine = FALSE;
    BOOLEAN                         ret = TRUE;
    WDFCONTEXT                      completionContext = WDF_NO_CONTEXT;
    WDF_REQUEST_SEND_OPTIONS        options;
    WDFMEMORY                       outputMemory;
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);
	UNREFERENCED_PARAMETER(forwardWithCompletionRoutine);


    PAGED_CODE();

    DebugPrint(("Entered HidFilter_EvtIoInternalDeviceControl\n"));

    hDevice = WdfIoQueueGetDevice(Queue);
    devExt = FilterGetData(hDevice);
/*
    switch (IoControlCode) {

    //
    // Connect a keyboard class device driver to the port driver.
    //
   
	case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
	//case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
	//case IOCTL_HID_GET_REPORT_DESCRIPTOR:

		forwardWithCompletionRoutine = TRUE;
		completionContext = devExt;
		break;
	default:
		break;
    }
	*/


    //
    // Forward the request down. WdfDeviceGetIoTarget returns
    // the default target, which represents the device attached to us below in
    // the stack.
    //

    if (0) {

        //
        // Format the request with the output memory so the completion routine
        // can access the return data in order to cache it into the context area
        //
        
        status = WdfRequestRetrieveOutputMemory(Request, &outputMemory); 

        if (!NT_SUCCESS(status)) {
            DebugPrint(("WdfRequestRetrieveOutputMemory failed: 0x%x\n", status));
            WdfRequestComplete(Request, status);
            return;
        }

        status = WdfIoTargetFormatRequestForInternalIoctl(WdfDeviceGetIoTarget(hDevice),
                                                         Request,
                                                         IoControlCode,
                                                         NULL,
                                                         NULL,
                                                         outputMemory,
                                                         NULL);

        if (!NT_SUCCESS(status)) {
            DebugPrint(("WdfIoTargetFormatRequestForInternalIoctl failed: 0x%x\n", status));
            WdfRequestComplete(Request, status);
            return;
        }
    
        // 
        // Set our completion routine with a context area that we will save
        // the output data into
        //
        WdfRequestSetCompletionRoutine(Request,
                                    HidFilterRequestCompletionRoutine,
                                    completionContext);

        ret = WdfRequestSend(Request,
                             WdfDeviceGetIoTarget(hDevice),
                             WDF_NO_SEND_OPTIONS);

        if (ret == FALSE) {
            status = WdfRequestGetStatus (Request);
            DebugPrint( ("WdfRequestSend failed: 0x%x\n", status));
            WdfRequestComplete(Request, status);
        }

    }
    else
    {

		//WdfRequestFormatRequestUsingCurrentType(Request);
        //
        // We are not interested in post processing the IRP so 
        // fire and forget.
        //
		//WdfRequestSetCompletionRoutine(Request, CompletionRoutine2, WDF_NO_CONTEXT);
		//ret = WdfRequestSend(Request, WdfDeviceGetIoTarget(hDevice), WDF_NO_SEND_OPTIONS);

        //WDF_REQUEST_SEND_OPTIONS_INIT(&options,WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
		//WdfRequestFormatRequestUsingCurrentType(Request);
		WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
		ret = WdfRequestSend(Request, WdfDeviceGetIoTarget(hDevice), &options);
		



        if (ret == FALSE) {
            status = WdfRequestGetStatus (Request);
            DebugPrint(("WdfRequestSend failed: 0x%x\n", status));
            WdfRequestComplete(Request, status);
        }
        
    }

    return;
	//UNREFERENCED_PARAMETER(options);
}


VOID
CompletionRoutine2(
	WDFREQUEST                  Request,
	WDFIOTARGET                 Target,
	PWDF_REQUEST_COMPLETION_PARAMS CompletionParams,
	WDFCONTEXT                  Context
)
{
	UNREFERENCED_PARAMETER(Target);
	UNREFERENCED_PARAMETER(Context);

	NTSTATUS    status = CompletionParams->IoStatus.Status;
	WdfRequestComplete(Request, status);
}

VOID
HidFilterRequestCompletionRoutine(
    WDFREQUEST                  Request,
    WDFIOTARGET                 Target,
    PWDF_REQUEST_COMPLETION_PARAMS CompletionParams,
    WDFCONTEXT                  Context
   )
/*++

Routine Description:

    Completion Routine

Arguments:

    Target - Target handle
    Request - Request handle
    Params - request completion params
    Context - Driver supplied context


Return Value:

    VOID

--*/
{
    WDFMEMORY   buffer = CompletionParams->Parameters.Ioctl.Output.Buffer;
    NTSTATUS    status = CompletionParams->IoStatus.Status;


	UNREFERENCED_PARAMETER(buffer);
	
	PHID_DEVICE_ATTRIBUTES   deviceAttributes = NULL;

    UNREFERENCED_PARAMETER(Target);
 
    //
    // Save the keyboard attributes in our context area so that we can return
    // them to the app later.
    //
	if (NT_SUCCESS(status) &&
		CompletionParams->Type == WdfRequestTypeDeviceControlInternal)
	{
		switch (CompletionParams->Parameters.Ioctl.IoControlCode)
		{
		case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
			//
			// This IOCTL is METHOD_NEITHER so WdfRequestRetrieveOutputMemory
			// will correctly retrieve buffer from Irp->UserBuffer. 
			// Remember that HIDCLASS provides the buffer in the Irp->UserBuffer
			// field irrespective of the ioctl buffer type. However, framework is very
			// strict about type checking. You cannot get Irp->UserBuffer by using
			// WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
			// internal ioctl.
			//
			if (CompletionParams->Parameters.Ioctl.Output.Length >= sizeof(HID_DEVICE_ATTRIBUTES)) {
				status = WdfRequestRetrieveOutputBuffer(Request,
					sizeof(HID_DEVICE_ATTRIBUTES),
					&deviceAttributes,
					NULL);
				//把截获的Attribute保存在设备扩展内
				RtlCopyMemory(&((PDEVICE_EXTENSION)Context)->HidAttributes, deviceAttributes, sizeof(HID_DEVICE_ATTRIBUTES));
			}
			break;
		case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
			break;
		case IOCTL_HID_GET_REPORT_DESCRIPTOR:
			break;
		default:
			break;
		}
    }

    WdfRequestComplete(Request, status);

    return;
}

/*
VOID
HidFilter_DispatchPassThrough( //Passes a request on to the lower driver.

	_In_ WDFREQUEST Request,
	_In_ WDFIOTARGET Target
)
{
	//
	// Pass the IRP to the target
	//

	WDF_REQUEST_SEND_OPTIONS options;
	BOOLEAN ret;
	NTSTATUS status = STATUS_SUCCESS;

	//
	// We are not interested in post processing the IRP so 
	// fire and forget.
	//
	WDF_REQUEST_SEND_OPTIONS_INIT(&options,
		WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);

	ret = WdfRequestSend(Request, Target, &options);

	if (ret == FALSE) {
		status = WdfRequestGetStatus(Request);
		DebugPrint(("WdfRequestSend failed: 0x%x\n", status));
		WdfRequestComplete(Request, status);
	}

	return;
}


VOID
HidFilter_EvtIoDefault(
	_In_
	WDFQUEUE Queue,
	_In_
	WDFREQUEST Request
)
{
	WDFIOTARGET Target;
	WDFDEVICE Device = WdfIoQueueGetDevice(Queue);
	Target = WdfDeviceGetIoTarget(Device);
	HidFilter_DispatchPassThrough(
		Request,
		Target
	);
}
*/
VOID
HidFilter_EvtIoWrite(
	WDFQUEUE Queue,
	WDFREQUEST Request,
	size_t Length
)
{
	UNREFERENCED_PARAMETER(Queue);
	UNREFERENCED_PARAMETER(Request);
	DebugPrint(("WdfRequestSend failed: 0x%x\n", Length));

}

