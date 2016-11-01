// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.
#include <cstdio>
#include <string>

#include "rocksdb/db.h"
#include "rocksdb/slice.h"
#include "rocksdb/options.h"
#include "BlueRocksEnv.h"
#include "BlueFS.h"
#include "common/errno.h"
#include "common/debug.h"

#include "include/assert.h"
#include "include/compat.h"

//#include "auth/Auth.h"
#include "common/ceph_argparse.h"
#include "common/common_init.h"
#include "common/ConfUtils.h"
#include "common/version.h"
#include "common/config.h"
#include "include/intarith.h"
#include "include/str_list.h"
#include "msg/msg_types.h"
//#include <deque>
#include <stdarg.h>
#include <stdlib.h>
#include <vector>

#include "global/global_init.h"
#include "include/stringify.h"

#define dout_subsys ceph_subsys_bdev
#undef dout_prefix
#define dout_prefix *_dout << "bdev "

using namespace rocksdb;

int myblue_path_fd = -1;

int setup_block_dev(
  string name,
  string epath,
  uint64_t size,
  bool create)
{
  dout(20) << __func__ << " name " << name << " path " << epath
	   << " size " << size << " create=" << (int)create << dendl;
  int r = 0;
  int flags = O_RDWR;

  if (create)
    flags |= O_CREAT;
  if (epath.length()) {
    r = ::symlinkat(epath.c_str(), myblue_path_fd, name.c_str());
    if (r < 0) {
      r = -errno;
      derr << __func__ << " failed to create " << name << " symlink to "
           << epath << ": " << cpp_strerror(r) << dendl;
      return r;
    }

    if (!epath.compare(0, strlen(SPDK_PREFIX), SPDK_PREFIX)) {
      int fd = ::openat(myblue_path_fd, epath.c_str(), flags, 0644);
      if (fd < 0) {
	r = -errno;
	derr << __func__ << " failed to open " << epath << " file: "
	     << cpp_strerror(r) << dendl;
	return r;
      }
      string serial_number = epath.substr(strlen(SPDK_PREFIX));
      r = ::write(fd, serial_number.c_str(), serial_number.size());
      assert(r == (int)serial_number.size());
      dout(1) << __func__ << " created " << name << " symlink to "
              << epath << dendl;
      VOID_TEMP_FAILURE_RETRY(::close(fd));
    }
  }
  if (size) {
    int fd = ::openat(myblue_path_fd, name.c_str(), flags, 0644);
    if (fd >= 0) {
      // block file is present
      struct stat st;
      int r = ::fstat(fd, &st);
      if (r == 0 &&
	  S_ISREG(st.st_mode) &&   // if it is a regular file
	  st.st_size == 0) {       // and is 0 bytes
	r = ::ftruncate(fd, size);
	if (r < 0) {
	  r = -errno;
	  derr << __func__ << " failed to resize " << name << " file to "
	       << size << ": " << cpp_strerror(r) << dendl;
	  VOID_TEMP_FAILURE_RETRY(::close(fd));
	  return r;
	}

	if (g_conf->bluestore_block_preallocate_file) {
#ifdef HAVE_POSIX_FALLOCATE
	  r = ::posix_fallocate(fd, 0, size);
	  if (r) {
	    derr << __func__ << " failed to prefallocate " << name << " file to "
	      << size << ": " << cpp_strerror(r) << dendl;
	    VOID_TEMP_FAILURE_RETRY(::close(fd));
	    return -r;
	  }
#else
	  char data[1024*128];
	  for (uint64_t off = 0; off < size; off += sizeof(data)) {
	    if (off + sizeof(data) > size)
	      r = ::write(fd, data, size - off);
	    else
	      r = ::write(fd, data, sizeof(data));
	    if (r < 0) {
	      r = -errno;
	      derr << __func__ << " failed to prefallocate w/ write " << name << " file to "
		<< size << ": " << cpp_strerror(r) << dendl;
	      VOID_TEMP_FAILURE_RETRY(::close(fd));
	      return r;
	    }
	  }
#endif
	}
	dout(1) << __func__ << " resized " << name << " file to "
		<< pretty_si_t(size) << "B" << dendl;
      }
      VOID_TEMP_FAILURE_RETRY(::close(fd));
    } else {
      int r = -errno;
      if (r != -ENOENT) {
	derr << __func__ << " failed to open " << name << " file: "
	     << cpp_strerror(r) << dendl;
	return r;
      }
    }
  }
  return 0;
}

