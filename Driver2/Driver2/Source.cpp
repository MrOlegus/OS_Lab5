#include <ntddk.h>

#include "Globals.h"
#include "DispatchProcedures.h"
#include "NotifyRoutines.h"

Globals globals;

void RLoggerUnload(_In_ PDRIVER_OBJECT DriverObject);

extern "C" NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT  DriverObject, _In_ PUNICODE_STRING RegistryPath) {
	DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "привет мир");
	
	UNREFERENCED_PARAMETER(RegistryPath);

	UNICODE_STRING devName = RTL_CONSTANT_STRING(L"\\Device\\RLogger");
	UNICODE_STRING symLink = RTL_CONSTANT_STRING(L"\\??\\RLogger");
	PDEVICE_OBJECT DeviceObject;
	NTSTATUS status;

	status = IoCreateDevice(DriverObject, 0, &devName, FILE_DEVICE_UNKNOWN, 0, FALSE, &DeviceObject);
	if (!NT_SUCCESS(status)) {
		DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "Failed to create device");
		return status;
	}

	status = IoCreateSymbolicLink(&symLink, &devName);
	if (!NT_SUCCESS(status)) {
		DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "Failed to create link");
		IoDeleteDevice(DeviceObject);
		return status;
	}

	UNICODE_STRING altitude = RTL_CONSTANT_STRING(L"228.421337211488911");
	status = CmRegisterCallbackEx(OnRegistryNotify, &altitude, DriverObject, NULL, &globals.regCookie, NULL);
	if (!NT_SUCCESS(status)) {
		DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "Failed to register callback");
		IoDeleteSymbolicLink(&symLink);
		IoDeleteDevice(DeviceObject);
		return status;
	}

	DriverObject->DriverUnload = RLoggerUnload;

	DriverObject->MajorFunction[IRP_MJ_CREATE] = DriverObject->MajorFunction[IRP_MJ_CLOSE] = RLoggerCreateClose;
	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = RLoggerDeviceControl;

	KdPrint(("Driver init"));

	return STATUS_SUCCESS;
}

void RLoggerUnload(_In_ PDRIVER_OBJECT DriverObject) {
	UNICODE_STRING symLink = RTL_CONSTANT_STRING(L"\\??\\RLogger");

	IoDeleteSymbolicLink(&symLink);
	IoDeleteDevice(DriverObject->DeviceObject);
	CmUnRegisterCallback(globals.regCookie);

	DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "Driver unload");
}
