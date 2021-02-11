#include <sys/wait.h>

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

int main(int argc, char** argv) {
  po::options_description options{"Options"};
  options.add_options()("help,h", "Print help message");

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

  int cpid = fork();
  if (cpid == -1) {
    errExit("fork failed");
  }

  if (cpid == 0) {
    // Container
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
