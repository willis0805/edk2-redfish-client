/** @file
  Native tests of the Linux SNP backend. These are NOT firmware boot tests.
  Uses the real edk2 protocol definitions and real libslirp, with the ABI
  assembly gaskets replaced by direct native calls for this test executable.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#define GasketSnpCreateMapping   EmuSnpCreateMapping
#define GasketSnpStart           EmuSnpStart
#define GasketSnpStop            EmuSnpStop
#define GasketSnpInitialize      EmuSnpInitialize
#define GasketSnpReset           EmuSnpReset
#define GasketSnpShutdown        EmuSnpShutdown
#define GasketSnpReceiveFilters  EmuSnpReceiveFilters
#define GasketSnpStationAddress  EmuSnpStationAddress
#define GasketSnpStatistics      EmuSnpStatistics
#define GasketSnpMCastIpToMac    EmuSnpMCastIpToMac
#define GasketSnpNvData          EmuSnpNvData
#define GasketSnpGetStatus       EmuSnpGetStatus
#define GasketSnpTransmit        EmuSnpTransmit
#define GasketSnpReceive         EmuSnpReceive
#define GasketSnpThunkOpen       EmuSnpThunkOpen
#define GasketSnpThunkClose      EmuSnpThunkClose

#include "LinuxPacketFilter.c"
#include <assert.h>

EFI_GUID  gEmuSnpProtocolGuid = EMU_SNP_PROTOCOL_GUID;

#define CHECK(Expression)  do {\
  if (!(Expression)) { \
    fprintf (stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #Expression); \
    exit (1); \
  } \
} while (0)

STATIC
VOID
Put16 (
  UINT8   *Buffer,
  UINT16  Value
  )
{
  Buffer[0] = (UINT8)(Value >> 8);
  Buffer[1] = (UINT8)Value;
}

STATIC
UINT16
Get16 (
  CONST UINT8  *Buffer
  )
{
  return (UINT16)((Buffer[0] << 8) | Buffer[1]);
}

STATIC
VOID
Drain (
  EMU_SNP_PROTOCOL  *Snp
  )
{
  UINT8  Frame[SNP_FRAME_SIZE];
  UINTN  Length;
  VOID   *Tx;

  do {
    CHECK (Snp->GetStatus (Snp, NULL, &Tx) == EFI_SUCCESS);
  } while (Tx != NULL);

  do {
    Length = sizeof (Frame);
  } while (Snp->Receive (Snp, NULL, &Length, Frame, NULL, NULL, NULL) == EFI_SUCCESS);
}

STATIC
VOID
TestQueues (
  EMU_SNP_PROTOCOL  *Snp,
  EMU_SNP_PRIVATE   *Private
  )
{
  UINT8                   Frames[SNP_QUEUE_SIZE + 1][60];
  UINT8                   Result[SNP_FRAME_SIZE];
  UINTN                   Index;
  UINTN                   Length;
  UINTN                   Header;
  UINT16                  Type;
  UINT32                  Interrupts;
  VOID                    *Tx;
  EFI_MAC_ADDRESS         Src;
  EFI_MAC_ADDRESS         Dst;
  EFI_NETWORK_STATISTICS  Stats;

  memset (Frames, 0, sizeof (Frames));
  for (Index = 0; Index < SNP_QUEUE_SIZE + 1; Index++) {
    memcpy (Frames[Index], Private->Mode->CurrentAddress.Addr, 6);
    Put16 (&Frames[Index][12], 0x88b5); /* experimental EtherType; libslirp ignores it */
    Frames[Index][14] = (UINT8)Index;
  }

  CHECK (Snp->Transmit (Snp, 0, 13, Frames[0], NULL, NULL, NULL) == EFI_BUFFER_TOO_SMALL);
  CHECK (Snp->Transmit (Snp, 0, SNP_FRAME_SIZE + 1, Frames[0], NULL, NULL, NULL) == EFI_INVALID_PARAMETER);
  CHECK (Snp->Transmit (Snp, 1, 60, Frames[0], NULL, NULL, NULL) == EFI_INVALID_PARAMETER);
  CHECK (Snp->Transmit (Snp, 0, 60, NULL, NULL, NULL, NULL) == EFI_INVALID_PARAMETER);
  for (Index = 0; Index < SNP_QUEUE_SIZE; Index++) {
    CHECK (Snp->Transmit (Snp, 0, 60, Frames[Index], NULL, NULL, NULL) == EFI_SUCCESS);
  }

  CHECK (Snp->Transmit (Snp, 0, 60, Frames[SNP_QUEUE_SIZE], NULL, NULL, NULL) == EFI_NOT_READY);
  CHECK (Snp->GetStatus (Snp, &Interrupts, NULL) == EFI_SUCCESS);
  CHECK (Interrupts == EFI_SIMPLE_NETWORK_TRANSMIT_INTERRUPT);
  CHECK (Snp->GetStatus (Snp, &Interrupts, NULL) == EFI_SUCCESS);
  CHECK (Interrupts == 0);
  for (Index = 0; Index < SNP_QUEUE_SIZE; Index++) {
    CHECK (Snp->GetStatus (Snp, NULL, &Tx) == EFI_SUCCESS);
    CHECK (Tx == Frames[Index]);
  }

  CHECK (Snp->GetStatus (Snp, NULL, &Tx) == EFI_SUCCESS);
  CHECK (Tx == NULL);
  for (Index = 0; Index < SNP_QUEUE_SIZE + 1; Index++) {
    CHECK (SnpSendPacket (Frames[Index], 60, Private) == 60);
  }

  CHECK (Private->RxCount == SNP_QUEUE_SIZE);
  CHECK (Private->Statistics.RxDroppedFrames == 1);
  Length = 1;
  CHECK (Snp->Receive (Snp, NULL, &Length, Result, NULL, NULL, NULL) == EFI_BUFFER_TOO_SMALL);
  CHECK (Length == 60 && Private->RxCount == SNP_QUEUE_SIZE);
  for (Index = 0; Index < SNP_QUEUE_SIZE; Index++) {
    Length = sizeof (Result);
    CHECK (Snp->Receive (Snp, &Header, &Length, Result, &Src, &Dst, &Type) == EFI_SUCCESS);
    CHECK (Header == SNP_HEADER_SIZE && Length == 60 && Type == 0x88b5);
    CHECK (Result[14] == Index);
    CHECK (memcmp (Dst.Addr, Private->Mode->CurrentAddress.Addr, 6) == 0);
  }

  Length = sizeof (Result);
  CHECK (Snp->Receive (Snp, NULL, &Length, Result, NULL, NULL, NULL) == EFI_NOT_READY);
  CHECK (Snp->Receive (Snp, NULL, NULL, Result, NULL, NULL, NULL) == EFI_INVALID_PARAMETER);
  Length = 0;
  CHECK (Snp->Statistics (Snp, FALSE, &Length, NULL) == EFI_BUFFER_TOO_SMALL);
  CHECK (Length == sizeof (Stats));
  CHECK (Snp->Statistics (Snp, FALSE, &Length, &Stats) == EFI_SUCCESS);
  CHECK (Stats.TxGoodFrames == SNP_QUEUE_SIZE && Stats.RxGoodFrames == SNP_QUEUE_SIZE);
  CHECK (Snp->Statistics (Snp, TRUE, NULL, NULL) == EFI_SUCCESS);
  CHECK (Private->Statistics.RxDroppedFrames == 0);
  Drain (Snp);
  puts ("PASS SNP bounded queues, FIFO recycling, interrupts, short RX buffers and statistics");
}

