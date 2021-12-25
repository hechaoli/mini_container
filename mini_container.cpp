#include <limits.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include <boost/program_options.hpp>
#include <fstream>
#include <iostream>
#include <sstream>

#define NIS_DOMAIN_NAME_MAX (64)

void errExit(const char* msg) {
  perror(msg);
  exit(EXIT_FAILURE);
}

namespace po = boost::program_options;

const std::string kDefaultBridgeName = "br0";
const std::string kDefaultBridgeIp = "10.0.0.1";
const std::string kDefaultBridgePrefixLen = "16";

// The code assumes this cgroup already exists and all controllers are enabled.
const std::string kCgroupRoot = "/sys/fs/cgroup/mini_container/";
bool verbose = false;

struct ResourceLimit {
  long long maxRamBytes;
  ResourceLimit() : maxRamBytes(0) {}
};

std::string getHostname() {
  char hostname[HOST_NAME_MAX];
  if (gethostname(hostname, HOST_NAME_MAX) != 0) {
    errExit("gethostname");
  }
  return std::string(hostname);
}

std::string getNisDomainName() {
  char domainname[NIS_DOMAIN_NAME_MAX];
  if (getdomainname(domainname, NIS_DOMAIN_NAME_MAX) != 0) {
    errExit("getdomainname");
  }
  return std::string(domainname);
}

void runContainer(const std::string& cmd) {
  std::istringstream iss(cmd);
  std::vector<std::string> tokens{std::istream_iterator<std::string>{iss},
                                  std::istream_iterator<std::string>{}};
  std::vector<char*> args;

  for (const auto& arg : tokens) {
    args.push_back(const_cast<char*>(arg.c_str()));
  }
  args.push_back(nullptr);

  if (verbose) {
    std::cout << "[Container] Running command: " << cmd << std::endl;
    std::cout << "[Container] Container hostname: " << getHostname()
              << std::endl;
    std::cout << "[Container] Container NIS domain name: "
              << getNisDomainName() << std::endl;
    execv(tokens[0].c_str(), args.data());
  }

  errExit("execv failed");  // Only reached if execv() fails
}

void setupFilesystem(const std::string& rootfs) {
  if (rootfs.empty()) {
    return;
  }
  // (1) Create a new namespace
  if (unshare(CLONE_NEWNS) == -1) {
    errExit("unshare(CLONE_NEWNS)");
  }
  // (2) Change the propagation type of all mount points to MS_SLAVE.
  // Equivalent to "mount --make-rslave /"
  if (mount(
        "" /* source: IGNORED */,
        "/" /* target */,
        nullptr /* filesystemtype: IGNORED*/,
        MS_SLAVE | MS_REC /* mountflags */,
        nullptr /* data: IGNORED*/) == -1) {
    errExit("mount(/, MS_SLAVE | MS_REC)");
  }

  // (3) Bind mount rootfs to itself so that it becomes a mount point.
  // Because the source of a mount move must be a mount point.
  if (mount(
        rootfs.c_str() /* source */,
        rootfs.c_str() /* target */,
        nullptr /* filesystemtype: IGNORED*/,
        MS_BIND | MS_REC /* mountflags */,
        nullptr /* data: IGNORED*/) == -1) {
    errExit("mount(rootfs, rootfs, MS_BIND | MS_REC)");
  }

  // (4) Enter rootfs
  if (chdir(rootfs.c_str()) == -1) {
    errExit("chdir(rootfs)");
  }
  // (5) Mount move rootfs to "/".
  if (mount(
        rootfs.c_str() /* source */,
        "/" /* target */,
        nullptr /* filesystemtype: IGNORED*/,
        MS_MOVE /* mountflags */,
        nullptr /* data: IGNORED*/) == -1) {
    errExit("mount(rootfs, /, MS_MOVE)");
  }
  // (6) Change the container's root to rootfs
  if (chroot(".") == -1) {
    errExit("chroot(\".\")");
  }
  // (7) Change current directory to "/"
  if (chdir("/") == -1) {
    errExit("chdir(\"/\")");
  }
  // (8) Let any changes in the container propagae to its children if any
  if (mount(
        "" /* source: IGNORED */,
        "/" /* target */,
        nullptr /* filesystemtype: IGNORED*/,
        MS_SHARED | MS_REC /* mountflags */,
        nullptr /* data: IGNORED*/) == -1) {
    errExit("mount(/, MS_SHARED | MS_REC)");
  }
  // (9) Mount procfs for the container
  if (mount(
        "proc" /* source */,
        "/proc" /* target */,
        "proc" /* filesystemtype */,
        MS_NOSUID | MS_NOEXEC | MS_NODEV /* mountflags */,
        nullptr /* data: IGNORED*/) == -1) {
    errExit("mount(proc, /proc, MS_NOSUID | MS_NOEXEC | MS_NODEV)");
  }
}

