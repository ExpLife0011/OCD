/*++

Copyright (c) Microsoft Corporation.  All rights reserved.

    THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY
    KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A PARTICULAR
    PURPOSE.

Module Name:

    device.c

Abstract:

    Code for handling WDF device-specific requests

Environment:

    kernel mode only

Revision History:

--*/

#include "device.h"

#ifdef  WPP_TRACING
#include "device.tmh"
#endif

#define BOOLIFY(expr) (!!(expr))

//
// Macro for determining if debugger is currently attached.
// KD_DEBUGGER_NOT_PRESENT behavior:
//
// If debugging was not enabled on the computer at boot time,
// KD_DEBUGGER_NOT_PRESENT value is 1.
//
// If debugging was enabled on the computer at boot time, but no kernel
// debugger is currently attached, KD_DEBUGGER_NOT_PRESENT value is 1.
//
// If debugging was enabled on the computer at boot time, and a kernel debugger
// is currently attached, KD_DEBUGGER_NOT_PRESENT value is 0.
//
// If a kernel debugger was recently attached or removed, the value of 
// KD_DEBUGGER_NOT_PRESENT may not reflect the new state.
//
#define IS_DEBUGGER_ATTACHED() (BOOLIFY(KD_DEBUGGER_NOT_PRESENT) == FALSE ? TRUE : FALSE)


VOID
WpCrasherDoBugCheck (
    )
/*++

Routine Description:

    This routine raises a debug break if debugger is connected. Otherwise forces
    a BugCheck.

Arguments:
    None.

Return Value:

    None.
--*/
{
    if (IS_DEBUGGER_ATTACHED() != FALSE)
    {
        __debugbreak();
    }
    else
    {
#pragma prefast(suppress: 28159, "By Design, Need to bugcheck as user requested through touch pattern")
        KeBugCheckEx(MANUALLY_INITIATED_CRASH, 1, 0, 0, 0);
    }
}


VOID
WpCrasherEvtDeviceContextCleanup (
    IN WDFOBJECT Device
    )

/*++

Routine Description:

    This routine is invoked by the framework when the device is being deleted
    in response to IRP_MN_REMOVE_DEVICE request. This callback must perform any
    cleanup operations that are necessary before the specified device is
    removed.

Arguments:

    Device - Supplies a handle to the framework device object.

Return Value:

    None.

--*/

{
    PDEVICE_EXTENSION devContext = NULL;

	devContext = GetDeviceContext(Device);
	
	//
    //  Deregister for monitor state notifications      
	//
	if (devContext->MonitorChangeNotificationHandle != NULL)  
    {  
        PoUnregisterPowerSettingCallback( devContext->MonitorChangeNotificationHandle);  
        devContext->MonitorChangeNotificationHandle = NULL;  
    }  
}

BOOLEAN
WpCrasherEvtInterruptIsr(
    IN WDFINTERRUPT Interrupt,
    IN ULONG MessageID
    )