STATIC
VOID
TestFilters (
  EMU_SNP_PROTOCOL  *Snp,
  EMU_SNP_PRIVATE   *Private
  )
{
  UINT8            Frame[60];
  EFI_IP_ADDRESS   Ip;
  EFI_MAC_ADDRESS  Mac;

  memset (Frame, 0, sizeof (Frame));
  memset (&Ip, 0, sizeof (Ip));
  Ip.v4.Addr[0] = 224;
  Ip.v4.Addr[3] = 1;
  CHECK (Snp->MCastIpToMac (Snp, FALSE, &Ip, &Mac) == EFI_SUCCESS);
  CHECK (Mac.Addr[0] == 1 && Mac.Addr[2] == 0x5e && Mac.Addr[5] == 1);
  CHECK (Snp->ReceiveFilters (Snp, 0x80000000, 0, FALSE, 0, NULL) == EFI_INVALID_PARAMETER);
  CHECK (Snp->ReceiveFilters (Snp, EFI_SIMPLE_NETWORK_RECEIVE_MULTICAST, 0, FALSE, 0, NULL) == EFI_INVALID_PARAMETER);
  CHECK (Snp->ReceiveFilters (Snp, EFI_SIMPLE_NETWORK_RECEIVE_MULTICAST, 0, FALSE, 1, &Mac) == EFI_SUCCESS);
  memcpy (Frame, Mac.Addr, 6);
  CHECK (SnpAccept (Private, Frame));
  Frame[5]++;
  CHECK (!SnpAccept (Private, Frame));
  CHECK (Snp->ReceiveFilters (Snp, EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS_MULTICAST, 0, FALSE, 0, NULL) == EFI_SUCCESS);
  CHECK (SnpAccept (Private, Frame));
  memset (Frame, 0xff, 6);
  CHECK (SnpAccept (Private, Frame));
  CHECK (Snp->ReceiveFilters (Snp, 0, SNP_FILTERS, TRUE, 0, NULL) == EFI_SUCCESS);
  CHECK (!SnpAccept (Private, Frame));
  CHECK (Snp->ReceiveFilters (Snp, EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS, 0, FALSE, 0, NULL) == EFI_SUCCESS);
  CHECK (SnpAccept (Private, Frame));
  CHECK (
    Snp->ReceiveFilters (
           Snp,
           EFI_SIMPLE_NETWORK_RECEIVE_UNICAST | EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST,
           EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS,
           FALSE,
           0,
           NULL
           ) == EFI_SUCCESS
    );
  puts ("PASS SNP receive filters and multicast mapping");
}