void setHostAndDomainName(
    const std::string& hostname,
    const std::string& nisDomainName) {
  if (!hostname.empty() &&
      sethostname(hostname.c_str(), hostname.length()) != 0) {
    errExit("sethostname");
  }
  if (!nisDomainName.empty() &&
      setdomainname(nisDomainName.c_str(), nisDomainName.length()) != 0) {
    errExit("setdomainname");
  }
}

// Called in parent (agent) process.
void prepareNetwork(int containerPid) {
  // (1) Create the default bridge if it doesn't exist.
  std::string cmd = "ip link add name " + kDefaultBridgeName + " type bridge";
  system(cmd.c_str());

  // (2) Make sure the bridge is up.
  cmd = "ip link set " + kDefaultBridgeName + " up";
  if (system(cmd.c_str()) != 0) {
    errExit("setting default bridge up failed");
  }

  // (3) Add IP to the bridge if it doesn't exist.
  cmd = "ip addr add " + kDefaultBridgeIp + "/" + kDefaultBridgePrefixLen +
        " brd + dev " + kDefaultBridgeName;
  system(cmd.c_str());

  // (4) Create a veth pair between host and container
  // The veth interface name on the host is int the format "veth<container_pid>"
  const std::string vethName = "veth" + std::to_string(containerPid);
  cmd = "ip link add " + vethName + " type veth peer name eth0 netns " +
        std::to_string(containerPid);
  if (system(cmd.c_str()) != 0) {
    errExit("adding veth pair failed");
  }

  // (5) Bring up the veth interface
  cmd = "ip link set " + vethName + " up";
  if (system(cmd.c_str()) != 0) {
    errExit("setting veth up failed");
  }

  // (6) Add the veth interface as a port of the bridge
  cmd = "ip link set " + vethName + " master " + kDefaultBridgeName;
  if (system(cmd.c_str()) != 0) {
    errExit("adding veth to bridge failed");
  }

  // (7) Enable IP forwarding
  cmd = "sysctl -w net.ipv4.ip_forward=1";
  if (system(cmd.c_str()) != 0) {
    errExit("enabling net.ipv4.ip_forward failed");
  }

  // (8) Enable NAT
  cmd = "iptables -t nat -A POSTROUTING -s " + kDefaultBridgeIp + "/" +
        kDefaultBridgePrefixLen + " -j MASQUERADE";
  if (system(cmd.c_str()) != 0) {
    errExit("enabling NAT failed");
  }
}

// Called in child (container) process
void setupNetwork(const std::string& ip) {
  // (1) Bring up lo interface
  std::string cmd = "ip link set dev lo up";
  if (system(cmd.c_str()) != 0) {
    errExit("bring up lo device failed");
  }

  // (2) Add IP to eth0
  cmd = "ip addr add " + ip + "/" + kDefaultBridgePrefixLen + " dev eth0";
  if (system(cmd.c_str()) != 0) {
    errExit("adding IP to eth0 failed");
  }

  // (3) Bring up eth0 interface
  cmd = "ip link set dev eth0 up";
  if (system(cmd.c_str()) != 0) {
    errExit("bring up eth0 failed");
  }

  // (4) Set default gateway
  cmd = "ip route add default via " + kDefaultBridgeIp;
  if (system(cmd.c_str()) != 0) {
    errExit("setting default gateway failed");
  }
}


