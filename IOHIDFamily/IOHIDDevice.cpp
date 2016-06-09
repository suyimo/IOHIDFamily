/*
 * Copyright (c) 2000 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.1 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 * 
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

#include <IOKit/IOLib.h>    // IOMalloc/IOFree
#include <IOKit/IOBufferMemoryDescriptor.h>
#include "IOHIDDevice.h"
#include "IOHIDElement.h"
#include "IOHIDParserPriv.h"
#include "IOHIDPointing.h"

//===========================================================================
// IOHIDDevice class

#undef  super
#define super IOService

OSDefineMetaClassAndAbstractStructors( IOHIDDevice, IOService )

// Number of slots in the report handler dispatch table.
//
#define kReportHandlerSlots         8

// Convert from a report ID to a dispatch table slot index.
//
#define GetReportHandlerSlot(id)    ((id) & (kReportHandlerSlots - 1))

#define GetElement(index)  \
    (IOHIDElement *) _elementArray->getObject((UInt32)index)

// Serialize access to the elements for report handling,
// event queueing, and report creation.
//
#define ELEMENT_LOCK                IOLockLock( _elementLock )
#define ELEMENT_UNLOCK              IOLockUnlock( _elementLock )

// Describes the handler(s) at each report dispatch table slot.
//
struct IOHIDReportHandler
{
    IOHIDElement * head[ kIOHIDReportTypeCount ];
};

#define GetHeadElement(slot, type)  _reportHandlers[slot].head[type]

// #define DEBUG 1
#ifdef  DEBUG
#define DLOG(fmt, args...)  IOLog(fmt, ## args)
#else
#define DLOG(fmt, args...)
#endif
            
#define GetPointingNub(service) \
            OSDynamicCast(IOHIDPointing, service)

// *** GAME DEVICE HACK ***
static SInt32 g3DGameControllerCount = 0;
// *** END GAME DEVICE HACK ***

//---------------------------------------------------------------------------
// Static helper function that will return a new IOHIDevice depending
// on the type of HID device.
static IOHIDevice * CreateIOHIDeviceNub(IOService * owner, IOService * provider)
{
    IOHIDevice 	*nub = 0;
    OSString 	*defaultBehavior;
    if (owner == NULL || OSDynamicCast(IOHIPointing, provider))
        return  0;
    
    defaultBehavior = OSDynamicCast(OSString, 
                            owner->getProperty("HIDDefaultBehavior"));
     
    // Fixing a bug in IOUSBFamily that adds a space after the key
    if (!defaultBehavior) {
        defaultBehavior = OSDynamicCast(OSString, 
                            owner->getProperty("HIDDefaultBehavior "));
    }
    
    if (defaultBehavior &&
        defaultBehavior->isEqualTo("Mouse")) {
                            
        nub = new IOHIDPointing;
        
        if (nub &&
            (!nub->init() || 
             !nub->attach(owner) || 
             !nub->start(owner))) 
        {
            nub->release();
            nub = 0;
        }
    }
    
    return nub;
}

//---------------------------------------------------------------------------
// Notification handler to grab an instance of the Display Manager
bool IOHIDDevice::publishNotificationHandler(
			void * target,
			void * /* ref */,
			IOService * newService )
{
    IOHIDDevice * self = (IOHIDDevice *) target;

    // avoiding OSDynamicCast & dependency on graphics family
    if( newService->metaCast("IODisplayWrangler")) {
        if( !self->_displayManager) {
            self->_displayManager = newService;
            self->_displayManager->retain();
        }
    }

    return true;
}


//---------------------------------------------------------------------------
// Initialize an IOHIDDevice object.

bool IOHIDDevice::init( OSDictionary * dict )
{
    _reserved = IONew( ExpansionData, 1 );

    if (!_reserved)
        return false;
        
    _pointingNub = 0;
    _displayManager = 0;
    _publishNotify = 0;

    // Create an OSSet to store client objects. Initial capacity
    // (which can grow) is set at 2 clients.

    _clientSet = OSSet::withCapacity(2);
    if ( _clientSet == 0 )
        return false;

    return super::init(dict);
}

//---------------------------------------------------------------------------
// Free an IOHIDDevice object after its retain count drops to zero.
// Release all resource.

void IOHIDDevice::free()
{
    if ( _reportHandlers )
    {
        IOFree( _reportHandlers,
                sizeof(IOHIDReportHandler) * kReportHandlerSlots );
        _reportHandlers = 0;
    }

    if ( _elementArray )
    {
        _elementArray->release();
        _elementArray = 0;
    }
    
    if ( _elementValuesDescriptor )
    {
        _elementValuesDescriptor->release();
        _elementValuesDescriptor = 0;
    }

    if ( _elementLock )
    {
        IOLockFree( _elementLock );
        _elementLock = 0;
    }
    
    if ( _clientSet )
    {
        // Should not have any clients.
        assert(_clientSet->getCount() == 0);
        _clientSet->release();
        _clientSet = 0;
    }
    
    if (_publishNotify)
    {
        _publishNotify->release();
        _publishNotify = 0;
    }
    
    if (_displayManager)
    {
        _displayManager->release();
        _displayManager = 0;
    }
    
    if ( _reserved )
    {        
        IODelete( _reserved, ExpansionData, 1 );
    }


    return super::free();
}

//---------------------------------------------------------------------------
// Start up the IOHIDDevice.