STATIC
VOID
TestArp (
  EMU_SNP_PROTOCOL         *Snp,
  EFI_SIMPLE_NETWORK_MODE  *Mode,
  UINT8                    *HostMac
  )
{
  UINT8               Arp[42];
  UINT8               Reply[SNP_FRAME_SIZE];
  UINTN               Length;
  UINT16              Protocol;
  EFI_MAC_ADDRESS     Dest;
  STATIC CONST UINT8  GuestIp[4] = { 10, 0, 2, 15 };
  STATIC CONST UINT8  HostIp[4]  = { 10, 0, 2, 2 };

  memset (Arp, 0, sizeof (Arp));
  memset (&Dest, 0xff, sizeof (Dest));
  Protocol = 0x0806;
  Put16 (Arp + 14, 1);
  Put16 (Arp + 16, 0x0800);
  Arp[18] = 6;
  Arp[19] = 4;
  Put16 (Arp + 20, 1);
  memcpy (Arp + 22, Mode->CurrentAddress.Addr, 6);
  memcpy (Arp + 28, GuestIp, 4);
  memcpy (Arp + 38, HostIp, 4);
  CHECK (Snp->Transmit (Snp, 14, sizeof (Arp), Arp, NULL, &Dest, &Protocol) == EFI_SUCCESS);
  CHECK (memcmp (Arp + 6, Mode->CurrentAddress.Addr, 6) == 0);
  Length = sizeof (Reply);
  CHECK (Snp->Receive (Snp, NULL, &Length, Reply, NULL, NULL, &Protocol) == EFI_SUCCESS);
  CHECK (Protocol == 0x0806 && Length >= sizeof (Arp));
  CHECK (Get16 (Reply + 20) == 2);
  CHECK (memcmp (Reply + 28, HostIp, 4) == 0);
  CHECK (memcmp (Reply + 38, GuestIp, 4) == 0);
  memcpy (HostMac, Reply + 22, 6);
  Drain (Snp);
  puts ("PASS real libslirp ARP request/reply for 10.0.2.2");
}