bool writeToFile(const std::string& file, const std::string& data) {
  std::ofstream ofs(file);
  if (ofs.is_open()) {
    ofs << data;
    ofs.close();
  } else {
    std::cout << "Error: Failed to open " << file << std::endl;
    return false;
  }
  return true;
}

std::string getContainerCgroup(int cpid) {
  return kCgroupRoot + std::to_string(cpid);
}

bool setupCgroup(int cpid, const ResourceLimit& limit) {
  // (1) Create a cgroup at <root>/<cpid>
  const std::string cgroupPath = getContainerCgroup(cpid);
  if (mkdir(cgroupPath.c_str(), 0755) == -1) {
    perror("mkdir(cgroupPath.c_str(), 0755)");
    return false;
  }

  // (2) Set up resource limit
  // Memory
  if (limit.maxRamBytes > 0) {
    // Try not to reclaim before hitting 75% of the max limit
    int memoryLow = limit.maxRamBytes * 75 / 100;
    int memoryMax = limit.maxRamBytes;
    if (!writeToFile(cgroupPath + "/memory.low", std::to_string(memoryLow))) {
      return false;
    }
    if (!writeToFile(cgroupPath + "/memory.max", std::to_string(memoryMax))) {
      return false;
    }
  }

  // (3) Move the container process to the cgroup
  return writeToFile(cgroupPath + "/cgroup.procs", std::to_string(cpid));
}

void removeCgroup(const std::string& cgroupPath) {
  if (rmdir(cgroupPath.c_str()) == -1) {
    errExit("rmdir(cgroupPath)");
  }
}

void waitForAgent(int pipefd[2]) {
  // Close unused write end of the pipe
  if (close(pipefd[1]) == -1) {
    errExit("[Container] close(pipefd[1])");
  }
  int ret = 0;
  bool success = false;
  // Wait until agent writes to the pipe
  do {
    ret = read(pipefd[0], &success, sizeof(success));
  } while (ret == -1 && errno == EINTR);
  if (ret != sizeof(success) || !success) {
    errExit("[Container] Preparation failed");
  }

  // Read end of the pipe is no longer needed. Close it.
  if (close(pipefd[0]) == -1) {
    errExit("[Container] close(pipefd[0])");
  }
}

