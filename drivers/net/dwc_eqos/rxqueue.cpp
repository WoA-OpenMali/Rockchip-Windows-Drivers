#include "precomp.h"
#include "rxqueue.h"
#include "queue_common.h"
#include "device.h"
#include "registers.h"
#include "trace.h"

static_assert(sizeof(RxDescriptor) == QueueDescriptorSize);

struct RxQueueContext
{
    ChannelRegisters* channelRegs;

    NETADAPTER adapter;
    NET_RING* packetRing;
    NET_RING* fragmentRing;
    WDFCOMMONBUFFER descBuffer;
    RxDescriptor* descVirtual;
    PHYSICAL_ADDRESS descPhysical;
    NET_EXTENSION fragmentLogical;
    UINT32 descCount;   // A power of 2 between QueueDescriptorMinCount and QueueDescriptorMaxCount.
    bool running;

    UINT32 descBegin;
    UINT32 descEnd;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(RxQueueContext, RxQueueGetContext)

// Gets channelRegs->RxCurrentAppDesc, converted to an index.
static UINT32
GetDescNext(_In_ RxQueueContext* context)
{
    // DISPATCH_LEVEL
    auto const current = Read32(&context->channelRegs->RxCurrentAppDesc);
    return QueueDescriptorAddressToIndex(current, context->descPhysical, context->descCount);
}

// Sets channelRegs->RxDescTailPointer to the physical address of the descriptor at the given index.
static void
SetDescEnd(
    _In_ RxQueueContext* context,
    _In_ UINT32 index)
{
    // DISPATCH_LEVEL
    NT_ASSERT(index < context->descCount);
    UINT32 const offset = index * sizeof(RxDescriptor);
    Write32(&context->channelRegs->RxDescTailPointer, context->descPhysical.LowPart + offset);
    context->descEnd = index;
}

static EVT_PACKET_QUEUE_START RxQueueStart;
static void
RxQueueStart(_In_ NETPACKETQUEUE queue)
{
    // PASSIVE_LEVEL, nonpaged (resume path)
    auto const context = RxQueueGetContext(queue);

    context->running = true;
    context->descBegin = 0;
    context->descEnd = 0;

    Write32(&context->channelRegs->RxDescListAddressHigh, context->descPhysical.HighPart);
    Write32(&context->channelRegs->RxDescListAddress, context->descPhysical.LowPart);
    Write32(&context->channelRegs->RxDescRingLength, context->descCount - 1);
    SetDescEnd(context, context->descEnd);

#if DBG
    auto const descNext = GetDescNext(context);
    NT_ASSERT(descNext == context->descEnd); // If this fires, we need more reset logic.
#endif

    ChannelRxControl_t rxControl = {};
    rxControl.Start = true;
    rxControl.ReceiveBufferSize = RxBufferSize;
    rxControl.RxPbl = QueueBurstLength;
    Write32(&context->channelRegs->RxControl, rxControl);

    TraceEntryExit(RxQueueStart, LEVEL_INFO);
}

static EVT_PACKET_QUEUE_ADVANCE RxQueueAdvance;
static void
RxQueueAdvance(_In_ NETPACKETQUEUE queue)
{
    // DISPATCH_LEVEL
    auto const context = RxQueueGetContext(queue);
    auto const pktEnd = context->packetRing->EndIndex;
    auto const fragEnd = context->fragmentRing->EndIndex;
    auto const descMask = context->descCount - 1u;
    UINT32 descIndex, pktIndex, fragIndex;
    UINT32 donePkts = 0, queuedPkts = 0;

    /*
    Fragment indexes:
    [fragBegin] old [fragNext] new [fragEnd] owned by NetAdapterCx [fragBegin]

    Descriptor indexes:
    [descBegin] RECEIVED [descNext] RECEIVING [descEnd] EMPTY [descBegin-1]
    */

    // Indicate received packets.

    pktIndex = context->packetRing->BeginIndex;
    fragIndex = context->fragmentRing->BeginIndex;

    auto const descNext = GetDescNext(context);
    for (descIndex = context->descBegin; descIndex != descNext; descIndex = (descIndex + 1) & descMask)
    {
        NT_ASSERT(fragIndex != fragEnd);
        if (pktIndex == pktEnd || fragIndex == fragEnd)
        {
            break;
        }

        auto const& desc = context->descVirtual[descIndex];
        auto const descWrite = desc.Write;

        NT_ASSERT(!descWrite.Own);
        if (descWrite.Own)
        {
            // I've never seen this happen, but if it does, we need to stop here.
            TraceWrite("RxQueueAdvance-own", LEVEL_WARNING,
                TraceLoggingUInt32(descIndex, "descIndex"),
                TraceLoggingHexInt32(reinterpret_cast<UINT32 const*>(&descWrite)[3], "RDES3"));
            break;
        }

        NT_ASSERT(descWrite.FragmentIndex == fragIndex);

        auto const pkt = NetRingGetPacketAtIndex(context->packetRing, pktIndex);
        pkt->FragmentIndex = fragIndex;
        pkt->FragmentCount = 1;
        pkt->Layout = {};

        auto const frag = NetRingGetFragmentAtIndex(context->fragmentRing, fragIndex);
        frag->Offset = 0;
        NT_ASSERT(RxBufferSize <= frag->Capacity);

        if (descWrite.ErrorSummary ||
            descWrite.ContextType ||
            !descWrite.FirstDescriptor ||
            !descWrite.LastDescriptor) // TODO: Jumbo frames (multi-descriptor packets)
        {
            if (descWrite.ErrorSummary)
            {
                TraceWrite("RxQueueAdvance-Dropped", LEVEL_INFO,
                    TraceLoggingUInt32(descIndex, "descIndex"),
                    TraceLoggingHexInt32(reinterpret_cast<UINT32 const*>(&descWrite)[3], "RDES3"));
            }
            else
            {
                TraceWrite("RxQueueAdvance-Unexpected", LEVEL_WARNING,
                    TraceLoggingUInt32(descIndex, "descIndex"),
                    TraceLoggingHexInt32(reinterpret_cast<UINT32 const*>(&descWrite)[3], "RDES3"));
            }

            pkt->Ignore = true;
            frag->ValidLength = 0;
        }
        else
        {
            NT_ASSERT(descWrite.PacketLength >= 4); // PacketLength includes CRC
            frag->ValidLength = descWrite.PacketLength - 4;
        }

        pktIndex = NetRingIncrementIndex(context->packetRing, pktIndex);
        fragIndex = NetRingIncrementIndex(context->fragmentRing, fragIndex);
        donePkts += 1;
    }

    context->descBegin = descIndex;
    context->packetRing->BeginIndex = pktIndex;
    context->fragmentRing->BeginIndex = fragIndex;

    if (context->running)
    {
        // Prepare more descriptors.

        fragIndex = context->fragmentRing->NextIndex;

        auto const descFull = (context->descBegin - 1u) & descMask;
        for (descIndex = context->descEnd; descIndex != descFull; descIndex = (descIndex + 1) & descMask)
        {
            if (fragIndex == fragEnd)
            {
                break;
            }

            NT_ASSERT(RxBufferSize <= NetRingGetFragmentAtIndex(context->fragmentRing, fragIndex)->Capacity);
            auto const fragLogicalAddress = NetExtensionGetFragmentLogicalAddress(&context->fragmentLogical, fragIndex)->LogicalAddress;

            RxDescriptorRead descRead = {};
            descRead.Buf1ApLow = fragLogicalAddress & 0xFFFFFFFF;
            descRead.Buf1ApHigh = fragLogicalAddress >> 32;
            descRead.Buf1Valid = true;
            descRead.InterruptOnCompletion = true;
            descRead.Own = true;
            descRead.FragmentIndex = fragIndex;

            context->descVirtual[descIndex].Read = descRead;

            fragIndex = NetRingIncrementIndex(context->fragmentRing, fragIndex);
            queuedPkts += 1;
        }

        if (descIndex != context->descEnd)
        {
            SetDescEnd(context, descIndex);
            context->fragmentRing->NextIndex = fragIndex;
        }
    }
    else if (descIndex == descNext)
    {
        while (pktIndex != pktEnd)
        {
            auto const pkt = NetRingGetPacketAtIndex(context->packetRing, pktIndex);
            pkt->Ignore = true;
            pktIndex = NetRingIncrementIndex(context->packetRing, pktIndex);
        }

        context->packetRing->BeginIndex = pktEnd;
        context->fragmentRing->BeginIndex = fragEnd;
    }

    TraceEntryExit(RxQueueAdvance, LEVEL_VERBOSE,
        TraceLoggingUInt32(donePkts),
        TraceLoggingUInt32(queuedPkts));
}

static EVT_PACKET_QUEUE_SET_NOTIFICATION_ENABLED RxQueueSetNotificationEnabled;
static void
RxQueueSetNotificationEnabled(
    _In_ NETPACKETQUEUE queue,
    _In_ BOOLEAN notificationEnabled)
{
    // PASSIVE_LEVEL, nonpaged (resume path)
    auto const context = RxQueueGetContext(queue);
    DeviceSetNotificationRxQueue(context->adapter, notificationEnabled ? queue : nullptr);
    TraceEntryExit(RxQueueSetNotificationEnabled, LEVEL_VERBOSE,
        TraceLoggingBoolean(notificationEnabled, "enabled"));
}

static EVT_PACKET_QUEUE_CANCEL RxQueueCancel;
static void
RxQueueCancel(_In_ NETPACKETQUEUE queue)
{
    // DISPATCH_LEVEL (verifier says so)
    auto const context = RxQueueGetContext(queue);

    // Shut down and flush. RxQueueAdvance should return the remaining packets.

    context->running = false;

    ChannelRxControl_t rxControl = {};
    rxControl.Start = false;
    rxControl.RxPacketFlush = true;
    Write32(&context->channelRegs->RxControl, rxControl);

    TraceEntryExit(RxQueueCancel, LEVEL_INFO);
}

static EVT_PACKET_QUEUE_STOP RxQueueStop;
__declspec(code_seg("PAGE"))
static void
RxQueueStop(_In_ NETPACKETQUEUE queue)
{
    // PASSIVE_LEVEL
    PAGED_CODE();
    auto const context = RxQueueGetContext(queue);

    DeviceSetNotificationRxQueue(context->adapter, nullptr);

    TraceEntryExit(RxQueueStop, LEVEL_INFO);
}

static EVT_WDF_OBJECT_CONTEXT_CLEANUP RxQueueCleanup;
static void
RxQueueCleanup(_In_ WDFOBJECT queue)
{
    // DISPATCH_LEVEL
    auto context = RxQueueGetContext(queue);
    if (context->descBuffer)
    {
        WdfObjectDelete(context->descBuffer);
        context->descBuffer = nullptr;
    }

    TraceEntryExit(RxQueueCleanup, LEVEL_VERBOSE);
}

_Use_decl_annotations_ NTSTATUS
RxQueueCreate(
    NETADAPTER adapter,
    NETRXQUEUE_INIT* queueInit,
    WDFDMAENABLER dma,
    ChannelRegisters* channelRegs)
{
    // PASSIVE_LEVEL, nonpaged (resume path)
    NTSTATUS status;
    NETPACKETQUEUE queue = nullptr;

    // Create queue.

    {
        NET_PACKET_QUEUE_CONFIG config;
        NET_PACKET_QUEUE_CONFIG_INIT(&config, RxQueueAdvance, RxQueueSetNotificationEnabled, RxQueueCancel);
        config.EvtStart = RxQueueStart;
        config.EvtStop = RxQueueStop;

        WDF_OBJECT_ATTRIBUTES attributes;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, RxQueueContext);
        attributes.EvtCleanupCallback = RxQueueCleanup;

        status = NetRxQueueCreate(queueInit, &attributes, &config, &queue);
        if (!NT_SUCCESS(status))
        {
            TraceWrite("NetRxQueueCreate-failed", LEVEL_ERROR,
                TraceLoggingNTStatus(status));
            goto Done;
        }
    }