void rm_block_dev(string f)
{
  ::unlink(f.c_str());
}

string get_temp_bdev(uint64_t size)
{
  static int n = 0;
  string fn = "ceph_test_bluefs.tmp.block." + stringify(getpid())
    + "." + stringify(++n);
  int fd = ::open(fn.c_str(), O_CREAT|O_RDWR|O_TRUNC, 0644);
  assert(fd >= 0);
  int r = ::ftruncate(fd, size);
  assert(r >= 0);
  ::close(fd);
  return fn;
}

void rm_temp_bdev(string f)
{
  ::unlinkat(myblue_path_fd, f.c_str(), 0);
}

int main(int argc, char **argv) {
  DB* db;
  Options options;
  Env *env = NULL;
  BlueFS *bluefs = NULL;

  int r = 0;
  vector<const char*> args;
  Status s;
  std::string value;
  uuid_d fsid;
  uint64_t size = 1048576 * 1024; //5GiB
  std::string fn = get_temp_bdev(size);


  // init the environment, so that logs will be print normally.
  ::argv_to_vec(argc, (const char**)argv, args);
  ::env_to_vec(args);
  ::global_init(NULL, args, CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_UTILITY, 0);


  // Optimize RocksDB. This is the easiest way to get RocksDB to perform well
  options.IncreaseParallelism();
  options.OptimizeLevelStyleCompaction();
  // create the DB if it's not already present
  options.create_if_missing = true;

  // init bluefs
  bluefs = new BlueFS;
  bluefs->add_block_device(BlueFS::BDEV_DB, fn);
  bluefs->add_block_extent(BlueFS::BDEV_DB, 1048576, size - 1048576);
  bluefs->mkfs(fsid);
  r = bluefs->mount();
  if (r < 0) {
    std::cout << __func__ << " failed bluefs mouunt: " << cpp_strerror(r) << std::endl;
    goto free_bluefs;
  }

  // add bluefs as the backend
  env = new BlueRocksEnv(bluefs);
  options.env = env;

  // open DB
  s = DB::Open(options, fn, &db);
  assert(s.ok());

  // Put key-value
  s = db->Put(WriteOptions(), "key1", "value");
  assert(s.ok());

  // get value
  s = db->Get(ReadOptions(), "key1", &value);
  assert(s.ok());
  assert(value == "value");

  // atomically apply a set of updates
  {
    WriteBatch batch;
    batch.Delete("key1");
    batch.Put("key2", value);
    s = db->Write(WriteOptions(), &batch);
  }

  s = db->Get(ReadOptions(), "key1", &value);
  assert(s.IsNotFound());

  db->Get(ReadOptions(), "key2", &value);
  assert(value == "value");


  delete db;
  delete env;
  env = NULL;
  // umount_bluefs:
  bluefs->umount();
  rm_temp_bdev(fn);
 free_bluefs:
  assert(bluefs);
  delete bluefs;
  bluefs = NULL;


  std::string path = "/home/fwu/myblue";
  myblue_path_fd = ::open(path.c_str(), O_DIRECTORY);
  setup_block_dev("ssd_block", g_conf->bluestore_block_path, 
		   g_conf->bluestore_block_size,
		   g_conf->bluestore_block_create);
  rm_block_dev("ssd_block");
  ::close(myblue_path_fd);
  return r;
}