bool IOHIDDevice::start( IOService * provider )
{
    IOMemoryDescriptor * reportDescriptor;
    IOReturn             ret;
    // IOHIDPointing *	 tempNub;

    if ( super::start(provider) != true )
        return false;

    // Allocate a mutex lock to serialize report handling.

    _elementLock = IOLockAlloc();
    if ( _elementLock == 0 )
        return false;

    // Allocate memory for report handler dispatch table.

    _reportHandlers = (IOHIDReportHandler *)
                      IOMalloc( sizeof(IOHIDReportHandler) *
                                kReportHandlerSlots );
    if ( _reportHandlers == 0 )
        return false;

    bzero( _reportHandlers, sizeof(IOHIDReportHandler) * kReportHandlerSlots );

    // Call handleStart() before fetching the report descriptor.

    if ( handleStart(provider) != true )
        return false;

    // Fetch report descriptor for the device, and parse it.

    if ( ( newReportDescriptor(&reportDescriptor) != kIOReturnSuccess ) ||
         ( reportDescriptor == 0 ) )
        return false;

    ret = parseReportDescriptor( reportDescriptor );
    reportDescriptor->release();

    if ( ret != kIOReturnSuccess )
        return false;

    // Once the report descriptors have been parsed, we are ready
    // to handle reports from the device.

    _readyForInputReports = true;

    // Publish properties to the registry before any clients are
    // attached.

    if ( publishProperties(provider) != true )
        return false;

    // Create an IOHIDevice nub
    // This has to be done after we call publishProperties
    // becuase we determine the nub to create based on the
    // device's PrimaryUsage and PrimaryUsagePage
    _pointingNub = GetPointingNub(CreateIOHIDeviceNub(this, provider));

    // Add a notification to get an instance of the Display
    // Manager.  This will allow us to tickle it upon receiveing
    // new reports.  Only do this if the device is has a primary
    // usage of generic desktop
    OSNumber *primaryUsagePage = OSDynamicCast(OSNumber,
                                    getProperty(kIOHIDPrimaryUsagePageKey));
    OSNumber *primaryUsage = OSDynamicCast(OSNumber,
                                    getProperty(kIOHIDPrimaryUsageKey));
    if (primaryUsagePage && 
       (primaryUsagePage->unsigned32BitValue() == kHIDPage_GenericDesktop)) 
    {
        _publishNotify = addNotification( gIOPublishNotification, 
                            serviceMatching("IODisplayWrangler"),
                            &IOHIDDevice::publishNotificationHandler,
                            this, 0 );
    }
    
    // *** GAME DEVICE HACK ***
    if ((primaryUsagePage && (primaryUsagePage->unsigned32BitValue() == 0x05)) &&
        (primaryUsage && (primaryUsage->unsigned32BitValue() == 0x01))) {
        OSIncrementAtomic(&g3DGameControllerCount);
    }
    // *** END GAME DEVICE HACK ***

    // Publish ourself to the registry and trigger client matching.
    registerService();

    return true;
}

//---------------------------------------------------------------------------
// Stop the IOHIDDevice.

void IOHIDDevice::stop(IOService * provider)
{
    // *** GAME DEVICE HACK ***
    OSNumber *primaryUsagePage = OSDynamicCast(OSNumber,
                                    getProperty(kIOHIDPrimaryUsagePageKey));
    OSNumber *primaryUsage = OSDynamicCast(OSNumber,
                                    getProperty(kIOHIDPrimaryUsageKey));
                                    
    if ((primaryUsagePage && (primaryUsagePage->unsigned32BitValue() == 0x05)) &&
        (primaryUsage && (primaryUsage->unsigned32BitValue() == 0x01))) {
        OSDecrementAtomic(&g3DGameControllerCount);
    }
    // *** END GAME DEVICE HACK ***
    
    handleStop(provider);

    if ( _elementLock )
    {
        ELEMENT_LOCK;
        _readyForInputReports = false;
        ELEMENT_UNLOCK;
    }
    
    if ( _pointingNub ) {
    
        _pointingNub->stop(this);
        _pointingNub->detach(this);
        
        _pointingNub->release();
        _pointingNub = 0;
    }
    


    super::stop(provider);
}

//---------------------------------------------------------------------------
// Compare the properties in the supplied table to this object's properties.

static bool CompareProperty( IOService * owner, OSDictionary * matching, const char * key )
{
    // We return success if we match the key in the dictionary with the key in
    // the property table, or if the prop isn't present
    //
    OSObject 	* value;
    bool	matches;
    
    value = matching->getObject( key );

    if( value)
        matches = value->isEqualTo( owner->getProperty( key ));
    else
        matches = true;

    return matches;
}

bool IOHIDDevice::matchPropertyTable(OSDictionary * table, SInt32 * score)
{
    bool match = true;

    // Ask our superclass' opinion.
    if (super::matchPropertyTable(table, score) == false)  return false;

    // Compare properties.        
    if (!CompareProperty(this, table, kIOHIDTransportKey) 	||
        !CompareProperty(this, table, kIOHIDVendorIDKey) 	||
        !CompareProperty(this, table, kIOHIDProductIDKey) 	||
        !CompareProperty(this, table, kIOHIDVersionNumberKey) 	||
        !CompareProperty(this, table, kIOHIDManufacturerKey) 	||
        !CompareProperty(this, table, kIOHIDSerialNumberKey) 	||
        !CompareProperty(this, table, kIOHIDLocationIDKey) 	||
        !CompareProperty(this, table, kIOHIDPrimaryUsageKey) 	||
        !CompareProperty(this, table, kIOHIDPrimaryUsagePageKey))
        match = false;

    // *** HACK ***
    // RY: For games that are accidentaly matching on the keys
    // PrimaryUsage = 0x01
    // PrimaryUsagePage = 0x05
    // If there no devices present that contain these values,
    // then return true.
    if (!match && (g3DGameControllerCount <= 0) && table) {
        OSNumber *primaryUsage = OSDynamicCast(OSNumber, table->getObject(kIOHIDPrimaryUsageKey));
        OSNumber *primaryUsagePage = OSDynamicCast(OSNumber, table->getObject(kIOHIDPrimaryUsagePageKey));

        if ((primaryUsage && (primaryUsage->unsigned32BitValue() == 0x01)) &&
            (primaryUsagePage && (primaryUsagePage->unsigned32BitValue() == 0x05))) {
            match = true;
            IOLog("IOHIDManager: It appears that an application is attempting to locate an invalid device.  A workaround is in currently in place, but will be removed after version 10.2\n");
        }
    }
    // *** END HACK ***
        
    return match;
}



