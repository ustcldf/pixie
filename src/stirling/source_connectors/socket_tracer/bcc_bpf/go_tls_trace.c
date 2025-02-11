/*
 * This code runs using bpf in the Linux kernel.
 * Copyright 2018- The Pixie Authors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

// LINT_C_FILE: Do not remove this line. It ensures cpplint treats this as a C file.

#include "src/stirling/source_connectors/socket_tracer/bcc_bpf/go_trace_common.h"
#include "src/stirling/source_connectors/socket_tracer/bcc_bpf/macros.h"
#include "src/stirling/source_connectors/socket_tracer/bcc_bpf_intf/symaddrs.h"

// Key: TGID
// Value: Symbol addresses for the binary with that TGID.
BPF_HASH(go_tls_symaddrs_map, uint32_t, struct go_tls_symaddrs_t);

struct go_tls_conn_args {
  void* conn_ptr;
  char* plaintext_ptr;
};

// Key is tgid + goid (goroutine id).
// Value is a pointer to the argument to the crypto/tls.(*Conn) Write and Read functions.
// This map is used to connect arguments to return values.
BPF_HASH(active_tls_conn_op_map, struct tgid_goid_t, struct go_tls_conn_args);

// Probe for the crypto/tls library's write.
//
// Function signature:
//   func (c *Conn) Write(b []byte) (int, error)
//
// Symbol:
//   crypto/tls.(*Conn).Write
int probe_entry_tls_conn_write(struct pt_regs* ctx) {
  uint64_t id = bpf_get_current_pid_tgid();
  uint32_t tgid = id >> 32;
  uint32_t pid = id;

  int64_t* goid_ptr = get_goid(tgid, pid);
  if (goid_ptr == NULL) {
    return 0;
  }

  struct tgid_goid_t tgid_goid = {};
  tgid_goid.tgid = tgid;
  tgid_goid.goid = *goid_ptr;

  struct go_tls_symaddrs_t* symaddrs = go_tls_symaddrs_map.lookup(&tgid);
  if (symaddrs == NULL) {
    return 0;
  }

  // Required argument offsets.
  REQUIRE_LOCATION(symaddrs->Write_c_loc, 0);
  REQUIRE_LOCATION(symaddrs->Write_b_loc, 0);

  // ---------------------------------------------
  // Extract arguments (on stack)
  // ---------------------------------------------

  const void* sp = (const void*)ctx->sp;

  struct go_tls_conn_args args;
  bpf_probe_read(&args.conn_ptr, sizeof(void*), sp + symaddrs->Write_c_loc.offset);
  bpf_probe_read(&args.plaintext_ptr, sizeof(char*), sp + symaddrs->Write_b_loc.offset);

  active_tls_conn_op_map.update(&tgid_goid, &args);

  return 0;
}

static __inline int probe_return_tls_conn_write_core(struct pt_regs* ctx, uint64_t id,
                                                     uint32_t tgid, struct go_tls_conn_args* args) {
  struct go_tls_symaddrs_t* symaddrs = go_tls_symaddrs_map.lookup(&tgid);
  if (symaddrs == NULL) {
    return 0;
  }

  REQUIRE_LOCATION(symaddrs->Write_retval0_loc, 0);
  REQUIRE_LOCATION(symaddrs->Write_retval1_loc, 0);

  const void* sp = (const void*)ctx->sp;

  int64_t retval0;
  bpf_probe_read(&retval0, sizeof(retval0), sp + symaddrs->Write_retval0_loc.offset);

  struct go_interface retval1;
  bpf_probe_read(&retval1, sizeof(retval1), sp + symaddrs->Write_retval1_loc.offset);

  // If function returns an error, then there's no data to trace.
  if (retval1.ptr != 0) {
    return 0;
  }

  struct go_common_symaddrs_t* common_symaddrs = go_common_symaddrs_map.lookup(&tgid);
  if (common_symaddrs == NULL) {
    return 0;
  }

  // To call get_fd_from_conn_intf, cast the conn_ptr into a go_interface.
  struct go_interface conn_intf;
  conn_intf.type = common_symaddrs->tls_Conn;
  conn_intf.ptr = args->conn_ptr;
  int fd = get_fd_from_conn_intf_core(conn_intf, common_symaddrs);
  if (fd == kInvalidFD) {
    return 0;
  }

  set_conn_as_ssl(tgid, fd);

  struct data_args_t data_args;
  data_args.source_fn = kGoTLSConnWrite;
  data_args.buf = args->plaintext_ptr;
  data_args.msg_len = 0;  // Unused.
  data_args.fd = fd;

  process_data(/* vecs */ false, ctx, id, kEgress, &data_args, retval0, /* ssl */ true);

  return 0;
}

