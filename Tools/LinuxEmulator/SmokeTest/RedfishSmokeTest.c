/** @file
  Exercise actual UEFI Redfish HTTP/REST EX/TCP/SNP against the local simulator.
  Only the fixed libslirp host alias is accepted. Never use this against a BMC.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <Protocol/RestEx.h>
#include <Protocol/EdkIIRedfishFeature.h>
#include <Protocol/ServiceBinding.h>
#include <Protocol/ShellParameters.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Library/JsonLib.h>
#include <Protocol/EdkIIRedfishHttpProtocol.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

STATIC EDKII_REDFISH_HTTP_PROTOCOL  *mHttp;

STATIC
EFI_STATUS
GetJson (
  IN REDFISH_SERVICE    Service,
  IN CHAR16             *Uri,
  OUT REDFISH_RESPONSE  *Response
  )
{
  EFI_STATUS  Status;

  ZeroMem (Response, sizeof (*Response));
  Status = mHttp->GetResource (mHttp, Service, Uri, NULL, Response, FALSE);
  if (EFI_ERROR (Status)) {
    Print (L"REDFISH_SMOKE: GET %s: %r\n", Uri, Status);
    return Status;
  }

  if ((Response->StatusCode == NULL) || (*Response->StatusCode != HTTP_STATUS_200_OK) ||
      (Response->Payload == NULL))
  {
    return EFI_PROTOCOL_ERROR;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                          Status;
  EFI_STATUS                          CleanupStatus;
  EFI_SHELL_PARAMETERS_PROTOCOL       *Args;
  EFI_SERVICE_BINDING_PROTOCOL        *Binding;
  EDKII_REDFISH_FEATURE_PROTOCOL      *Feature;
  EFI_REST_EX_PROTOCOL                *RestEx;
  EFI_HANDLE                          Child;
  EFI_REST_EX_HTTP_CONFIG_DATA        Config;
  EFI_HTTPv4_ACCESS_POINT             Access;
  REDFISH_CONFIG_SERVICE_INFORMATION  Information;
  REDFISH_SERVICE                     Service;
  REDFISH_RESPONSE                    Response;
  EDKII_JSON_OBJECT                   Object;
  EDKII_JSON_ARRAY                    Members;
  CONST CHAR8                         *Value;
  CHAR16                              Location[32];
  CHAR16                              SystemUri[256];
  CHAR16                              *End;
  UINTN                               Port;

  Binding = NULL;
  RestEx  = NULL;
  Child   = NULL;
  Service = NULL;
  ZeroMem (&Response, sizeof (Response));
  Status = gBS->HandleProtocol (ImageHandle, &gEfiShellParametersProtocolGuid, (VOID **)&Args);
  if (EFI_ERROR (Status) || (Args->Argc != 2)) {
    Print (L"Usage: RedfishSmokeTest.efi <local simulator port>\n");
    return EFI_INVALID_PARAMETER;
  }

  Status = StrDecimalToUintnS (Args->Argv[1], &End, &Port);
  if (EFI_ERROR (Status) || (*End != L'\0') || (Port < 1024) || (Port > 65535)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = gBS->LocateProtocol (&gEdkIIRedfishHttpProtocolGuid, NULL, (VOID **)&mHttp);
  if (EFI_ERROR (Status)) {
    Print (L"REDFISH_SMOKE: HTTP protocol unavailable: %r\n", Status);
    return Status;
  }

  Status = gBS->LocateProtocol (&gEdkIIRedfishFeatureProtocolGuid, NULL, (VOID **)&Feature);
  if (EFI_ERROR (Status)) {
    Print (L"REDFISH_SMOKE: client feature core unavailable: %r\n", Status);
    return Status;
  }

  Print (L"REDFISH_SMOKE: client feature core present\n");
  UnicodeSPrint (Location, sizeof (Location), L"10.0.2.2:%u", Port);
  Status = gBS->LocateProtocol (&gEfiRestExServiceBindingProtocolGuid, NULL, (VOID **)&Binding);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Status = Binding->CreateChild (Binding, &Child);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Status = gBS->HandleProtocol (Child, &gEfiRestExProtocolGuid, (VOID **)&RestEx);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  ZeroMem (&Config, sizeof (Config));
  ZeroMem (&Access, sizeof (Access));
  Access.LocalAddress.Addr[0]                = 10;
  Access.LocalAddress.Addr[2]                = 2;
  Access.LocalAddress.Addr[3]                = 15;
  Access.LocalSubnet.Addr[0]                 = 255;
  Access.LocalSubnet.Addr[1]                 = 255;
  Access.LocalSubnet.Addr[2]                 = 255;
  Config.HttpConfigData.HttpVersion          = HttpVersion11;
  Config.HttpConfigData.TimeOutMillisec      = 10000;
  Config.HttpConfigData.AccessPoint.IPv4Node = &Access;
  Config.SendReceiveTimeout                  = 10000;
  Status                                     = RestEx->Configure (RestEx, (EFI_REST_EX_CONFIG_DATA)&Config);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  ZeroMem (&Information, sizeof (Information));
  Information.RedfishServiceRestExHandle = Child;
  Information.RedfishServiceLocation     = Location;
  Service                                = mHttp->CreateService (mHttp, &Information);
  if (Service == NULL) {
    Status = EFI_DEVICE_ERROR;
    goto Done;
  }

  Status = GetJson (Service, L"/redfish/v1", &Response);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Object = JsonValueGetObject (mHttp->JsonInPayload (mHttp, Response.Payload));
  Value  = JsonValueGetAsciiString (JsonObjectGetValue (Object, "RedfishVersion"));
  if (Value == NULL) {
    Status = EFI_PROTOCOL_ERROR;
    goto Done;
  }

  Print (L"REDFISH_SMOKE: service root version %a\n", Value);
  mHttp->FreeResponse (mHttp, &Response);
  Status = GetJson (Service, L"/redfish/v1/Systems", &Response);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Object  = JsonValueGetObject (mHttp->JsonInPayload (mHttp, Response.Payload));
  Members = JsonValueGetArray (JsonObjectGetValue (Object, "Members"));
  Object  = JsonValueGetObject (JsonArrayGetValue (Members, 0));
  Value   = JsonValueGetAsciiString (JsonObjectGetValue (Object, "@odata.id"));
  if ((Value == NULL) || (AsciiStrnCmp (Value, "/redfish/v1/Systems/", 20) != 0) ||
      RETURN_ERROR (AsciiStrToUnicodeStrS (Value, SystemUri, ARRAY_SIZE (SystemUri))))
  {
    Status = EFI_PROTOCOL_ERROR;
    goto Done;
  }

  mHttp->FreeResponse (mHttp, &Response);
  ZeroMem (&Response, sizeof (Response));
  Status = mHttp->PatchResource (mHttp, Service, SystemUri, "{\"AssetTag\":\"LinuxEmulatorSmoke\"}", 0, NULL, &Response);
  if (EFI_ERROR (Status) || (Response.StatusCode == NULL) ||
      ((*Response.StatusCode != HTTP_STATUS_200_OK) && (*Response.StatusCode != HTTP_STATUS_204_NO_CONTENT)))
  {
    Status = EFI_PROTOCOL_ERROR;
    goto Done;
  }

  mHttp->FreeResponse (mHttp, &Response);
  Status = GetJson (Service, SystemUri, &Response);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Object = JsonValueGetObject (mHttp->JsonInPayload (mHttp, Response.Payload));
  Value  = JsonValueGetAsciiString (JsonObjectGetValue (Object, "AssetTag"));
  if ((Value == NULL) || (AsciiStrCmp (Value, "LinuxEmulatorSmoke") != 0)) {
    Status = EFI_PROTOCOL_ERROR;
    goto Done;
  }

  Status = EFI_SUCCESS;
Done:
  mHttp->FreeResponse (mHttp, &Response);
  if (Service != NULL) {
    CleanupStatus = mHttp->FreeService (mHttp, Service);
    if (!EFI_ERROR (Status) && EFI_ERROR (CleanupStatus)) {
      Status = CleanupStatus;
    }
  }

  if (RestEx != NULL) {
    CleanupStatus = RestEx->Configure (RestEx, NULL);
    if (!EFI_ERROR (Status) && EFI_ERROR (CleanupStatus)) {
      Status = CleanupStatus;
    }
  }

  if (Child != NULL) {
    CleanupStatus = Binding->DestroyChild (Binding, Child);
    if (!EFI_ERROR (Status) && EFI_ERROR (CleanupStatus)) {
      Status = CleanupStatus;
    }
  }

  if (EFI_ERROR (Status)) {
    Print (L"REDFISH_SMOKE: FAIL %r\n", Status);
  } else {
    Print (L"REDFISH_SMOKE: authenticated GET/PATCH/GET PASS\n");
  }

  return Status;
}