//---------------------------------------------------------------------------
// Fetch and publish HID properties to the registry.

bool IOHIDDevice::publishProperties(IOService * provider)
{
    OSObject * prop;

#define SET_PROP(func, key)          \
    do {                             \
        prop = func ## ();           \
        if (prop) {                  \
            setProperty(key, prop);  \
            prop->release();         \
        }                            \
    } while (0)
    
    SET_PROP( newTransportString,        kIOHIDTransportKey );
    SET_PROP( newVendorIDNumber,         kIOHIDVendorIDKey );
    SET_PROP( newProductIDNumber,        kIOHIDProductIDKey );
    SET_PROP( newVersionNumber,          kIOHIDVersionNumberKey );
    SET_PROP( newManufacturerString,     kIOHIDManufacturerKey );
    SET_PROP( newProductString,          kIOHIDProductKey );
    SET_PROP( newLocationIDNumber,       kIOHIDLocationIDKey );
    
    // RY: By default we publish the SerialNumber number, but if a
    // SerialNumber string is present, overwrite that table entry.
    SET_PROP( newSerialNumber,           kIOHIDSerialNumberKey );
    SET_PROP( newSerialNumberString,     kIOHIDSerialNumberKey );
    
    SET_PROP( newPrimaryUsageNumber,     kIOHIDPrimaryUsageKey );
    SET_PROP( newPrimaryUsagePageNumber, kIOHIDPrimaryUsagePageKey );

    return true;
}

//---------------------------------------------------------------------------
// Derived from start() and stop().

bool IOHIDDevice::handleStart(IOService * provider)
{
    return true;
}

void IOHIDDevice::handleStop(IOService * provider)
{
}

//---------------------------------------------------------------------------
// Handle a client open on the interface.

bool IOHIDDevice::handleOpen(IOService *  client,
                                    IOOptionBits options,
                                    void *       argument)
{
    bool  accept         = false;

    do {
        // Was this object already registered as our client?

        if ( _clientSet->containsObject(client) )
        {
            DLOG("%s: multiple opens from client %lx\n",
                 getName(), (UInt32) client);
            accept = true;
            break;
        }

        // Add the new client object to our client set.

        if ( _clientSet->setObject(client) == false )
        {
            break;
        }

        accept = true;
    }
    while (false);


    return accept;
}

//---------------------------------------------------------------------------
// Handle a client close on the interface.

void IOHIDDevice::handleClose(IOService * client, IOOptionBits options)
{
    // Remove the object from the client OSSet.

    if ( _clientSet->containsObject(client) )
    {
        // Remove the client from our OSSet.
        _clientSet->removeObject(client);
    }
}

//---------------------------------------------------------------------------
// Query whether a client has an open on the interface.

bool IOHIDDevice::handleIsOpen(const IOService * client) const
{
    if (client)
        return _clientSet->containsObject(client);
    else
        return (_clientSet->getCount() > 0);
}


//---------------------------------------------------------------------------
// Create a new user client.

IOReturn IOHIDDevice::newUserClient( task_t          owningTask,
                                     void *          security_id,
                                     UInt32          type,
                                     IOUserClient ** handler )
{
    return super::newUserClient(owningTask, security_id, type, handler);
}

//---------------------------------------------------------------------------
// Default implementation of the HID property 'getter' functions.

OSString * IOHIDDevice::newTransportString() const
{
    return 0;
}

OSString * IOHIDDevice::newManufacturerString() const
{
    return 0;
}

OSString * IOHIDDevice::newProductString() const
{
    return 0;
}

OSNumber * IOHIDDevice::newVendorIDNumber() const
{
    return 0;
}

OSNumber * IOHIDDevice::newProductIDNumber() const
{
    return 0;
}

OSNumber * IOHIDDevice::newVersionNumber() const
{
    return 0;
}

OSNumber * IOHIDDevice::newSerialNumber() const
{
    return 0;
}

OSNumber * IOHIDDevice::newPrimaryUsageNumber() const
{
    return 0;
}

OSNumber * IOHIDDevice::newPrimaryUsagePageNumber() const
{
    return 0;
}

//---------------------------------------------------------------------------
// Handle input reports (USB Interrupt In pipe) from the device.

IOReturn IOHIDDevice::handleReport( IOMemoryDescriptor * report,
                                    IOHIDReportType      reportType,
                                    IOOptionBits         options )
{
    AbsoluteTime   currentTime;
    void *         reportData;
    IOByteCount    reportLength;
    IOByteCount    segmentSize;
    IOReturn       ret = kIOReturnNotReady;
    bool           changed = false;

    // Only input reports are currently handled.
    
    //if ( reportType != kIOHIDReportTypeInput )
    //    return kIOReturnUnsupported;

    // Get current time.

    clock_get_uptime( &currentTime );

    // Get a pointer to the data in the descriptor.

    reportData   = report->getVirtualSegment(0, &segmentSize);
    reportLength = report->getLength();

    if ( reportLength == 0 )
        return kIOReturnBadArgument;

    // Are there multiple segments in the descriptor? If so,
    // allocate a buffer and copy the data from the descriptor.

    if ( segmentSize != reportLength )
    {
        reportData = IOMalloc( reportLength );
        if ( reportData == 0 )
            return kIOReturnNoMemory;

        report->readBytes( 0, reportData, reportLength );
    }

    ELEMENT_LOCK;

    if ( _readyForInputReports )
    {
        IOHIDElement * element;
        UInt8          reportID;

        // The first byte in the report, may be the report ID.
        // XXX - Do we need to advance the start of the report data?
        
        reportID = ( _reportCount > 1 ) ? *((UInt8 *) reportData) : 0;

        // Get the first element in the report handler chain.
            
        element = GetHeadElement( GetReportHandlerSlot(reportID),
                                    reportType);

        while ( element )
        {
            changed |= element->processReport( reportID,
                                    reportData,
                                    reportLength << 3,
                                    &currentTime,
                                    &element );
        }

        ret = kIOReturnSuccess;
    }

    ELEMENT_UNLOCK;

    // Free memory if we allocated a buffer above.

    if ( segmentSize != reportLength )
    {
        IOFree( reportData, reportLength );
    }

#if 0 // XXX - debugging
{
    UInt32 * buf = (UInt32 *) _elementValuesDescriptor->getBytesNoCopy();
    
    for (UInt32 words = 0; words < (_elementValuesDescriptor->getLength() / 4);
         words+=6, buf+=6)
    {
        IOLog("%3ld: %08lx %08lx %08lx %08lx %08lx %08lx\n",
              words,
              buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);
    }
}
#endif

    // Tickle the displayManager if any value changed and the
    // the device is open
    if ( changed && _displayManager && _clientSet->getCount()) 
    {
        _displayManager->activityTickle(0,0);
    }

    // pass the report to the IOHIDPointing nub
    if ( _pointingNub )
    {
        _pointingNub->handleReport(report, options );
    }

    return ret;
}