STATIC
VOID
TestUdpLoopback (
  EMU_SNP_PROTOCOL         *Snp,
  EFI_SIMPLE_NETWORK_MODE  *Mode,
  UINT8                    *HostMac
  )
{
  int                 Server;
  struct sockaddr_in  Address;
  struct sockaddr_in  Peer;
  socklen_t           AddressSize;
  struct pollfd       PollFd;
  UINT8               Frame[60];
  UINT8               Reply[SNP_FRAME_SIZE];
  UINT8               *Ip;
  UINT8               *Udp;
  UINT32              Sum;
  UINTN               Index;
  UINTN               Length;
  UINT16              Type;
  EFI_STATUS          Status;
  int64_t             Deadline;
  char                Payload[16];

  Server = socket (AF_INET, SOCK_DGRAM, 0);
  CHECK (Server >= 0);
  memset (&Address, 0, sizeof (Address));
  Address.sin_family      = AF_INET;
  Address.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
  CHECK (bind (Server, (struct sockaddr *)&Address, sizeof (Address)) == 0);
  AddressSize = sizeof (Address);
  CHECK (getsockname (Server, (struct sockaddr *)&Address, &AddressSize) == 0);

  memset (Frame, 0, sizeof (Frame));
  memcpy (Frame, HostMac, 6);
  memcpy (Frame + 6, Mode->CurrentAddress.Addr, 6);
  Put16 (Frame + 12, 0x0800);
  Ip    = Frame + 14;
  Ip[0] = 0x45;
  Put16 (Ip + 2, 32);
  Ip[8]  = 64;
  Ip[9]  = 17;
  Ip[12] = 10;
  Ip[14] = 2;
  Ip[15] = 15;
  Ip[16] = 10;
  Ip[18] = 2;
  Ip[19] = 2;
  Sum    = 0;
  for (Index = 0; Index < 20; Index += 2) {
    Sum += Get16 (Ip + Index);
  }

  while (Sum >> 16) {
    Sum = (Sum & 0xffff) + (Sum >> 16);
  }

  Put16 (Ip + 10, (UINT16) ~Sum);
  Udp = Ip + 20;
  Put16 (Udp, 12345);
  Put16 (Udp + 2, ntohs (Address.sin_port));
  Put16 (Udp + 4, 12);
  memcpy (Udp + 8, "ping", 4); /* IPv4 UDP checksum zero is valid. */
  CHECK (Snp->Transmit (Snp, 0, 46, Frame, NULL, NULL, NULL) == EFI_SUCCESS);

  PollFd.fd     = Server;
  PollFd.events = POLLIN;
  CHECK (poll (&PollFd, 1, 1000) == 1);
  AddressSize = sizeof (Peer);
  CHECK (recvfrom (Server, Payload, sizeof (Payload), 0, (struct sockaddr *)&Peer, &AddressSize) == 4);
  CHECK (memcmp (Payload, "ping", 4) == 0);
  CHECK (sendto (Server, "pong", 4, 0, (struct sockaddr *)&Peer, AddressSize) == 4);

  Deadline = SnpClock (NULL) + 1000000000;
  do {
    Length = sizeof (Reply);
    Status = Snp->Receive (Snp, NULL, &Length, Reply, NULL, NULL, &Type);
    if ((Status == EFI_SUCCESS) && (Type == 0x0800) && (Length >= 46) && (Reply[23] == 17)) {
      break;
    }

    CHECK (Status == EFI_SUCCESS || Status == EFI_NOT_READY);
    usleep (1000);
  } while (SnpClock (NULL) < Deadline);

  CHECK (Status == EFI_SUCCESS && Type == 0x0800 && Length >= 46 && Reply[23] == 17);
  CHECK (Get16 (Reply + 36) == 12345);
  CHECK (memcmp (Reply + 42, "pong", 4) == 0);
  close (Server);
  Drain (Snp);
  puts ("PASS real libslirp bidirectional UDP to unprivileged host loopback socket");
}

STATIC
VOID
TimerCount (
  VOID  *Context
  )
{
  (*(UINTN *)Context)++;
}