int probe_return_tls_conn_write(struct pt_regs* ctx) {
  uint64_t id = bpf_get_current_pid_tgid();
  uint32_t tgid = id >> 32;
  uint32_t pid = id;

  int64_t* goid_ptr = get_goid(tgid, pid);
  if (goid_ptr == NULL) {
    return 0;
  }

  struct tgid_goid_t tgid_goid = {};
  tgid_goid.tgid = tgid;
  tgid_goid.goid = *goid_ptr;

  struct go_tls_conn_args* args = active_tls_conn_op_map.lookup(&tgid_goid);
  if (args == NULL) {
    return 0;
  }

  probe_return_tls_conn_write_core(ctx, id, tgid, args);

  active_tls_conn_op_map.delete(&tgid_goid);

  return 0;
}

// Probe for the crypto/tls library's read.
//
// Function signature:
//   func (c *Conn) Read(b []byte) (int, error)
//
// Symbol:
//   crypto/tls.(*Conn).Read
int probe_entry_tls_conn_read(struct pt_regs* ctx) {
  uint64_t id = bpf_get_current_pid_tgid();
  uint32_t tgid = id >> 32;
  uint32_t pid = id;

  int64_t* goid_ptr = get_goid(tgid, pid);
  if (goid_ptr == NULL) {
    return 0;
  }

  struct tgid_goid_t tgid_goid = {};
  tgid_goid.tgid = tgid;
  tgid_goid.goid = *goid_ptr;

  //  bpf_trace_printk("probe_entry_tls_conn_read id=%llx goid=%lld sp=%llx\n", id, *goid_ptr,
  //  ctx->sp);

  struct go_tls_symaddrs_t* symaddrs = go_tls_symaddrs_map.lookup(&tgid);
  if (symaddrs == NULL) {
    return 0;
  }

  // Required argument offsets.
  REQUIRE_LOCATION(symaddrs->Read_c_loc, 0);
  REQUIRE_LOCATION(symaddrs->Read_b_loc, 0);

  // ---------------------------------------------
  // Extract arguments (on stack)
  // ---------------------------------------------

  const void* sp = (const void*)ctx->sp;

  struct go_tls_conn_args args;
  bpf_probe_read(&args.conn_ptr, sizeof(void*), sp + symaddrs->Read_c_loc.offset);
  bpf_probe_read(&args.plaintext_ptr, sizeof(char*), sp + symaddrs->Read_b_loc.offset);

  active_tls_conn_op_map.update(&tgid_goid, &args);

  return 0;
}

static __inline int probe_return_tls_conn_read_core(struct pt_regs* ctx, uint64_t id, uint32_t tgid,
                                                    struct go_tls_conn_args* args) {
  struct go_tls_symaddrs_t* symaddrs = go_tls_symaddrs_map.lookup(&tgid);
  if (symaddrs == NULL) {
    return 0;
  }

  REQUIRE_LOCATION(symaddrs->Read_retval0_loc, 0);
  REQUIRE_LOCATION(symaddrs->Read_retval1_loc, 0);

  const void* sp = (const void*)ctx->sp;

  int64_t retval0;
  bpf_probe_read(&retval0, sizeof(retval0), sp + symaddrs->Read_retval0_loc.offset);

  struct go_interface retval1;
  bpf_probe_read(&retval1, sizeof(retval1), sp + symaddrs->Read_retval1_loc.offset);

  // If function returns an error, then there's no data to trace.
  if (retval1.ptr != 0) {
    return 0;
  }

  // To call get_fd_from_conn_intf, cast the conn_ptr into a go_interface.
  // TODO(oazizi): Consider changing get_fd_from_conn_intf so this is not required.

  struct go_common_symaddrs_t* common_symaddrs = go_common_symaddrs_map.lookup(&tgid);
  if (common_symaddrs == NULL) {
    return 0;
  }

  struct go_interface conn_intf;
  conn_intf.type = common_symaddrs->tls_Conn;
  conn_intf.ptr = args->conn_ptr;
  int fd = get_fd_from_conn_intf_core(conn_intf, common_symaddrs);
  if (fd == kInvalidFD) {
    return 0;
  }

  set_conn_as_ssl(tgid, fd);

  struct data_args_t data_args;
  data_args.source_fn = kGoTLSConnRead;
  data_args.buf = args->plaintext_ptr;
  data_args.msg_len = 0;  // Unused.
  data_args.fd = fd;

  process_data(/* vecs */ false, ctx, id, kIngress, &data_args, retval0, /* ssl */ true);

  return 0;
}

int probe_return_tls_conn_read(struct pt_regs* ctx) {
  uint64_t id = bpf_get_current_pid_tgid();
  uint32_t tgid = id >> 32;
  uint32_t pid = id;

  int64_t* goid_ptr = get_goid(tgid, pid);
  if (goid_ptr == NULL) {
    return 0;
  }

  struct tgid_goid_t tgid_goid = {};
  tgid_goid.tgid = tgid;
  tgid_goid.goid = *goid_ptr;

  struct go_tls_conn_args* args = active_tls_conn_op_map.lookup(&tgid_goid);
  if (args == NULL) {
    return 0;
  }

  probe_return_tls_conn_read_core(ctx, id, tgid, args);

  active_tls_conn_op_map.delete(&tgid_goid);

  return 0;
}