//---------------------------------------------------------------------------
// Get a report from the device.

IOReturn IOHIDDevice::getReport( IOMemoryDescriptor * report,
                                 IOHIDReportType      reportType,
                                 IOOptionBits         options )
{
    return kIOReturnUnsupported;
}

//---------------------------------------------------------------------------
// Send a report to the device.

IOReturn IOHIDDevice::setReport( IOMemoryDescriptor * report,
                                 IOHIDReportType      reportType,
                                 IOOptionBits         options = 0 )
{
    return kIOReturnUnsupported;
}

//---------------------------------------------------------------------------
// Parse a report descriptor, and update the property table with
// the IOHIDElement hierarchy discovered.

IOReturn IOHIDDevice::parseReportDescriptor( IOMemoryDescriptor * report,
                                             IOOptionBits         options )
{
    OSStatus             status;
    HIDPreparsedDataRef  parseData;
    void *               reportData;
    IOByteCount          reportLength;
    IOByteCount          segmentSize;
    IOReturn             ret;

    reportData   = report->getVirtualSegment(0, &segmentSize);
    reportLength = report->getLength();

    if ( segmentSize != reportLength )
    {
        reportData = IOMalloc( reportLength );
        if ( reportData == 0 )
            return kIOReturnNoMemory;

        report->readBytes( 0, reportData, reportLength );
    }

    // Parse the report descriptor.

    status = HIDOpenReportDescriptor(
                reportData,      /* report descriptor */
                reportLength,    /* report size in bytes */
                &parseData,      /* pre-parse data */
                0 );             /* flags */

    if ( segmentSize != reportLength )
    {
        IOFree( reportData, reportLength );
    }

    if ( status != kHIDSuccess )
    {
        return kIOReturnError;
    }

    // Create a hierarchy of IOHIDElement objects.

    ret = createElementHierarchy( parseData );

    getReportCountAndSizes( parseData );

    // Release memory.

    HIDCloseReportDescriptor( parseData );

    return ret;
}

//---------------------------------------------------------------------------
// Build the element hierarchy to describe the device capabilities to
// user-space.

IOReturn
IOHIDDevice::createElementHierarchy( HIDPreparsedDataRef parseData )
{
	OSStatus   status;
    HIDCapabilities    caps;
    IOReturn   ret = kIOReturnNoMemory;
    bool       success;

    do {    
        // Get a summary of device capabilities.

        status = HIDGetCapabilities( parseData, &caps );
        if ( status != kHIDSuccess )
        {
            ret = kIOReturnError;
            break;
        }

        // Dump HIDCapabilities structure contents.

        DLOG("Report bytes: input:%ld output:%ld feature:%ld\n",
             caps.inputReportByteLength,
             caps.outputReportByteLength,
             caps.featureReportByteLength);
        DLOG("Collections : %ld\n", caps.numberCollectionNodes);
        DLOG("Buttons     : input:%ld output:%ld feature:%ld\n",
             caps.numberInputButtonCaps,
             caps.numberOutputButtonCaps,
             caps.numberFeatureButtonCaps);
        DLOG("Values      : input:%ld output:%ld feature:%ld\n",
             caps.numberInputValueCaps,
             caps.numberOutputValueCaps,
             caps.numberFeatureValueCaps);        

        _maxInputReportSize    = caps.inputReportByteLength;
        _maxOutputReportSize   = caps.outputReportByteLength;
        _maxFeatureReportSize  = caps.featureReportByteLength;

        // Create an OSArray to store all HID elements.

        _elementArray = OSArray::withCapacity(
                                     caps.numberCollectionNodes   +
                                     caps.numberInputButtonCaps   +
                                     caps.numberInputValueCaps    +
                                     caps.numberOutputButtonCaps  +
                                     caps.numberOutputValueCaps   +
                                     caps.numberFeatureButtonCaps +
                                     caps.numberFeatureValueCaps  +
                                     10 );
        if ( _elementArray == 0 ) break;

        _elementArray->setCapacityIncrement(10);

        // Add collections to the element array.

        success = createCollectionElements(
                                  parseData,
                                  _elementArray,
                                  caps.numberCollectionNodes );
        if ( success != true )
            break;

        // Everything added to the element array from this point on
        // are "data" elements. We cache the starting index.

        _dataElementIndex = _elementArray->getCount();

        // Add input buttons to the element array.

        if ( !createButtonElements( parseData,
                                    _elementArray,
                                    kHIDInputReport,
                                    kIOHIDElementTypeInput_Button,
                                    caps.numberInputButtonCaps ) ) break;

        // Add output buttons to the element array.

        if ( !createButtonElements( parseData,
                                    _elementArray,
                                    kHIDOutputReport,
                                    kIOHIDElementTypeOutput,
                                    caps.numberOutputButtonCaps ) ) break;

        // Add feature buttons to the element array.
        
        if ( !createButtonElements( parseData,
                                    _elementArray,
                                    kHIDFeatureReport,
                                    kIOHIDElementTypeFeature,
                                    caps.numberFeatureButtonCaps ) ) break;

        // Add input values to the element array.

        if ( !createValueElements( parseData,
                                   _elementArray,
                                   kHIDInputReport,
                                   kIOHIDElementTypeInput_Misc,
                                   caps.numberInputValueCaps ) ) break;

        // Add output values to the element array.

        if ( !createValueElements( parseData,
                                   _elementArray,
                                   kHIDOutputReport,
                                   kIOHIDElementTypeOutput,
                                   caps.numberOutputValueCaps ) ) break;

        // Add feature values to the element array.
    
        if ( !createValueElements( parseData,
                                   _elementArray,
                                   kHIDFeatureReport,
                                   kIOHIDElementTypeFeature,
                                   caps.numberFeatureValueCaps ) ) break;

        // Create a memory to store current element values.

        _elementValuesDescriptor = createMemoryForElementValues();
        if ( _elementValuesDescriptor == 0 )
            break;

        // Element hierarchy has been built, add it to the property table.

        IOHIDElement * root = (IOHIDElement *) _elementArray->getObject( 0 );
        if ( root )
        {
            setProperty( kIOHIDElementKey, root->getChildArray() );
        }

        ret = kIOReturnSuccess;
    }
    while ( false );

    return ret;
}

