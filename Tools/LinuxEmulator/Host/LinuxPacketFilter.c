/** @file
  Rootless Linux EMU_SNP_PROTOCOL backend using libslirp.

  The firmware still uses its normal SNP, IP, TCP, HTTP and Redfish drivers.
  Only Ethernet frames cross this host boundary. No TAP device, packet socket,
  host interface configuration, elevated privilege or background thread is used.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "Host.h"

#ifndef __APPLE__

  #include <libslirp.h>
  #include <stddef.h>

#define EMU_SNP_PRIVATE_SIGNATURE  SIGNATURE_32 ('E', 'M', 's', 'l')
#define SNP_HEADER_SIZE            14
#define SNP_MTU                    1500
#define SNP_FRAME_SIZE             (SNP_HEADER_SIZE + SNP_MTU)
#define SNP_QUEUE_SIZE             64
#define SNP_POLL_SIZE              256
#define SNP_TIMER_LIMIT            16
#define SNP_FILTERS                (EFI_SIMPLE_NETWORK_RECEIVE_UNICAST | \
                                   EFI_SIMPLE_NETWORK_RECEIVE_MULTICAST | \
                                   EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST | \
                                   EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS | \
                                   EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS_MULTICAST)

typedef struct SNP_TIMER SNP_TIMER;
struct SNP_TIMER {
  SNP_TIMER       *Next;
  SlirpTimerCb    Callback;
  VOID            *CallbackContext;
  int64_t         ExpiresMs;
  BOOLEAN         Armed;
};

typedef struct {
  UINTN    Length;
  UINT8    Data[SNP_FRAME_SIZE];
} SNP_PACKET;

typedef struct {
  UINTN                      Signature;
  EMU_IO_THUNK_PROTOCOL      *Thunk;
  EMU_SNP_PROTOCOL           EmuSnp;
  EFI_SIMPLE_NETWORK_MODE    *Mode;
  Slirp                      *Slirp;
  SNP_TIMER                  *Timers;
  UINTN                      TimerCount;
  struct pollfd              PollFds[SNP_POLL_SIZE];
  UINTN                      PollCount;
  SNP_PACKET                 Rx[SNP_QUEUE_SIZE];
  UINTN                      RxHead;
  UINTN                      RxCount;
  VOID                       *Tx[SNP_QUEUE_SIZE];
  UINTN                      TxHead;
  UINTN                      TxCount;
  UINT32                     InterruptStatus;
  EFI_NETWORK_STATISTICS     Statistics;
} EMU_SNP_PRIVATE;

/* SIGALRM is the EmulatorPkg firmware timer interrupt. Keep the host library
   and queue operations atomic against that interrupt, restoring its previous
   mask on every return. This is not a cross-thread synchronization primitive:
   the emulator invokes SNP on its single firmware execution thread. */
