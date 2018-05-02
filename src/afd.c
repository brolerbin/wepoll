#include <malloc.h>
#include <stdlib.h>

#include "afd.h"
#include "error.h"
#include "nt.h"
#include "util.h"
#include "win.h"
#include "ws.h"

#define FILE_DEVICE_NETWORK 0x00000012
#define METHOD_BUFFERED 0
#define AFD_POLL 9

#define _AFD_CONTROL_CODE(operation, method) \
  ((FILE_DEVICE_NETWORK) << 12 | (operation << 2) | method)

#define IOCTL_AFD_POLL _AFD_CONTROL_CODE(AFD_POLL, METHOD_BUFFERED)

#define _AFD_ANY_PROTOCOL -1

/* clang-format off */
static const GUID _AFD_PROVIDER_GUID_LIST[] = {
  /* MSAFD Tcpip [TCP+UDP+RAW / IP] */
  {0xe70f1aa0, 0xab8b, 0x11cf,
   {0x8c, 0xa3, 0x00, 0x80, 0x5f, 0x48, 0xa1, 0x92}},
  /* MSAFD Tcpip [TCP+UDP+RAW / IPv6] */
  {0xf9eab0c0, 0x26d4, 0x11d0,
   {0xbb, 0xbf, 0x00, 0xaa, 0x00, 0x6c, 0x34, 0xe4}},
  /* MSAFD RfComm [Bluetooth] */
  {0x9fc48064, 0x7298, 0x43e4,
   {0xb7, 0xbd, 0x18, 0x1f, 0x20, 0x89, 0x79, 0x2a}},
  /* MSAFD Irda [IrDA] */
  {0x3972523d, 0x2af1, 0x11d1,
   {0xb6, 0x55, 0x00, 0x80, 0x5f, 0x36, 0x42, 0xcc}}};
/* clang-format on */

/* This protocol info record is used by afd_create_driver_socket() to create
 * sockets that can be used as the first argument to afd_poll(). It is
 * populated on startup by afd_global_init().
 */
static WSAPROTOCOL_INFOW _afd_driver_socket_template;

static const WSAPROTOCOL_INFOW* _afd_find_protocol_info(
    const WSAPROTOCOL_INFOW* infos, size_t infos_count, int protocol_id) {
  size_t i, j;

  for (i = 0; i < infos_count; i++) {
    const WSAPROTOCOL_INFOW* info = &infos[i];

    /* Apply protocol id filter. */
    if (protocol_id != _AFD_ANY_PROTOCOL && protocol_id != info->iProtocol)
      continue;

    /* Filter out non-MSAFD protocols. */
    for (j = 0; j < array_count(_AFD_PROVIDER_GUID_LIST); j++) {
      if (memcmp(&info->ProviderId,
                 &_AFD_PROVIDER_GUID_LIST[j],
                 sizeof info->ProviderId) == 0)
        return info;
    }
  }

  return NULL; /* Not found. */
}

int afd_global_init(void) {
  WSAPROTOCOL_INFOW* infos;
  ssize_t infos_count;
  const WSAPROTOCOL_INFOW* afd_info;

  /* Load the winsock catalog. */
  infos_count = ws_get_protocol_catalog(&infos);
  if (infos_count < 0)
    return_error(-1);

  /* Find a WSAPROTOCOL_INDOW structure that we can use to create an MSAFD
   * socket. Preferentially we pick a UDP socket, otherwise try TCP or any
   * other type.
   */
  do {
    afd_info = _afd_find_protocol_info(infos, infos_count, IPPROTO_UDP);
    if (afd_info != NULL)
      break;

    afd_info = _afd_find_protocol_info(infos, infos_count, IPPROTO_TCP);
    if (afd_info != NULL)
      break;

    afd_info = _afd_find_protocol_info(infos, infos_count, _AFD_ANY_PROTOCOL);
    if (afd_info != NULL)
      break;

    free(infos);
    return_error(-1, WSAENETDOWN); /* No suitable protocol found. */
  } while (0);

  /* Copy found protocol information from the catalog to a static buffer. */
  _afd_driver_socket_template = *afd_info;

  free(infos);
  return 0;
}