//---------------------------------------------------------------------------
// Fetch the total number of reports and the size of each report.

bool IOHIDDevice::getReportCountAndSizes( HIDPreparsedDataRef parseData )
{
    HIDPreparsedDataPtr data   = (HIDPreparsedDataPtr) parseData;
    HIDReportSizes *    report = data->reports;

    _reportCount = data->reportCount;

    DLOG("Report count: %ld\n", _reportCount);
    
    for ( UInt32 num = 0; num < data->reportCount; num++, report++ )
    {

        DLOG("Report ID: %ld input:%ld output:%ld feature:%ld\n",
             report->reportID,
             report->inputBitCount,
             report->outputBitCount,
             report->featureBitCount);
        
        setReportSize( report->reportID,
                       kIOHIDReportTypeInput,
                       report->inputBitCount );
        
        setReportSize( report->reportID,
                       kIOHIDReportTypeOutput,
                       report->outputBitCount );

        setReportSize( report->reportID,
                       kIOHIDReportTypeFeature,
                       report->featureBitCount );
    }
    
    return true;
}

//---------------------------------------------------------------------------
// Set the report size for the first element in the report handler chain.

bool IOHIDDevice::setReportSize( UInt8           reportID,
                                 IOHIDReportType reportType,
                                 UInt32          numberOfBits )
{
    IOHIDElement * element;
    bool           ret = false;
    
    element = GetHeadElement( GetReportHandlerSlot(reportID), reportType );
    
    while ( element )
    {
        if ( element->getReportID() == reportID )
        {
            element->setReportSize( numberOfBits );
            ret = true;
            break;
        }
        element = element->getNextReportHandler();
    }
    return ret;
}

//---------------------------------------------------------------------------
// Add collection elements to the OSArray object provided.

bool
IOHIDDevice::createCollectionElements( HIDPreparsedDataRef parseData,
                                       OSArray *           array,
                                       UInt32              maxCount )
{
    OSStatus              status;
    HIDCollectionNodePtr  collections;
    UInt32                count = maxCount;
    bool                  ret   = false;
    UInt32                index;

    do {
        // Allocate memory to fetch all collections from the parseData.

        collections = (HIDCollectionNodePtr)
                      IOMalloc( maxCount * sizeof(HIDCollectionNode) );

        if ( collections == 0 ) break;

        status = HIDGetCollectionNodes(
                    collections,    /* collectionNodes     */
                    &count,         /* collectionNodesSize */
                    parseData );    /* preparsedDataRef    */

        if ( status != kHIDSuccess ) break;

        // Create an IOHIDElement for each collection.

        for ( index = 0; index < count; index++ )
        {
            IOHIDElement * element;

            element = IOHIDElement::collectionElement(
                                              this,
                                              kIOHIDElementTypeCollection,
                                              &collections[index] );
            if ( element == 0 ) break;

            element->release();
        }
        if ( index < count ) break;

        // Create linkage for the collection hierarchy.
        // Starts at 1 to skip the root (virtual) collection.

        for ( index = 1; index < count; index++ )
        {
            if ( !linkToParent( array, collections[index].parent, index ) )
                break;
        }
        if ( index < count ) break;

        ret = true;
    }
    while ( false );

    if ( collections )
        IOFree( collections, maxCount * sizeof(HIDCollectionNode) );

    return ret;
}

//---------------------------------------------------------------------------
// Link an element in the array to another element in the array as its child.

bool IOHIDDevice::linkToParent( const OSArray * array,
                                UInt32          parentIndex,
                                UInt32          childIndex )
{
    IOHIDElement * child  = (IOHIDElement *) array->getObject( childIndex );
    IOHIDElement * parent = (IOHIDElement *) array->getObject( parentIndex );

    return ( parent ) ? parent->addChildElement( child ) : false;
}

//---------------------------------------------------------------------------
// Add Button elements (1 bit value) to the collection.

