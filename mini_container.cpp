#include <limits.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include <boost/program_options.hpp>
#include <iostream>
#include <sstream>

#define NIS_DOMAIN_NAME_MAX (64)

void errExit(const char* msg) {
  perror(msg);
  exit(EXIT_FAILURE);
}

namespace po = boost::program_options;

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

  std::cout << "[Container] Running command: " << cmd << std::endl;
  std::cout << "[Container] Container hostname: " << getHostname()
            << std::endl;
  std::cout << "[Container] Container NIS domain name: "
            << getNisDomainName() << std::endl;
  execv(tokens[0].c_str(), args.data());

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

int main(int argc, char** argv) {
  std::string rootfs;
  std::string hostname;
  std::string domain;
  bool enablePid = false;
  bool enableIpc = false;

  po::options_description options{"Options"};
  options.add_options()
    ("help,h", "Print help message")
    ("rootfs,r", po::value<std::string>(&rootfs),
     "Root filesystem path of the container")
    ("pid,p", po::bool_switch(&enablePid)->default_value(false),
     "Enable PID isolation")
    ("hostname,h", po::value<std::string>(&hostname),
     "Hostname of the container")
    ("domain,d", po::value<std::string>(&domain),
     "NIS domain name of the container")
    ("ipc,i", po::bool_switch(&enableIpc),
     "Enable IPC isolation");

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
    setupFilesystem(rootfs);
    setHostAndDomainName(hostname, domain);
    runContainer(cmd);
  } else {
    // Agent
    std::cout << "[Agent] Container pid: " << cpid << std::endl;
    std::cout << "[Agent] Agent pid: " << getpid() << std::endl;
    std::cout << "[Agent] Agent hostname: " << getHostname() << std::endl;
    std::cout << "[Agent] Agent NIS domain name: " << getNisDomainName()
              << std::endl;
    if (waitpid(cpid, NULL, 0) == -1) {
      errExit("waitpid failed");
    }
  }

  return 0;
}
