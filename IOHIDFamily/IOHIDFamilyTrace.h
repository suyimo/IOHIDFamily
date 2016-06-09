/*
 * @APPLE_LICENSE_HEADER_START@
 * 
 * Copyright (c) 2009 Apple Computer, Inc.  All Rights Reserved.
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
#ifndef _IOKIT_HID_IOHIDFAMILYTRACE_H // {
#define _IOKIT_HID_IOHIDFAMILYTRACE_H

#include <sys/kdebug.h>

#define IOHID_DEBUG_CODE(code)          IOKDBG_CODE(DBG_IOHID, code)
#define IOHID_DEBUG(code, a, b, c, d)   KERNEL_DEBUG_CONSTANT(IOHID_DEBUG_CODE(code), a, b, c, d, 0)

enum kIOHIDDebugCodes {
    kIOHIDDebugCode_Unexpected,
    kIOHIDDebugCode_KeyboardLEDThreadTrigger,
    kIOHIDDebugCode_KeyboardLEDThreadActive,
    kIOHIDDebugCode_KeyboardSetParam,
    kIOHIDDebugCode_KeyboardCapsThreadTrigger,
    kIOHIDDebugCode_KeyboardCapsThreadActive,
    kIOHIDDebugCode_PostEvent,
    kIOHIDDebugCode_NewUserClient,
    kIOHIDDebugCode_InturruptReport,
    kIOHIDDebugCode_DispatchScroll,
    kIOHIDDebugCode_DispatchRelativePointer,
    kIOHIDDebugCode_DispatchAbsolutePointer,
    kIOHIDDebugCode_DispatchKeyboard,
    kIOHIDDebugCode_EjectCallback,
    kIOHIDDebugCode_CapsCallback,
    kIOHIDDebugCode_HandleReport,
    kIOHIDDebugCode_DispatchTabletPointer,
    kIOHIDDebugCode_DispatchTabletProx,
    kIOHIDDebugCode_DispatchHIDEvent,
    kIOHIDDebugCode_CalculatedCapsDelay,
    kIOHIDDebugCode_Invalid
};

#endif // _IOKIT_HID_IOHIDFAMILYTRACE_H }