bool IOHIDDevice::createButtonElements( HIDPreparsedDataRef parseData,
                                        OSArray *           array,
                                        UInt32              hidReportType,
                                        IOHIDElementType    elementType,
                                        UInt32              maxCount )
{
    OSStatus          		status;
    HIDButtonCapabilitiesPtr 	buttons = 0;
    UInt32			count   = maxCount;
    bool			ret     = false;
    IOHIDElement *		element;
    IOHIDElement *		parent;

    do {
        if ( maxCount == 0 )
        {
            ret = true;
            break;
        }
        
        // Allocate memory to fetch all button elements from the parseData.

        buttons = (HIDButtonCapabilitiesPtr) IOMalloc( maxCount *
                                               sizeof(HIDButtonCapabilities) );
        if ( buttons == 0 ) break;

        status = HIDGetButtonCapabilities( hidReportType,  /* HIDReportType    */
                                   buttons,        /* buttonCaps       */
                                   &count,         /* buttonCapsSize   */
                                   parseData );    /* preparsedDataRef */

        if ( status != kHIDSuccess ) break;

        // Create an IOHIDElement for each button and link it to its
        // parent collection.

        ret = true;

        for ( UInt32 i = 0; i < count; i++ )
        {            
            parent  = (IOHIDElement *) array->getObject(
                                              buttons[i].collection );

            element = IOHIDElement::buttonElement(
                                          this,
                                          elementType,
                                          &buttons[i],
                                          parent );
            if ( element == 0 )
            {
                ret = false;
                break;
            }
            element->release();
        }
    }
    while ( false );

    if ( buttons )
        IOFree( buttons, maxCount * sizeof(HIDButtonCapabilities) );
    
    return ret;
}

//---------------------------------------------------------------------------
// Add Value elements to the collection.

bool IOHIDDevice::createValueElements( HIDPreparsedDataRef parseData,
                                       OSArray *           array,
                                       UInt32              hidReportType,
                                       IOHIDElementType    elementType,
                                       UInt32              maxCount )
{
    OSStatus         status;
    HIDValueCapabilitiesPtr  values = 0;
    UInt32           count  = maxCount;
    bool             ret    = false;
    IOHIDElement *   element;
    IOHIDElement *   parent;

    do {
        if ( maxCount == 0 )
        {
            ret = true;
            break;
        }

        // Allocate memory to fetch all value elements from the parseData.

        values = (HIDValueCapabilitiesPtr) IOMalloc( maxCount *
                                             sizeof(HIDValueCapabilities) );
        if ( values == 0 ) break;

        status = HIDGetValueCapabilities( hidReportType,  /* HIDReportType    */
                                  values,         /* valueCaps        */
                                  &count,         /* valueCapsSize    */
                                  parseData );    /* preparsedDataRef */

        if ( status != kHIDSuccess ) break;

        // Create an IOHIDElement for each value and link it to its
        // parent collection.

        ret = true;

        for ( UInt32 i = 0; i < count; i++ )
        {
            parent  = (IOHIDElement *) array->getObject(
                                              values[i].collection );

            element = IOHIDElement::valueElement(
                                         this,
                                         elementType,
                                         &values[i],
                                         parent );

            if ( element == 0 )
            {
                ret = false;
                break;
            }
            element->release();
        }
    }
    while ( false );

    if ( values )
        IOFree( values, maxCount * sizeof(HIDValueCapabilities) );
    
    return ret;
}

//---------------------------------------------------------------------------
// Called by an IOHIDElement to register itself.

bool IOHIDDevice::registerElement( IOHIDElement *       element,
                                   IOHIDElementCookie * cookie )
{
    IOHIDReportType reportType;
    UInt32          index = _elementArray->getCount();

    // Add the element to the elements array.

    if ( _elementArray->setObject( index, element ) != true )
    {
        return false;
    }

    // If the element can contribute to an Input, Output, or Feature
    // report, then add it to the chain of report handlers.

    if ( element->getReportType( &reportType ) )
    {
        IOHIDReportHandler * reportHandler;
        UInt32               slot;

        slot = GetReportHandlerSlot( element->getReportID() );

        reportHandler = &_reportHandlers[slot];

        if ( reportHandler->head[reportType] )
        {
            element->setNextReportHandler( reportHandler->head[reportType] );
        }
        reportHandler->head[reportType] = element;
    }

    // The cookie returned is simply an index to the element in the
    // elements array. We may decide to obfuscate it later on.

    *cookie = (IOHIDElementCookie) index;

    return true;
}

//---------------------------------------------------------------------------
// Create a buffer memory descriptor, and divide the memory buffer
// for each data element.

IOBufferMemoryDescriptor * IOHIDDevice::createMemoryForElementValues()
{
    IOBufferMemoryDescriptor * descriptor;
    IOHIDElement *             element;
    UInt32                     capacity = 0;
    UInt8 *                    start;
    UInt8 *                    buffer;

    // Discover the amount of memory required to publish the
    // element values for all "data" elements.

    for ( UInt32 slot = 0; slot < kReportHandlerSlots; slot++ )
    {
        for ( UInt32 type = 0; type < kIOHIDReportTypeCount; type++ )
        {
            element = GetHeadElement(slot, type);
            while ( element )
            {
                capacity += element->getElementValueSize();
                element   = element->getNextReportHandler();
            }
        }
    }

    // Allocate an IOBufferMemoryDescriptor object.

	DLOG("Element value capacity %ld\n", capacity);

    descriptor = IOBufferMemoryDescriptor::withOptions(
                   kIOMemorySharingTypeMask,
                   capacity );

    if ( ( descriptor == 0 ) || ( descriptor->getBytesNoCopy() == 0 ) )
    {
        if ( descriptor ) descriptor->release();
        return 0;
    }

    // Now assign the update memory area for each report element.

    start = buffer = (UInt8 *) descriptor->getBytesNoCopy();

    for ( UInt32 slot = 0; slot < kReportHandlerSlots; slot++ )
    {
        for ( UInt32 type = 0; type < kIOHIDReportTypeCount; type++ )
        {
            element = GetHeadElement(slot, type);
            while ( element )
            {
                assert ( buffer < (start + capacity) );
            
                element->setMemoryForElementValue( (IOVirtualAddress) buffer,
                                                (void *) (buffer - start) );
    
                buffer += element->getElementValueSize();
                element = element->getNextReportHandler();
            }
        }
    }

    return descriptor;
}

