#include <ntddk.h>

#define PROCESS_X L"C:\\windows\\system32\\notepad.exe"
#define PROCESS_Y L"C:\\Windows\\System32\\mspaint.exe"

enum EventType {
	None,
	ProcessCreate,
	ProcessExit
};

struct ProcessEventInfo {
	EventType eventType;
	ULONG processId;
};

struct ProcessEventItem {
	ProcessEventInfo eventInfo;
	LIST_ENTRY readListEntry;
	LIST_ENTRY toSearchListEntry;
	int linksCount;
};

struct Globals {
	LIST_ENTRY readListHead;
	LIST_ENTRY SearchListHead;
	FAST_MUTEX mutex;
};

void Driver1Unload(_In_ PDRIVER_OBJECT DriverObject);
NTSTATUS Driver1CreateClose(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp);
NTSTATUS Driver1Read(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp);
void OnProcessNotify(PEPROCESS Process, HANDLE ProcessId, PPS_CREATE_NOTIFY_INFO CreateInfo);

NTSTATUS CompleteIrp(PIRP Irp, NTSTATUS status = STATUS_SUCCESS, ULONG_PTR info = 0);

Globals globals;

extern "C" NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT  DriverObject, _In_ PUNICODE_STRING RegistryPath) {
	UNREFERENCED_PARAMETER(RegistryPath);

	NTSTATUS status;

	PDEVICE_OBJECT DeviceObject;
	UNICODE_STRING devName = RTL_CONSTANT_STRING(L"\\Device\\Driver1");
	status = IoCreateDevice(DriverObject, 0, &devName, FILE_DEVICE_UNKNOWN, 0, FALSE, &DeviceObject);
	if (!NT_SUCCESS(status)) {
		KdPrint(("Failed to create device"));
		return status;
	}

	DeviceObject->Flags |= DO_BUFFERED_IO;

	UNICODE_STRING symLink = RTL_CONSTANT_STRING(L"\\??\\Driver1");
	status = IoCreateSymbolicLink(&symLink, &devName);
	if (!NT_SUCCESS(status)) {
		KdPrint(("Failed to create link"));
		IoDeleteDevice(DeviceObject);
		return status;
	}

	status = PsSetCreateProcessNotifyRoutineEx(OnProcessNotify, FALSE);
	if (!NT_SUCCESS(status)) {
		KdPrint(("Failed to register callback"));
		IoDeleteSymbolicLink(&symLink);
		IoDeleteDevice(DeviceObject);
		return status;
	}

	InitializeListHead(&globals.SearchListHead);
	InitializeListHead(&globals.readListHead);
	ExInitializeFastMutex(&globals.mutex);

	DriverObject->DriverUnload = Driver1Unload;
	DriverObject->MajorFunction[IRP_MJ_CREATE] = DriverObject->MajorFunction[IRP_MJ_CLOSE] = Driver1CreateClose;
	DriverObject->MajorFunction[IRP_MJ_READ] = Driver1Read;

	KdPrint(("Driver init"));

	return STATUS_SUCCESS;
}

void Driver1Unload(_In_ PDRIVER_OBJECT DriverObject) {
	UNICODE_STRING symLink = RTL_CONSTANT_STRING(L"\\??\\Driver1");

	PsSetCreateProcessNotifyRoutineEx(OnProcessNotify, TRUE);
	IoDeleteSymbolicLink(&symLink);
	IoDeleteDevice(DriverObject->DeviceObject);

	KdPrint(("Driver unload"));
}

NTSTATUS Driver1CreateClose(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp) {
	UNREFERENCED_PARAMETER(DeviceObject);

	return CompleteIrp(Irp, STATUS_SUCCESS, 0);
}