int main(int argc, char** argv) {
  std::string rootfs;
  std::string hostname;
  std::string domain;
  std::string ip;

  bool enablePid = false;
  bool enableIpc = false;

  ResourceLimit limit;

  po::options_description options{"Options"};
  options.add_options()
    ("help,h", "Print help message")
    ("verbose,v", po::bool_switch(&verbose)->default_value(false),
     "Enable verose logging")
    ("rootfs,r", po::value<std::string>(&rootfs),
     "Root filesystem path of the container")
    ("pid,p", po::bool_switch(&enablePid)->default_value(false),
     "Enable PID isolation")
    ("hostname,h", po::value<std::string>(&hostname),
     "Hostname of the container")
    ("domain,d", po::value<std::string>(&domain),
     "NIS domain name of the container")
    ("ipc,i", po::bool_switch(&enableIpc),
     "Enable IPC isolation")
    // TODO: Dynamically allocate IP address.
    ("ip", po::value<std::string>(&ip),
     "IP of the container")
    ("max-ram,R", po::value<long long>(&limit.maxRamBytes),
     "The max amount of ram (in bytes) that the container can use");

  std::string cmd;
  po::options_description hiddenOptions{"Hidden Options"};
  hiddenOptions.add_options()("cmd", po::value<std::string>(&cmd));

  po::positional_options_description posOptions;
  posOptions.add("cmd", -1);

  po::options_description cmdlineOptions;
  cmdlineOptions.add(options).add(hiddenOptions);

  po::variables_map vm;
  try {
    po::parsed_options parsedOptions = po::command_line_parser(argc, argv)
                                           .options(cmdlineOptions)
                                           .positional(posOptions)
                                           .run();

    po::store(parsedOptions, vm);
    po::notify(vm);
  } catch (const po::error& ex) {
    std::cerr << ex.what() << std::endl;
    return -1;
  }
  if (vm.count("help") || !vm.count("cmd")) {
    std::cout << "Usage: " << argv[0] << " [options] COMMAND" << std::endl
              << std::endl;
    std::cout << options << std::endl;
    return 0;
  }

  int flags = SIGCHLD;
  if (!rootfs.empty()) {
    flags |= CLONE_NEWNS;
  }
  if (enablePid) {
    flags |= CLONE_NEWPID;
  }
  if (!hostname.empty() || !domain.empty()) {
    flags |= CLONE_NEWUTS;
  }
  if (enableIpc) {
    flags |= CLONE_NEWIPC;
  }
  if (!ip.empty()) {
    // TODO: Validate the IP address and make sure it belongs to the default
    // bridge network
    flags |= CLONE_NEWNET;
  }

  int pipefd[2];
  if (pipe(pipefd) != 0) {
    errExit("pipe failed");
  }
  int readfd = pipefd[0];
  int writefd = pipefd[1];

  // We need to make a raw syscall because we need something like fork(flags)
  // but there is no such wrapper available. In other words, we need to fork
  // current process and create namespaces specified by flags.
  //
  // See https://www.man7.org/linux/man-pages/man2/clone.2.html for this raw
  // syscall signature. This order actually assumes it's on x86-64.
  int cpid = syscall(
      SYS_clone,
      flags,
      nullptr /*stack*/,
      nullptr /*parent_tid*/,
      nullptr /*child_tid*/,
      0 /*tls: only meaningful if CLONE_SETTLS flag is set*/);
  if (cpid == -1) {
    errExit("fork failed");
  }

  if (cpid == 0) {
    // Container
    std::cout << "[Container] Waiting for agent to finish preparation ..."
              << std::endl;
    waitForAgent(pipefd);

    if (!ip.empty()) {
      std::cout << "[Container] Setting up container network ..." << std::endl;
      setupNetwork(ip);
      std::cout << "[Container] Done setting up container network" << std::endl;
    }

    setupFilesystem(rootfs);
    setHostAndDomainName(hostname, domain);
    runContainer(cmd);
  } else {
    // Agent
    if (verbose) {
      std::cout << "[Agent] Container pid: " << cpid << std::endl;
      std::cout << "[Agent] Agent pid: " << getpid() << std::endl;
      std::cout << "[Agent] Agent hostname: " << getHostname() << std::endl;
      std::cout << "[Agent] Agent NIS domain name: " << getNisDomainName()
                << std::endl;
    }
    if (!ip.empty()) {
      std::cout << "[Agent] Preparing network for container ..." << std::endl;
      prepareNetwork(cpid);
      std::cout << "[Agent] Done preparing network for container" << std::endl;
    }

    bool success = setupCgroup(cpid, limit);

    // Close unused read end of the pipe
    if (close(readfd) == -1) {
      errExit("[Agent] close(readfd)");
    }
    // Notify the container to continue
    if (write(writefd, &success, sizeof(success)) == -1) {
      errExit("[Agent] write(writefd)");
    }
    // Close write end of the pipe
    if (close(writefd) == -1) {
      errExit("[Agent] close(writefd)");
    }

    int status;
    if (waitpid(cpid, &status, 0) == -1) {
      errExit("[Agent] waitpid failed");
    }
    if (verbose) {
      std::cout << "[Agent] The container exited with status: " << status
                << std::endl;
    }
    removeCgroup(getContainerCgroup(cpid));
  }

  return 0;
}