STATIC
EFI_STATUS
SnpLock (
  EMU_SNP_PROTOCOL  *This,
  EMU_SNP_PRIVATE   **Private,
  sigset_t          *OldMask
  )
{
  sigset_t  Mask;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *Private = (EMU_SNP_PRIVATE *)((UINT8 *)This - offsetof (EMU_SNP_PRIVATE, EmuSnp));
  if (((*Private)->Signature != EMU_SNP_PRIVATE_SIGNATURE) || ((*Private)->Mode == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  sigemptyset (&Mask);
  sigaddset (&Mask, SIGALRM);
  if (sigprocmask (SIG_BLOCK, &Mask, OldMask) != 0) {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
SnpUnlock (
  sigset_t    *OldMask,
  EFI_STATUS  Status
  )
{
  sigprocmask (SIG_SETMASK, OldMask, NULL);
  return Status;
}

STATIC
EFI_STATUS
SnpInitialized (
  EMU_SNP_PRIVATE  *Private
  )
{
  if (Private->Mode->State == EfiSimpleNetworkStopped) {
    return EFI_NOT_STARTED;
  }

  if ((Private->Mode->State != EfiSimpleNetworkInitialized) || (Private->Slirp == NULL)) {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

STATIC
int64_t
SnpClock (
  VOID  *Context
  )
{
  struct timespec  Now;

  (void)Context;
  if (clock_gettime (CLOCK_MONOTONIC, &Now) != 0) {
    return 0;
  }

  return (int64_t)Now.tv_sec * 1000000000 + Now.tv_nsec;
}

STATIC
BOOLEAN
SnpBroadcast (
  CONST UINT8  *Address
  )
{
  STATIC CONST UINT8  Broadcast[6] = { 255, 255, 255, 255, 255, 255 };

  return (BOOLEAN)(memcmp (Address, Broadcast, sizeof (Broadcast)) == 0);
}

STATIC
BOOLEAN
SnpAccept (
  EMU_SNP_PRIVATE  *Private,
  CONST UINT8      *Frame
  )
{
  EFI_SIMPLE_NETWORK_MODE  *Mode;
  UINTN                    Index;
  UINT32                   Filters;

  Mode    = Private->Mode;
  Filters = Mode->ReceiveFilterSetting;
  if ((Filters & EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS) != 0) {
    return TRUE;
  }

  if (SnpBroadcast (Frame)) {
    return (BOOLEAN)((Filters & EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST) != 0);
  }

  if ((Frame[0] & 1) != 0) {
    if ((Filters & EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS_MULTICAST) != 0) {
      return TRUE;
    }

    if ((Filters & EFI_SIMPLE_NETWORK_RECEIVE_MULTICAST) != 0) {
      for (Index = 0; Index < Mode->MCastFilterCount; Index++) {
        if (memcmp (Frame, Mode->MCastFilter[Index].Addr, 6) == 0) {
          return TRUE;
        }
      }
    }

    return FALSE;
  }

  return (BOOLEAN)(((Filters & EFI_SIMPLE_NETWORK_RECEIVE_UNICAST) != 0) &&
                   (memcmp (Frame, Mode->CurrentAddress.Addr, 6) == 0));
}

STATIC
ssize_t
SnpSendPacket (
  CONST VOID  *Buffer,
  size_t      Length,
  VOID        *Context
  )
{
  EMU_SNP_PRIVATE  *Private;
  SNP_PACKET       *Packet;
  CONST UINT8      *Frame;

  Private = Context;
  Frame   = Buffer;
  Private->Statistics.RxTotalFrames++;
  Private->Statistics.RxTotalBytes += Length;
  if ((Length < SNP_HEADER_SIZE) || (Length > SNP_FRAME_SIZE) ||
      (Private->Mode == NULL) || (Private->Mode->State != EfiSimpleNetworkInitialized) ||
      !SnpAccept (Private, Frame) || (Private->RxCount == SNP_QUEUE_SIZE))
  {
    Private->Statistics.RxDroppedFrames++;
    /* Dropping when the bounded queue is full is permitted by libslirp. */
    return (ssize_t)Length;
  }

  Packet = &Private->Rx[(Private->RxHead + Private->RxCount) % SNP_QUEUE_SIZE];
  memcpy (Packet->Data, Buffer, Length);
  Packet->Length = Length;
  Private->RxCount++;
  Private->InterruptStatus |= EFI_SIMPLE_NETWORK_RECEIVE_INTERRUPT;
  Private->Statistics.RxGoodFrames++;
  if (SnpBroadcast (Frame)) {
    Private->Statistics.RxBroadcastFrames++;
  } else if ((Frame[0] & 1) != 0) {
    Private->Statistics.RxMulticastFrames++;
  } else {
    Private->Statistics.RxUnicastFrames++;
  }

  return (ssize_t)Length;
}

STATIC
VOID
SnpGuestError (
  CONST char  *Message,
  VOID        *Context
  )
{
  (void)Context;
  fprintf (stderr, "libslirp: %s\n", Message);
}

STATIC
VOID *
SnpTimerNew (
  SlirpTimerCb  Callback,
  VOID          *CallbackContext,
  VOID          *Context
  )
{
  EMU_SNP_PRIVATE  *Private;
  SNP_TIMER        *Timer;

  Private = Context;
  if (Private->TimerCount == SNP_TIMER_LIMIT) {
    return NULL;
  }

  Timer = calloc (1, sizeof (*Timer));
  if (Timer != NULL) {
    Timer->Callback        = Callback;
    Timer->CallbackContext = CallbackContext;
    Timer->Next            = Private->Timers;
    Private->Timers        = Timer;
    Private->TimerCount++;
  }

  return Timer;
}

STATIC
VOID
SnpTimerFree (
  VOID  *TimerData,
  VOID  *Context
  )
{
  EMU_SNP_PRIVATE  *Private;
  SNP_TIMER        **Link;

  Private = Context;
  for (Link = &Private->Timers; *Link != NULL; Link = &(*Link)->Next) {
    if (*Link == TimerData) {
      *Link = (*Link)->Next;
      Private->TimerCount--;
      free (TimerData);
      return;
    }
  }
}

STATIC
VOID
SnpTimerMod (
  VOID     *TimerData,
  int64_t  ExpiresMs,
  VOID     *Context
  )
{
  SNP_TIMER  *Timer;

  (void)Context;
  Timer = TimerData;
  if (Timer != NULL) {
    Timer->ExpiresMs = ExpiresMs;
    Timer->Armed     = TRUE;
  }
}

STATIC
VOID
SnpRegisterFd (
  int   Fd,
  VOID  *Context
  )
{
  /* Each pump obtains a fresh complete poll list from libslirp. */
  (void)Fd;
  (void)Context;
}

STATIC
VOID
SnpNotify (
  VOID  *Context
  )
{
  /* All progress happens synchronously in Receive/GetStatus/Transmit. */
  (void)Context;
}

STATIC CONST SlirpCb  mSlirpCallbacks = {
  .send_packet        = SnpSendPacket,
  .guest_error        = SnpGuestError,
  .clock_get_ns       = SnpClock,
  .timer_new          = SnpTimerNew,
  .timer_free         = SnpTimerFree,
  .timer_mod          = SnpTimerMod,
  .register_poll_fd   = SnpRegisterFd,
  .unregister_poll_fd = SnpRegisterFd,
  .notify             = SnpNotify
};

STATIC
int
SnpAddPoll (
  int   Fd,
  int   Events,
  VOID  *Context
  )
{
  EMU_SNP_PRIVATE  *Private;
  struct pollfd    *PollFd;
  int              Index;

  Private = Context;
  if (Private->PollCount == SNP_POLL_SIZE) {
    return -1;
  }

  Index           = (int)Private->PollCount++;
  PollFd          = &Private->PollFds[Index];
  PollFd->fd      = Fd;
  PollFd->events  = 0;
  PollFd->revents = 0;
  if ((Events & SLIRP_POLL_IN) != 0) {
    PollFd->events |= POLLIN;
  }

  if ((Events & SLIRP_POLL_OUT) != 0) {
    PollFd->events |= POLLOUT;
  }

  if ((Events & SLIRP_POLL_PRI) != 0) {
    PollFd->events |= POLLPRI;
  }

  return Index;
}

STATIC
int
SnpGetEvents (
  int   Index,
  VOID  *Context
  )
{
  EMU_SNP_PRIVATE  *Private;
  short            Events;
  int              Result;

  Private = Context;
  if ((Index < 0) || ((UINTN)Index >= Private->PollCount)) {
    return 0;
  }

  Events = Private->PollFds[Index].revents;
  Result = 0;
  if ((Events & POLLIN) != 0) {
    Result |= SLIRP_POLL_IN;
  }

  if ((Events & POLLOUT) != 0) {
    Result |= SLIRP_POLL_OUT;
  }

  if ((Events & POLLPRI) != 0) {
    Result |= SLIRP_POLL_PRI;
  }

  if ((Events & (POLLERR | POLLNVAL)) != 0) {
    Result |= SLIRP_POLL_ERR;
  }

  if ((Events & POLLHUP) != 0) {
    Result |= SLIRP_POLL_HUP;
  }

  return Result;
}

STATIC
VOID
SnpPump (
  EMU_SNP_PRIVATE  *Private
  )
{
  uint32_t   Timeout;
  int        Result;
  SNP_TIMER  *Timer;
  UINTN      Iteration;
  int64_t    Now;

  /* Never wait in firmware polling. libslirp advances its TCP timers here. */
  Private->PollCount = 0;
  Timeout            = 0;
  slirp_pollfds_fill (Private->Slirp, &Timeout, SnpAddPoll, Private);
  Result = poll (Private->PollFds, Private->PollCount, 0);
  slirp_pollfds_poll (Private->Slirp, Result < 0, SnpGetEvents, Private);

  /* Timers may modify/free their own list nodes; restart the search each time.
     A bound prevents a zero-duration rearming timer from monopolizing the CPU. */
  for (Iteration = 0; Iteration < SNP_TIMER_LIMIT * 4; Iteration++) {
    Now = SnpClock (Private) / 1000000;
    for (Timer = Private->Timers; Timer != NULL; Timer = Timer->Next) {
      if (Timer->Armed && (Timer->ExpiresMs <= Now)) {
        break;
      }
    }

    if (Timer == NULL) {
      break;
    }

    Timer->Armed = FALSE;
    Timer->Callback (Timer->CallbackContext);
  }
}

STATIC
VOID
SnpClearQueues (
  EMU_SNP_PRIVATE  *Private
  )
{
  Private->RxHead          = 0;
  Private->RxCount         = 0;
  Private->TxHead          = 0;
  Private->TxCount         = 0;
  Private->InterruptStatus = 0;
}

STATIC
VOID
SnpCleanup (
  EMU_SNP_PRIVATE  *Private
  )
{
  if (Private->Slirp != NULL) {
    slirp_cleanup (Private->Slirp);
    Private->Slirp = NULL;
  }

  while (Private->Timers != NULL) {
    SnpTimerFree (Private->Timers, Private);
  }

  SnpClearQueues (Private);
}

STATIC
EFI_STATUS
SnpCreateStack (
  EMU_SNP_PRIVATE  *Private
  )
{
  SlirpConfig  Config;

  memset (&Config, 0, sizeof (Config));
  Config.version               = 4;
  Config.in_enabled            = true;
  Config.in6_enabled           = false;
  Config.vnetwork.s_addr       = htonl (0x0a000200);
  Config.vnetmask.s_addr       = htonl (0xffffff00);
  Config.vhost.s_addr          = htonl (0x0a000202);
  Config.vdhcp_start.s_addr    = htonl (0x0a00020f);
  Config.vnameserver.s_addr    = htonl (0x0a000203);
  Config.vhostname             = "redfish-emulator";
  Config.if_mtu                = SNP_MTU;
  Config.if_mru                = SNP_MTU;
  Config.disable_host_loopback = false;
  Config.enable_emu            = false;
  /* DNS forwarding and TFTP are unnecessary for the local simulator. */
  Config.disable_dns = true;
  Private->Slirp     = slirp_new (&Config, &mSlirpCallbacks, Private);
  if (Private->Slirp == NULL) {
    SnpCleanup (Private);
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EmuSnpCreateMapping (
  IN EMU_SNP_PROTOCOL         *This,
  IN EFI_SIMPLE_NETWORK_MODE  *Mode
  )
{
  EMU_SNP_PRIVATE     *Private;
  STATIC CONST UINT8  Mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };

  if ((This == NULL) || (Mode == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Private = (EMU_SNP_PRIVATE *)((UINT8 *)This - offsetof (EMU_SNP_PRIVATE, EmuSnp));
  if (Private->Mode != NULL) {
    return EFI_ALREADY_STARTED;
  }

  Private->Mode = Mode;
  memset (Mode, 0, sizeof (*Mode));
  Mode->State                 = EfiSimpleNetworkStopped;
  Mode->HwAddressSize         = 6;
  Mode->MediaHeaderSize       = SNP_HEADER_SIZE;
  Mode->MaxPacketSize         = SNP_MTU;
  Mode->ReceiveFilterMask     = SNP_FILTERS;
  Mode->MaxMCastFilterCount   = MAX_MCAST_FILTER_CNT;
  Mode->IfType                = 1;
  Mode->MultipleTxSupported   = TRUE;
  Mode->MediaPresentSupported = TRUE;
  Mode->MediaPresent          = TRUE;
  memset (Mode->BroadcastAddress.Addr, 255, 6);
  memcpy (Mode->PermanentAddress.Addr, Mac, sizeof (Mac));
  Mode->PermanentAddress.Addr[5] += (UINT8)Private->Thunk->Instance;
  Mode->CurrentAddress            = Mode->PermanentAddress;
  return EFI_SUCCESS;
}

EFI_STATUS
EmuSnpStart (
  IN EMU_SNP_PROTOCOL  *This
  )
{
  EMU_SNP_PRIVATE  *Private;
  sigset_t         OldMask;
  EFI_STATUS       Status;

  Status = SnpLock (This, &Private, &OldMask);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Private->Mode->State != EfiSimpleNetworkStopped) {
    return SnpUnlock (&OldMask, EFI_ALREADY_STARTED);
  }

  Private->Mode->State = EfiSimpleNetworkStarted;
  return SnpUnlock (&OldMask, EFI_SUCCESS);
}

EFI_STATUS
EmuSnpStop (
  IN EMU_SNP_PROTOCOL  *This
  )
{
  EMU_SNP_PRIVATE  *Private;
  sigset_t         OldMask;
  EFI_STATUS       Status;

  Status = SnpLock (This, &Private, &OldMask);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Private->Mode->State == EfiSimpleNetworkStopped) {
    return SnpUnlock (&OldMask, EFI_NOT_STARTED);
  }

  if (Private->Mode->State != EfiSimpleNetworkStarted) {
    return SnpUnlock (&OldMask, EFI_DEVICE_ERROR);
  }

  Private->Mode->State = EfiSimpleNetworkStopped;
  return SnpUnlock (&OldMask, EFI_SUCCESS);
}

EFI_STATUS
EmuSnpInitialize (
  IN EMU_SNP_PROTOCOL  *This,
  IN UINTN             ExtraRxBufferSize OPTIONAL,
  IN UINTN             ExtraTxBufferSize OPTIONAL
  )
{
  EMU_SNP_PRIVATE  *Private;
  sigset_t         OldMask;
  EFI_STATUS       Status;

  (void)ExtraRxBufferSize;
  (void)ExtraTxBufferSize;
  Status = SnpLock (This, &Private, &OldMask);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Private->Mode->State == EfiSimpleNetworkStopped) {
    return SnpUnlock (&OldMask, EFI_NOT_STARTED);
  }

  if (Private->Mode->State != EfiSimpleNetworkStarted) {
    return SnpUnlock (&OldMask, EFI_DEVICE_ERROR);
  }

  SnpClearQueues (Private);
  Private->Mode->ReceiveFilterSetting = 0;
  Private->Mode->MCastFilterCount     = 0;
  Status                              = SnpCreateStack (Private);
  if (!EFI_ERROR (Status)) {
    Private->Mode->State = EfiSimpleNetworkInitialized;
    fprintf (
      stderr,
      "Linux SNP: libslirp %s, MAC %02x:%02x:%02x:%02x:%02x:%02x, host 10.0.2.2\n",
      slirp_version_string (),
      Private->Mode->CurrentAddress.Addr[0],
      Private->Mode->CurrentAddress.Addr[1],
      Private->Mode->CurrentAddress.Addr[2],
      Private->Mode->CurrentAddress.Addr[3],
      Private->Mode->CurrentAddress.Addr[4],
      Private->Mode->CurrentAddress.Addr[5]
      );
  }

  return SnpUnlock (&OldMask, Status);
}

EFI_STATUS
EmuSnpReset (
  IN EMU_SNP_PROTOCOL  *This,
  IN BOOLEAN           ExtendedVerification
  )
{
  EMU_SNP_PRIVATE  *Private;
  sigset_t         OldMask;
  EFI_STATUS       Status;

  (void)ExtendedVerification;
  Status = SnpLock (This, &Private, &OldMask);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SnpInitialized (Private);
  if (!EFI_ERROR (Status)) {
    SnpCleanup (Private);
    Status = SnpCreateStack (Private);
    if (EFI_ERROR (Status)) {
      Private->Mode->State = EfiSimpleNetworkStarted;
    }
  }

  return SnpUnlock (&OldMask, Status);
}

EFI_STATUS
EmuSnpShutdown (
  IN EMU_SNP_PROTOCOL  *This
  )
{
  EMU_SNP_PRIVATE  *Private;
  sigset_t         OldMask;
  EFI_STATUS       Status;

  Status = SnpLock (This, &Private, &OldMask);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SnpInitialized (Private);
  if (!EFI_ERROR (Status)) {
    SnpCleanup (Private);
    Private->Mode->ReceiveFilterSetting = 0;
    Private->Mode->MCastFilterCount     = 0;
    Private->Mode->State                = EfiSimpleNetworkStarted;
  }

  return SnpUnlock (&OldMask, Status);
}

EFI_STATUS
EmuSnpReceiveFilters (
  IN EMU_SNP_PROTOCOL  *This,
  IN UINT32            Enable,
  IN UINT32            Disable,
  IN BOOLEAN           ResetMCastFilter,
  IN UINTN             MCastFilterCnt OPTIONAL,
  IN EFI_MAC_ADDRESS   *MCastFilter OPTIONAL
  )
{
  EMU_SNP_PRIVATE  *Private;
  sigset_t         OldMask;
  EFI_STATUS       Status;
  UINTN            Index;

  Status = SnpLock (This, &Private, &OldMask);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SnpInitialized (Private);
  if (EFI_ERROR (Status)) {
    return SnpUnlock (&OldMask, Status);
  }

  if (((Enable | Disable) & ~SNP_FILTERS) != 0) {
    return SnpUnlock (&OldMask, EFI_INVALID_PARAMETER);
  }

  if (!ResetMCastFilter && (MCastFilterCnt != 0)) {
    if ((MCastFilter == NULL) || (MCastFilterCnt > MAX_MCAST_FILTER_CNT)) {
      return SnpUnlock (&OldMask, EFI_INVALID_PARAMETER);
    }

    for (Index = 0; Index < MCastFilterCnt; Index++) {
      if (((MCastFilter[Index].Addr[0] & 1) == 0) || SnpBroadcast (MCastFilter[Index].Addr)) {
        return SnpUnlock (&OldMask, EFI_INVALID_PARAMETER);
      }
    }
  }

  if (((Enable & EFI_SIMPLE_NETWORK_RECEIVE_MULTICAST) != 0) &&
      (ResetMCastFilter || ((MCastFilterCnt == 0) && (Private->Mode->MCastFilterCount == 0))))
  {
    return SnpUnlock (&OldMask, EFI_INVALID_PARAMETER);
  }

  if (ResetMCastFilter) {
    Private->Mode->MCastFilterCount = 0;
    memset (Private->Mode->MCastFilter, 0, sizeof (Private->Mode->MCastFilter));
  } else if (MCastFilterCnt != 0) {
    Private->Mode->MCastFilterCount = (UINT32)MCastFilterCnt;
    memcpy (Private->Mode->MCastFilter, MCastFilter, MCastFilterCnt * sizeof (*MCastFilter));
  }

  Private->Mode->ReceiveFilterSetting = (Private->Mode->ReceiveFilterSetting | Enable) & ~Disable;
  return SnpUnlock (&OldMask, EFI_SUCCESS);
}

EFI_STATUS
EmuSnpStationAddress (
  IN EMU_SNP_PROTOCOL  *This,
  IN BOOLEAN           Reset,
  IN EFI_MAC_ADDRESS   *New OPTIONAL
  )
{
  EMU_SNP_PRIVATE  *Private;
  sigset_t         OldMask;
  EFI_STATUS       Status;

  (void)Reset;
  (void)New;
  Status = SnpLock (This, &Private, &OldMask);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SnpInitialized (Private);
  return SnpUnlock (&OldMask, EFI_ERROR (Status) ? Status : EFI_UNSUPPORTED);
}

EFI_STATUS
EmuSnpStatistics (
  IN EMU_SNP_PROTOCOL         *This,
  IN BOOLEAN                  Reset,
  IN OUT UINTN                *StatisticsSize OPTIONAL,
  OUT EFI_NETWORK_STATISTICS  *StatisticsTable OPTIONAL
  )
{
  EMU_SNP_PRIVATE  *Private;
  sigset_t         OldMask;
  EFI_STATUS       Status;
  UINTN            Size;

  Status = SnpLock (This, &Private, &OldMask);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SnpInitialized (Private);
  if (EFI_ERROR (Status)) {
    return SnpUnlock (&OldMask, Status);
  }

  if (Reset) {
    memset (&Private->Statistics, 0, sizeof (Private->Statistics));
    return SnpUnlock (&OldMask, EFI_SUCCESS);
  }

  if (StatisticsSize == NULL) {
    return SnpUnlock (&OldMask, EFI_INVALID_PARAMETER);
  }

  Size            = *StatisticsSize;
  *StatisticsSize = sizeof (Private->Statistics);
  if ((Size < sizeof (Private->Statistics)) || (StatisticsTable == NULL)) {
    return SnpUnlock (&OldMask, EFI_BUFFER_TOO_SMALL);
  }

  memcpy (StatisticsTable, &Private->Statistics, sizeof (*StatisticsTable));
  return SnpUnlock (&OldMask, EFI_SUCCESS);
}

EFI_STATUS
EmuSnpMCastIpToMac (
  IN EMU_SNP_PROTOCOL  *This,
  IN BOOLEAN           IPv6,
  IN EFI_IP_ADDRESS    *IP,
  OUT EFI_MAC_ADDRESS  *MAC
  )
{
  EMU_SNP_PRIVATE  *Private;
  sigset_t         OldMask;
  EFI_STATUS       Status;

  Status = SnpLock (This, &Private, &OldMask);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SnpInitialized (Private);
  if (EFI_ERROR (Status)) {
    return SnpUnlock (&OldMask, Status);
  }

  if ((IP == NULL) || (MAC == NULL) ||
      (IPv6 ? (IP->v6.Addr[0] != 0xff) : ((IP->v4.Addr[0] & 0xf0) != 0xe0)))
  {
    return SnpUnlock (&OldMask, EFI_INVALID_PARAMETER);
  }

  memset (MAC, 0, sizeof (*MAC));
  if (IPv6) {
    MAC->Addr[0] = MAC->Addr[1] = 0x33;
    memcpy (&MAC->Addr[2], &IP->v6.Addr[12], 4);
  } else {
    MAC->Addr[0] = 0x01;
    MAC->Addr[1] = 0x00;
    MAC->Addr[2] = 0x5e;
    MAC->Addr[3] = IP->v4.Addr[1] & 0x7f;
    MAC->Addr[4] = IP->v4.Addr[2];
    MAC->Addr[5] = IP->v4.Addr[3];
  }

  return SnpUnlock (&OldMask, EFI_SUCCESS);
}

EFI_STATUS
EmuSnpNvData (
  IN EMU_SNP_PROTOCOL  *This,
  IN BOOLEAN           ReadWrite,
  IN UINTN             Offset,
  IN UINTN             BufferSize,
  IN OUT VOID          *Buffer
  )
{
  EMU_SNP_PRIVATE  *Private;
  sigset_t         OldMask;
  EFI_STATUS       Status;

  (void)ReadWrite;
  (void)Offset;
  (void)BufferSize;
  (void)Buffer;
  Status = SnpLock (This, &Private, &OldMask);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SnpInitialized (Private);
  return SnpUnlock (&OldMask, EFI_ERROR (Status) ? Status : EFI_UNSUPPORTED);
}

EFI_STATUS
EmuSnpGetStatus (
  IN EMU_SNP_PROTOCOL  *This,
  OUT UINT32           *InterruptStatus OPTIONAL,
  OUT VOID             **TxBuf OPTIONAL
  )
{
  EMU_SNP_PRIVATE  *Private;
  sigset_t         OldMask;
  EFI_STATUS       Status;

  Status = SnpLock (This, &Private, &OldMask);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SnpInitialized (Private);
  if (EFI_ERROR (Status)) {
    return SnpUnlock (&OldMask, Status);
  }

  SnpPump (Private);
  if (InterruptStatus != NULL) {
    *InterruptStatus         = Private->InterruptStatus;
    Private->InterruptStatus = 0;
  }

  if (TxBuf != NULL) {
    *TxBuf = NULL;
    if (Private->TxCount != 0) {
      *TxBuf          = Private->Tx[Private->TxHead];
      Private->TxHead = (Private->TxHead + 1) % SNP_QUEUE_SIZE;
      Private->TxCount--;
    }
  }

  return SnpUnlock (&OldMask, EFI_SUCCESS);
}

EFI_STATUS
EmuSnpTransmit (
  IN EMU_SNP_PROTOCOL  *This,
  IN UINTN             HeaderSize,
  IN UINTN             BufferSize,
  IN VOID              *Buffer,
  IN EFI_MAC_ADDRESS   *SrcAddr OPTIONAL,
  IN EFI_MAC_ADDRESS   *DestAddr OPTIONAL,
  IN UINT16            *Protocol OPTIONAL
  )
{
  EMU_SNP_PRIVATE  *Private;
  sigset_t         OldMask;
  EFI_STATUS       Status;
  UINT8            *Frame;

  Status = SnpLock (This, &Private, &OldMask);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SnpInitialized (Private);
  if (EFI_ERROR (Status)) {
    return SnpUnlock (&OldMask, Status);
  }

  if ((Buffer == NULL) || ((HeaderSize != 0) &&
                           ((HeaderSize != SNP_HEADER_SIZE) || (DestAddr == NULL) || (Protocol == NULL))))
  {
    return SnpUnlock (&OldMask, EFI_INVALID_PARAMETER);
  }

  if (BufferSize < SNP_HEADER_SIZE) {
    return SnpUnlock (&OldMask, EFI_BUFFER_TOO_SMALL);
  }

  if (BufferSize > SNP_FRAME_SIZE) {
    return SnpUnlock (&OldMask, EFI_INVALID_PARAMETER);
  }

  SnpPump (Private);
  if (Private->TxCount == SNP_QUEUE_SIZE) {
    return SnpUnlock (&OldMask, EFI_NOT_READY);
  }

  Frame = Buffer;
  if (HeaderSize != 0) {
    if (SrcAddr == NULL) {
      SrcAddr = &Private->Mode->CurrentAddress;
    }

    memcpy (Frame, DestAddr->Addr, 6);
    memcpy (Frame + 6, SrcAddr->Addr, 6);
    Frame[12] = (UINT8)(*Protocol >> 8);
    Frame[13] = (UINT8)*Protocol;
  }

  /* slirp_input consumes/copies the frame before returning. Preserve each
     caller buffer exactly once in FIFO order until GetStatus recycles it. */
  slirp_input (Private->Slirp, Frame, (int)BufferSize);
  Private->Tx[(Private->TxHead + Private->TxCount) % SNP_QUEUE_SIZE] = Buffer;
  Private->TxCount++;
  Private->InterruptStatus |= EFI_SIMPLE_NETWORK_TRANSMIT_INTERRUPT;
  Private->Statistics.TxTotalFrames++;
  Private->Statistics.TxGoodFrames++;
  Private->Statistics.TxTotalBytes += BufferSize;
  if (SnpBroadcast (Frame)) {
    Private->Statistics.TxBroadcastFrames++;
  } else if ((Frame[0] & 1) != 0) {
    Private->Statistics.TxMulticastFrames++;
  } else {
    Private->Statistics.TxUnicastFrames++;
  }

  SnpPump (Private);
  return SnpUnlock (&OldMask, EFI_SUCCESS);
}

EFI_STATUS
EmuSnpReceive (
  IN EMU_SNP_PROTOCOL  *This,
  OUT UINTN            *HeaderSize OPTIONAL,
  IN OUT UINTN         *BufferSize,
  OUT VOID             *Buffer,
  OUT EFI_MAC_ADDRESS  *SrcAddr OPTIONAL,
  OUT EFI_MAC_ADDRESS  *DestAddr OPTIONAL,
  OUT UINT16           *Protocol OPTIONAL
  )
{
  EMU_SNP_PRIVATE  *Private;
  sigset_t         OldMask;
  EFI_STATUS       Status;
  SNP_PACKET       *Packet;

  Status = SnpLock (This, &Private, &OldMask);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SnpInitialized (Private);
  if (EFI_ERROR (Status)) {
    return SnpUnlock (&OldMask, Status);
  }

  if ((Buffer == NULL) || (BufferSize == NULL)) {
    return SnpUnlock (&OldMask, EFI_INVALID_PARAMETER);
  }

  SnpPump (Private);
  if (Private->RxCount == 0) {
    return SnpUnlock (&OldMask, EFI_NOT_READY);
  }

  Packet = &Private->Rx[Private->RxHead];
  if (*BufferSize < Packet->Length) {
    *BufferSize = Packet->Length;
    return SnpUnlock (&OldMask, EFI_BUFFER_TOO_SMALL);
  }

  memcpy (Buffer, Packet->Data, Packet->Length);
  *BufferSize = Packet->Length;
  if (HeaderSize != NULL) {
    *HeaderSize = SNP_HEADER_SIZE;
  }

  if (SrcAddr != NULL) {
    memset (SrcAddr, 0, sizeof (*SrcAddr));
    memcpy (SrcAddr->Addr, Packet->Data + 6, 6);
  }

  if (DestAddr != NULL) {
    memset (DestAddr, 0, sizeof (*DestAddr));
    memcpy (DestAddr->Addr, Packet->Data, 6);
  }

  if (Protocol != NULL) {
    *Protocol = (UINT16)((Packet->Data[12] << 8) | Packet->Data[13]);
  }

  Private->RxHead = (Private->RxHead + 1) % SNP_QUEUE_SIZE;
  Private->RxCount--;
  return SnpUnlock (&OldMask, EFI_SUCCESS);
}

EMU_SNP_PROTOCOL  gEmuSnpProtocol = {
  GasketSnpCreateMapping,
  GasketSnpStart,
  GasketSnpStop,
  GasketSnpInitialize,
  GasketSnpReset,
  GasketSnpShutdown,
  GasketSnpReceiveFilters,
  GasketSnpStationAddress,
  GasketSnpStatistics,
  GasketSnpMCastIpToMac,
  GasketSnpNvData,
  GasketSnpGetStatus,
  GasketSnpTransmit,
  GasketSnpReceive
};

EFI_STATUS
EmuSnpThunkOpen (
  IN EMU_IO_THUNK_PROTOCOL  *This
  )
{
  EMU_SNP_PRIVATE        *Private;
  UINTN                  Index;
  STATIC CONST EFI_GUID  Guid   = EMU_SNP_PROTOCOL_GUID;
  STATIC CONST CHAR16    Name[] = { 's', 'l', 'i', 'r', 'p', 0 };

  if ((This == NULL) || (This->Protocol == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (memcmp (This->Protocol, &Guid, sizeof (Guid)) != 0) {
    return EFI_UNSUPPORTED;
  }

  if (This->Private != NULL) {
    return EFI_ALREADY_STARTED;
  }

  if ((This->ConfigString != NULL) && (This->ConfigString[0] != 0)) {
    for (Index = 0; Index < sizeof (Name) / sizeof (Name[0]); Index++) {
      if (This->ConfigString[Index] != Name[Index]) {
        return EFI_UNSUPPORTED;
      }
    }
  }

  Private = calloc (1, sizeof (*Private));
  if (Private == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Private->Signature = EMU_SNP_PRIVATE_SIGNATURE;
  Private->Thunk     = This;
  memcpy (&Private->EmuSnp, &gEmuSnpProtocol, sizeof (gEmuSnpProtocol));
  This->Interface = &Private->EmuSnp;
  This->Private   = Private;
  return EFI_SUCCESS;
}

EFI_STATUS
EmuSnpThunkClose (
  IN EMU_IO_THUNK_PROTOCOL  *This
  )
{
  EMU_SNP_PRIVATE        *Private;
  sigset_t               Mask;
  sigset_t               OldMask;
  STATIC CONST EFI_GUID  Guid = EMU_SNP_PROTOCOL_GUID;

  if ((This == NULL) || (This->Protocol == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (memcmp (This->Protocol, &Guid, sizeof (Guid)) != 0) {
    return EFI_UNSUPPORTED;
  }

  Private = This->Private;
  if (Private == NULL) {
    return EFI_NOT_STARTED;
  }

  sigemptyset (&Mask);
  sigaddset (&Mask, SIGALRM);
  if (sigprocmask (SIG_BLOCK, &Mask, &OldMask) != 0) {
    return EFI_DEVICE_ERROR;
  }

  SnpCleanup (Private);
  if (Private->Mode != NULL) {
    Private->Mode->State = EfiSimpleNetworkStopped;
  }

  This->Private   = NULL;
  This->Interface = NULL;
  free (Private);
  return SnpUnlock (&OldMask, EFI_SUCCESS);
}

EMU_IO_THUNK_PROTOCOL  gSnpThunkIo = {
  &gEmuSnpProtocolGuid,
  NULL,
  NULL,
  0,
  GasketSnpThunkOpen,
  GasketSnpThunkClose,
  NULL
};

#endif