void OnProcessNotify(PEPROCESS Process, HANDLE ProcessId, PPS_CREATE_NOTIFY_INFO CreateInfo) {
	UNREFERENCED_PARAMETER(Process);
	UNREFERENCED_PARAMETER(ProcessId);
	UNREFERENCED_PARAMETER(CreateInfo);

	if (CreateInfo != NULL) {

		UNICODE_STRING targetImage = RTL_CONSTANT_STRING(PROCESS_X);
		if ((CreateInfo->ImageFileName == NULL) || (RtlCompareUnicodeString(&targetImage, CreateInfo->ImageFileName, TRUE) != 0)) {
			return;
		}
		//KdPrint(("Target process created"));

		ProcessEventItem* eItem = (ProcessEventItem*)ExAllocatePoolWithTag(PagedPool, sizeof(ProcessEventItem), 'gaTm');
		if (eItem == NULL) {
			KdPrint(("Failed to allocate pool"));
			return;
		}

		eItem->eventInfo.eventType = ProcessCreate;
		eItem->eventInfo.processId = HandleToULong(ProcessId);
		eItem->linksCount = 2;

		ExAcquireFastMutex(&globals.mutex);

		InsertTailList(&globals.SearchListHead, &eItem->toSearchListEntry);
		InsertTailList(&globals.readListHead, &eItem->readListEntry);

		ExReleaseFastMutex(&globals.mutex);

		KdPrint(("Start event enqueued"));
	}
	else {

		ULONG ulongProcessId = HandleToULong(ProcessId);
		ProcessEventItem* killedProcess = NULL;

		bool needRelise = false;
		ExAcquireFastMutex(&globals.mutex);

		PLIST_ENTRY next = globals.SearchListHead.Flink;
		while (next != &globals.SearchListHead) {

			ProcessEventItem* curr = CONTAINING_RECORD(next, ProcessEventItem, toSearchListEntry);
			if (curr->eventInfo.processId == ulongProcessId) {

				killedProcess = curr;

				RemoveEntryList(&killedProcess->toSearchListEntry);
				killedProcess->linksCount--;
				needRelise = killedProcess->linksCount == 0;

				break;
			}

			next = next->Flink;
		}

		ExReleaseFastMutex(&globals.mutex);
		
		if (killedProcess == NULL) {
			return;
		}

		if (needRelise) {
			ExFreePool(killedProcess);
			KdPrint(("Event dequeued"));
		}

		ProcessEventItem* eItem = (ProcessEventItem*)ExAllocatePoolWithTag(PagedPool, sizeof(ProcessEventItem), 'gaTm');
		if (eItem == NULL) {
			KdPrint(("Failed to allocate pool"));
			return;
		}

		eItem->eventInfo.eventType = ProcessExit;
		eItem->eventInfo.processId = ulongProcessId;
		eItem->linksCount = 1;
		
		
		ExAcquireFastMutex(&globals.mutex);

		InsertTailList(&globals.readListHead, &eItem->readListEntry);

		ExReleaseFastMutex(&globals.mutex);
		KdPrint(("Exit event enqueued"));
	}
}

NTSTATUS Driver1Read(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp) {
	UNREFERENCED_PARAMETER(DeviceObject);

	PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
	ULONG length = stack->Parameters.Read.Length;

	if (length < sizeof(ProcessEventInfo)) {
		return CompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
	}

	ProcessEventInfo* sysbuffer = (ProcessEventInfo*)Irp->AssociatedIrp.SystemBuffer;
	ProcessEventItem* readingItem = NULL;

	bool needRelise = false;
	ExAcquireFastMutex(&globals.mutex);

	if (globals.readListHead.Flink != &globals.readListHead) {
		readingItem = CONTAINING_RECORD(globals.readListHead.Flink, ProcessEventItem, readListEntry);
		RemoveEntryList(&readingItem->readListEntry);
		readingItem->linksCount--;
		needRelise = readingItem->linksCount == 0;
	}

	ExReleaseFastMutex(&globals.mutex);

	if (readingItem != NULL) {
		sysbuffer->eventType = readingItem->eventInfo.eventType;
		sysbuffer->processId = readingItem->eventInfo.processId;

		if (needRelise) {
			ExFreePool(readingItem);
			KdPrint(("Event dequeued"));
		}

		return CompleteIrp(Irp, STATUS_SUCCESS, sizeof(ProcessEventInfo));
	}
	
	return CompleteIrp(Irp, STATUS_SUCCESS, 0);
}

NTSTATUS CompleteIrp(PIRP Irp, NTSTATUS status, ULONG_PTR info) {
	Irp->IoStatus.Status = status;
	Irp->IoStatus.Information = info;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return status;
}