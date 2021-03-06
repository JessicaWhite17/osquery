/*
 *  Copyright (c) 2014, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include <map>
#include <set>

#include <libproc.h>
#include <sys/sysctl.h>

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/trim.hpp>

#include <osquery/core.h>
#include <osquery/filesystem.h>
#include <osquery/logger.h>
#include <osquery/tables.h>

namespace osquery {
namespace tables {

std::set<int> getProcList(const QueryContext &context) {
  std::set<int> pidlist;
  if (context.constraints.count("pid") > 0 &&
      context.constraints.at("pid").exists()) {
    pidlist = context.constraints.at("pid").getAll<int>(EQUALS);
  }

  // No equality matches, get all pids.
  if (!pidlist.empty()) {
    return pidlist;
  }

  int bufsize = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
  if (bufsize <= 0) {
    VLOG(1) << "An error occurred retrieving the process list";
    return pidlist;
  }

  // arbitrarily create a list with 2x capacity in case more processes have
  // been loaded since the last proc_listpids was executed
  pid_t pids[2 * bufsize / sizeof(pid_t)];

  // now that we've allocated "pids", let's overwrite num_pids with the actual
  // amount of data that was returned for proc_listpids when we populate the
  // pids data structure
  bufsize = proc_listpids(PROC_ALL_PIDS, 0, pids, sizeof(pids));
  if (bufsize <= 0) {
    VLOG(1) << "An error occurred retrieving the process list";
    return pidlist;
  }

  int num_pids = bufsize / sizeof(pid_t);
  for (int i = 0; i < num_pids; ++i) {
    // if the pid is negative or 0, it doesn't represent a real process so
    // continue the iterations so that we don't add it to the results set
    if (pids[i] <= 0) {
      continue;
    }
    pidlist.insert(pids[i]);
  }

  return pidlist;
}

std::map<int, int> getParentMap(std::set<int> &pidlist) {
  std::map<int, int> pidmap;

  struct kinfo_proc proc;
  size_t size = sizeof(proc);

  for (const auto &pid : pidlist) {
    int name[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    if (sysctl((int *)name, 4, &proc, &size, NULL, 0) == -1) {
      break;
    }

    if (size > 0) {
      pidmap[pid] = (int)proc.kp_eproc.e_ppid;
    }
  }

  return pidmap;
}

inline std::string getProcPath(int pid) {
  char path[PROC_PIDPATHINFO_MAXSIZE] = "\0";
  int bufsize = proc_pidpath(pid, path, sizeof(path));
  if (bufsize <= 0) {
    path[0] = '\0';
  }

  return std::string(path);
}

struct proc_cred {
  struct {
    uid_t uid;
    gid_t gid;
  } real, effective;
};

inline bool getProcCred(int pid, proc_cred &cred) {
  struct proc_bsdshortinfo bsdinfo;

  if (proc_pidinfo(pid, PROC_PIDT_SHORTBSDINFO, 0, &bsdinfo, sizeof(bsdinfo)) ==
      sizeof(bsdinfo)) {
    cred.real.uid = bsdinfo.pbsi_ruid;
    cred.real.gid = bsdinfo.pbsi_ruid;
    cred.effective.uid = bsdinfo.pbsi_uid;
    cred.effective.gid = bsdinfo.pbsi_gid;
    return true;
  }
  return false;
}

// Get the max args space
static int genMaxArgs() {
  int mib[2] = {CTL_KERN, KERN_ARGMAX};

  int argmax = 0;
  size_t size = sizeof(argmax);
  if (sysctl(mib, 2, &argmax, &size, NULL, 0) == -1) {
    VLOG(1) << "An error occurred retrieving the max arg size";
    return 0;
  }

  return argmax;
}

void genProcRootAndCWD(int pid, Row &r) {
  r["cwd"] = "";
  r["root"] = "";

  struct proc_vnodepathinfo pathinfo;
  if (proc_pidinfo(
          pid, PROC_PIDVNODEPATHINFO, 0, &pathinfo, sizeof(pathinfo)) ==
      sizeof(pathinfo)) {
    if (pathinfo.pvi_cdir.vip_vi.vi_stat.vst_dev != 0) {
      r["cwd"] = std::string(pathinfo.pvi_cdir.vip_path);
    }

    if (pathinfo.pvi_rdir.vip_vi.vi_stat.vst_dev != 0) {
      r["root"] = std::string(pathinfo.pvi_rdir.vip_path);
    }
  }
}

std::vector<std::string> getProcRawArgs(int pid, size_t argmax) {
  std::vector<std::string> args;
  uid_t euid = geteuid();

  char procargs[argmax];
  const char *cp = procargs;
  int mib[3] = {CTL_KERN, KERN_PROCARGS2, pid};

  if (sysctl(mib, 3, &procargs, &argmax, NULL, 0) == -1) {
    if (euid == 0) {
      VLOG(1) << "An error occurred retrieving the env for pid: " << pid;
    }

    return args;
  }

  // Here we make the assertion that we are interested in all non-empty strings
  // in the proc args+env
  do {
    std::string s = std::string(cp);
    if (s.length() > 0) {
      args.push_back(s);
    }
    cp += args.back().size() + 1;
  } while (cp < procargs + argmax);
  return args;
}

std::map<std::string, std::string> getProcEnv(int pid, size_t argmax) {
  std::map<std::string, std::string> env;
  auto args = getProcRawArgs(pid, argmax);

  // Since we know that all envs will have an = sign and are at the end of the
  // list, we iterate from the end forward until we stop seeing = signs.
  // According to the // ps source, there is no programmatic way to know where
  // args stop and env begins, so args at the end of a command string which
  // contain "=" may erroneously appear as env vars.
  for (auto itr = args.rbegin(); itr < args.rend(); ++itr) {
    size_t idx = itr->find_first_of("=");
    if (idx == std::string::npos) {
      break;
    }
    std::string key = itr->substr(0, idx);
    std::string value = itr->substr(idx + 1);
    env[key] = value;
  }

  return env;
}

std::vector<std::string> getProcArgs(int pid, size_t argmax) {
  auto raw_args = getProcRawArgs(pid, argmax);
  std::vector<std::string> args;
  bool collect = false;

  // Iterate from the back until we stop seeing environment vars
  // Then start pushing args (in reverse order) onto a vector.
  // We trim the args of leading/trailing whitespace to make
  // analysis easier.
  for (auto itr = raw_args.rbegin(); itr < raw_args.rend(); ++itr) {
    if (collect) {
      std::string arg = *itr;
      boost::algorithm::trim(arg);
      args.push_back(arg);
    } else {
      size_t idx = itr->find_first_of("=");
      if (idx == std::string::npos) {
        collect = true;
      }
    }
  }

  // We pushed them on backwards, so we need to fix that.
  std::reverse(args.begin(), args.end());

  return args;
}

QueryData genProcesses(QueryContext &context) {
  QueryData results;

  auto pidlist = getProcList(context);
  auto parent_pid = getParentMap(pidlist);
  int argmax = genMaxArgs();

  for (auto &pid : pidlist) {
    if (!context.constraints["pid"].matches<int>(pid)) {
      // Optimize by not searching when a pid is a constraint.
      continue;
    }

    Row r;
    r["pid"] = INTEGER(pid);
    r["path"] = getProcPath(pid);
    // OS X proc_name only returns 16 bytes, use the basename of the path.
    r["name"] = boost::filesystem::path(r["path"]).filename().string();

    // The command line invocation including arguments.
    std::string cmdline = boost::algorithm::join(getProcArgs(pid, argmax), " ");
    boost::algorithm::trim(cmdline);
    r["cmdline"] = cmdline;
    genProcRootAndCWD(pid, r);

    proc_cred cred;
    if (getProcCred(pid, cred)) {
      r["uid"] = BIGINT(cred.real.uid);
      r["gid"] = BIGINT(cred.real.gid);
      r["euid"] = BIGINT(cred.effective.uid);
      r["egid"] = BIGINT(cred.effective.gid);
    } else {
      r["uid"] = "-1";
      r["gid"] = "-1";
      r["euid"] = "-1";
      r["egid"] = "-1";
    }

    // Find the parent process.
    const auto parent_it = parent_pid.find(pid);
    if (parent_it != parent_pid.end()) {
      r["parent"] = INTEGER(parent_it->second);
    } else {
      r["parent"] = "-1";
    }

    // If the path of the executable that started the process is available and
    // the path exists on disk, set on_disk to 1. If the path is not
    // available, set on_disk to -1. If, and only if, the path of the
    // executable is available and the file does NOT exist on disk, set on_disk
    // to 0.
    r["on_disk"] = osquery::pathExists(r["path"]).toString();

    // systems usage and time information
    struct rusage_info_v2 rusage_info_data;
    int rusage_status = proc_pid_rusage(
        pid, RUSAGE_INFO_V2, (rusage_info_t *)&rusage_info_data);
    // proc_pid_rusage returns -1 if it was unable to gather information
    if (rusage_status == 0) {
      // size/memory information
      r["wired_size"] = TEXT(rusage_info_data.ri_wired_size);
      r["resident_size"] = TEXT(rusage_info_data.ri_resident_size);
      r["phys_footprint"] = TEXT(rusage_info_data.ri_phys_footprint);

      // time information
      r["user_time"] = TEXT(rusage_info_data.ri_user_time / 1000000);
      r["system_time"] = TEXT(rusage_info_data.ri_system_time / 1000000);
      r["start_time"] = TEXT(rusage_info_data.ri_proc_start_abstime);
    } else {
      r["wired_size"] = "-1";
      r["resident_size"] = "-1";
      r["phys_footprint"] = "-1";
      r["user_time"] = "-1";
      r["system_time"] = "-1";
      r["start_time"] = "-1";
    }

    results.push_back(r);
  }

  return results;
}

QueryData genProcessEnvs(QueryContext &context) {
  QueryData results;

  auto pidlist = getProcList(context);
  int argmax = genMaxArgs();
  for (auto &pid : pidlist) {
    if (!context.constraints["pid"].matches<int>(pid)) {
      // Optimize by not searching when a pid is a constraint.
      continue;
    }

    auto env = getProcEnv(pid, argmax);
    for (auto env_itr = env.begin(); env_itr != env.end(); ++env_itr) {
      Row r;

      r["pid"] = INTEGER(pid);
      r["key"] = env_itr->first;
      r["value"] = env_itr->second;

      results.push_back(r);
    }
  }

  return results;
}
}
}