//---------------------------------------------------------------------------
// Get a reference to the memory descriptor created by
// createMemoryForElementValues().

IOMemoryDescriptor * IOHIDDevice::getMemoryWithCurrentElementValues() const
{
    return _elementValuesDescriptor;
}

//---------------------------------------------------------------------------
// Start delivering events from the given element to the specified
// event queue.

IOReturn IOHIDDevice::startEventDelivery( IOHIDEventQueue *  queue,
                                          IOHIDElementCookie cookie,
                                          IOOptionBits       options )
{
    IOHIDElement * element;
    UInt32         elementIndex = (UInt32) cookie;
    IOReturn       ret = kIOReturnBadArgument;

    if ( ( queue == 0 ) || ( elementIndex < _dataElementIndex ) )
        return kIOReturnBadArgument;

    ELEMENT_LOCK;

	do {
        if (( element = GetElement(elementIndex) ) == 0)
            break;
        
        ret = element->addEventQueue( queue ) ?
              kIOReturnSuccess : kIOReturnNoMemory;
    }
    while ( false );

    ELEMENT_UNLOCK;
    
    return ret;
}

//---------------------------------------------------------------------------
// Stop delivering events from the given element to the specified
// event queue.

IOReturn IOHIDDevice::stopEventDelivery( IOHIDEventQueue *  queue,
                                         IOHIDElementCookie cookie )
{
    IOHIDElement * element;
    UInt32         elementIndex = (UInt32) cookie;
    bool           removed      = false;

    // If the cookie provided was zero, then loop and remove the queue
    // from all elements.

    if ( elementIndex == 0 )
        elementIndex = _dataElementIndex;
	else if ( (queue == 0 ) || ( elementIndex < _dataElementIndex ) )
        return kIOReturnBadArgument;

    ELEMENT_LOCK;

	do {
        if (( element = GetElement(elementIndex++) ) == 0)
            break;

        removed = element->removeEventQueue( queue ) || removed;
    }
    while ( cookie == 0 );

    ELEMENT_UNLOCK;
    
    return removed ? kIOReturnSuccess : kIOReturnNotFound;
}

//---------------------------------------------------------------------------
// Check whether events from the given element will be delivered to
// the specified event queue.

IOReturn IOHIDDevice::checkEventDelivery( IOHIDEventQueue *  queue,
                                          IOHIDElementCookie cookie,
                                          bool *             started )
{
    IOHIDElement * element = GetElement( cookie );

    if ( !queue || !element || !started )
        return kIOReturnBadArgument;

    ELEMENT_LOCK;

    *started = element->hasEventQueue( queue );

    ELEMENT_UNLOCK;
    
    return kIOReturnSuccess;
}

#define SetCookiesTransactionState(element, cookies, count, state, index, offset) \
    for (index = offset; index < count; index++) { 			\
        element = GetElement(cookies[index]); 				\
        if (element == NULL) 						\
            continue; 							\
        element->setTransactionState (state);				\
    }

//---------------------------------------------------------------------------
// Update the value of the given element, by getting a report from
// the device.  Assume that the cookieCount > 0

OSMetaClassDefineReservedUsed(IOHIDDevice,  0);
IOReturn IOHIDDevice::updateElementValues(IOHIDElementCookie *cookies, UInt32 cookieCount) {
    IOMemoryDescriptor *	report = NULL;
    IOHIDElement *		element = NULL;
    IOHIDReportType		reportType;
    IOByteCount			maxReportLength;
    UInt8			reportID;
    UInt32			index;
    IOReturn			ret = kIOReturnError;
    
    ELEMENT_LOCK;
    
    SetCookiesTransactionState(element, cookies, 
            cookieCount, kIOHIDTransactionStatePending, index, 0);
            
    ELEMENT_UNLOCK;
    
    maxReportLength = max(_maxOutputReportSize, 
                            max(_maxFeatureReportSize, _maxInputReportSize));
    
    // Allocate a mem descriptor with the maxReportLength.
    // This way, we only have to allocate one mem discriptor
    report = IOBufferMemoryDescriptor::withCapacity(
                            maxReportLength, kIODirectionIn);
        
    if (report == NULL) {
        ret = kIOReturnNoMemory;
        goto UPDATE_ELEMENT_CLEANUP;
    }

    // Iterate though all the elements in the 
    // transaction.  Generate reports if needed.
    for (index = 0; index < cookieCount; index++) {
        element = GetElement(cookies[index]);
        
        if (element == NULL)
            continue;
            
        if ( element->getTransactionState() 
                != kIOHIDTransactionStatePending )
            continue;
                        
        if ( !element->getReportType(&reportType) )
            continue;

        reportID = element->getReportID();
        
        ret = getReport(report, reportType, reportID);
    
        if (ret != kIOReturnSuccess)
            break;
            
        // If we have a valid report, go ahead and process it.
        ret = handleReport(report, reportType);
        
        if (ret != kIOReturnSuccess)
            break;
    }
    
    // release the report
    report->release();

UPDATE_ELEMENT_CLEANUP:

    ELEMENT_LOCK;
    // If needed, set the transaction state for the 
    // remaining elements to idle.
    SetCookiesTransactionState(element, cookies, 
            cookieCount, kIOHIDTransactionStateIdle, index, index);
    ELEMENT_UNLOCK;
        
    return ret;
}

