#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/syscall.h>

#include <boost/program_options.hpp>
#include <iostream>
#include <sstream>

void errExit(const char* msg) {
  perror(msg);
  exit(EXIT_FAILURE);
}

namespace po = boost::program_options;

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

int main(int argc, char** argv) {
  std::string rootfs;
  bool enablePid = false;
  po::options_description options{"Options"};
  options.add_options()
    ("help,h", "Print help message")
    ("rootfs,r", po::value<std::string>(&rootfs),
      "Root filesystem path of the container")
    ("pid,p", po::value<bool>(&enablePid),
      "Enable PID isolation");

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
    runContainer(cmd);
  } else {
    // Agent
    std::cout << "[Agent] Container pid: " << cpid << std::endl;
    std::cout << "[Agent] Agent pid: " << getpid() << std::endl;
    if (waitpid(cpid, NULL, 0) == -1) {
      errExit("waitpid failed");
    }
  }

  return 0;
}