/*++

  Routine Description:

    This routine responds to interrupts generated by the controller.

    This routine recording the touch inputs. If user performs below input:
      1.Run finger on the screen for ~7 secs (the graceful period is 5 ~ 15
        secs).
      2.Stop running the finger on the screen for ~7 secs (the graceful period
        is 5 ~ 15 secs).
      3.Repeat step 1 and 2.

    The device BSODs after the fourth time to generate a crash dump.

    This routine also checks the system state, if system is "hang", it will
    BugCheck before all 4 inputs are completed.

  Arguments:

    Interrupt - a handle to a framework interrupt object
    MessageID - message number identifying the device's hardware interrupt
        message (if using MSI)

  Return Value:

    TRUE if interrupt recognized.

--*/
{
    PDEVICE_EXTENSION devContext = NULL;
    ULONGLONG currentTime = 0;
    ULONGLONG stoppedTime = 0;
    ULONGLONG swippingTime = 0;

    UNREFERENCED_PARAMETER(MessageID);

    currentTime = KeQueryUnbiasedInterruptTime();

    devContext = GetDeviceContext(WdfInterruptGetDevice(Interrupt));

    //
    // Start recording the touch inputs if we haven't started yet.
    //
    if (devContext->TimeStampInputPatternBegin == 0)
    {
        devContext->TimeStampInputPatternBegin = currentTime;
        devContext->TimeStampSwipeBegin = currentTime;
        devContext->TimeStampLastInterrupt = currentTime;
        devContext->NumberOfSwipes = 0;
        devContext->SwipeThreholdReached = FALSE;

        TraceMessage(TRACE_LEVEL_ERROR,
                     TRACE_FLAG_INTERRUPT,
                     ("Start to log inputs"));

        goto Exit;
    }

    //
    // If the entire process took over 60 seconds, restart.
    //
    if (currentTime - devContext->TimeStampInputPatternBegin
        > INPUT_PATTERN_MAX_PERIOD)
    {
        TraceMessage(TRACE_LEVEL_ERROR,
                     TRACE_FLAG_INTERRUPT,
                     ("Total input time Longer than 60 secs, restart"));
        devContext->TimeStampInputPatternBegin = 0;
        goto Exit;
    }

    //
    // Time since last touch interrupt.
    //
    stoppedTime = currentTime -
            devContext->TimeStampLastInterrupt;

    //
    // Time since the current swipe start.
    //
    swippingTime = devContext->TimeStampLastInterrupt -
            devContext->TimeStampSwipeBegin;

    devContext->TimeStampLastInterrupt = currentTime;

    //
    // 2 touch inputs within 0.2 seconds will be considered as in one swipe. If
    // the interval of 2 touch inputs is more than 0.2 seconds, it will be
    // considered as 2 swipes.
    //
    if (stoppedTime > SINGLE_SWIPE_MAX_INTERVAL)
    {
        //
        // Bewteen each swipe, user should wait for 5~15 seconds. If the wait
        // time is out of this window, restart.
        //
        if (stoppedTime < SINGLE_SWIPE_MIN_PERIOD ||
            stoppedTime > SINGLE_SWIPE_MAX_PERIOD)
        {
            TraceMessage(TRACE_LEVEL_ERROR,
                         TRACE_FLAG_INTERRUPT,
                         ("Stopped time: %u ms, restart",
                         (ULONG)stoppedTime / 10000));
            devContext->TimeStampInputPatternBegin = 0;
            goto Exit;
        }

        //
        // Start logging the next swipe.
        //
        devContext->TimeStampSwipeBegin = currentTime;
        swippingTime = 0;
        devContext->SwipeThreholdReached = FALSE;
        TraceMessage(TRACE_LEVEL_ERROR,
                     TRACE_FLAG_INTERRUPT,
                     ("Start to swipe: %u",
                     devContext->NumberOfSwipes + 1));
    }

    if (swippingTime > SINGLE_SWIPE_MAX_PERIOD)
    {
        TraceMessage(TRACE_LEVEL_ERROR,
                     TRACE_FLAG_INTERRUPT,
                     ("Swipe time longer than 10 secs, restart"));
        devContext->TimeStampInputPatternBegin = 0;
        goto Exit;
    }

    if (swippingTime < SINGLE_SWIPE_MIN_PERIOD ||
        devContext->SwipeThreholdReached == TRUE)
    {
        goto Exit;
    }

    //
    // User continuously swiped for 5 seconds
    //
    devContext->NumberOfSwipes++;
    devContext->SwipeThreholdReached = TRUE;
    TraceMessage(TRACE_LEVEL_INFORMATION,
                 TRACE_FLAG_INTERRUPT,
                 ("Number of swipe: %u", devContext->NumberOfSwipes));

    if (devContext->NumberOfSwipes == INPUT_PATTERN_NUM_OF_SWIPES)
    {
        //
        // If user swipes INPUT_PATTERN_NUM_OF_SWIPES times, force a BugCheck.
        //
        WpCrasherDoBugCheck();
        devContext->TimeStampInputPatternBegin = 0;
    }
    else
    {
        NT_ASSERT(devContext->NumberOfSwipes <
                  INPUT_PATTERN_NUM_OF_SWIPES);
    }

	//
	// Returing TRUE in the case of level triggered interrupts does not let the Touch ISR get called by the Interrupt Dispatcher
	// Whereas in the case of edge triggered interrupts,  all the ISRs chained to the interrupt vector are called 
	// in either case FALSE (NotClaimed) or TRUE (Claimed)
	//

Exit:
    return FALSE;
}

NTSTATUS
WpCrasherOnPowerSettingsChange(
    _In_ LPCGUID SettingGuid,
    _In_reads_bytes_(ValueLength) PVOID Value,
    _In_ ULONG ValueLength,
    _Inout_opt_ PVOID CbContext
    )
/*++

Routine Description:

    This callback is invoked on monitor state changes. Here we soft-connect/disconnect
    the interrupt based on the monitor state.

Arguments:

    SettingGuid - GUID_MONITOR_POWER_ON (the only notification registered)
    Value - Either MONITOR_IS_ON or MONITOR_IS_OFF
    ValueLength - Ignored, always sizeof(ULONG)
    CbContext - Callback Context, in this case it is WDFDEVICE

Return Value:

    NTSTATUS indicating STATUS_SUCCESS or STATUS_INVALID_PARAMETER 

--*/
{
    ULONG monitorState;
    NTSTATUS status = STATUS_SUCCESS;
	WDFDEVICE fxDevice = (WDFDEVICE)CbContext;
	PDEVICE_EXTENSION devContext = NULL;

    //
    // Should never happen, but we don't care about events unrelated to display
    //
    if (!InlineIsEqualGUID(SettingGuid, &GUID_MONITOR_POWER_ON))
    {
        goto Exit;
    }

    //
    // Should never happen, but check for bad parameters for this notification
    //
    if (Value == NULL || ValueLength != sizeof(ULONG) || CbContext == NULL)
    {
		status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

	devContext = GetDeviceContext(fxDevice); 
    monitorState = *((PULONG) Value);

	if(devContext->LastProcessedMonitorState == monitorState)
	{
		goto Exit;
	}

	devContext->LastProcessedMonitorState = monitorState;

	//
	// Report Interrupt as Active or Inactive based on monitor on/off state 
	//
	if(monitorState == MONITOR_IS_ON)
	{
		WdfInterruptReportActive(devContext->InterruptObject);
	}
	else
	{
		WdfInterruptReportInactive(devContext->InterruptObject);
	}


Exit:
    return status;
}