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


using namespace rocksdb;

std::string kDBPath = "/tmp/myblue";

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
  ::unlink(f.c_str());
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

  return r;
}