    // Configure queue.

    {
        auto const rings = NetRxQueueGetRingCollection(queue);

        auto const context = RxQueueGetContext(queue);
        context->channelRegs = channelRegs;

        context->adapter = adapter;
        context->packetRing = NetRingCollectionGetPacketRing(rings);
        context->fragmentRing = NetRingCollectionGetFragmentRing(rings);
        context->descCount = QueueDescriptorCount(context->fragmentRing->NumberOfElements);

        TraceWrite("RxQueueCreate-size", LEVEL_VERBOSE,
            TraceLoggingHexInt32(context->packetRing->NumberOfElements, "packets"),
            TraceLoggingHexInt32(context->fragmentRing->NumberOfElements, "fragments"),
            TraceLoggingHexInt32(context->descCount, "descriptors"));

        WDF_COMMON_BUFFER_CONFIG bufferConfig;
        WDF_COMMON_BUFFER_CONFIG_INIT(&bufferConfig, QueueDescriptorAlignment - 1);
        status = WdfCommonBufferCreateWithConfig(
            dma,
            sizeof(RxDescriptor) * context->descCount,
            &bufferConfig,
            WDF_NO_OBJECT_ATTRIBUTES,
            &context->descBuffer);
        if (!NT_SUCCESS(status))
        {
            TraceWrite("WdfCommonBufferCreate-failed", LEVEL_ERROR,
                TraceLoggingNTStatus(status));
            goto Done;
        }

        context->descVirtual = static_cast<RxDescriptor*>(
            WdfCommonBufferGetAlignedVirtualAddress(context->descBuffer));
        memset(context->descVirtual, 0, sizeof(RxDescriptor) * context->descCount);
        context->descPhysical =
            WdfCommonBufferGetAlignedLogicalAddress(context->descBuffer);
        TraceWrite("RxQueueCreate-desc", LEVEL_VERBOSE,
            TraceLoggingHexInt64(context->descPhysical.QuadPart, "physical"),
            TraceLoggingPointer(context->descVirtual, "virtual"));

        NET_EXTENSION_QUERY query;
        NET_EXTENSION_QUERY_INIT(&query,
            NET_FRAGMENT_EXTENSION_LOGICAL_ADDRESS_NAME,
            NET_FRAGMENT_EXTENSION_LOGICAL_ADDRESS_VERSION_1,
            NetExtensionTypeFragment);
        NetRxQueueGetExtension(queue, &query, &context->fragmentLogical);
    }

    status = STATUS_SUCCESS;

Done:

    TraceEntryExitWithStatus(RxQueueCreate, LEVEL_INFO, status);
    return status;
}