//---------------------------------------------------------------------------
// Post the value of the given element, by sending a report to
// the device.  Assume that the cookieCount > 0
OSMetaClassDefineReservedUsed(IOHIDDevice,  1);
IOReturn IOHIDDevice::postElementValues(IOHIDElementCookie * cookies, UInt32 cookieCount) {
    
    OSArray			*pendingReports = NULL;
    IOBufferMemoryDescriptor	*report = NULL;
    IOHIDElement 		*element = NULL;
    IOHIDElement 		*cookieElement = NULL;
    UInt8			*reportData = NULL;
    IOByteCount			maxReportLength = 0;
    IOByteCount			reportLength = 0;
    IOHIDReportType		reportType;
    UInt8			reportID;
    UInt32 			index;
    IOReturn			ret = kIOReturnError;
    
    // Return an error if no cookies are being set
    if (cookieCount == 0)
        return ret;
        
    ELEMENT_LOCK;
    
    // Set the transaction state on the specified cookies
    SetCookiesTransactionState(cookieElement, cookies, 
            cookieCount, kIOHIDTransactionStatePending, index, 0);
    
    // Most times transaction will consist of items in one report
    pendingReports = OSArray::withCapacity(1);
    
    if ( pendingReports == NULL ) {
        ret = kIOReturnNoMemory;
        goto POST_ELEMENT_CLEANUP;
    }
        
    // Get the max report size
    maxReportLength = max(_maxOutputReportSize, _maxFeatureReportSize);
 
    // Iterate though all the elements in the 
    // transaction.  Generate reports if needed. 
    for (index = 0; index < cookieCount; index ++) {

        cookieElement = GetElement(cookies[index]);
    
        if ( cookieElement == NULL )
            continue;
          
        // Continue on to the next element if 
        // we've already processed this one
        if ( cookieElement->getTransactionState() 
                != kIOHIDTransactionStatePending )
            continue;
            
        if ( !cookieElement->getReportType(&reportType) )
            continue;

        
        // Allocate a contiguous mem descriptor with the maxReportLength.
        // This way, we only have to allocate one mem buffer.
        report = IOBufferMemoryDescriptor::withCapacity(maxReportLength, kIODirectionOutIn, true);
        
        if ( report == NULL ) {
            ret = kIOReturnNoMemory;
            goto POST_ELEMENT_CLEANUP;
        }
            
        // Obtain the buffer
        reportData = (UInt8 *)report->getBytesNoCopy();
                
        reportID = cookieElement->getReportID();

        // Start at the head element and iterate through
        element = GetHeadElement(GetReportHandlerSlot(reportID), reportType);
                
        while ( element ) {
            
            element->createReport(reportID, reportData, &reportLength, &element);
            
            // If the reportLength was set, then this is
            // the head element for this report
            if ( reportLength ) {
                report->setLength(reportLength);
                reportLength = 0;
            }
                
        }
        
        // If there are multiple reports, append
        // the reportID to the first byte
        if ( _reportCount > 1 ) 
            reportData[0] = reportID;
                  
          
        // Add the new report to the array of pending reports
        // It will be sent to the device after the elementLock
        // has been released
        pendingReports->setObject(report);
        report->release();
    }

POST_ELEMENT_CLEANUP:
    // If needed, set the transaction state for the 
    // remaining elements to idle.
    SetCookiesTransactionState(cookieElement, cookies, 
            cookieCount, kIOHIDTransactionStateIdle, index, index);
    
    ELEMENT_UNLOCK;
    
    // Now that we have formulated all the reports for this transaction,
    // let's go ahead and post them to the device.
    for (index = 0; index < pendingReports->getCount(); index++) {
        report = (IOBufferMemoryDescriptor *)(pendingReports->getObject(index));
        
        if (report == NULL)
            continue;
        
        // Send the report to the device
        ret = setReport( report, reportType, reportID);
        
        if ( ret != kIOReturnSuccess )
            break;
    }

    pendingReports->release();

    return ret;
}

OSMetaClassDefineReservedUsed(IOHIDDevice,  2);
OSString * IOHIDDevice::newSerialNumberString() const
{
    return 0;
}

OSMetaClassDefineReservedUsed(IOHIDDevice,  3);
OSNumber * IOHIDDevice::newLocationIDNumber() const
{
    return 0;
}

OSMetaClassDefineReservedUnused(IOHIDDevice,  4);
OSMetaClassDefineReservedUnused(IOHIDDevice,  5);
OSMetaClassDefineReservedUnused(IOHIDDevice,  6);
OSMetaClassDefineReservedUnused(IOHIDDevice,  7);
OSMetaClassDefineReservedUnused(IOHIDDevice,  8);
OSMetaClassDefineReservedUnused(IOHIDDevice,  9);
OSMetaClassDefineReservedUnused(IOHIDDevice, 10);
OSMetaClassDefineReservedUnused(IOHIDDevice, 11);
OSMetaClassDefineReservedUnused(IOHIDDevice, 12);
OSMetaClassDefineReservedUnused(IOHIDDevice, 13);
OSMetaClassDefineReservedUnused(IOHIDDevice, 14);
OSMetaClassDefineReservedUnused(IOHIDDevice, 15);
OSMetaClassDefineReservedUnused(IOHIDDevice, 16);
OSMetaClassDefineReservedUnused(IOHIDDevice, 17);
OSMetaClassDefineReservedUnused(IOHIDDevice, 18);
OSMetaClassDefineReservedUnused(IOHIDDevice, 19);
OSMetaClassDefineReservedUnused(IOHIDDevice, 20);
OSMetaClassDefineReservedUnused(IOHIDDevice, 21);
OSMetaClassDefineReservedUnused(IOHIDDevice, 22);
OSMetaClassDefineReservedUnused(IOHIDDevice, 23);
OSMetaClassDefineReservedUnused(IOHIDDevice, 24);
OSMetaClassDefineReservedUnused(IOHIDDevice, 25);
OSMetaClassDefineReservedUnused(IOHIDDevice, 26);
OSMetaClassDefineReservedUnused(IOHIDDevice, 27);
OSMetaClassDefineReservedUnused(IOHIDDevice, 28);
OSMetaClassDefineReservedUnused(IOHIDDevice, 29);
OSMetaClassDefineReservedUnused(IOHIDDevice, 30);
OSMetaClassDefineReservedUnused(IOHIDDevice, 31);