int afd_create_driver_socket(HANDLE iocp, SOCKET* driver_socket_out) {
  SOCKET socket;

  socket = WSASocketW(_afd_driver_socket_template.iAddressFamily,
                      _afd_driver_socket_template.iSocketType,
                      _afd_driver_socket_template.iProtocol,
                      &_afd_driver_socket_template,
                      0,
                      WSA_FLAG_OVERLAPPED);
  if (socket == INVALID_SOCKET)
    return_error(-1);

  /* TODO: use WSA_FLAG_NOINHERIT on Windows versions that support it. */
  if (!SetHandleInformation((HANDLE) socket, HANDLE_FLAG_INHERIT, 0))
    goto error;

  if (CreateIoCompletionPort((HANDLE) socket, iocp, 0, 0) == NULL)
    goto error;

  *driver_socket_out = socket;
  return 0;

error:;
  DWORD error = GetLastError();
  closesocket(socket);
  return_error(-1, error);
}

int afd_poll(SOCKET driver_socket,
             AFD_POLL_INFO* poll_info,
             OVERLAPPED* overlapped) {
  IO_STATUS_BLOCK iosb;
  IO_STATUS_BLOCK* iosb_ptr;
  HANDLE event = NULL;
  void* apc_context;
  NTSTATUS status;

  if (overlapped != NULL) {
    /* Overlapped operation. */
    iosb_ptr = (IO_STATUS_BLOCK*) &overlapped->Internal;
    event = overlapped->hEvent;

    /* Do not report iocp completion if hEvent is tagged. */
    if ((uintptr_t) event & 1) {
      event = (HANDLE)((uintptr_t) event & ~(uintptr_t) 1);
      apc_context = NULL;
    } else {
      apc_context = overlapped;
    }

  } else {
    /* Blocking operation. */
    iosb_ptr = &iosb;
    event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (event == NULL)
      return_error(-1);
    apc_context = NULL;
  }

  iosb_ptr->Status = STATUS_PENDING;
  status = NtDeviceIoControlFile((HANDLE) driver_socket,
                                 event,
                                 NULL,
                                 apc_context,
                                 iosb_ptr,
                                 IOCTL_AFD_POLL,
                                 poll_info,
                                 sizeof *poll_info,
                                 poll_info,
                                 sizeof *poll_info);

  if (overlapped == NULL) {
    /* If this is a blocking operation, wait for the event to become
     * signaled, and then grab the real status from the io status block.
     */
    if (status == STATUS_PENDING) {
      DWORD r = WaitForSingleObject(event, INFINITE);

      if (r == WAIT_FAILED) {
        DWORD error = GetLastError();
        CloseHandle(event);
        return_error(-1, error);
      }

      status = iosb_ptr->Status;
    }

    CloseHandle(event);
  }

  if (status == STATUS_SUCCESS)
    return 0;
  else if (status == STATUS_PENDING)
    return_error(-1, ERROR_IO_PENDING);
  else
    return_error(-1, RtlNtStatusToDosError(status));
}

static int _afd_get_protocol_info(SOCKET socket,
                                  WSAPROTOCOL_INFOW* protocol_info) {
  int opt_len;
  size_t i;

  opt_len = sizeof *protocol_info;
  if (getsockopt(socket,
                 SOL_SOCKET,
                 SO_PROTOCOL_INFOW,
                 (char*) protocol_info,
                 &opt_len) != 0)
    return_error(-1);

  for (i = 0; i < array_count(_AFD_PROVIDER_GUID_LIST); i++) {
    if (memcmp(&protocol_info->ProviderId,
               &_AFD_PROVIDER_GUID_LIST[i],
               sizeof protocol_info->ProviderId) == 0) {
      return 0;
    }
  }

  /* Socket doesn't appear to be controlled by MSAFD. */
  return_error(-1, ERROR_DEVICE_FEATURE_NOT_SUPPORTED);
}

WEPOLL_INTERNAL int afd_get_protocol_info(SOCKET socket,
                                          SOCKET* afd_socket_out,
                                          WSAPROTOCOL_INFOW* protocol_info) {
  SOCKET afd_socket;
  int r;

  /* Try to get protocol information, assuming that the given socket is an AFD
   * socket. This should almost always be the case, and if it is, that saves us
   * a call to WSAIoctl(). */
  afd_socket = socket;
  r = _afd_get_protocol_info(afd_socket, protocol_info);

  if (r < 0) {
    /* If getting protocol information failed, it might be due to the socket
     * not being an AFD socket. If so, attempt to fetch the underlying base
     * socket, then try again to obtain protocol information. */
    DWORD error = GetLastError();
    if (error != ERROR_DEVICE_FEATURE_NOT_SUPPORTED)
      return -1;

    afd_socket = ws_get_base_socket(socket);
    if (afd_socket == INVALID_SOCKET || afd_socket == socket)
      return_error(-1, error);

    r = _afd_get_protocol_info(afd_socket, protocol_info);
    if (r < 0)
      return -1;
  }

  *afd_socket_out = afd_socket;
  return r;
}