int
main (
  int   Argc,
  char  **Argv,
  char  **Envp
  )
{
  EMU_IO_THUNK_PROTOCOL    Thunk;
  EMU_SNP_PROTOCOL         *Snp;
  EMU_SNP_PRIVATE          *Private;
  EFI_SIMPLE_NETWORK_MODE  Mode;
  UINT8                    HostMac[6];
  CHAR16                   ShortName[2] = { 'x', 0 };
  SNP_TIMER                *Timer;
  UINTN                    TimerCalls = 0;
  sigset_t                 Before;
  sigset_t                 After;

  (void)Argc;
  (void)Argv;
  (void)Envp;
  Thunk              = gSnpThunkIo;
  Thunk.ConfigString = ShortName;
  CHECK (Thunk.Open (&Thunk) == EFI_UNSUPPORTED);
  Thunk.ConfigString = NULL;
  CHECK (Thunk.Open (&Thunk) == EFI_SUCCESS);
  CHECK (Thunk.Open (&Thunk) == EFI_ALREADY_STARTED);
  Snp     = Thunk.Interface;
  Private = Thunk.Private;
  CHECK (Snp->Start (Snp) == EFI_INVALID_PARAMETER);
  CHECK (Snp->CreateMapping (Snp, &Mode) == EFI_SUCCESS);
  CHECK (Snp->Initialize (Snp, 0, 0) == EFI_NOT_STARTED);
  CHECK (Snp->Stop (Snp) == EFI_NOT_STARTED);
  CHECK (Snp->Start (Snp) == EFI_SUCCESS);
  CHECK (Snp->Start (Snp) == EFI_ALREADY_STARTED);
  CHECK (Snp->GetStatus (Snp, NULL, NULL) == EFI_DEVICE_ERROR);
  CHECK (Snp->Initialize (Snp, 0, 0) == EFI_SUCCESS);
  CHECK (Snp->Initialize (Snp, 0, 0) == EFI_DEVICE_ERROR);
  CHECK (Snp->Stop (Snp) == EFI_DEVICE_ERROR);
  CHECK (Mode.ReceiveFilterSetting == 0 && Mode.MultipleTxSupported);
  CHECK (Mode.CurrentAddress.Addr[5] == 0x56);
  CHECK (
    Snp->ReceiveFilters (
           Snp,
           EFI_SIMPLE_NETWORK_RECEIVE_UNICAST | EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST,
           0,
           FALSE,
           0,
           NULL
           ) == EFI_SUCCESS
    );

  sigprocmask (SIG_SETMASK, NULL, &Before);
  TestQueues (Snp, Private);
  TestFilters (Snp, Private);
  TestArp (Snp, &Mode, HostMac);
  TestUdpLoopback (Snp, &Mode, HostMac);
  Timer = SnpTimerNew (TimerCount, &TimerCalls, Private);
  CHECK (Timer != NULL);
  SnpTimerMod (Timer, SnpClock (NULL) / 1000000 - 1, Private);
  CHECK (Snp->GetStatus (Snp, NULL, NULL) == EFI_SUCCESS);
  CHECK (TimerCalls == 1);
  CHECK (Snp->GetStatus (Snp, NULL, NULL) == EFI_SUCCESS);
  CHECK (TimerCalls == 1);
  SnpTimerFree (Timer, Private);
  CHECK (Private->TimerCount == 0);
  CHECK (Snp->Reset (Snp, FALSE) == EFI_SUCCESS);
  CHECK (Private->RxCount == 0 && Private->TxCount == 0);
  TestArp (Snp, &Mode, HostMac);
  CHECK (Snp->Shutdown (Snp) == EFI_SUCCESS);
  CHECK (Snp->Shutdown (Snp) == EFI_DEVICE_ERROR);
  CHECK (Snp->Stop (Snp) == EFI_SUCCESS);
  sigprocmask (SIG_SETMASK, NULL, &After);
  CHECK (sigismember (&Before, SIGALRM) == sigismember (&After, SIGALRM));
  CHECK (Thunk.Close (&Thunk) == EFI_SUCCESS);
  CHECK (Thunk.Private == NULL && Thunk.Interface == NULL);
  CHECK (Thunk.Close (&Thunk) == EFI_NOT_STARTED);
  CHECK (Thunk.Open (&Thunk) == EFI_SUCCESS);
  CHECK (Thunk.Close (&Thunk) == EFI_SUCCESS);
  puts ("PASS lifecycle, timer delivery, reset, SIGALRM mask restoration, close/reopen");
  puts ("Linux SNP native backend tests passed (firmware execution is tested separately).");
  return 0;
}
